#!/usr/bin/env python3
"""Check fw_extract on synthetic images; no BIOS or firmware dump is involved.

The KEY1 table is arbitrary here, which is enough: the cipher only has to
round-trip, and a real table is what the part CRC checks at run time.
"""
import struct
import unittest

import fw_extract

LOAD = 0x02300000


def fake_bios() -> bytes:
    table = bytes((i * 37 + 11) & 0xFF
                  for i in range(fw_extract.KEY1_WORDS * 4))
    return bytes(fw_extract.KEY1_OFFSET) + table


def lz_literals(data: bytes) -> bytes:
    """The simplest valid stream: every flag bit clear."""
    out = bytearray(struct.pack("<I", (len(data) << 8) | 0x10))
    for at in range(0, len(data), 8):
        out.append(0)
        out += data[at:at + 8]
    return bytes(out)


def encrypt_stream(key: fw_extract.Key1, stream: bytes) -> bytes:
    stream += bytes(-len(stream) % 8)
    out = bytearray()
    for at in range(0, len(stream), 8):
        left, right = struct.unpack_from("<II", stream, at)
        out += struct.pack("<II", *key.encrypt(left, right))
    return bytes(out)


def header(arm9_rom, arm9_ram, arm7_rom, arm7_ram, crc=0) -> bytearray:
    """Shift amounts 1,6,1,6, as dumps use: ROM << 3, RAM << 8."""
    head = bytearray(0x40)
    struct.pack_into("<H", head, 0x06, crc)
    head[8:12] = b"MACP"
    struct.pack_into("<H", head, 0x0C, arm9_rom >> 3)
    struct.pack_into("<H", head, 0x0E, (0x02800000 - arm9_ram) >> 8)
    struct.pack_into("<H", head, 0x10, arm7_rom >> 3)
    struct.pack_into("<H", head, 0x12, (0x03810000 - arm7_ram) >> 8)
    struct.pack_into("<H", head, 0x14, 0x0C71)
    return head


def flash(arm9: bytes, arm7: bytes, key: fw_extract.Key1) -> bytearray:
    image = bytearray(b"\xFF" * 0x40000)
    a9 = encrypt_stream(key, lz_literals(arm9))
    a7 = encrypt_stream(key, lz_literals(arm7))
    image[0x180:0x180 + len(a9)] = a9
    image[0x400:0x400 + len(a7)] = a7
    crc = fw_extract.crc16(arm9 + arm7)
    image[:0x40] = header(0x180, 0x021F0000, 0x400, 0x0380F800, crc)
    return image


class Lzss(unittest.TestCase):
    def test_back_reference(self):
        # "abc" then a run from distance 3, flagged by the fourth bit.
        stream = struct.pack("<I", (9 << 8) | 0x10)
        stream += bytes([0x10]) + b"abc" + bytes([0x30, 0x02])
        self.assertEqual(fw_extract.lzss(iter(stream)), b"abcabcabc")

    def test_rejects_other_headers(self):
        with self.assertRaises(ValueError):
            fw_extract.lzss(iter(struct.pack("<I", (4 << 8) | 0x11)))

    def test_rejects_reference_before_start(self):
        stream = struct.pack("<I", (4 << 8) | 0x10) + bytes([0x80, 0x10, 0x05])
        with self.assertRaises(ValueError):
            fw_extract.lzss(iter(stream))


class Key1(unittest.TestCase):
    def test_round_trip(self):
        key = fw_extract.Key1(fake_bios(), 0x5043414D, 2, 3)
        self.assertEqual(key.decrypt(*key.encrypt(0x12345678, 0x9ABCDEF0)),
                         (0x12345678, 0x9ABCDEF0))


class Extract(unittest.TestCase):
    def setUp(self):
        self.bios = fake_bios()
        self.key = fw_extract.Key1(self.bios, 0x5043414D, 2, 3)
        self.arm9 = bytes(range(16))
        self.arm7 = bytes(range(100, 132))

    def test_retail_layout(self):
        parts = fw_extract.extract(
            bytes(flash(self.arm9, self.arm7, self.key)), self.bios)
        self.assertEqual([p["name"] for p in parts], ["boot9", "boot7"])
        self.assertEqual(parts[0]["load"], 0x021F0000)
        self.assertEqual(parts[1]["load"], 0x0380F800)
        self.assertEqual(parts[1]["data"], self.arm7)

    def test_flashme_adds_the_saved_parts(self):
        image = flash(self.arm9, self.arm7, self.key)
        image[0x17C] = 2
        real9, real7 = b"real arm9 code..", b"real arm7 code.."
        image[0x20000:0x20000 + len(lz_literals(real9))] = lz_literals(real9)
        image[0x21000:0x21000 + len(lz_literals(real7))] = lz_literals(real7)
        image[0x3F680:0x3F6C0] = header(0x20000, 0x0231FE00, 0x21000, 0x037FA600)
        parts = {p["name"]: p for p in fw_extract.extract(bytes(image), self.bios)}
        self.assertEqual(sorted(parts), ["boot7", "boot9", "stub7", "stub9"])
        self.assertEqual(parts["stub9"]["data"], self.arm9)
        self.assertEqual(parts["boot9"]["data"], real9)
        self.assertEqual(parts["boot7"]["load"], 0x037FA600)

    def test_wrong_bios_fails_the_crc(self):
        image = flash(self.arm9, self.arm7, self.key)
        struct.pack_into("<H", image, 6, 0x1234)
        with self.assertRaises(ValueError):
            fw_extract.extract(bytes(image), self.bios)


class Copies(unittest.TestCase):
    def test_autoload_sources_end_at_the_table(self):
        code = b"\x11" * 0x40 + b"\x22" * 0x20
        table = struct.pack("<6I", 0x01FF8000, 0x40, 0, 0x03000000, 0x20, 0)
        image = bytes(0x100) + code + table + bytes(0x40)
        self.assertEqual(fw_extract.find_autoload(image, LOAD), [
            {"runtime": 0x01FF8000, "source": LOAD + 0x100, "size": 0x40},
            {"runtime": 0x03000000, "source": LOAD + 0x140, "size": 0x20}])

    def test_autoload_ignores_a_table_over_zero_fill(self):
        table = struct.pack("<3I", 0x01FF8000, 0x40, 0)
        self.assertEqual(
            fw_extract.find_autoload(bytes(0x100) + table, LOAD), [])

    def test_literal_copy(self):
        source, end = LOAD + 0x40, LOAD + 0xA8
        image = bytes(0x20) + struct.pack("<3I", source, 0x023FEE00, end)
        image += bytes(0x100)
        self.assertEqual(fw_extract.find_literal_copies(image, LOAD), [
            {"runtime": 0x023FEE00, "source": source, "size": 0x68}])

    def test_address_inside_the_image_is_not_a_destination(self):
        self.assertFalse(fw_extract.relocatable(LOAD + 4, LOAD, 0x100))
        self.assertTrue(fw_extract.relocatable(LOAD + 0x100, LOAD, 0x100))


if __name__ == "__main__":
    unittest.main()
