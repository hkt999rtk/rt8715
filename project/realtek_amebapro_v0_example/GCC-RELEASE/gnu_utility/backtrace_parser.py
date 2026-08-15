#!/usr/bin/env python3
#coding:utf-8

"""
Backtrace parser adapted for Realtek RTL8195BH / AmebaPro SDK.

Resolves crash backtrace addresses to source file:line (text mode) or
to the nearest linker symbol (map mode, default).

Usage:
  # map mode (default) — resolve addresses against a linker map file
  python3 backtrace_parser.py <crash_log> <map_file>
  python3 backtrace_parser.py map <crash_log> <map_file>

  # text mode — resolve addresses against an ELF via addr2line
  python3 backtrace_parser.py text <crash_log> <elf_file>

Or pipe backtrace lines directly:
  grep "P-stack back trace" -A20 serial.log | python3 backtrace_parser.py - <map_file>
"""

import os
import sys
import subprocess
import argparse
import re

MATCH_ADDR = re.compile(r'([0-9a-fA-F]{5,8})')

_TOOLCHAIN_PREFIX = "arm-none-eabi-"
_ADDR2LINE = _TOOLCHAIN_PREFIX + "addr2line"
_ADDR2LINE_OPT = "-pfiCe"  # -p: pretty-print, -f: function, -i: inlined, -C: demangle, -e: exe
_NM = _TOOLCHAIN_PREFIX + "nm"
_NM_OPT = "-nlCS"  # numeric sort, demangle, no symbols

# ============================================================================
# map mode — parse linker map file
# ============================================================================

# Matches lines like:
#   SIZE:   "                0x0000000000010000       0x14 application_is/Debug/obj/api_lib.o"
#   SYMBOL: "                0x0000000000010040                netconn_new_with_proto_and_callback"
_MAP_LINE_RE = re.compile(r'\s+(0x[0-9a-fA-F]{16})\s+(.+)')

_map_cache = {}  # path -> sorted list of (addr, name, obj)


def _short_obj(path):
    """Extract a short object/archive name from a full path."""
    if not path:
        return '?'
    # Archive member: ".../libfoo.a(bar.o)" → "libfoo.a(bar.o)"
    m = re.search(r'([^/\\]+\([^)]+\))$', path)
    if m:
        return m.group(1)
    # Plain object: ".../foo.o" → "foo.o"
    basename = os.path.basename(path)
    if basename:
        return basename
    return path


def parse_map_file(map_path):
    """Parse a GNU ld linker map file, return sorted (addr, name, obj) list."""
    global _map_cache
    if map_path in _map_cache:
        return _map_cache[map_path]

    syms = []
    current_obj = '?'
    try:
        with open(map_path, 'r', encoding='utf-8', errors='replace') as f:
            for line in f:
                m = _MAP_LINE_RE.match(line)
                if not m:
                    continue
                try:
                    addr = int(m.group(1), 16)
                except ValueError:
                    continue
                rest = m.group(2).strip()

                if rest.startswith('0x'):
                    # SIZE line: record the object file for subsequent symbols
                    parts = rest.split(None, 1)
                    if len(parts) > 1:
                        current_obj = _short_obj(parts[1])
                    continue

                # Extract symbol name: first whitespace-delimited token
                name = rest.split()[0] if rest else ''

                # Skip section headers, empty names, and object-file references
                if not name or name.startswith('.') or '/' in name or '\\' in name:
                    continue

                syms.append((addr, name, current_obj))
    except OSError as e:
        print(f"Error reading map file: {e}", file=sys.stderr)
        _map_cache[map_path] = []
        return []

    # Deduplicate: keep the first occurrence of each address
    seen = set()
    deduped = []
    for addr, name, obj in syms:
        if addr not in seen:
            seen.add(addr)
            deduped.append((addr, name, obj))

    deduped.sort(key=lambda x: x[0])
    _map_cache[map_path] = deduped
    return deduped


