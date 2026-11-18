#!/usr/bin/env python3
"""Recipes that rebuild one release's code from another release's ROM.

Two releases of a game often share every data file and differ only in code
(arm9, overlays, the overlay table). A build made from release A carries
most of A's code already, in the banks' validation copies (the
NdsStaticValidation rows in the generated *_dispatch.c files). A recipe says
how to rebuild each of A's code files from those copies plus the same file
of release B, so a player who owns B can run the build:

    usage: release_recipe.py make BASE OTHER RECOMP OUT [--report out.json]
           release_recipe.py blank RECIPE OUT
           release_recipe.py fill RECIPE BASE RECOMP OUT

BASE and OTHER are unpacked ROMs (nds_folder.py), RECOMP the generated bank
directory. `make` checks that applying the recipe to OTHER gives back BASE's
files byte for byte before writing it, and prints how many bytes the recipe
has to carry itself (literals): those are the only bytes of BASE in it.

`blank` zeroes the literals, leaving a recipe with no bytes of BASE at
all, which can be kept in version control. `fill` puts them back from BASE
and checks the recipe still works with the banks in RECOMP (every KNOWN
span still covered by validation copies that agree with BASE), so someone
who owns BASE only can build. What a recipe takes from OTHER cannot change:
OTHER is a fixed image, named by its SHA-1.

Code files are rebuilt decompressed. "header.bin" is the header's first
0x20 bytes (title, game code, ...), written over OTHER's. Everything else
(arm7, data, the overlay table but its compressed sizes, the ARM9 footer) is
checked to be the same in both releases and taken from OTHER. A file's ops, in order, cover it once:

    KNOWN n      n bytes from the validation copies
    SHIFT d      later COPY/WORD/BL read OTHER's file at pos + d
    COPY n       n bytes from OTHER's file
    WORD k       a 4-byte word from OTHER's file, plus k (a moved pointer)
    BL k         an ARM BL from OTHER's file, its offset field plus k
    LIT n bytes  n bytes the recipe carries

Format (little-endian): "NRCP", u32 version, the SHA-1 of BASE's image
(what the rebuilt folder runs as, and whose banks give the copies), the
SHA-1 of OTHER's image (what it applies to), u32 file count; per file:
u16 name length, name, u16 bank name length, bank name (empty: no
validation copies), u32 size, the result's SHA-1, u32 segment count, segments (u32 file offset,
u32 address, u32 size: where the bank's addresses lie in the file), u32 op
bytes, ops. An op is one byte (the kind above, 0-5) then its arguments as
LEB128 (n, k: k and d zigzag-encoded), LIT's bytes after n.

The runner applies the same format (runner/src/release_recipe.cpp).
"""
from __future__ import annotations

import argparse
import bisect
import hashlib
import json
import re
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import nds_folder  # noqa: E402

try:
    import ndspy.codeCompression as code_compression
    import ndspy.rom
except ImportError as exc:  # pragma: no cover
    raise SystemExit("ndspy is required (requirements.txt)") from exc

MAGIC = b"NRCP"
VERSION = 1
KNOWN, SHIFT, COPY, WORD, BL, LIT = range(6)

VALIDATION = re.compile(
    r"NdsStaticValidation \w+ = \{0x([0-9A-Fa-f]+)u, (\d+)u, (\w+)")
ARRAY = re.compile(r"static const uint8_t (\w+)\[\d*\] = \{([^}]*)\}")


# ----------------------------------------------------------------------------
# Validation copies


def validation_rows(dispatch: Path):
    """(address, bytes) of every validation row in a dispatch source."""
    text = dispatch.read_text()
    arrays = {name: bytes(int(x.strip().rstrip("uU"), 0)
                          for x in body.split(",") if x.strip())
              for name, body in ARRAY.findall(text)}
    for address, length, name in VALIDATION.findall(text):
        yield int(address, 16), arrays[name][: int(length)]


