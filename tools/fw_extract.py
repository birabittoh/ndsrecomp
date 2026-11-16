#!/usr/bin/env python3
"""Decode a DS firmware dump's boot code offline, for recompilation.

The ARM7 BIOS decodes the firmware's boot parts into RAM at power-on, which is
why this code otherwise exists only at run time. The decode needs nothing but
the dump and the ARM7 BIOS, so a bank can be built from the exact bytes the
guest will install, with no capture run and no code path left unobserved.

Flash layout (GBATEK; field order verified against the dumps here):

    0x0C part 1 ROM address   ARM9 boot    (<< 2 + shift1)
    0x0E part 1 RAM address   (0x02800000 - (value << 2 + shift2))
    0x10 part 2 ROM address   ARM7 boot    (<< 2 + shift3)
    0x12 part 2 RAM address   (0x03810000 - (value << 2 + shift4))
    0x14 shift1..4, three bits each

A part is an LZSS stream behind a Blowfish layer keyed with the ARM7 BIOS's
KEY1 table (level 2, modulo 3, seeded by the firmware identifier).

FlashMe keeps that format for a small stub and moves the real boot code aside:
flash[0x17C] != 0xFF marks it, a copy of the original header sits at 0x3F680
(0x3FC80 for the first version), and the parts it names are plain LZSS. The
stub is what the BIOS loads; it installs those parts itself and releases the
ARM9, which spins on a jump target at the end of its 12 byte part.

    fw_extract.py firmware.bin biosnds7.rom --out <dir>

writes <dir>/<part>.bin and <dir>/parts.json, with one entry per part:
{name, cpu, load, size, sha1, file, copies}.

`copies` lists code the part relocates at run time, as {runtime, source, size}.
Such code has no address of its own in the image, so a bank needs it declared.
Two forms exist in the firmware. An autoload table of {ram, size, bss} triples
sits right after its sources, as in NitroSDK. And a one-off copy keeps
{source, destination, source end} together in a literal pool. Both are found by
shape, so a false match is possible, but only costs dead rows: the runtime
validates live bytes before any row can run.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
import sys
from pathlib import Path

M32 = 0xFFFFFFFF
KEY1_OFFSET = 0x30
KEY1_WORDS = 0x412
MAX_PART_SIZE = 0x400000


class Key1:
    """KEY1 Blowfish; the same schedule as runner/src/io.cpp."""

    def __init__(self, bios7: bytes, seed: int, level: int, modulo: int):
        self.p = list(struct.unpack_from(f"<{KEY1_WORDS}I", bios7, KEY1_OFFSET))
        keycode = [seed, seed >> 1, (seed << 1) & M32]
        if level >= 1:
            self._apply(keycode, modulo)
        if level >= 2:
            self._apply(keycode, modulo)

    def _f(self, value: int) -> int:
        p = self.p
        out = p[0x012 + (value >> 24)]
        out = (out + p[0x112 + ((value >> 16) & 0xFF)]) & M32
        out ^= p[0x212 + ((value >> 8) & 0xFF)]
        return (out + p[0x312 + (value & 0xFF)]) & M32

    def encrypt(self, left: int, right: int) -> tuple[int, int]:
        for i in range(16):
            mixed = right ^ self.p[i]
            right = self._f(mixed) ^ left
            left = mixed
        return right ^ self.p[16], left ^ self.p[17]

    def decrypt(self, left: int, right: int) -> tuple[int, int]:
        for i in range(17, 1, -1):
            mixed = right ^ self.p[i]
            right = self._f(mixed) ^ left
            left = mixed
        return right ^ self.p[1], left ^ self.p[0]

    def _apply(self, keycode: list[int], modulo: int) -> None:
        keycode[1], keycode[2] = self.encrypt(keycode[1], keycode[2])
        keycode[0], keycode[1] = self.encrypt(keycode[0], keycode[1])
        for i in range(18):
            swapped = struct.unpack(">I", struct.pack("<I", keycode[i % modulo]))[0]
            self.p[i] ^= swapped
        left = right = 0
        for i in range(0, KEY1_WORDS, 2):
            left, right = self.encrypt(left, right)
            self.p[i], self.p[i + 1] = right, left


def plain_bytes(flash: bytes, offset: int):
    yield from flash[offset:]


def key1_bytes(flash: bytes, offset: int, key: Key1):
    """The part's byte stream: 8 byte blocks through Blowfish."""
    while offset + 8 <= len(flash):
        left, right = struct.unpack_from("<II", flash, offset)
        yield from struct.pack("<II", *key.decrypt(left, right))
        offset += 8


def lzss(stream) -> bytes:
    """Header word (size << 8 | 0x10), then flag bytes, MSB first."""
    header = bytes(next(stream) for _ in range(4))
    value = struct.unpack("<I", header)[0]
    size = value >> 8
    if value & 0xFF != 0x10 or size == 0 or size > MAX_PART_SIZE:
        raise ValueError(f"not an LZSS part (header {value:08X})")
    out = bytearray()
    while len(out) < size:
        flags = next(stream)
        for bit in range(8):
            if len(out) >= size:
                break
            if flags & (0x80 >> bit):
                pair = (next(stream) << 8) | next(stream)
                distance = (pair & 0xFFF) + 1
                if distance > len(out):
                    raise ValueError("LZSS back reference before the start")
                for _ in range((pair >> 12) + 3):
                    out.append(out[-distance])
            else:
                out.append(next(stream))
    return bytes(out[:size])


