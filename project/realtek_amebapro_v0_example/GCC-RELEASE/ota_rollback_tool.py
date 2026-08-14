#!/usr/bin/env python3
"""Inspect and re-serialise existing AmebaPro OTA images.

This POC understands the uncompressed Smart OTA container used by CarBox and
the non-secure AmebaPro firmware image currently produced by this tree.  It
changes only the boot-selection serial number and its HMAC-SHA256 signature,
the firmware's trailing additive checksum, and the enclosing Smart checksum.
"""

from __future__ import annotations

import argparse
import hashlib
import hmac
import json
import os
from dataclasses import dataclass
from pathlib import Path
import struct
import sys
import tempfile
from typing import Iterable


SMART_FILE_HEADER_SIZE = 8
SMART_IMAGE_HEADER_SIZE = 24
SMART_SIGNATURE = b"OTA\x00"
SMART_APP_IMAGE_ID = 1

FIRMWARE_HEADER_START = 0xE0
FIRMWARE_HEADER_SIZE = 0x60
FIRMWARE_SERIAL_OFFSET = 0x14
FIRMWARE_OTA_SIGNATURE_SIZE = 32
FIRMWARE_CHAIN_END = 0xFFFFFFFF
FIRMWARE_CHECKSUM_SIZE = 4

# These image types carry serial fields in the current CarBox image.  Only the
# first CINIT serial is used for boot selection and changed by this tool.
BOOT_SERIAL_IMAGE_TYPES = {
    11: "CINIT",
    8: "XIP",
    2: "FWHS",
}

MAX_SMART_IMAGES = 8
MAX_FIRMWARE_HEADERS = 32
UINT32_MAX_USABLE = 0xFFFFFFFE


class FormatError(ValueError):
    """The input is not a supported, internally consistent image."""


def additive_checksum(data: bytes | bytearray | memoryview) -> int:
    return sum(data) & 0xFFFFFFFF


@dataclass(frozen=True)
class SmartImage:
    header_offset: int
    checksum: int
    length: int
    offset: int
    image_id: int


@dataclass(frozen=True)
class SmartPackage:
    fw_version: int
    images: tuple[SmartImage, ...]

    @property
    def app_image(self) -> SmartImage:
        apps = [image for image in self.images if image.image_id == SMART_APP_IMAGE_ID]
        if len(apps) != 1:
            raise FormatError(f"expected exactly one APP image, found {len(apps)}")
        return apps[0]


@dataclass(frozen=True)
class FirmwareHeader:
    offset: int
    image_length: int
    next_offset: int
    image_type: int
    serial: int

    @property
    def type_name(self) -> str:
        return BOOT_SERIAL_IMAGE_TYPES.get(self.image_type, f"TYPE_{self.image_type}")


@dataclass(frozen=True)
class FirmwareImage:
    data: bytes
    headers: tuple[FirmwareHeader, ...]
    attached_checksum: int

    @property
    def boot_headers(self) -> tuple[FirmwareHeader, ...]:
        return tuple(
            header for header in self.headers if header.image_type in BOOT_SERIAL_IMAGE_TYPES
        )

    @property
    def boot_serial(self) -> int:
        return self.selection_header.serial

    @property
    def selection_header(self) -> FirmwareHeader:
        # boot_get_serial() in lib_boot.a authenticates and reads serial_no from
        # the first partition header at partition_start + 0xe0.  Serial fields
        # in later chained sub-image headers are not used for FW1/FW2 choice.
        header = self.headers[0]
        if header.offset != FIRMWARE_HEADER_START or header.image_type != 11:
            raise FormatError("first firmware header is not the CINIT selection header")
        return header


@dataclass(frozen=True)
class ParsedInput:
    raw: bytes
    smart: SmartPackage | None
    firmware: FirmwareImage

    @property
    def kind(self) -> str:
        if self.smart is None:
            return "firmware_is"
        return "ota_app" if len(self.smart.images) == 1 else "ota_all"