def known_bytes(rows, segments, size):
    """Bytes of a file the rows give, and a mask of them.

    A byte two rows disagree on is unknown (the runner does the same)."""
    known = bytearray(size)
    have = bytearray(size)
    bad = bytearray(size)
    for address, data in rows:
        for seg_off, seg_addr, seg_size in segments:
            lo = max(address, seg_addr)
            hi = min(address + len(data), seg_addr + seg_size)
            for a in range(lo, hi):
                i = seg_off + a - seg_addr
                b = data[a - address]
                if have[i] and known[i] != b:
                    bad[i] = 1
                known[i] = b
                have[i] = 1
    for i in range(size):
        if bad[i]:
            have[i] = 0
    return known, have


def arm9_segments(image: bytes, ram: int, params_off: int):
    """Where the ARM9 file's bytes run: its load address, then autoloads."""
    if params_off is None or params_off + 0x14 > len(image):
        return [(0, ram, len(image))]
    table_start, table_end, source = struct.unpack_from("<3I", image, params_off)
    segments = [(0, ram, source - ram)]
    offset = source - ram
    for entry in range(table_start - ram, table_end - ram, 12):
        address, size, _bss = struct.unpack_from("<3I", image, entry)
        segments.append((offset, address, size))
        offset += size
    return segments


# ----------------------------------------------------------------------------
# Building a file's ops


def runs(mask):
    i, n = 0, len(mask)
    while i < n:
        j = i
        while j < n and mask[j] == mask[i]:
            j += 1
        yield i, j, bool(mask[i])
        i = j


def anchors(known, have, other, min_len=16, window=0x1000):
    """(offset, other offset) of validated spans found verbatim in OTHER."""
    out, delta = [], 0
    for start, end, covered in runs(have):
        if not covered or end - start < min_len:
            continue
        span = bytes(known[start:end])
        if other[start + delta:end + delta] == span:
            out.append((start, start + delta))
            continue
        lo = max(0, start + delta - window)
        hi = min(len(other), end + delta + window)
        hits = []
        at = other.find(span, lo, hi)
        while at != -1 and len(hits) < 2:
            hits.append(at)
            at = other.find(span, at + 1, hi)
        if len(hits) == 1:
            delta = hits[0] - start
            out.append((start, hits[0]))
    return out


class AddressMap:
    """OTHER address -> BASE address, piecewise, for one file's range."""

    def __init__(self, base_address, pairs, size):
        self.lo, self.hi = base_address, base_address + size
        points = sorted({(o, b - o) for b, o in pairs})
        self.at = [base_address + o for o, _ in points]
        self.delta = [d for _, d in points]

    def __call__(self, address):
        k = bisect.bisect_right(self.at, address) - 1
        return address + (self.delta[k] if k >= 0 else 0)


def choose_shifts(target, other, have, anc, chunk=64):
    """The shift to read OTHER at for each byte of a gap."""
    starts = [a for a, _ in anc]
    shift = [0] * len(target)
    for start, end, covered in runs(have):
        k = bisect.bisect_right(starts, start) - 1
        before = anc[k][1] - anc[k][0] if k >= 0 else 0
        if covered:
            for i in range(start, end):
                shift[i] = before
            continue
        near = {anc[j][1] - anc[j][0]
                for j in range(max(0, k - 3), min(len(anc), k + 5))} or {0}
        near.add(before)
        for c in range(start, end, chunk):
            ce = min(end, c + chunk)

            def wrong(d):
                return sum(1 for i in range(c, ce)
                           if not 0 <= i + d < len(other) or other[i + d] != target[i])
            best = min(sorted(near, key=lambda d: d != before), key=wrong)
            for i in range(c, ce):
                shift[i] = best
    return shift


def word(data, i):
    return int.from_bytes(data[i:i + 4], "little") if 0 <= i <= len(data) - 4 else None


def bl_target(insn, address):
    if insn is None or (insn & 0x0F000000) != 0x0B000000:
        return None
    offset = insn & 0xFFFFFF
    if offset & 0x800000:
        offset -= 0x1000000
    return address + 8 + 4 * offset


