#!/usr/bin/env python3
"""Unpacked Nintendo DS ROMs: a .nds as a folder of files, and back.

The layout follows ndstool's, with a manifest that keeps every offset, so an
unchanged folder packs back into the exact image it came from:

    rom.toml       the manifest (below)
    header.bin     the header area, up to the header size (0x4000)
    arm9.bin  arm7.bin  y9.bin  y7.bin  fnt.bin  banner.bin
    overlay/overlay9_NNNN.bin  overlay/overlay7_NNNN.bin
    data/...       the file system, by its names
    unnamed/NNNN.bin   FAT entries with neither a name nor an overlay
    extra/OOOOOOOO.bin bytes outside all of the above that are not padding

rom.toml:

    [rom]
    game_code = "YDQP"   revision = 0
    sha1 = "..."         size = 0x10000000   fill = 0xFF
    identity = "..."     # optional: the SHA-1 of the image whose code the
                         # folder carries, if not its own (a converted
                         # release); the runner runs it as that image
    [regions]            # fixed parts, path = offset
    "arm9.bin" = 0x4000
    [extra]              # same, for the extra/ files
    files = [            # by FAT id: [path, offset, size]
      ["overlay/overlay9_0000.bin", 0x..., 0x...],
    ]

Packing puts each file back at its offset. A file that shrank stays there;
one that grew goes after the last used byte, the image grows to the next
power of two if it has to, and the header is updated (used size, chip size,
CRC). The fixed parts must keep their sizes.

The runner reads the same format (ndsrecomp/runner/src/rom_folder.cpp).

usage:
    nds_folder.py unpack game.nds DIR
    nds_folder.py pack DIR game.nds
"""
from __future__ import annotations

import hashlib
import os
import shutil
import struct
import sys
from pathlib import Path

MANIFEST = "rom.toml"
FORMAT = 1

try:
    import tomllib
except ModuleNotFoundError:  # Python < 3.11
    tomllib = None


def u16(data, offset):
    return struct.unpack_from("<H", data, offset)[0]


def u32(data, offset):
    return struct.unpack_from("<I", data, offset)[0]


def banner_size(rom, offset):
    if offset == 0 or offset + 2 > len(rom):
        return 0
    return {1: 0x840, 2: 0x940, 3: 0xA40, 0x103: 0x23C0}.get(u16(rom, offset), 0x840)


def crc16(data):
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def fnt_names(rom, fnt_off, fnt_size):
    """FAT id -> path below data/, from the file name table."""
    names = {}
    if fnt_size < 8:
        return names
    dir_count = u16(rom, fnt_off + 6)

    def walk(dir_id, prefix, depth):
        if depth > 64 or (dir_id & 0xFFF) >= dir_count:
            return
        entry = fnt_off + 8 * (dir_id & 0xFFF)
        sub_off, file_id = u32(rom, entry), u16(rom, entry + 4)
        p = fnt_off + sub_off
        while p < fnt_off + fnt_size:
            kind = rom[p]
            p += 1
            if kind == 0:
                return
            name = rom[p:p + (kind & 0x7F)].decode("latin-1")
            p += kind & 0x7F
            if kind & 0x80:
                walk(u16(rom, p), prefix + name + "/", depth + 1)
                p += 2
            else:
                names[file_id] = prefix + name
                file_id += 1

    walk(0xF000, "", 0)
    return names


def safe_name(name):
    parts = name.split("/")
    return all(part not in ("", ".", "..") and "\\" not in part and ":" not in part
               for part in parts)