def resolve_from_map(map_path, addr_int):
    """Find nearest symbol <= addr_int from the linker map file.
    Returns (result_str, obj) or (None, None)."""
    syms = parse_map_file(map_path)
    if not syms:
        return None, None
    best = None
    best_obj = '?'
    for a, name, obj in syms:
        if a <= addr_int:
            best = f"{name}+0x{addr_int - a:x}"
            best_obj = obj
        else:
            break
    return best, best_obj


def _print_crash_header(info):
    """Print crash metadata: assert location, task, LR."""
    parts = []
    if info.get('assert_file'):
        parts.append(f"assert: {info['assert_file']}:{info.get('assert_line','?')}")
    if info.get('crash_task'):
        parts.append(f"task: {info['crash_task']}")
    if parts:
        print(f"Crash: {', '.join(parts)}")


def parse_crash_log_map(crash_fp, map_path):
    """Parse crash log and resolve addresses against a linker map file."""
    info, addrs = _extract_backtrace_addrs(crash_fp)
    if not addrs:
        print("No backtrace addresses found in input.")
        return

    _print_crash_header(info)

    syms = parse_map_file(map_path)
    if not syms:
        print("Failed to parse map file or no symbols found.")
        return

    valid = []
    skipped = 0
    for addr_str in addrs:
        if _is_garbage_addr(addr_str):
            skipped += 1
            continue
        try:
            addr_int = int(addr_str, 16)
        except ValueError:
            skipped += 1
            continue
        result, obj = resolve_from_map(map_path, addr_int)
        if result and _looks_plausible(result):
            valid.append((addr_str, addr_int, f"{result}  [{obj}]"))
        else:
            skipped += 1

    print(f"Stack backtrace ({len(valid)} valid frames, {skipped} skipped) from {map_path}:")
    print()
    for i, (addr_str, addr_int, result) in enumerate(valid):
        print(f"  #{i:<3} {addr_str:>8s}  {result}")
    if not valid:
        print("  (no valid frames)")


# ============================================================================
# text mode — addr2line + nm fallback
# ============================================================================

# cache: elf_file -> nearest symbol from nm
_nm_cache = {}

# Linker-defined section boundary symbols:
#   __dtcm_ram_bss_start__, __psram_code_text_end__, __cinit_ro_end__, etc.
_SECTION_MARKER_RE = re.compile(
    r'(^__.*_(start|end|begin|limit|size|length|base)__$)'   # linker __xxx_start__ etc.
    r'|(^(RAM|ROM|FLASH|ERAM|SRAM|DTCM|ITCM|XIP).*(_LENGTH|_END|_START|_LIMIT|_BASE)$)'  # RAM_LENGTH etc.
    r'|(.*(_LENGTH|_END|_START|_LIMIT|_BASE)__$)'            # __xxx_LENGTH__ etc.
)

# Known fill / garbage patterns (case-insensitive hex match on address string)
_GARBAGE_PATTERNS = (
    re.compile(r'^0+$'),                        # 00000000
    re.compile(r'^(a5)+$', re.I),               # 0xA5A5A5A5 (ARM Realtek fill)
    re.compile(r'^deadbeef$', re.I),            # 0xDEADBEEF
    re.compile(r'^f+$', re.I),                  # 0xFFFFFFFF
    re.compile(r'^[0-9a-f]?([0-9a-f])\1{5,}$', re.I),  # repeated hex digit (fill patterns)
)
# Maximum plausible offset for a valid backtrace address
_MAX_PLAUSIBLE_OFFSET = 0x40000   # 256 KB