class Ops:
    def __init__(self):
        self.out = bytearray()
        self.pending = None  # (kind, n) to merge runs
        self.lit = bytearray()
        self.counts = {"known": 0, "copy": 0, "word": 0, "bl": 0, "literal": 0}

    @staticmethod
    def leb(value):
        out = bytearray()
        while True:
            byte = value & 0x7F
            value >>= 7
            if value:
                out.append(byte | 0x80)
            else:
                out.append(byte)
                return bytes(out)

    @staticmethod
    def zigzag(value):
        return (value << 1) ^ (value >> 63) if value < 0 else value << 1

    def flush(self):
        if self.pending:
            kind, n = self.pending
            self.out += bytes([kind]) + self.leb(n)
            if kind == LIT:
                self.out += self.lit
                self.lit = bytearray()
            self.pending = None

    def run(self, kind, n, data=b""):
        if self.pending and self.pending[0] == kind:
            self.pending = (kind, self.pending[1] + n)
        else:
            self.flush()
            self.pending = (kind, n)
        if kind == LIT:
            self.lit += data

    def one(self, kind, value):
        self.flush()
        self.out += bytes([kind]) + self.leb(self.zigzag(value))

    def finish(self):
        self.flush()
        return bytes(self.out)


def build_ops(target, other, known, have, shift, maps, address):
    """Ops that turn OTHER (+ known) into target."""
    ops = Ops()
    delta = 0
    i, n = 0, len(target)
    while i < n:
        if have[i]:
            j = i
            while j < n and have[j]:
                j += 1
            ops.run(KNOWN, j - i)
            ops.counts["known"] += j - i
            i = j
            continue
        d = shift[i]
        if d != delta:
            ops.one(SHIFT, d)
            delta = d
        src = i + d
        if 0 <= src < len(other) and other[src] == target[i]:
            ops.run(COPY, 1)
            ops.counts["copy"] += 1
            i += 1
            continue
        # A whole differing word in the gap: a moved pointer or branch?
        w = i & ~3
        if w == i and i + 4 <= n and not any(have[i:i + 4]):
            theirs, ours = word(other, src), word(target, i)
            fixed = False
            if theirs is not None:
                for m in maps:
                    if m.lo <= theirs < m.hi + 0x100000 and m(theirs) == ours:
                        ops.one(WORD, (ours - theirs + 0x80000000) % 0x100000000 - 0x80000000)
                        ops.counts["word"] += 4
                        fixed = True
                        break
                if not fixed:
                    t_other = bl_target(theirs, address + src)
                    t_ours = bl_target(ours, address + i)
                    if (t_other is not None and t_ours is not None
                            and (theirs & 0xFF000000) == (ours & 0xFF000000)):
                        for m in maps:
                            if m.lo <= t_other < m.hi and m(t_other) == t_ours:
                                k = ((ours & 0xFFFFFF) - (theirs & 0xFFFFFF)) % 0x1000000
                                if k >= 0x800000:
                                    k -= 0x1000000
                                ops.one(BL, k)
                                ops.counts["bl"] += 4
                                fixed = True
                                break
            if fixed:
                i += 4
                continue
        ops.run(LIT, 1, target[i:i + 1])
        ops.counts["literal"] += 1
        i += 1
    return ops


# ----------------------------------------------------------------------------
# Applying (mirrors runner/src/release_recipe.cpp)


def read_leb(data, pos):
    value = shift = 0
    while True:
        byte = data[pos]
        pos += 1
        value |= (byte & 0x7F) << shift
        shift += 7
        if not byte & 0x80:
            return value, pos


def unzigzag(value):
    return (value >> 1) ^ -(value & 1)