def parse_smart_package(data: bytes) -> SmartPackage | None:
    if len(data) < SMART_FILE_HEADER_SIZE + SMART_IMAGE_HEADER_SIZE:
        return None
    if data[SMART_FILE_HEADER_SIZE : SMART_FILE_HEADER_SIZE + 4] != SMART_SIGNATURE:
        return None

    fw_version, header_count = struct.unpack_from("<II", data, 0)
    if not 1 <= header_count <= MAX_SMART_IMAGES:
        raise FormatError(f"invalid Smart HdrNum={header_count}")
    total_header_size = SMART_FILE_HEADER_SIZE + header_count * SMART_IMAGE_HEADER_SIZE
    if total_header_size > len(data):
        raise FormatError("truncated Smart header")

    images: list[SmartImage] = []
    expected_offset = total_header_size
    for index in range(header_count):
        header_offset = SMART_FILE_HEADER_SIZE + index * SMART_IMAGE_HEADER_SIZE
        signature, header_length, checksum, length, offset, image_id = struct.unpack_from(
            "<4sIIIII", data, header_offset
        )
        if signature != SMART_SIGNATURE:
            raise FormatError(f"Smart image {index}: invalid signature {signature!r}")
        if header_length != SMART_IMAGE_HEADER_SIZE:
            raise FormatError(f"Smart image {index}: invalid header length {header_length}")
        if offset != expected_offset:
            raise FormatError(
                f"Smart image {index}: non-compact offset 0x{offset:x}, "
                f"expected 0x{expected_offset:x}"
            )
        end = offset + length
        if length == 0 or end > len(data):
            raise FormatError(f"Smart image {index}: invalid payload range")
        actual_checksum = additive_checksum(memoryview(data)[offset:end])
        if checksum != actual_checksum:
            raise FormatError(
                f"Smart image {index}: checksum 0x{checksum:08x}, "
                f"calculated 0x{actual_checksum:08x}"
            )
        images.append(SmartImage(header_offset, checksum, length, offset, image_id))
        expected_offset = end

    if expected_offset != len(data):
        raise FormatError(
            f"Smart package has {len(data) - expected_offset} trailing byte(s)"
        )
    package = SmartPackage(fw_version, tuple(images))
    package.app_image
    return package


def parse_firmware(data: bytes) -> FirmwareImage:
    minimum = FIRMWARE_HEADER_START + FIRMWARE_HEADER_SIZE + FIRMWARE_CHECKSUM_SIZE
    if len(data) < minimum:
        raise FormatError("firmware payload is too small")

    body = memoryview(data)[:-FIRMWARE_CHECKSUM_SIZE]
    attached_checksum = struct.unpack_from("<I", data, len(data) - 4)[0]
    calculated_checksum = additive_checksum(body)
    if attached_checksum != calculated_checksum:
        raise FormatError(
            f"firmware checksum 0x{attached_checksum:08x}, "
            f"calculated 0x{calculated_checksum:08x}"
        )

    headers: list[FirmwareHeader] = []
    seen: set[int] = set()
    offset = FIRMWARE_HEADER_START
    for _ in range(MAX_FIRMWARE_HEADERS):
        if offset in seen:
            raise FormatError(f"firmware header loop at 0x{offset:x}")
        seen.add(offset)
        if offset + FIRMWARE_HEADER_SIZE > len(body):
            raise FormatError(f"firmware header at 0x{offset:x} is truncated")

        image_length, next_offset = struct.unpack_from("<II", body, offset)
        image_type = body[offset + 8]
        serial = struct.unpack_from("<I", body, offset + FIRMWARE_SERIAL_OFFSET)[0]
        if image_length == 0:
            raise FormatError(f"firmware header at 0x{offset:x} has zero image length")
        image_end = offset + FIRMWARE_HEADER_SIZE + image_length
        if image_end > len(body):
            raise FormatError(f"firmware image at 0x{offset:x} exceeds payload")

        headers.append(
            FirmwareHeader(offset, image_length, next_offset, image_type, serial)
        )
        if next_offset == FIRMWARE_CHAIN_END:
            # elf2bin pads the final image to a 64-byte boundary.
            trailing = len(body) - image_end
            if not 0 <= trailing <= 64:
                raise FormatError(f"firmware has unexpected {trailing} byte final padding")
            break
        if next_offset < FIRMWARE_HEADER_SIZE + image_length:
            raise FormatError(
                f"firmware header at 0x{offset:x} has overlapping next offset "
                f"0x{next_offset:x}"
            )
        offset += next_offset
    else:
        raise FormatError("firmware header chain is too long")

    firmware = FirmwareImage(data, tuple(headers), attached_checksum)
    firmware.boot_serial
    return firmware


def parse_input(data: bytes) -> ParsedInput:
    smart = parse_smart_package(data)
    if smart is None:
        firmware_data = data
    else:
        app = smart.app_image
        firmware_data = data[app.offset : app.offset + app.length]
    return ParsedInput(data, smart, parse_firmware(firmware_data))