def load_nm_symbols(elf_file, exclude_section_markers=False):
    """Load all function symbols from nm output, return sorted (addr, name) list.

    Handles both formats:
      -nlC:  <addr> <type> <name>
      -nlCS: <addr> <size> <type> <name>\\t<file:line>
    """
    global _nm_cache
    cache_key = (elf_file, exclude_section_markers)
    if cache_key in _nm_cache:
        return _nm_cache[cache_key]

    try:
        out = subprocess.check_output(
            [_NM, _NM_OPT, elf_file],
            stderr=subprocess.DEVNULL,
            timeout=30
        ).decode('utf-8', errors='replace')
    except Exception:
        _nm_cache[cache_key] = []
        return []

    syms = []
    for line in out.splitlines():
        line = line.strip()
        if not line:
            continue
        parts = line.split(None, 3)  # split on any whitespace, max 3 splits
        if len(parts) < 3:
            continue

        if len(parts) == 3:
            # Format: addr type name
            addr_str, kind, name = parts
        else:
            # Format: addr size type name[\\tfile:line]
            addr_str, size_str, kind, rest = parts
            # Strip line-number suffix from name
            name = rest.split('\t')[0] if '\t' in rest else rest.split()[0]

        if kind.upper() not in ('T', 'W', 't', 'w'):
            continue
        if exclude_section_markers and _SECTION_MARKER_RE.match(name):
            continue
        try:
            addr = int(addr_str, 16)
        except ValueError:
            continue
        syms.append((addr, name))
    syms.sort(key=lambda x: x[0])
    _nm_cache[cache_key] = syms
    return syms


def find_nearest_symbol(elf_file, addr_int):
    """Find the nearest function symbol <= addr_int."""
    syms = load_nm_symbols(elf_file)
    if not syms:
        return None
    best = None
    for a, name in syms:
        if a <= addr_int:
            best = f"{name}+0x{addr_int - a:x}"
        else:
            break
    return best


def _shorten_path(text):
    """Shorten absolute paths in addr2line output: collapse ../.. and strip to project root.
    '/home/linzh/.../GCC-RELEASE/../../../component/BoxApp/encoder.c:2716' → 'component/BoxApp/encoder.c:2716'"""
    def _collapse(m):
        path = m.group(0)
        # Normalize: collapse 'dir/../' sequences
        parts = [p for p in path.split('/') if p]
        i = 0
        while i < len(parts):
            if parts[i] == '..' and i > 0 and parts[i-1] not in ('..',):
                del parts[i]
                del parts[i-1]
                i -= 1
            else:
                i += 1
        path = '/'.join(parts)

        # Strip down to first recognizable project directory
        for marker in ('component/', 'code/', 'project/', 'sdk/', 'src/'):
            idx = path.find(marker)
            if idx >= 0:
                path = path[idx:]
                break
        return path

    return re.sub(r'[\w./-]+\.(c|h|cpp|hpp|s|S):\d+', _collapse, text)


def resolve_addr(elf_file, addr):
    """Run addr2line for a single address; returns multi-line string with inline chain,
    or None if unresolved. Falls back to nm when addr2line returns ??:?."""
    try:
        out = subprocess.check_output(
            [_ADDR2LINE, _ADDR2LINE_OPT, elf_file, addr],
            stderr=subprocess.STDOUT,
            timeout=5
        ).decode('utf-8', errors='replace').strip()
        if out and '??' not in out:
            return _shorten_path(out)
    except Exception:
        pass

    # addr2line returned ??:? or failed — fall back to nm
    try:
        addr_int = int(addr, 16)
        sym = find_nearest_symbol(elf_file, addr_int)
        if sym:
            return sym
    except ValueError:
        pass
    return None


def parse_crash_log_text(crash_fp, elf_file, obj_lookup=None):
    """Parse crash log and resolve backtrace via addr2line (file:line precision)."""
    info, addrs = _extract_backtrace_addrs(crash_fp)
    if not addrs:
        print("No backtrace addresses found in input.")
        return

    _print_crash_header(info)

    # Filter valid frames first
    valid = []
    skipped = 0
    for addr_str in addrs:
        if _is_garbage_addr(addr_str):
            skipped += 1
            continue
        try:
            addr_int = int(addr_str, 16)
        except ValueError:
            skipped += 1
            continue
        result = resolve_addr(elf_file, addr_str)
        if result and '??' not in result and _looks_plausible(result):
            valid.append((addr_str, addr_int, result))
        else:
            skipped += 1

    print(f"Stack backtrace ({len(valid)} valid frames, {skipped} skipped) using {elf_file}:")
    print()
    for i, (addr_str, addr_int, result) in enumerate(valid):
        for line in _format_addr_result(addr_str, result, obj_lookup, addr_int, frame_num=i):
            print(line)
    if not valid:
        print("  (no valid frames)")