def apply_ops(ops, size, other, known, have):
    out = bytearray(size)
    pos = p = delta = 0
    while p < len(ops):
        kind = ops[p]
        p += 1
        if kind == KNOWN:
            n, p = read_leb(ops, p)
            if not all(have[pos:pos + n]):
                raise ValueError(f"KNOWN at {pos:#x} covers unknown bytes")
            out[pos:pos + n] = known[pos:pos + n]
            pos += n
        elif kind == SHIFT:
            v, p = read_leb(ops, p)
            delta = unzigzag(v)
        elif kind == COPY:
            n, p = read_leb(ops, p)
            out[pos:pos + n] = other[pos + delta:pos + delta + n]
            pos += n
        elif kind in (WORD, BL):
            v, p = read_leb(ops, p)
            k = unzigzag(v)
            w = int.from_bytes(other[pos + delta:pos + delta + 4], "little")
            if kind == WORD:
                w = (w + k) & 0xFFFFFFFF
            else:
                w = (w & 0xFF000000) | ((w + k) & 0xFFFFFF)
            out[pos:pos + 4] = w.to_bytes(4, "little")
            pos += 4
        elif kind == LIT:
            n, p = read_leb(ops, p)
            out[pos:pos + n] = ops[p:p + n]
            p += n
            pos += n
        else:
            raise ValueError(f"bad op {kind}")
    if pos != size:
        raise ValueError(f"ops cover {pos:#x} of {size:#x} bytes")
    return bytes(out)


# ----------------------------------------------------------------------------
# Releases


def code_files(rom_dir: Path):
    """{folder path: (decompressed bytes, ram address, bank, params offset)}."""
    rom = ndspy.rom.NintendoDSRom(nds_folder.read_rom(rom_dir))
    files = {}
    arm9 = code_compression.decompress(bytes(rom.arm9))
    params = None
    footer = bytes(rom.arm9PostData)
    if len(footer) >= 8 and struct.unpack_from("<I", footer, 0)[0] == 0xDEC00621:
        params = struct.unpack_from("<I", footer, 4)[0]
    files["arm9.bin"] = (arm9, rom.arm9RamAddress, "arm9", params)
    for oid, overlay in sorted(rom.loadArm9Overlays().items()):
        files[f"overlay/overlay9_{oid:04d}.bin"] = (
            bytes(overlay.data), overlay.ramAddress, f"ov{oid:03d}", None)
    return files


def bank_source(recomp: Path, prefix: str, bank: str):
    """The dispatch source of a bank: arm9, or ovNNN (an ARM9 overlay)."""
    name = "arm9" if bank == "arm9" else f"arm9_{bank}"
    path = recomp / f"{prefix}_{name}_dispatch.c"
    return path if path.is_file() else None


HEADER_IDENTITY = 0x20


def folder_sha1(folder: Path) -> bytes:
    """The SHA-1 of the image a folder was unpacked from (its rom.toml)."""
    import tomllib
    with open(folder / nds_folder.MANIFEST, "rb") as f:
        return bytes.fromhex(tomllib.load(f)["rom"]["sha1"])


def check_layout(base: Path, other: Path):
    """What the runner takes from OTHER as it is must agree with BASE."""
    a = ndspy.rom.NintendoDSRom(nds_folder.read_rom(base))
    b = ndspy.rom.NintendoDSRom(nds_folder.read_rom(other))
    if bytes(a.arm9PostData) != bytes(b.arm9PostData):
        raise SystemExit("the ARM9 footers differ (module params moved)")
    for field in ("arm9RamAddress", "arm9EntryAddress", "arm7RamAddress",
                  "arm7EntryAddress"):
        if getattr(a, field) != getattr(b, field):
            raise SystemExit(f"{field} differs")
    if bytes(a.arm7) != bytes(b.arm7):
        raise SystemExit("arm7.bin differs; recipes cover ARM9 code only")
    ya, yb = (base / "y9.bin").read_bytes(), (other / "y9.bin").read_bytes()
    if len(ya) != len(yb) or any(
            ya[i:i + 0x1C] != yb[i:i + 0x1C] or ya[i + 0x1F] != yb[i + 0x1F]
            for i in range(0, len(ya), 0x20)):
        raise SystemExit("the overlay tables differ beyond compressed sizes")