def header_parts(header: bytes) -> dict:
    u16 = lambda at: struct.unpack_from("<H", header, at)[0]
    shift = u16(0x14)
    s1, s2, s3, s4 = (shift >> n & 7 for n in (0, 3, 6, 9))
    return {
        "arm9_rom": u16(0x0C) << (2 + s1),
        "arm9_ram": (0x02800000 - (u16(0x0E) << (2 + s2))) & M32,
        "arm7_rom": u16(0x10) << (2 + s3),
        "arm7_ram": (0x03810000 - (u16(0x12) << (2 + s4))) & M32,
        "boot_crc": u16(0x06),
    }


def crc16(data: bytes, value: int = 0xFFFF) -> int:
    for byte in data:
        value ^= byte
        for _ in range(8):
            value = (value >> 1) ^ (0xA001 if value & 1 else 0)
    return value


def relocatable(addr: int, load: int, size: int) -> bool:
    """An address code can be copied to: ITCM, main RAM or the WRAM areas."""
    if load <= addr < load + size:
        return False
    return (0x01FF8000 <= addr < 0x02000000 or
            0x02000000 <= addr < 0x02400000 or
            0x03000000 <= addr < 0x03810000)


def find_autoload(image: bytes, load: int) -> list[dict]:
    words = struct.unpack(f"<{len(image) // 4}I", image[:len(image) // 4 * 4])

    def triple(i):
        ram, size, bss = words[i:i + 3]
        return (relocatable(ram, load, len(image)) and ram % 4 == 0 and
                0 < size <= 0x8000 and size % 4 == 0 and
                bss <= 0x8000 and bss % 4 == 0)

    best = []
    i = 0
    while i + 3 <= len(words):
        n = 0
        while i + 3 * (n + 1) <= len(words) and triple(i + 3 * n):
            n += 1
        rows = [words[i + 3 * k:i + 3 * k + 3] for k in range(n)]
        total = sum(row[1] for row in rows)
        ascending = all(a[0] + a[1] <= b[0] for a, b in zip(rows, rows[1:]))
        # A real table's sources end exactly where it starts, so they are
        # code and data, never the zero fill that surrounds lookalikes.
        if n and ascending and total <= i * 4 and any(
                words[i - total // 4:i]) and n > len(best):
            source = i * 4 - total
            best = []
            for ram, size, _ in rows:
                best.append({"runtime": ram, "source": load + source, "size": size})
                source += size
        i += 3 * n if n else 1
    return best


def find_literal_copies(image: bytes, load: int) -> list[dict]:
    copies = []
    end = load + len(image)
    for at in range(0, len(image) - 11, 4):
        source, runtime, stop = struct.unpack_from("<III", image, at)
        if (load <= source < stop <= end and source % 4 == 0 and
                stop - source <= 0x1000 and
                relocatable(runtime, load, len(image)) and runtime % 4 == 0):
            copies.append({"runtime": runtime, "source": source,
                           "size": stop - source})
    return copies


def find_copies(image: bytes, load: int) -> list[dict]:
    seen = {}
    for copy in find_autoload(image, load) + find_literal_copies(image, load):
        seen.setdefault(copy["runtime"], copy)
    return [seen[key] for key in sorted(seen)]


def extract(flash: bytes, bios7: bytes) -> list[dict]:
    if len(bios7) < KEY1_OFFSET + KEY1_WORDS * 4:
        raise ValueError("ARM7 BIOS is too small to hold the KEY1 table")
    live = header_parts(flash)
    key = Key1(bios7, struct.unpack_from("<I", flash, 8)[0], 2, 3)
    arm9 = lzss(key1_bytes(flash, live["arm9_rom"], key))
    arm7 = lzss(key1_bytes(flash, live["arm7_rom"], key))
    # The header CRC covers exactly these two parts, so it proves the key,
    # the part addresses and the decompressor all at once.
    got = crc16(arm9 + arm7)
    if got != live["boot_crc"]:
        raise ValueError(f"boot parts fail their CRC ({got:04X} != "
                         f"{live['boot_crc']:04X}); wrong BIOS for this dump?")

    flashme = flash[0x17C] != 0xFF
    parts = []
    prefix = "stub" if flashme else "boot"
    parts.append((f"{prefix}9", "arm9", live["arm9_ram"], arm9))
    parts.append((f"{prefix}7", "arm7", live["arm7_ram"], arm7))
    if flashme:
        saved = 0x3F680 if flash[0x17C] > 1 else 0x3FC80
        orig = header_parts(flash[saved:saved + 0x40])
        parts.append(("boot9", "arm9", orig["arm9_ram"],
                      lzss(plain_bytes(flash, orig["arm9_rom"]))))
        parts.append(("boot7", "arm7", orig["arm7_ram"],
                      lzss(plain_bytes(flash, orig["arm7_rom"]))))
    return [{"name": name, "cpu": cpu, "load": load, "size": len(data),
             "sha1": hashlib.sha1(data).hexdigest(),
             "copies": find_copies(data, load), "data": data}
            for name, cpu, load, data in parts]


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("firmware", type=Path)
    ap.add_argument("bios7", type=Path)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    try:
        parts = extract(args.firmware.read_bytes(), args.bios7.read_bytes())
    except ValueError as exc:
        print(f"fw_extract: {exc}", file=sys.stderr)
        return 1
    args.out.mkdir(parents=True, exist_ok=True)
    listing = []
    for part in parts:
        data = part.pop("data")
        part["file"] = f"{part['name']}.bin"
        (args.out / part["file"]).write_bytes(data)
        listing.append(part)
        print(f"  {part['name']:6s} {part['cpu']} 0x{part['load']:08X} "
              f"{part['size']:7d} bytes")
        for copy in part["copies"]:
            print(f"         copy 0x{copy['source']:08X} -> "
                  f"0x{copy['runtime']:08X} +0x{copy['size']:X}")
    (args.out / "parts.json").write_text(json.dumps(listing, indent=1) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