# ============================================================================
# symbols mode — nm symbol table lookup
# ============================================================================

def parse_crash_log_symbols(crash_fp, elf_file, obj_lookup=None):
    """Parse crash log and resolve addresses via nm symbol table only."""
    info, addrs = _extract_backtrace_addrs(crash_fp)
    if not addrs:
        print("No backtrace addresses found in input.")
        return

    _print_crash_header(info)

    syms = load_nm_symbols(elf_file, exclude_section_markers=True)
    if not syms:
        print("Failed to load symbols via nm.")
        return

    valid = []
    skipped = 0
    for addr_str in addrs:
        if _is_garbage_addr(addr_str):
            skipped += 1
            continue
        try:
            addr_int = int(addr_str, 16)
        except ValueError:
            skipped += 1
            continue
        result = _resolve_with_obj(syms, addr_int, obj_lookup)
        if result and _looks_plausible(result):
            valid.append((addr_str, addr_int, result))
        else:
            skipped += 1

    print(f"Stack backtrace ({len(valid)} valid frames, {skipped} skipped) from nm on {elf_file}:")
    print()
    for i, (addr_str, addr_int, result) in enumerate(valid):
        print(f"  #{i:<3} {addr_str:>8s}  {result}")
    if not valid:
        print("  (no valid frames)")


# ============================================================================
# shared helpers
# ============================================================================

def _is_garbage_addr(addr_str):
    """Return True if the address matches a known garbage/fill pattern,
    or is a very low address (< 0x100, likely vector table / NULL area)."""
    try:
        addr_int = int(addr_str, 16)
    except ValueError:
        return True
    if addr_int < 0x100:
        return True
    for pat in _GARBAGE_PATTERNS:
        if pat.fullmatch(addr_str):
            return True
    return False


def _is_section_boundary(name):
    """Return True if the name is a linker section boundary marker."""
    return bool(_SECTION_MARKER_RE.match(name))


def _looks_plausible(result_str, max_offset=_MAX_PLAUSIBLE_OFFSET):
    """Return False if the resolved result is a section boundary marker or has
    an implausibly large offset (indicating the address is garbage)."""
    # Extract name from "name+0xNNN" or "name at file:line" format
    if '+' in result_str:
        name = result_str.split('+')[0]
    elif ' at ' in result_str:
        name = result_str.split(' at ')[0]
    else:
        name = result_str

    # Section boundary markers are never useful in a backtrace
    if _is_section_boundary(name):
        return False

    m = re.search(r'\+0x([0-9a-fA-F]+)', result_str)
    if not m:
        return True  # no offset, always plausible
    offset = int(m.group(1), 16)
    return offset <= max_offset


def _format_addr_result(addr_str, result, obj_lookup, addr_int, frame_num=None):
    """Format one resolved line as a backtrace frame. Returns list of lines."""
    if not result:
        return [f"  #{frame_num:<3} {addr_str:>8s}  ??"] if frame_num is not None else [f"  {addr_str}: ??"]

    lines = result.split('\n')
    first = lines[0]
    rest = lines[1:] if len(lines) > 1 else []

    obj_tag = ''
    if obj_lookup:
        obj = _find_obj_for_addr(obj_lookup, addr_int)
        obj_tag = f"  [{obj}]"

    # Frame header:
    if frame_num is not None:
        header = f"  #{frame_num:<3} {addr_str:>8s}  {first}{obj_tag}"
    else:
        pad = ' ' * max(1, 50 - len(first))
        header = f"  {addr_str}: {first}{pad}{obj_tag}"

    output = [header]

    # Inline chain (addr2line -i output)
    indent = ' ' * 15  # align under function name
    for l in rest:
        output.append(f"{indent}{l}")

    return output