def make(args):
    base = code_files(args.base)
    other = code_files(args.other)
    skip = set(args.skip_bank or [])
    fillers = {}
    for spec in args.filler or []:
        name, _, span = spec.rpartition(":")
        lo, _, hi = span.partition("-")
        fillers.setdefault(name, []).append((int(lo, 0), int(hi, 0)))
    plans = []
    # Pass 1: anchors, shifts and address maps for every file.
    for name, (data, address, bank, params) in base.items():
        theirs = other.get(name)
        if theirs is None:
            raise SystemExit(f"{args.other} has no {name}")
        segments = (arm9_segments(data, address, params) if bank == "arm9"
                    else [(0, address, len(data))])
        source = None if bank in skip else bank_source(args.recomp, args.prefix, bank)
        rows = list(validation_rows(source)) if source else []
        known, have = known_bytes(rows, segments, len(data))
        if data == theirs[0]:
            have = bytearray(len(data))  # copied whole; no ops needed beyond COPY
        anc = anchors(known, have, theirs[0])
        shift = choose_shifts(data, theirs[0], have, anc)
        # Filler: the bytes no validation copy holds are OTHER's own, read in
        # place, and the rebuilt file differs from BASE there.
        for lo, hi in fillers.get(name, ()):
            changed = 0
            data = bytearray(data)
            for i in range(lo, min(hi, len(data))):
                if not have[i]:
                    shift[i] = 0
                    if data[i] != theirs[0][i]:
                        changed += 1
                    data[i] = theirs[0][i]
            data = bytes(data)
            print(f"  {name}: filler {lo:#x}-{hi:#x}, {changed} bytes left "
                  f"as {args.other.name}'s")
        # map: OTHER offset -> BASE offset, from the chosen shifts
        pairs, last = [], None
        for i in range(len(data)):
            if shift[i] != last:
                pairs.append((i, i + shift[i]))
                last = shift[i]
        plans.append(dict(name=name, data=data, other=theirs[0], address=address,
                          bank=source.name[: -len("_dispatch.c")] if rows else "", segments=segments if rows else [],
                          known=known, have=have, shift=shift,
                          map=AddressMap(address, pairs, len(data))))
    arm9_map = next(p["map"] for p in plans if p["name"] == "arm9.bin")
    overlay_maps = [p["map"] for p in plans if p["name"] != "arm9.bin"]

    body = bytearray()
    total = {"known": 0, "copy": 0, "word": 0, "bl": 0, "literal": 0}
    report = []
    for plan in plans:
        own = plan["map"]
        maps = [own] + ([arm9_map] if own is not arm9_map else []) + \
               [m for m in overlay_maps if m is not own]
        ops = build_ops(plan["data"], plan["other"], plan["known"], plan["have"],
                        plan["shift"], maps, plan["address"])
        encoded = ops.finish()
        rebuilt = apply_ops(encoded, len(plan["data"]), plan["other"],
                            plan["known"], plan["have"])
        if rebuilt != plan["data"]:
            bad = next(i for i in range(len(rebuilt)) if rebuilt[i] != plan["data"][i])
            raise SystemExit(f"{plan['name']}: recipe does not rebuild it (first "
                             f"difference at {bad:#x})")
        for key in total:
            total[key] += ops.counts[key]
        report.append({"file": plan["name"], **ops.counts, "op_bytes": len(encoded)})
        name = plan["name"].encode()
        bank = plan["bank"].encode()
        body += struct.pack("<H", len(name)) + name
        body += struct.pack("<H", len(bank)) + bank
        body += struct.pack("<I", len(plan["data"]))
        body += hashlib.sha1(plan["data"]).digest()
        body += struct.pack("<I", len(plan["segments"]))
        for seg in plan["segments"]:
            body += struct.pack("<III", *seg)
        body += struct.pack("<I", len(encoded)) + encoded
        if ops.counts["literal"]:
            print(f"  {plan['name']}: {ops.counts['literal']} literal bytes")
    # The header's identity fields (title, game code, maker, ...). The rest
    # of the header describes the layout, which stays OTHER's.
    ours = (args.base / "header.bin").read_bytes()[:HEADER_IDENTITY]
    theirs = (args.other / "header.bin").read_bytes()[:HEADER_IDENTITY]
    ops = Ops()
    for a, b in zip(ours, theirs):
        if a == b:
            ops.run(COPY, 1)
        else:
            ops.run(LIT, 1, bytes([a]))
            total["literal"] += 1
    encoded = ops.finish()
    body += struct.pack("<H", len(b"header.bin")) + b"header.bin"
    body += struct.pack("<HI", 0, HEADER_IDENTITY) + hashlib.sha1(ours).digest()
    body += struct.pack("<I", 0)
    body += struct.pack("<I", len(encoded)) + encoded
    check_layout(args.base, args.other)
    blob = (MAGIC + struct.pack("<I", VERSION) + folder_sha1(args.base)
            + folder_sha1(args.other) + struct.pack("<I", len(plans) + 1)
            + bytes(body))
    args.out.write_bytes(blob)
    print(f"{args.out}: {len(blob)} bytes, {len(plans) + 1} files; bytes "
          + ", ".join(f"{k} {v}" for k, v in total.items()))
    if args.report:
        args.report.write_text(json.dumps(report, indent=1) + "\n")


