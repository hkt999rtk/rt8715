#!/usr/bin/env python3
"""Check that the derived customer archive waits on the screen TCP socket."""

import pathlib
import re
import subprocess
import tempfile
import unittest


BUILD = pathlib.Path(__file__).resolve().parents[3] / "GCC-RELEASE"
TOOLCHAIN = pathlib.Path(
    "/home/kevin/work/toolchains/arm-none-eabi-gcc-6.4.1-realtek/"
    "asdk/linux/newlib/bin"
)


class ScreenTxPollPatchTest(unittest.TestCase):
    def test_only_normal_frame_delay_becomes_poll_wait(self):
        with tempfile.TemporaryDirectory() as directory:
            archive = pathlib.Path(directory) / "Accessory2_poll.a"
            source = BUILD / "carplay_app/lib_Accessory2.a"
            subprocess.run(
                [
                    "sh", str(BUILD / "carplay_app/patch_video_handover_archive.sh"),
                    "accessory-wait", str(TOOLCHAIN / "arm-none-eabi-ar"),
                    str(TOOLCHAIN / "arm-none-eabi-objcopy"), str(source),
                    str(archive), "AirPlayScreen.o", "0", "0", "0", "0", "0", "1",
                ],
                check=True, capture_output=True, text=True,
            )
            image = subprocess.run(
                [str(TOOLCHAIN / "arm-none-eabi-ar"), "p", str(archive),
                 "AirPlayScreen.o"],
                check=True, capture_output=True,
            ).stdout
            obj = pathlib.Path(directory) / "AirPlayScreen.o"
            obj.write_bytes(image)
            relocations = subprocess.run(
                [str(TOOLCHAIN / "arm-none-eabi-readelf"), "-W", "-r", str(obj)],
                check=True, capture_output=True, text=True,
            ).stdout
            normal = relocations.split(
                "Relocation section '.rel.text.AirPlayScreen_SendScreenNormalFrame'", 1
            )[1].split("Relocation section ", 1)[0]
            self.assertRegex(normal, r"000000e2\s+.*carbox_screen_tx_poll_wait")
            self.assertNotRegex(normal, r"000000e2\s+.*vTaskDelay")
            self.assertRegex(normal, r"000000dc\s+.*carbox_screen_block_quiet")
            self.assertRegex(normal, r"00000012\s+.*puts")
            self.assertRegex(relocations, r"R_ARM_THM_CALL\s+00000000\s+vTaskDelay")
            disassembly = subprocess.run(
                [str(TOOLCHAIN / "arm-none-eabi-objdump"), "-d", "-j",
                 ".text.AirPlayScreen_SendScreenNormalFrame", str(obj)],
                check=True, capture_output=True, text=True,
            ).stdout
            self.assertRegex(disassembly, r"\be0:\s+6828\s+ldr\s+r0, \[r5, #0\]")

    def test_frame_id_hooks_only_normal_frame_and_eagain_print(self):
        with tempfile.TemporaryDirectory() as directory:
            archive = pathlib.Path(directory) / "Accessory2_frame_id.a"
            source = BUILD / "carplay_app/lib_Accessory2.a"
            subprocess.run(
                [
                    "sh", str(BUILD / "carplay_app/patch_video_handover_archive.sh"),
                    "accessory-wait", str(TOOLCHAIN / "arm-none-eabi-ar"),
                    str(TOOLCHAIN / "arm-none-eabi-objcopy"), str(source),
                    str(archive), "AirPlayScreen.o", "0", "0", "0", "0", "0", "1", "1",
                ],
                check=True, capture_output=True, text=True,
            )
            obj = pathlib.Path(directory) / "AirPlayScreen.o"
            obj.write_bytes(subprocess.run(
                [str(TOOLCHAIN / "arm-none-eabi-ar"), "p", str(archive),
                 "AirPlayScreen.o"],
                check=True, capture_output=True,
            ).stdout)
            relocations = subprocess.run(
                [str(TOOLCHAIN / "arm-none-eabi-readelf"), "-W", "-r", str(obj)],
                check=True, capture_output=True, text=True,
            ).stdout
            dispatch = relocations.split(
                "Relocation section '.rel.text.AirPlayScreen_SendScreenFrame'", 1
            )[1].split("Relocation section ", 1)[0]
            normal = relocations.split(
                "Relocation section '.rel.text.AirPlayScreen_SendScreenNormalFrame'", 1
            )[1].split("Relocation section ", 1)[0]
            self.assertRegex(dispatch, r"00000018\s+.*carbox_screen_normal_frame_probe")
            self.assertRegex(dispatch, r"00000014\s+.*AirPlayScreen_SendScreenConfigFrame")
            self.assertRegex(normal, r"000000dc\s+.*carbox_screen_block_note")
            self.assertRegex(normal, r"00000012\s+.*puts")
            self.assertRegex(normal, r"000000e2\s+.*carbox_screen_tx_poll_wait")


if __name__ == "__main__":
    unittest.main()