def _build_obj_lookup(map_path):
    """Build an {addr: obj_name} lookup from a map file for cross-reference."""
    syms = parse_map_file(map_path)
    if not syms:
        return {}
    return {addr: obj for addr, name, obj in syms}


def _find_obj_for_addr(obj_lookup, addr_int):
    """Find the object file for the nearest address <= addr_int."""
    if not obj_lookup:
        return '?'
    obj = '?'
    for a in sorted(obj_lookup):
        if a <= addr_int:
            obj = obj_lookup[a]
        else:
            break
    return obj


def _resolve_with_obj(syms, addr_int, obj_lookup=None):
    """Resolve addr against syms list, optionally appending [obj]."""
    best = None
    for a, name in syms:
        if a <= addr_int:
            best = f"{name}+0x{addr_int - a:x}"
        else:
            break
    if not best:
        return None
    if obj_lookup:
        obj = _find_obj_for_addr(obj_lookup, addr_int)
        pad = ' ' * max(1, 45 - len(best))
        return f"{best}{pad}[{obj}]"
    return best


# Matches: ASSET FAILED: path/to/file.c:123
_ASSERT_RE = re.compile(r'ASSERT\s+FAILED[:\s]+(\S+):(\d+)', re.I)
# Matches: LR = 0xXXXXXXXX
_LR_RE = re.compile(r'LR\s*=\s*(0x[0-9a-fA-F]+)')
# Matches: current task: task_name
_TASK_RE = re.compile(r'current\s+task\s*[:\s]+(\S+)', re.I)
# Matches: Call stack trace header
_CALLSTACK_HDR_RE = re.compile(r'call\s+stack\s+trace', re.I)
# Matches: Raw stack dump header
_RAWSTACK_HDR_RE = re.compile(r'raw\s+stack\s+dump', re.I)
# Matches: #00 [SP+0x...] 0xXXXXXXXX
_STACKFRAME_RE = re.compile(r'#(\d+)\s*\[.*\]\s*(0x[0-9a-fA-F]+)')
# Matches: [00] 0xXXXXXXXX
_RAWSTACK_RE = re.compile(r'\[(\d+)\]\s*(0x[0-9a-fA-F]+)')