def parse(blob: bytes):
    """(base SHA-1, other SHA-1, [file dicts]) of a recipe."""
    if blob[:4] != MAGIC:
        raise SystemExit("not a recipe")
    version, = struct.unpack_from("<I", blob, 4)
    if version != VERSION:
        raise SystemExit(f"recipe version {version}, expected {VERSION}")
    base_sha1, other_sha1 = blob[8:28], blob[28:48]
    count, = struct.unpack_from("<I", blob, 48)
    pos, files = 52, []
    for _ in range(count):
        n, = struct.unpack_from("<H", blob, pos)
        name = blob[pos + 2:pos + 2 + n].decode()
        pos += 2 + n
        n, = struct.unpack_from("<H", blob, pos)
        bank = blob[pos + 2:pos + 2 + n].decode()
        pos += 2 + n
        size, = struct.unpack_from("<I", blob, pos)
        sha1 = blob[pos + 4:pos + 24]
        nseg, = struct.unpack_from("<I", blob, pos + 24)
        pos += 28
        segments = [struct.unpack_from("<III", blob, pos + 12 * i)
                    for i in range(nseg)]
        pos += 12 * nseg
        nops, = struct.unpack_from("<I", blob, pos)
        ops = blob[pos + 4:pos + 4 + nops]
        pos += 4 + nops
        files.append(dict(name=name, bank=bank, size=size, sha1=sha1,
                          segments=segments, ops=ops, ops_at=pos - nops))
    if pos != len(blob):
        raise SystemExit(f"{len(blob) - pos} bytes after the last file")
    return base_sha1, other_sha1, files


def literals(ops):
    """(file position, op position, length) of each LIT's bytes."""
    out = []
    pos = p = 0
    while p < len(ops):
        kind = ops[p]
        p += 1
        if kind in (KNOWN, COPY, LIT):
            n, p = read_leb(ops, p)
            if kind == LIT:
                out.append((pos, p, n))
                p += n
            pos += n
        elif kind == SHIFT:
            _, p = read_leb(ops, p)
        elif kind in (WORD, BL):
            _, p = read_leb(ops, p)
            pos += 4
        else:
            raise SystemExit(f"bad op {kind}")
    return out


def known_spans(ops):
    """(file position, length) of each KNOWN op, and the bytes covered."""
    out = []
    pos = p = 0
    while p < len(ops):
        kind = ops[p]
        p += 1
        if kind in (KNOWN, COPY, LIT):
            n, p = read_leb(ops, p)
            if kind == KNOWN:
                out.append((pos, n))
            elif kind == LIT:
                p += n
            pos += n
        elif kind == SHIFT:
            _, p = read_leb(ops, p)
        elif kind in (WORD, BL):
            _, p = read_leb(ops, p)
            pos += 4
    return out, pos


