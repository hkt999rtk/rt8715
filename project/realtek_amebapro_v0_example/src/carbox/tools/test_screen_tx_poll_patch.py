#!/usr/bin/env python3
"""Check that the derived customer archive waits on the screen TCP socket."""

import pathlib
import os
import re
import subprocess
import struct
import tempfile
import unittest
from unittest.mock import patch as mock_patch


BUILD = pathlib.Path(__file__).resolve().parents[3] / "GCC-RELEASE"
TOOLCHAIN = pathlib.Path(
    "/home/kevin/work/toolchains/arm-none-eabi-gcc-6.4.1-realtek/"
    "asdk/linux/newlib/bin"
)
OLD_ARCHIVE = pathlib.Path(os.environ.get(
    "CARBOX_SCREEN_OLD_ARCHIVE",
    "/home/kevin/work/carplay_app_before_mono_20261103/lib_Accessory2.a"))
NEW_ARCHIVE = BUILD / "carplay_app/lib_Accessory2.a"


class ScreenTxPollPatchTest(unittest.TestCase):
    def test_legacy_delay_archive_is_rejected(self):
        if not OLD_ARCHIVE.exists():
            self.skipTest("set CARBOX_SCREEN_OLD_ARCHIVE for legacy rejection check")
        with tempfile.TemporaryDirectory() as directory:
            obj = pathlib.Path(directory) / "screen.o"
            obj.write_bytes(subprocess.check_output([
                str(TOOLCHAIN / "arm-none-eabi-ar"), "p", str(OLD_ARCHIVE), "AirPlayScreen.o"]))
            result = subprocess.run([
                "python3", str(BUILD / "../src/carbox/tools/patch_screen_wait_relocation.py"),
                str(obj), "native-poll"], capture_output=True, text=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("unrecognized normal-frame layout", result.stderr)

    def test_mono_native_poll_and_early_usb_policy(self):
        from test_patch_airplay_music_output import elf_object
        with tempfile.TemporaryDirectory() as directory:
            archive = pathlib.Path(directory) / "Accessory2_mono.a"
            command = [
                "sh", str(BUILD / "carplay_app/patch_video_handover_archive.sh"),
                "accessory-wait", str(TOOLCHAIN / "arm-none-eabi-ar"),
                str(TOOLCHAIN / "arm-none-eabi-objcopy"), str(NEW_ARCHIVE),
                str(archive), "AirPlayScreen.o", "0", "0", "0", "0", "0", "1",
            ]
            subprocess.run(command, check=True, capture_output=True, text=True)
            extract = lambda path: subprocess.check_output([
                str(TOOLCHAIN / "arm-none-eabi-ar"), "p", str(path), "AirPlayScreen.o"])
            before, old_relocs, _ = elf_object(extract(NEW_ARCHIVE))
            after, new_relocs, symbols = elf_object(extract(archive))
            for name in before:
                if name.startswith(".text"):
                    self.assertEqual(before[name], after[name], name)
            expected = {0xC8: "carbox_screen_tcp_write", 0xDA: "carbox_screen_block_quiet"}
            normal = ".rel.text.AirPlayScreen_SendScreenNormalFrame"
            for name, records in old_relocs.items():
                updated = [(pc, kind, expected.get(pc, target))
                           if name == normal else (pc, kind, target)
                           for pc, kind, target in records]
                self.assertEqual(updated, new_relocs[name], name)
            self.assertNotIn("carbox_screen_tx_poll_wait", symbols)
            # Native polling does not feed the existing frame observer.  Reject this profile rather than report false zeros.
            rejected = subprocess.run(command + ["1"], capture_output=True, text=True)
            self.assertNotEqual(0, rejected.returncode)
            self.assertIn("frame poll observer", rejected.stderr)

    def test_mono_rejects_changed_poll_code_and_relocation(self):
        import patch_screen_wait_relocation as patch
        vendor = subprocess.check_output([
            str(TOOLCHAIN / "arm-none-eabi-ar"), "p", str(NEW_ARCHIVE), "AirPlayScreen.o"])
        eh = struct.unpack_from("<16sHHIIIIIHHHHHH", vendor)
        sections = [struct.unpack_from("<10I", vendor, eh[6] + 40 * i)
                    for i in range(eh[12])]
        names = sections[eh[13]]
        strings = vendor[names[4]:names[4] + names[5]]
        named = {strings[s[0]:strings.index(0, s[0])].decode(): s for s in sections}
        normal = named[".text.AirPlayScreen_SendScreenNormalFrame"]
        rel = named[".rel.text.AirPlayScreen_SendScreenNormalFrame"]
        entries = {struct.unpack_from("<I", vendor, p)[0]: p
                   for p in range(rel[4], rel[4] + rel[5], 8)}
        with tempfile.TemporaryDirectory() as directory:
            path = pathlib.Path(directory) / "screen.o"
            path.write_bytes(vendor)
            patch.main(path, "native-poll")
            self.assertEqual(vendor, path.read_bytes())
            with mock_patch.dict(os.environ, {"SCREEN_TX_POLL_TIMEOUT_MS": "100"}):
                with self.assertRaisesRegex(SystemExit, "requires.*250"):
                    patch.main(path, "native-poll")
            self.assertEqual(vendor, path.read_bytes())
            bad_code = bytearray(vendor)
            bad_code[normal[4] + 0xFE] ^= 1  # native poll timeout
            bad_reloc = bytearray(vendor)
            bad_reloc[entries[0x104] + 4:entries[0x104] + 8] = \
                vendor[entries[0x11E] + 4:entries[0x11E] + 8]
            for changed in (bad_code, bad_reloc):
                path.write_bytes(changed)
                with self.assertRaises(SystemExit):
                    patch.main(path, "native-poll")
                self.assertEqual(changed, path.read_bytes())

    def test_resource450_and_music_probes_on_both_revisions(self):
        import patch_carplay_resource_450 as resource
        import patch_airplay_music_output as music
        from test_patch_airplay_music_output import elf_object
        for source in (OLD_ARCHIVE, NEW_ARCHIVE):
            if not source.exists():
                continue
            with self.subTest(source=source), tempfile.TemporaryDirectory() as directory:
                output = pathlib.Path(directory) / "resource.a"
                ar = str(TOOLCHAIN / "arm-none-eabi-ar")
                objcopy = str(TOOLCHAIN / "arm-none-eabi-objcopy")
                vendor = subprocess.check_output([ar, "p", str(source), resource.MEMBER])
                resource.main(ar, objcopy, source, output)
                patched = subprocess.check_output([ar, "p", str(output), resource.MEMBER])
                before, old_relocs, _ = elf_object(vendor)
                after, new_relocs, _ = elf_object(patched)
                for name in before:
                    if name.startswith(".text"):
                        self.assertEqual(before[name], after[name], name)
                for name, records in old_relocs.items():
                    expected = [(pc, kind, resource.SHIM)
                                if name == ".rel" + resource.FUNCTION and pc in resource.CALLS
                                else (pc, kind, target) for pc, kind, target in records]
                    self.assertEqual(expected, new_relocs[name], name)
                music_output = pathlib.Path(directory) / "music.a"
                music.main(ar, objcopy, output, music_output)
                audio = subprocess.check_output([ar, "p", str(source), music.MEMBER])
                hooked = subprocess.check_output([ar, "p", str(music_output), music.MEMBER])
                audio_sections, audio_relocs, _ = elf_object(audio)
                hooked_sections, hooked_relocs, _ = elf_object(hooked)
                for name in audio_sections:
                    if name.startswith(".text"):
                        self.assertEqual(audio_sections[name], hooked_sections[name], name)
                replacements = {(".rel.text." + fn, pc): hook
                                for (fn, pc), (_, hook, _) in music.CALLS.items()}
                for name, records in audio_relocs.items():
                    self.assertEqual([(pc, kind, replacements.get((name, pc), target))
                                      for pc, kind, target in records], hooked_relocs[name])
                # An unknown member must fail before replacing an output file.
                bad_member = pathlib.Path(directory) / resource.MEMBER
                bad = bytearray(vendor)
                bad[64] ^= 1
                bad_member.write_bytes(bad)
                bad_archive = pathlib.Path(directory) / "bad.a"
                subprocess.run([ar, "rcs", str(bad_archive), str(bad_member)], check=True)
                output.write_bytes(b"sentinel")
                with self.assertRaises(SystemExit):
                    resource.main(ar, objcopy, bad_archive, output)
                self.assertEqual(b"sentinel", output.read_bytes())

    def test_event_cache_is_retained_old_and_restored_mono(self):
        import patch_airplay_event_response as event_patch
        from test_patch_airplay_music_output import elf_object
        for source in (OLD_ARCHIVE, NEW_ARCHIVE):
            if not source.exists():
                continue
            with self.subTest(source=source), tempfile.TemporaryDirectory() as directory:
                output = pathlib.Path(directory) / "events.a"
                ar = str(TOOLCHAIN / "arm-none-eabi-ar")
                subprocess.run([
                    "sh", str(BUILD / "carplay_app/patch_video_handover_archive.sh"),
                    "accessory-wait", ar, str(TOOLCHAIN / "arm-none-eabi-objcopy"),
                    str(source), str(output), "AirPlayEvent.o", "0", "1", "0", "0",
                ], check=True, capture_output=True, text=True)
                vendor = subprocess.check_output([ar, "p", str(source), "AirPlayEvent.o"])
                patched = subprocess.check_output([ar, "p", str(output), "AirPlayEvent.o"])
                # Both layouts must reject an altered argument instruction,
                # even though the function size and call positions still match.
                member = pathlib.Path(directory) / "event.o"
                eh = struct.unpack_from("<16sHHIIIIIHHHHHH", vendor)
                sections = [struct.unpack_from("<10I", vendor, eh[6] + 40 * i)
                            for i in range(eh[12])]
                shstr = sections[eh[13]]
                names = vendor[shstr[4]:shstr[4] + shstr[5]]
                text = next(s for s in sections if
                            names[s[0]:names.index(0, s[0])] ==
                            b".text.AirPlayEvent_DealWithSendHIDReport")
                changed = bytearray(vendor)
                changed[text[4] + 4] ^= 1
                member.write_bytes(changed)
                with self.assertRaisesRegex(SystemExit, "code hash"):
                    event_patch.main(member, inspect_only=True)
                self.assertEqual(changed, member.read_bytes())
                if source == OLD_ARCHIVE:
                    self.assertEqual(vendor, patched)
                else:
                    before, old_relocs, _ = elf_object(vendor)
                    after, new_relocs, _ = elf_object(patched)
                    for name in before:
                        if name.startswith(".text"):
                            self.assertEqual(before[name], after[name], name)
                    for name, records in old_relocs.items():
                        expected = [(pc, kind, "carbox_airplay_event_send_fast_response")
                                    if name == ".rel.text.AirPlayEvent_DealWithSendHIDReport" and pc == 0xA
                                    else (pc, kind, target) for pc, kind, target in records]
                        self.assertEqual(expected, new_relocs[name], name)


if __name__ == "__main__":
    unittest.main()