def _extract_backtrace_addrs(crash_fp):
    """Extract hex addresses from crash log lines. Supports two formats:
    1. 'P-stack back trace' — space-separated hex addresses
    2. ASSERT FAILED + register dump + call stack / raw stack

    Returns (crash_info, addrs) where crash_info is a dict with keys:
      assert_file, assert_line, crash_task, lr, stack_frames
    """
    info = {}
    addrs = []
    lines = crash_fp.read().splitlines()

    # --- Parse crash metadata ---
    for line in lines:
        line_s = line.strip()
        if not line_s:
            continue

        # ASSERT FAILED
        m = _ASSERT_RE.search(line_s)
        if m:
            info['assert_file'] = os.path.basename(m.group(1))
            info['assert_line'] = m.group(2)

        # Current task
        m = _TASK_RE.search(line_s)
        if m and 'crash_task' not in info:
            info['crash_task'] = m.group(1)

        # LR register
        m = _LR_RE.search(line_s)
        if m and 'lr' not in info:
            info['lr'] = m.group(1)

    # --- Extract stack frame addresses ---
    # Priority 1: "Call stack trace" section
    in_callstack = False
    callstack_frames = []
    for line in lines:
        line_s = line.strip()
        if not line_s:
            continue

        if _CALLSTACK_HDR_RE.search(line_s):
            in_callstack = True
            continue

        if in_callstack:
            if line_s.startswith('Raw') or line_s.startswith('---'):
                break
            m = _STACKFRAME_RE.search(line_s)
            if m:
                callstack_frames.append(('#' + m.group(1), m.group(2)))
            elif callstack_frames:
                break

    # Only use call stack trace if it has at least one plausible address
    cs_addrs = [addr for _, addr in callstack_frames]
    if callstack_frames and any(not _is_garbage_addr(a) for a in cs_addrs):
        info['stack_frames'] = callstack_frames
        addrs = cs_addrs

    # Priority 2: "Raw stack dump" section (fallback if call stack trace is empty/garbage)
    if not addrs:
        in_rawstack = False
        raw_frames = []
        for line in lines:
            line_s = line.strip()
            if _RAWSTACK_HDR_RE.search(line_s):
                in_rawstack = True
                continue
            if in_rawstack:
                if line_s.startswith('---') or line_s.startswith('Task'):
                    break
                m = _RAWSTACK_RE.search(line_s)
                if m:
                    raw_frames.append(('[stack]', m.group(2)))
        if raw_frames:
            info['stack_frames'] = raw_frames
            addrs = [addr for _, addr in raw_frames]

    # Priority 3: Legacy "P-stack back trace" format
    if not addrs:
        in_backtrace = False
        for line in lines:
            line_s = line.strip()
            if not line_s:
                continue
            if 'back trace' in line_s.lower() or 'backtrace' in line_s.lower():
                in_backtrace = True
                continue
            if in_backtrace:
                found = MATCH_ADDR.findall(line_s)
                if found:
                    addrs.extend(found)
                elif addrs:
                    break

    # Priority 4: desperate fallback — all hex values in the file
    if not addrs:
        for line in lines:
            addrs.extend(MATCH_ADDR.findall(line.strip()))

    # Prepend LR if present (most valuable address in ASSERT crash)
    if 'lr' in info and info['lr'] not in addrs:
        addrs.insert(0, info['lr'])
        if 'stack_frames' in info:
            info['stack_frames'].insert(0, ('LR', info['lr']))

    return info, addrs


# ============================================================================
# main
# ============================================================================

def main():
    parser = argparse.ArgumentParser(
        description='8715 backtrace parser — resolve crash addresses.',
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""Examples:
  %(prog)s crash.log image.map          # map mode (default)
  %(prog)s map crash.log image.map      # map mode (explicit)
  %(prog)s text crash.log image.axf     # text mode (addr2line)
  %(prog)s symbols crash.log image.axf  # symbols mode (nm)"""
    )
    parser.add_argument(
        'mode', nargs='?', default='map', choices=['map', 'text', 'symbols'],
        help='Resolution mode: "map" (default, linker map), "text" (addr2line), or "symbols" (nm)'
    )
    parser.add_argument(
        'crash_log',
        help='Crash log file path, or "-" for stdin'
    )
    parser.add_argument(
        'target_file',
        help='Linker map file (map mode) or ELF file (text/symbols mode)'
    )
    parser.add_argument(
        '--map', dest='map_file', metavar='MAP_FILE', default=None,
        help='Optional: cross-reference with linker map to show [object file]'
    )
    args = parser.parse_args()

    if args.crash_log == '-':
        crash_fp = sys.stdin
    else:
        crash_fp = open(args.crash_log, 'r', encoding='utf-8', errors='replace')

    obj_lookup = None
    if args.map_file:
        obj_lookup = _build_obj_lookup(args.map_file)

    try:
        if args.mode == 'map':
            parse_crash_log_map(crash_fp, args.target_file)
        elif args.mode == 'symbols':
            parse_crash_log_symbols(crash_fp, args.target_file, obj_lookup)
        else:
            parse_crash_log_text(crash_fp, args.target_file, obj_lookup)
    finally:
        if crash_fp is not sys.stdin:
            crash_fp.close()


if __name__ == "__main__":
    main()