def verify_firmware_signature(firmware: FirmwareImage, hash_key: bytes) -> None:
    if len(hash_key) != 32:
        raise FormatError("firmware hash key must be exactly 32 bytes")
    header = firmware.selection_header
    header_data = firmware.data[
        header.offset : header.offset + FIRMWARE_HEADER_SIZE
    ]
    expected = hmac.new(hash_key, header_data, hashlib.sha256).digest()
    if not hmac.compare_digest(
        firmware.data[:FIRMWARE_OTA_SIGNATURE_SIZE], expected
    ):
        raise FormatError(
            "hash key does not authenticate the input OTA signature; "
            "check partition.json FW1/FW2 hash_key"
        )


def repack(
    parsed: ParsedInput,
    new_serial: int,
    new_fw_version: int | None,
    hash_key: bytes,
) -> bytes:
    if not 1 <= new_serial <= UINT32_MAX_USABLE:
        raise FormatError(f"serial must be in range 1..{UINT32_MAX_USABLE}")
    old_serial = parsed.firmware.boot_serial
    if new_serial <= old_serial:
        raise FormatError(
            f"new serial {new_serial} must be greater than image serial {old_serial}"
        )

    verify_firmware_signature(parsed.firmware, hash_key)

    firmware_body = bytearray(parsed.firmware.data[:-FIRMWARE_CHECKSUM_SIZE])
    selection = parsed.firmware.selection_header
    struct.pack_into(
        "<I", firmware_body, selection.offset + FIRMWARE_SERIAL_OFFSET, new_serial
    )
    new_header = bytes(
        firmware_body[selection.offset : selection.offset + FIRMWARE_HEADER_SIZE]
    )
    firmware_body[:FIRMWARE_OTA_SIGNATURE_SIZE] = hmac.new(
        hash_key, new_header, hashlib.sha256
    ).digest()
    firmware_data = bytes(firmware_body) + struct.pack(
        "<I", additive_checksum(firmware_body)
    )

    if parsed.smart is None:
        output = firmware_data
    else:
        output_buffer = bytearray(parsed.raw)
        app = parsed.smart.app_image
        if len(firmware_data) != app.length:
            raise AssertionError("serial patch unexpectedly changed firmware length")
        output_buffer[app.offset : app.offset + app.length] = firmware_data
        struct.pack_into(
            "<I", output_buffer, app.header_offset + 8, additive_checksum(firmware_data)
        )
        if new_fw_version is not None:
            if not 0 <= new_fw_version <= 0xFFFFFFFF:
                raise FormatError("FwVer must fit in uint32")
            struct.pack_into("<I", output_buffer, 0, new_fw_version)
        output = bytes(output_buffer)

    verified = parse_input(output)
    verify_firmware_signature(verified.firmware, hash_key)
    if verified.firmware.boot_serial != new_serial:
        raise AssertionError("output serial verification failed")
    if parsed.smart is not None:
        for before, after in zip(parsed.smart.images, verified.smart.images):
            if before.image_id != SMART_APP_IMAGE_ID:
                old_payload = parsed.raw[before.offset : before.offset + before.length]
                new_payload = output[after.offset : after.offset + after.length]
                if old_payload != new_payload:
                    raise AssertionError(f"non-APP image ID {before.image_id} changed")
    return output


def describe(parsed: ParsedInput) -> Iterable[str]:
    yield f"Package type       : {parsed.kind}"
    if parsed.smart is not None:
        yield f"Smart FwVer        : {parsed.smart.fw_version}"
        for index, image in enumerate(parsed.smart.images):
            yield (
                f"Smart image[{index}]  : id={image.image_id} "
                f"offset=0x{image.offset:x} length={image.length} "
                f"checksum=0x{image.checksum:08x}"
            )
    yield f"Firmware length    : {len(parsed.firmware.data)}"
    yield f"Firmware checksum  : 0x{parsed.firmware.attached_checksum:08x} (valid)"
    yield f"Boot serial        : {parsed.firmware.boot_serial}"
    for header in parsed.firmware.headers:
        marker = " *" if header.image_type in BOOT_SERIAL_IMAGE_TYPES else ""
        yield (
            f"FW header          : {header.type_name:<8} offset=0x{header.offset:x} "
            f"length=0x{header.image_length:x} serial={header.serial}{marker}"
        )
    yield "OTA signature      : HMAC-SHA256 over CINIT header (key check required)"


