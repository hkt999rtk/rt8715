#!/usr/bin/env python3

import struct
import hashlib
import hmac
import unittest

import ota_rollback_tool as tool


HASH_KEY = bytes(range(32))


def make_firmware(serial: int) -> bytes:
    offsets = [0xE0, 0x200, 0x340, 0x480, 0x5C0, 0x700]
    types = [11, 8, 4, 2, 7, 7]
    serials = [serial, serial, 0, serial, 1, 2]
    body = bytearray(0x820)
    for index, (offset, image_type, image_serial) in enumerate(
        zip(offsets, types, serials)
    ):
        image_length = 0x80
        next_offset = (
            tool.FIRMWARE_CHAIN_END
            if index == len(offsets) - 1
            else offsets[index + 1] - offset
        )
        struct.pack_into("<II", body, offset, image_length, next_offset)
        body[offset + 8] = image_type
        struct.pack_into("<I", body, offset + tool.FIRMWARE_SERIAL_OFFSET, image_serial)
    body[:tool.FIRMWARE_OTA_SIGNATURE_SIZE] = hmac.new(
        HASH_KEY,
        body[tool.FIRMWARE_HEADER_START : tool.FIRMWARE_HEADER_START + tool.FIRMWARE_HEADER_SIZE],
        hashlib.sha256,
    ).digest()
    return bytes(body) + struct.pack("<I", tool.additive_checksum(body))


def make_smart(firmware: bytes, fatfs: bytes = b"") -> bytes:
    payloads = [(tool.SMART_APP_IMAGE_ID, firmware)]
    if fatfs:
        payloads.append((2, fatfs))
    header_size = tool.SMART_FILE_HEADER_SIZE + len(payloads) * tool.SMART_IMAGE_HEADER_SIZE
    result = bytearray(struct.pack("<II", 1234, len(payloads)))
    offset = header_size
    for image_id, payload in payloads:
        result.extend(
            struct.pack(
                "<4sIIIII",
                tool.SMART_SIGNATURE,
                tool.SMART_IMAGE_HEADER_SIZE,
                tool.additive_checksum(payload),
                len(payload),
                offset,
                image_id,
            )
        )
        offset += len(payload)
    for _, payload in payloads:
        result.extend(payload)
    return bytes(result)


class RollbackToolTest(unittest.TestCase):
    def test_raw_firmware_repack(self):
        parsed = tool.parse_input(make_firmware(100))
        output = tool.repack(parsed, 200, None, HASH_KEY)
        reparsed = tool.parse_input(output)
        tool.verify_firmware_signature(reparsed.firmware, HASH_KEY)
        self.assertEqual(reparsed.kind, "firmware_is")
        self.assertEqual(reparsed.firmware.boot_serial, 200)
        self.assertEqual(reparsed.firmware.headers[1].serial, 100)

    def test_ota_all_repack_preserves_fatfs(self):
        original = make_smart(make_firmware(100), b"fatfs payload")
        parsed = tool.parse_input(original)
        old_fatfs = parsed.smart.images[1]
        output = tool.repack(parsed, 200, 5678, HASH_KEY)
        reparsed = tool.parse_input(output)
        new_fatfs = reparsed.smart.images[1]
        self.assertEqual(reparsed.kind, "ota_all")
        self.assertEqual(reparsed.smart.fw_version, 5678)
        self.assertEqual(reparsed.firmware.boot_serial, 200)
        self.assertEqual(
            original[old_fatfs.offset : old_fatfs.offset + old_fatfs.length],
            output[new_fatfs.offset : new_fatfs.offset + new_fatfs.length],
        )

    def test_bad_firmware_checksum_is_rejected(self):
        damaged = bytearray(make_firmware(100))
        damaged[0x300] ^= 1
        with self.assertRaisesRegex(tool.FormatError, "firmware checksum"):
            tool.parse_input(bytes(damaged))

    def test_wrong_hash_key_is_rejected(self):
        parsed = tool.parse_input(make_firmware(100))
        with self.assertRaisesRegex(tool.FormatError, "does not authenticate"):
            tool.repack(parsed, 200, None, bytes(reversed(HASH_KEY)))

    def test_nonincrementing_serial_is_rejected(self):
        parsed = tool.parse_input(make_firmware(100))
        with self.assertRaisesRegex(tool.FormatError, "must be greater"):
            tool.repack(parsed, 100, None, HASH_KEY)


if __name__ == "__main__":
    unittest.main()