def overlay_ids(table, cpu):
    """FAT id -> overlay file name, from an overlay table (32 bytes each)."""
    out = {}
    for i in range(len(table) // 32):
        overlay_id, file_id = u32(table, 32 * i), u32(table, 32 * i + 24)
        out[file_id] = f"overlay/overlay{cpu}_{overlay_id:04d}.bin"
    return out


def toml_string(text):
    return '"' + text.replace("\\", "\\\\").replace('"', '\\"') + '"'


def unpack(rom: bytes, out_dir: Path):
    """Writes the folder for `rom`. The folder must not exist yet."""
    out_dir = Path(out_dir)
    if len(rom) < 0x200:
        raise ValueError("not a DS ROM: shorter than its header")
    header_size = max(0x200, min(u32(rom, 0x84) or 0x4000, len(rom)))
    fnt_off, fnt_size, fat_off, fat_size = struct.unpack_from("<IIII", rom, 0x40)
    regions = [
        ("header.bin", 0, header_size),
        ("arm9.bin", u32(rom, 0x20), u32(rom, 0x2C)),
        ("arm7.bin", u32(rom, 0x30), u32(rom, 0x3C)),
        ("y9.bin", u32(rom, 0x50), u32(rom, 0x54)),
        ("y7.bin", u32(rom, 0x58), u32(rom, 0x5C)),
        ("fnt.bin", fnt_off, fnt_size),
        ("banner.bin", u32(rom, 0x68), banner_size(rom, u32(rom, 0x68))),
    ]
    regions = [(name, off, size) for name, off, size in regions
               if size and off + size <= len(rom)]
    if fat_off + fat_size > len(rom):
        raise ValueError("FAT outside the image")

    def region(name):
        for n, off, size in regions:
            if n == name:
                return rom[off:off + size]
        return b""

    names = {i: "data/" + n for i, n in fnt_names(rom, fnt_off, fnt_size).items()
             if safe_name(n)}
    names.update(overlay_ids(region("y9.bin"), 9))
    names.update(overlay_ids(region("y7.bin"), 7))

    files = []
    for i in range(fat_size // 8):
        start, end = struct.unpack_from("<II", rom, fat_off + 8 * i)
        if end < start or end > len(rom):
            raise ValueError(f"FAT entry {i} outside the image")
        files.append((names.get(i, f"unnamed/{i:04d}.bin"), start, end - start))

    fill = rom[-1]
    covered = sorted([(off, size) for _, off, size in regions] +
                     [(fat_off, fat_size)] +
                     [(off, size) for _, off, size in files if size])
    extra, pos = [], 0
    for off, size in covered + [(len(rom), 0)]:
        gap = rom[pos:off]
        body = gap.strip(bytes([fill]))
        if body:
            first = pos + len(gap) - len(gap.lstrip(bytes([fill])))
            extra.append((f"extra/{first:08X}.bin", first, len(body)))
        pos = max(pos, off + size)

    tmp = out_dir.with_name(out_dir.name + ".partial")
    if tmp.exists():
        shutil.rmtree(tmp)
    for name, off, size in regions + extra + files:
        path = tmp / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(rom[off:off + size])

    game_code = rom[0x0C:0x10].decode("latin-1")
    lines = [
        "# An unpacked Nintendo DS ROM (ndsrecomp tools/nds_folder.py).",
        "# The files can be edited; offsets are the original image's.",
        "",
        "[rom]",
        f"format = {FORMAT}",
        f"game_code = {toml_string(game_code)}",
        f"revision = {rom[0x1E]}",
        f'sha1 = "{hashlib.sha1(rom).hexdigest()}"',
        f"size = 0x{len(rom):X}",
        f"fill = 0x{fill:02X}",
        f"fat = 0x{fat_off:X}",
        "",
        "[regions]",
        *(f"{toml_string(name)} = 0x{off:X}" for name, off, _ in regions),
        "",
        "[extra]",
        *(f"{toml_string(name)} = 0x{off:X}" for name, off, _ in extra),
        "",
        "[files]",
        "fat = [",
        *(f"  [{toml_string(name)}, 0x{off:X}, 0x{size:X}]," for name, off, size in files),
        "]",
        "",
    ]
    (tmp / MANIFEST).write_text("\n".join(lines), encoding="utf-8", newline="\n")
    tmp.rename(out_dir)


def read_manifest(folder: Path):
    text = (Path(folder) / MANIFEST).read_text(encoding="utf-8")
    if tomllib is None:
        raise RuntimeError("reading an unpacked ROM needs Python 3.11 or newer")
    return tomllib.loads(text)


def pack(folder: Path) -> bytes:
    folder = Path(folder)
    manifest = read_manifest(folder)
    info = manifest["rom"]
    if info.get("format") != FORMAT:
        raise ValueError(f"{folder / MANIFEST}: unknown format {info.get('format')}")
    fill = info["fill"]
    size = info["size"]

    def read(name):
        return (folder / name).read_bytes()

    entries = [(name, off, read(name)) for name, off in
               list(manifest["regions"].items()) + list(manifest["extra"].items())]
    files = [(name, off, slot, read(name)) for name, off, slot in manifest["files"]["fat"]]

    # Fixed parts keep their sizes; the header and the FAT say where they are.
    header = next((data for name, _, data in entries if name == "header.bin"), None)
    if header is None:
        raise ValueError("header.bin missing")
    for name, off, data in entries:
        expect = {"arm9.bin": 0x2C, "arm7.bin": 0x3C, "y9.bin": 0x54,
                  "y7.bin": 0x5C, "fnt.bin": 0x44}.get(name)
        if expect is not None and len(data) != u32(header, expect):
            raise ValueError(f"{name} is {len(data)} bytes, the header says {u32(header, expect)}")
        if name == "header.bin" and len(data) < 0x200:
            raise ValueError("header.bin is shorter than a header")

    used = u32(header, 0x80)
    end = max([used] + [off + len(data) for _, off, data in entries] +
              [off + slot for _, off, slot, _ in files])
    placed = []
    for name, off, slot, data in files:
        if len(data) > slot:
            off = (end + 0x1FF) & ~0x1FF
            end = off + len(data)
        placed.append((off, data))
    moved = any(len(data) > slot for _, _, slot, data in files)

    if end > size:
        while size < end:
            size *= 2
    image = bytearray(bytes([fill]) * size)
    for _, off, data in entries:
        image[off:off + len(data)] = data
    fat_off = info["fat"]
    for i, (off, data) in enumerate(placed):
        image[off:off + len(data)] = data
        struct.pack_into("<II", image, fat_off + 8 * i, off, off + len(data))

    if moved:
        struct.pack_into("<I", image, 0x80, end)
        chip = 0
        while (0x20000 << chip) < size:
            chip += 1
        image[0x14] = chip
        struct.pack_into("<H", image, 0x15E, crc16(image[:0x15E]))
    return bytes(image)


def is_folder(path) -> bool:
    return (Path(path) / MANIFEST).is_file()


def read_rom(path) -> bytes:
    """The image at `path`: a .nds file, or an unpacked folder packed back."""
    return pack(Path(path)) if is_folder(path) else Path(path).read_bytes()


def banner_image(path) -> bytes:
    """A small image holding just the header and the banner, for tools that
    read the banner through header 0x68, from a .nds or a folder."""
    path = Path(path)
    if is_folder(path):
        header = bytearray((path / "header.bin").read_bytes()[:0x200])
        struct.pack_into("<I", header, 0x68, len(header))
        return bytes(header) + (path / "banner.bin").read_bytes()
    with open(path, "rb") as f:
        rom = f.read(0x200)
        offset = u32(rom, 0x68) if len(rom) >= 0x6C else 0
        if offset < 0x200:
            return rom
        f.seek(offset)
        banner = f.read(0x23C0)
    header = bytearray(rom)
    struct.pack_into("<I", header, 0x68, len(header))
    return bytes(header) + banner


def read_file(path, name) -> bytes:
    """One file of the image at `path`, by its folder name ("banner.bin",
    "data/map/x.bin"), without packing a folder."""
    path = Path(path)
    if is_folder(path):
        return (path / name).read_bytes()
    rom = path.read_bytes()
    fnt_off, fnt_size, fat_off, _ = struct.unpack_from("<IIII", rom, 0x40)
    if name == "banner.bin":
        off = u32(rom, 0x68)
        return rom[off:off + banner_size(rom, off)]
    for file_id, n in fnt_names(rom, fnt_off, fnt_size).items():
        if "data/" + n == name:
            start, end = struct.unpack_from("<II", rom, fat_off + 8 * file_id)
            return rom[start:end]
    raise FileNotFoundError(name)


def main(argv):
    if len(argv) != 4 or argv[1] not in ("unpack", "pack"):
        print(__doc__.split("usage:")[1].rstrip(), file=sys.stderr)
        return 2
    if argv[1] == "unpack":
        out = Path(argv[3])
        if out.exists():
            print(f"{out} already exists", file=sys.stderr)
            return 1
        unpack(Path(argv[2]).read_bytes(), out)
    else:
        Path(argv[3]).write_bytes(pack(Path(argv[2])))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