def blank(args):
    blob = bytearray(args.recipe.read_bytes())
    _base, _other, files = parse(bytes(blob))
    count = 0
    for f in files:
        for _pos, at, n in literals(f["ops"]):
            blob[f["ops_at"] + at:f["ops_at"] + at + n] = bytes(n)
            count += n
    args.out.write_bytes(bytes(blob))
    print(f"{args.out}: {count} literal bytes blanked")


def fill(args):
    blob = bytearray(args.recipe.read_bytes())
    base_sha1, _other, files = parse(bytes(blob))
    if base_sha1 != folder_sha1(args.base):
        raise SystemExit(f"{args.recipe} rebuilds {base_sha1.hex()}, "
                         f"not {args.base}")
    base = code_files(args.base)
    base["header.bin"] = ((args.base / "header.bin").read_bytes()
                          [:HEADER_IDENTITY], 0, "", None)
    names = {f["name"] for f in files}
    problems = [f"{name}: not in the recipe" for name in base if name not in names]
    for f in files:
        if f["name"] not in base:
            problems.append(f"{f['name']}: not a code file of {args.base}")
            continue
        target = base[f["name"]][0]
        if f["size"] != len(target):
            problems.append(f"{f['name']}: size {f['size']:#x}, "
                            f"{args.base} has {len(target):#x}")
            continue
        rows = []
        if f["bank"]:
            source = args.recomp / f"{f['bank']}_dispatch.c"
            if not source.is_file():
                problems.append(f"{f['name']}: {source} missing")
                continue
            rows = list(validation_rows(source))
        known, have = known_bytes(rows, f["segments"], f["size"])
        spans, covered = known_spans(f["ops"])
        if covered != f["size"]:
            problems.append(f"{f['name']}: ops cover {covered:#x} of "
                            f"{f['size']:#x} bytes")
        bad = [pos for pos, n in spans
               if not all(have[pos:pos + n])
               or known[pos:pos + n] != target[pos:pos + n]]
        if bad:
            problems.append(f"{f['name']}: {len(bad)} spans no longer match "
                            f"the banks (first at {bad[0]:#x})")
        for pos, at, n in literals(f["ops"]):
            blob[f["ops_at"] + at:f["ops_at"] + at + n] = target[pos:pos + n]
    if problems:
        raise SystemExit("\n".join([f"{args.recipe} is stale:"] + problems))
    args.out.write_bytes(bytes(blob))
    print(f"{args.out}: filled from {args.base}, matches {args.recomp}")


def main():
    parser = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="command", required=True)
    m = sub.add_parser("make")
    m.add_argument("base", type=Path)
    m.add_argument("other", type=Path)
    m.add_argument("recomp", type=Path)
    m.add_argument("out", type=Path)
    m.add_argument("--prefix", required=True,
                   help="bank name prefix, e.g. dq9 for dq9_arm9_dispatch.c")
    m.add_argument("--skip-bank", action="append",
                   help="a bank whose validation copies are not cartridge "
                        "bytes (e.g. a decrypted overlay); repeatable")
    m.add_argument("--filler", action="append",
                   help="FILE:START-END, a range whose unvalidated bytes are "
                        "padding that need not be BASE's (e.g. the ARM9 "
                        "secure area's filler); repeatable")
    m.add_argument("--report", type=Path)
    b = sub.add_parser("blank")
    b.add_argument("recipe", type=Path)
    b.add_argument("out", type=Path)
    f = sub.add_parser("fill")
    f.add_argument("recipe", type=Path)
    f.add_argument("base", type=Path)
    f.add_argument("recomp", type=Path)
    f.add_argument("out", type=Path)
    args = parser.parse_args()
    if args.command == "make":
        make(args)
    elif args.command == "blank":
        blank(args)
    elif args.command == "fill":
        fill(args)


if __name__ == "__main__":
    main()
