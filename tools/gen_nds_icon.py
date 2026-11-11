#!/usr/bin/env python3
"""Writes the cartridge banner icon of a .nds as a Windows .ico and its .rc.

usage: gen_nds_icon.py <rom.nds> <out.ico> <out.rc>

Same decoding as runner/src/rom_icon.cpp. The .ico holds PNG entries at
32 (native) and 64/128/256 (nearest-neighbour), which Explorer, the taskbar
and Alt-Tab pick from. Needs no third-party packages.
"""
import struct
import sys
import zlib


def banner_icon(rom):
    banner = struct.unpack_from("<I", rom, 0x68)[0]
    if banner < 0x200 or banner + 0x240 > len(rom):
        raise SystemExit("ROM has no banner")
    bitmap = rom[banner + 0x20:banner + 0x220]
    pal = struct.unpack_from("<16H", rom, banner + 0x220)
    colours = []
    for i, c in enumerate(pal):
        r, g, b = c & 31, (c >> 5) & 31, (c >> 10) & 31
        colours.append((r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2,
                        0 if i == 0 else 255))
    px = [[None] * 32 for _ in range(32)]
    for y in range(32):
        for x in range(32):
            byte = bitmap[((y // 8) * 4 + x // 8) * 32 + (y % 8) * 4 + (x % 8) // 2]
            px[y][x] = colours[byte >> 4 if x & 1 else byte & 15]
    return px


def png(px, scale):
    side = 32 * scale
    raw = bytearray()
    for y in range(side):
        raw.append(0)
        for x in range(side):
            raw += bytes(px[y // scale][x // scale])

    def chunk(tag, data):
        body = tag + data
        return (struct.pack(">I", len(data)) + body +
                struct.pack(">I", zlib.crc32(body)))

    return (b"\x89PNG\r\n\x1a\n" +
            chunk(b"IHDR", struct.pack(">IIBBBBB", side, side, 8, 6, 0, 0, 0)) +
            chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def main():
    rom_path, ico_path, rc_path = sys.argv[1:4]
    with open(rom_path, "rb") as f:
        rom = f.read(0x100000)  # the banner sits near the start of the ROM
        if struct.unpack_from("<I", rom, 0x68)[0] + 0x240 > len(rom):
            f.seek(0)
            rom = f.read()
    px = banner_icon(rom)
    images = [(32 * s, png(px, s)) for s in (1, 2, 4, 8)]
    out = bytearray(struct.pack("<HHH", 0, 1, len(images)))
    offset = 6 + 16 * len(images)
    for size, data in images:
        out += struct.pack("<BBBBHHII", size & 255, size & 255, 0, 0, 1, 32,
                           len(data), offset)
        offset += len(data)
    for _, data in images:
        out += data
    with open(ico_path, "wb") as f:
        f.write(out)
    with open(rc_path, "w") as f:
        f.write('101 ICON "%s"\n' % ico_path.replace("\\", "/"))


main()