def atomic_write(path: Path, data: bytes) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    mode = None
    if path.exists():
        mode = path.stat().st_mode
    with tempfile.NamedTemporaryFile(dir=path.parent, prefix=f".{path.name}.", delete=False) as f:
        temporary = Path(f.name)
        try:
            f.write(data)
            f.flush()
            os.fsync(f.fileno())
            if mode is not None:
                os.chmod(temporary, mode)
            os.replace(temporary, path)
        except Exception:
            temporary.unlink(missing_ok=True)
            raise


def load(path: Path) -> ParsedInput:
    try:
        return parse_input(path.read_bytes())
    except OSError as error:
        raise FormatError(str(error)) from error


def parse_uint32(text: str) -> int:
    try:
        value = int(text, 0)
    except ValueError as error:
        raise argparse.ArgumentTypeError(f"invalid integer: {text}") from error
    if not 0 <= value <= 0xFFFFFFFF:
        raise argparse.ArgumentTypeError("value must fit in uint32")
    return value


def parse_hash_key(text: str) -> bytes:
    try:
        value = bytes.fromhex(text)
    except ValueError as error:
        raise argparse.ArgumentTypeError("hash key must be hexadecimal") from error
    if len(value) != 32:
        raise argparse.ArgumentTypeError("hash key must contain exactly 64 hex digits")
    return value


def load_partition_hash_key(path: Path) -> bytes:
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
        fw1 = parse_hash_key(document["partab"]["fw1"]["hash_key"])
        fw2 = parse_hash_key(document["partab"]["fw2"]["hash_key"])
    except (OSError, json.JSONDecodeError, KeyError, TypeError) as error:
        raise FormatError(f"cannot read FW1/FW2 hash keys from {path}: {error}") from error
    except argparse.ArgumentTypeError as error:
        raise FormatError(f"invalid hash key in {path}: {error}") from error
    if fw1 != fw2:
        raise FormatError(f"FW1 and FW2 hash keys differ in {path}")
    return fw1


def add_hash_key_arguments(parser: argparse.ArgumentParser) -> None:
    key_group = parser.add_mutually_exclusive_group()
    key_group.add_argument("--hash-key", type=parse_hash_key)
    key_group.add_argument(
        "--partition-json",
        type=Path,
        help="partition table JSON containing matching FW1/FW2 hash_key values",
    )


def resolve_hash_key(args: argparse.Namespace) -> bytes:
    if args.hash_key is not None:
        return args.hash_key
    partition_json = args.partition_json
    if partition_json is None:
        partition_json = Path(__file__).resolve().with_name("partition.json")
    return load_partition_hash_key(partition_json)


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    inspect_parser = subparsers.add_parser("inspect")
    inspect_parser.add_argument("input", type=Path)

    verify_parser = subparsers.add_parser("verify")
    verify_parser.add_argument("input", type=Path)
    add_hash_key_arguments(verify_parser)

    repack_parser = subparsers.add_parser("repack")
    repack_parser.add_argument("input", type=Path)
    repack_parser.add_argument("--serial", required=True, type=parse_uint32)
    repack_parser.add_argument(
        "--current-device-serial",
        type=parse_uint32,
        help="also require --serial to be greater than the currently running device SN",
    )
    repack_parser.add_argument("--fw-ver", type=parse_uint32)
    repack_parser.add_argument("--output", required=True, type=Path)
    add_hash_key_arguments(repack_parser)
    return parser


def main(argv: list[str] | None = None) -> int:
    args = build_parser().parse_args(argv)
    try:
        parsed = load(args.input)
        if args.command == "inspect":
            print("\n".join(describe(parsed)))
            return 0
        if args.command == "verify":
            verify_firmware_signature(parsed.firmware, resolve_hash_key(args))
            print("\n".join(describe(parsed)))
            print("Signature          : valid with FW1/FW2 hash key")
            return 0

        if args.input.resolve() == args.output.resolve():
            raise FormatError("refusing to overwrite the input file")
        if (
            args.current_device_serial is not None
            and args.serial <= args.current_device_serial
        ):
            raise FormatError(
                f"new serial {args.serial} must be greater than current device serial "
                f"{args.current_device_serial}"
            )

        hash_key = resolve_hash_key(args)
        output = repack(parsed, args.serial, args.fw_ver, hash_key)
        atomic_write(args.output, output)
        result = parse_input(output)
        print("\n".join(describe(result)))
        print(f"Output             : {args.output}")
        print("Signature          : recalculated and verified with FW1/FW2 hash key")
        return 0
    except FormatError as error:
        print(f"ERROR: {error}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
