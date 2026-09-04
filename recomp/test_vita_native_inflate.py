#!/usr/bin/env python3
"""Proof driver for the native zlib 1.1.4 inflate_codes seam
(ISAAC_VITA_NATIVE_INFLATE, recomp/runtime/host_vita_native_inflate.c).

Stages (all fail-closed; each prints one line of evidence):
  1. frozen-PE pins: size/sha, the call edge `call 0x5d6d80` at 0x5cf07b with
     its `add esp,4` return site 0x5cf080, the inflate_codes prologue, the two
     zlib 1.1.4 copy-source wrap loops (inflate_codes COPY, inflate_fast), the
     two .rdata messages the seam maps, inflate_mask[17] and the fixed Huffman
     tables (compared entry by entry with the vendored inffixed.h).
  2. vendored-source pins: sha256 of every zlib 1.1.4 file (byte-identical to
     the upstream tarball) and of the prefix headers.
  3. generated-corpus pins (--generated): sub_005d6d80 is defined once with
     coverage 12193; its only translated caller is the pinned site in
     inflate_blocks; the coverage ids of inflate_fast/inflate_flush/adler32,
     the memcpy import slot (index 281) and its thunk (coverage 12570).
  4. blob: thousands of randomized deflate streams from the system zlib
     (levels 0-9, wbits 9-15, raw/zlib/dict, all strategies, flush points,
     0..150 KB payloads), raw streams for smaller windows than they were made
     with (wrap loops), hand-built fixed-Huffman streams with the invalid
     distance codes 30/31 and literal/length codes 286/287.
  5. host oracles (gcc/clang, -Werror on the shipped code):
     core   shipped iz_ core vs pristine rf_ reference, per inflate() call;
     guest  the seam's guards/fallbacks/msg mapping/census on a fake CPU;
     e2e    the shipped seam + core driven end to end by the pristine
            inflate()/inflate_blocks() (ILP32 compiler required, e.g.
            `--e2e-cc "clang -m32"`; skipped with a notice otherwise).
  6. ARM cross-compile (--arm-cc) of the shipped TUs with the eboot flags,
     nm contract (wrap/real symbols, no allocator refs), Tag_ABI_enum_size,
     objdump instruction counts.

Usage: test_vita_native_inflate.py --pe <frozen.exe> [--generated DIR]
         [--cc gcc] [--e2e-cc "clang -m32"] [--arm-cc arm-vita-eabi-gcc]
         [--count 1500] [--seed 1] [--keep DIR]
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import random
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

HERE = Path(__file__).resolve().parent
RUNTIME = HERE / "runtime"
VENDOR = RUNTIME / "vendor" / "zlib114"
VITA = HERE / "vita"

PE_SIZE = 8_650_240
PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
GUEST_BASE = 0x98000000

# --- frozen PE pins ----------------------------------------------------------

CODE_PINS = [
    # rva, bytes, what
    (0x5cf07b, "e8 00 7d 00 00 83 c4 04",
     "inflate_blocks: call 0x5d6d80; add esp,4 (the seam's pinned return site 0x5cf080)"),
    (0x5d6d80, "55 8b ec 83 ec 20", "inflate_codes prologue (push ebp; mov ebp,esp; sub esp,0x20)"),
    (0x5d77f8, "03 f2 3b 71 28 72 f9",
     "inflate_fast 1.1.4 wrap loop: do { r += end-window } while (r < window)"),
    (0x5d7152, "8b d6 2b 51 0c 8b 4b 28 89 55 e4 3b d1 73 19 8b 53 2c 2b d1 89 55 e0 8b 55 e4",
     "inflate_codes COPY: f = q - dist; while (f < window) f += end-window"),
]
MSG_PINS = [
    (0x76a158, "invalid distance code"),
    (0x76a170, "invalid literal/length code"),
]
INFLATE_MASK_RVA = 0x73df70
FIXED_TD_RVA, FIXED_TD_COUNT = 0x7e8570, 32
FIXED_TL_RVA, FIXED_TL_COUNT = 0x7e8670, 512

# --- vendored source pins (zlib 1.1.4 tarball sha256 9e3e9731...8291e) ---------

VENDORED_SHA256 = {
    "adler32.c": "7a921ef4c444aa3f49c312b39499e17bf12b25856ca5a904ea768037016ab337",
    "infblock.c": "282797dee3077d40cbe492428c2eab75b04b496b854b180b3e85140f8ad58394",
    "infblock.h": "8e2b864ee159b0e8bc6ca7a9ac71369b3dc06588ea0d6c2b9e8b2d2248ef8004",
    "infcodes.c": "524e1f47ad64638cd2fd6dd163a8290b313f4d65968e20bd4e05860e1c4d0a4b",
    "infcodes.h": "665310317ceba2af7032fd50d16eaf191f0ae0c805af712f9f9f655807355c89",
    "inffast.c": "1043e4300e9a81e7897ebac507f4cbd75bdcdabd0a77410538404d5076f151f9",
    "inffast.h": "d446d816a4416c849c7e706c583f2f6500ebaa7840d60f4ce2d81a05363e5a35",
    "inffixed.h": "2a28f6ec7a9042b31ee2f214aff62a8ae9ab079a584e278a8e7e0f156b329d9e",
    "inflate.c": "ae643f81db42e3664ac56050146dc90212aba4f450aadcb8ed54908b494887d4",
    "inftrees.c": "43ac236f9a64e933ec06e5bde627e9bb2bda553f6db4b605ecff6862347cbf84",
    "inftrees.h": "52e1dac5dbd3fa922273adea9dae6c6b8c74acc58e1254f6af0bd47691ec36af",
    "infutil.c": "a098f985bdaaddfb9aee665f2dceb1cd6bb07ce997c19908f96b9d13984eeb29",
    "infutil.h": "e235b2fae8e43eb084c8cccd765a1ea8ebceb9cbfe2f6c3852c1cc4caed340ee",
    "zconf.h": "7f991f3b845b18fe68520af54f800c2e2756c26be419b42b46e59b57c492e56a",
    "zlib.h": "7388aeb8cc550dcbc158fd43dc90ee900f5fc6a3d055ceaf4c83aa441e6d387f",
    "zutil.c": "9ef7ed0e467652c1116864dd9b20a2a0602cd68d92312c55f4d6899ffca8ed18",
    "zutil.h": "07a2a41547262b9894db1ad591e724e61f1aa44075bb00538a621a09ef4633ff",
}

ZLIB_ALL = ["inflate.c", "infblock.c", "infcodes.c", "inffast.c",
            "inftrees.c", "infutil.c", "adler32.c", "zutil.c"]
SHIPPED_LEAVES = ["inffast.c", "infutil.c", "adler32.c"]   # + infcodes.c via the wrapper TU

SEAM_DEFS = ["-DISAAC_VITA_NATIVE_INFLATE=1", "-DGUEST_IMAGE_BASE=0x98000000u",
             "-DGUEST_STACK_REQUIRED=1"]
STRICT = ["-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-fno-strict-aliasing"]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def sha256(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1 << 20), b""):
            value.update(chunk)
    return value.hexdigest()


def run(command: list[str], cwd: Path | None = None) -> str:
    result = subprocess.run(command, check=False, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, cwd=cwd)
    if result.returncode != 0:
        raise AssertionError(
            f"command failed ({result.returncode}): {' '.join(command)}\n{result.stdout}")
    return result.stdout


def which_cc(explicit: str | None) -> list[str]:
    if explicit:
        words = shlex.split(explicit)
        require(shutil.which(words[0]) is not None, f"compiler not found: {words[0]}")
        return words
    for candidate in (os.environ.get("CC"), "gcc", "clang", "cc"):
        if candidate and shutil.which(shlex.split(candidate)[0]):
            return shlex.split(candidate)
    raise AssertionError("no host C compiler found; pass --cc")


# --- 1. frozen PE ------------------------------------------------------------

class PE:
    def __init__(self, path: Path) -> None:
        self.data = path.read_bytes()
        e_lfanew = struct.unpack_from("<I", self.data, 0x3c)[0]
        count = struct.unpack_from("<H", self.data, e_lfanew + 6)[0]
        optional = struct.unpack_from("<H", self.data, e_lfanew + 20)[0]
        first = e_lfanew + 24 + optional
        self.sections = []
        for index in range(count):
            name, vsize, va, rsize, rptr = struct.unpack_from("<8sIIII", self.data, first + 40 * index)
            self.sections.append((name.rstrip(b"\0").decode(), va, vsize, rptr, rsize))

    def read(self, rva: int, size: int) -> bytes:
        for _name, va, vsize, rptr, rsize in self.sections:
            if va <= rva < va + max(vsize, rsize):
                offset = rptr + (rva - va)
                return self.data[offset:offset + size]
        raise AssertionError(f"RVA 0x{rva:x} is in no section")


def parse_inffixed() -> tuple[list[tuple[int, int, int]], list[tuple[int, int, int]]]:
    text = (VENDOR / "inffixed.h").read_text()
    tables = {}
    for name in ("fixed_tl", "fixed_td"):
        start = text.index(f"local inflate_huft {name}[] = {{")
        end = text.index("};", start)
        body = text[start:end]
        entries = [(int(a), int(b), int(c)) for a, b, c in
                   re.findall(r"\{\{\{(\d+),(\d+)\}\},(\d+)\}", body)]
        tables[name] = entries
    return tables["fixed_tl"], tables["fixed_td"]


def verify_pe(path: Path) -> None:
    require(path.stat().st_size == PE_SIZE, "frozen PE size changed")
    require(sha256(path) == PE_SHA256, "frozen PE hash changed")
    pe = PE(path)
    for rva, hexbytes, what in CODE_PINS:
        want = bytes.fromhex(hexbytes.replace(" ", ""))
        got = pe.read(rva, len(want))
        require(got == want, f"PE code pin at 0x{rva:x} ({what}): {got.hex(' ')} != {want.hex(' ')}")
    for rva, text in MSG_PINS:
        want = text.encode("ascii") + b"\0"
        got = pe.read(rva, len(want))
        require(got == want, f"PE msg at 0x{GUEST_BASE + rva:08x} is {got!r}, expected {want!r}")
    mask = struct.unpack("<17I", pe.read(INFLATE_MASK_RVA, 68))
    require(list(mask) == [(1 << i) - 1 for i in range(17)], f"inflate_mask table drifted: {mask}")
    tl, td = parse_inffixed()
    require(len(tl) == FIXED_TL_COUNT and len(td) == FIXED_TD_COUNT, "inffixed.h shape")
    for name, rva, entries in (("fixed_tl", FIXED_TL_RVA, tl), ("fixed_td", FIXED_TD_RVA, td)):
        raw = pe.read(rva, 8 * len(entries))
        for index, (exop, bits, base) in enumerate(entries):
            e, b, _p0, _p1, bs = struct.unpack_from("<BBBBI", raw, 8 * index)
            require((e, b, bs) == (exop, bits, base),
                    f"PE {name}[{index}] = ({e},{b},{bs}) != inffixed.h ({exop},{bits},{base})")
    layout = (RUNTIME / "host_vita_native_inflate_layout.h").read_text()
    for macro, value in (("ISAAC_NI_INFLATE_CODES_RVA", 0x5d6d80), ("ISAAC_NI_CODES_RETURN_RVA", 0x5cf080),
                         ("ISAAC_NI_MSG_BAD_DIST_RVA", 0x76a158), ("ISAAC_NI_MSG_BAD_LITLEN_RVA", 0x76a170),
                         ("ISAAC_NI_FIXED_TD_RVA", FIXED_TD_RVA), ("ISAAC_NI_FIXED_TL_RVA", FIXED_TL_RVA),
                         ("ISAAC_NI_FIXED_TD_HUFTS", FIXED_TD_COUNT), ("ISAAC_NI_FIXED_TL_HUFTS", FIXED_TL_COUNT),
                         ("ISAAC_NI_ADLER32_RVA", 0x5cf3d0), ("ISAAC_NI_MEMCPY_IAT_RVA", 0x606488)):
        found = re.search(rf"#define\s+{macro}\s+(0x[0-9a-fA-F]+|\d+)", layout)
        require(found is not None and int(found.group(1), 0) == value,
                f"{macro} in host_vita_native_inflate_layout.h != 0x{value:x}")
    print(f"frozen PE: {len(CODE_PINS)} code pins, 2 msg strings, inflate_mask[17], "
          f"fixed_tl[{len(tl)}] + fixed_td[{len(td)}] == inffixed.h, layout RVAs pinned")


# --- 2. vendored sources -----------------------------------------------------

def verify_vendored() -> None:
    for name, digest in VENDORED_SHA256.items():
        got = sha256(VENDOR / name)
        require(got == digest, f"vendored {name} changed: {got}")
    prefix = (VENDOR / "izlib114_prefix.h").read_text()
    for symbol in ("inflate_codes", "inflate_fast", "inflate_flush", "adler32", "inflate_mask"):
        require(re.search(rf"#define\s+{symbol}\s+iz_{symbol}\b", prefix), f"prefix lacks {symbol}")
    print(f"vendored zlib 1.1.4: {len(VENDORED_SHA256)} files byte-identical to the tarball")


# --- 3. generated corpus -----------------------------------------------------

def verify_generated(generated: Path) -> None:
    sources = sorted(generated.glob("guest_*.c"))
    require(sources, f"no generated units in {generated}")
    owners, callers = [], []
    for source in sources:
        text = source.read_text(errors="replace")
        if "sub_005d6d80" not in text:
            continue
        for match in re.finditer(r"^void sub_005d6d80\(CPU \*__restrict c\)$", text, re.M):
            owners.append(source.name)
            body = text[match.end():match.end() + 400]
            require("guest_coverage_function(12193U);" in body, "inflate_codes coverage id != 12193")
        for line in text.splitlines():
            if re.search(r"\bsub_005d6d80\(c\)", line):
                callers.append((source.name, line.strip()))
    require(owners == ["guest_0181.c"], f"inflate_codes owner: {owners}")
    require(len(callers) == 1, f"inflate_codes must have one translated caller: {callers}")
    require(callers[0][0] == "guest_0180.c" and callers[0][1].startswith("GPUSH(0x5cf080U);"),
            f"caller site drifted: {callers[0]}")

    def coverage_of(unit: str, symbol: str) -> int:
        text = (generated / unit).read_text(errors="replace")
        match = re.search(rf"^void {symbol}\(CPU \*__restrict c\)$", text, re.M)
        require(match is not None, f"{symbol} not in {unit}")
        found = re.search(r"guest_coverage_function\((\d+)U\)", text[match.end():match.end() + 400])
        require(found is not None, f"{symbol} has no coverage note")
        return int(found.group(1))

    require(coverage_of("guest_0181.c", "sub_005d7610") == 12195, "inflate_fast coverage")
    require(coverage_of("guest_0181.c", "sub_005d6720") == 12191, "inflate_flush coverage")
    require(coverage_of("guest_0180.c", "sub_005cf3d0") == 12124, "adler32 coverage")
    require(coverage_of("guest_0180.c", "sub_005ce7d0") is not None, "inflate_blocks owner")
    table = (generated / "guest_table.c").read_text(errors="replace")
    start = table.index("static const guest_import s_imports[] = {")
    entries = re.findall(r"^\s*\{ (0x[0-9A-Fa-f]+)U, \"([^\"]+)\" \},", table[start:], re.M)
    require(len(entries) > 281 and entries[281] == ("0x00606488", "VCRUNTIME140.dll!memcpy"),
            f"memcpy import index 281 drifted: {entries[281] if len(entries) > 281 else None}")
    thunk_units = [s for s in sources if "void sub_005ec14c(CPU" in s.read_text(errors="replace")]
    require(len(thunk_units) == 1, f"memcpy thunk owner: {thunk_units}")
    thunk = thunk_units[0].read_text(errors="replace")
    at = thunk.index("void sub_005ec14c(CPU")
    body = thunk[at:at + 600]
    require("guest_coverage_function(12570U);" in body and "jmp dword ptr [0x98606488]" in body,
            "memcpy thunk (coverage 12570, jmp [IAT 0x606488]) drifted")
    # inflate_flush reaches memcpy through that thunk (two copy segments)
    flush = (generated / "guest_0181.c").read_text(errors="replace")
    a = flush.index("void sub_005d6720(CPU"); b = flush.index("\n/* sub_", a + 1)
    require(flush[a:b].count("sub_005ec14c(c)") == 2, "inflate_flush memcpy thunk calls != 2")
    require("sub_005d6720(c)" in flush[flush.index("void sub_005d6d80(CPU"):] and
            "sub_005d7610(c)" in flush[flush.index("void sub_005d6d80(CPU"):],
            "inflate_codes must call inflate_flush and inflate_fast")
    print("generated corpus: sub_005d6d80 owned by guest_0181.c (cov 12193), single caller "
          "guest_0180.c @0x5cf080, fast/flush/adler cov 12195/12191/12124, memcpy import 281, "
          "thunk sub_005ec14c cov 12570")


# --- 4. blob -----------------------------------------------------------------

WORDS = (b"room stage door item pickup npc effect tear bomb key coin heart soul "
         b"boss floor basement caves depths womb cathedral sheol chest dark home "
         b"<entity type= variant= subtype= x= y=/> gfx/ anm2 png stb xml lua ").split()


class BitWriter:
    def __init__(self) -> None:
        self.bits: list[int] = []

    def put(self, value: int, count: int) -> None:          # LSB first
        for i in range(count):
            self.bits.append((value >> i) & 1)

    def huff(self, code: int, length: int) -> None:         # MSB first
        for i in range(length - 1, -1, -1):
            self.bits.append((code >> i) & 1)

    def bytes(self) -> bytes:
        out = bytearray()
        for i in range(0, len(self.bits), 8):
            chunk = self.bits[i:i + 8]
            out.append(sum(bit << k for k, bit in enumerate(chunk)))
        return bytes(out)


def fixed_litlen(w: BitWriter, symbol: int) -> None:
    if symbol < 144:
        w.huff(0x30 + symbol, 8)
    elif symbol < 256:
        w.huff(0x190 + symbol - 144, 9)
    elif symbol < 280:
        w.huff(symbol - 256, 7)
    else:
        w.huff(0xC0 + symbol - 280, 8)


def badcode_stream(literals: bytes, bad: str, code: int, pad: int) -> bytes:
    """A final fixed-Huffman block: literals, then an invalid symbol.  bad ==
    'dist': length 3 (symbol 257) followed by distance code 30/31; bad ==
    'litlen': literal/length code 286/287.  pad zero bytes keep avail_in >= 10
    so inflate_fast (not inflate_codes) hits the error under wide schedules."""
    w = BitWriter()
    w.put(1, 1)          # BFINAL
    w.put(1, 2)          # BTYPE = 01 fixed
    for byte in literals:
        fixed_litlen(w, byte)
    if bad == "dist":
        fixed_litlen(w, 257)
        w.huff(code, 5)
    else:
        fixed_litlen(w, code)
    return w.bytes() + b"\0" * pad


def payload(rng: random.Random, size: int) -> bytes:
    style = rng.randrange(8)
    if size == 0:
        return b""
    if style == 0:
        return rng.randbytes(size)
    if style == 1:
        out = bytearray()
        while len(out) < size:
            out += rng.choice(WORDS) + (b" " if rng.random() < 0.8 else b"\n")
        return bytes(out[:size])
    if style == 2:
        unit = rng.randbytes(rng.randrange(1, 64))
        return (unit * (size // len(unit) + 1))[:size]
    if style == 3:
        out = bytearray()
        while len(out) < size:
            out += bytes([rng.randrange(256)]) * rng.randrange(1, 300)
        return bytes(out[:size])
    if style == 4:
        return bytes(size)
    if style == 5:
        # long-period repetition: matches farther than the small windows
        period = rng.choice([600, 1100, 2100, 4200, 9000, 20000])
        unit = rng.randbytes(min(period, size))
        return (unit * (size // len(unit) + 1))[:size]
    if style == 6:
        out = bytearray()
        while len(out) < size:
            r = rng.random()
            if r < 0.4:
                out += rng.choice(WORDS)
            elif r < 0.7:
                out += rng.randbytes(rng.randrange(1, 40))
            else:
                out += bytes([rng.randrange(256)]) * rng.randrange(3, 60)
        return bytes(out[:size])
    out = bytearray()
    i = 0
    while len(out) < size:
        out += b"%d,%d,%d;" % (i % 13, (i * 7) % 15, rng.randrange(4))
        i += 1
    return bytes(out[:size])


def pick_size(rng: random.Random) -> int:
    r = rng.random()
    if r < 0.08:
        return rng.randrange(0, 4)
    if r < 0.55:
        return rng.randrange(4, 2048)
    if r < 0.85:
        return rng.randrange(2048, 16384)
    if r < 0.97:
        return rng.randrange(16384, 65536)
    return rng.randrange(65536, 150_000)


def compress(rng: random.Random, data: bytes, wbits: int, zdict: bytes | None) -> bytes:
    level = rng.randrange(0, 10)
    strategy = rng.choice([zlib.Z_DEFAULT_STRATEGY, zlib.Z_DEFAULT_STRATEGY, zlib.Z_FILTERED,
                           zlib.Z_HUFFMAN_ONLY, zlib.Z_RLE, zlib.Z_FIXED])
    memlevel = rng.randrange(1, 10)
    kwargs = {"zdict": zdict} if zdict else {}
    comp = zlib.compressobj(level, zlib.DEFLATED, wbits, memlevel, strategy, **kwargs)
    out = bytearray()
    if data and rng.random() < 0.35:
        pos = 0
        while pos < len(data):
            step = rng.randrange(1, max(2, len(data) // rng.randrange(1, 6) + 1))
            out += comp.compress(data[pos:pos + step])
            pos += step
            if pos < len(data):
                out += comp.flush(rng.choice([zlib.Z_SYNC_FLUSH, zlib.Z_FULL_FLUSH,
                                              zlib.Z_PARTIAL_FLUSH, zlib.Z_BLOCK, zlib.Z_NO_FLUSH]))
    else:
        out += comp.compress(data)
    out += comp.flush(zlib.Z_FINISH)
    return bytes(out)


# zlib 1.1.x raw inflate (no trailer) needs one byte past the end of the deflate
# data before it can return Z_STREAM_END: the final symbol's table lookup wants
# lbits/dbits of lookahead and NEEDBITS leaves with Z_BUF_ERROR at avail_in == 0
# (documented in zlib 1.2's zlib.h; every 1.1.x raw consumer supplies the byte).
RAW_TAIL = b"\0"


def write_blob(path: Path, count: int, seed: int) -> dict[str, int]:
    rng = random.Random(seed)
    records = []
    kinds = {"zlib": 0, "raw": 0, "dict": 0, "smallwin": 0, "badcode": 0}
    for _ in range(count):
        data = payload(rng, pick_size(rng))
        r = rng.random()
        if r < 0.50:
            wbits = rng.randrange(9, 16)
            records.append((0, wbits, data, compress(rng, data, wbits, None), b""))
            kinds["zlib"] += 1
        elif r < 0.75:
            wbits = rng.randrange(9, 16)
            records.append((1, -wbits, data, compress(rng, data, -wbits, None) + RAW_TAIL, b""))
            kinds["raw"] += 1
        elif r < 0.87:
            wbits = rng.randrange(9, 16)
            zdict = payload(rng, rng.randrange(1, 32768))
            records.append((2, wbits, data, compress(rng, data, wbits, zdict), zdict))
            kinds["dict"] += 1
        else:
            made = rng.randrange(11, 16)
            decode = rng.randrange(9, made)
            records.append((3, -decode, data, compress(rng, data, -made, None) + RAW_TAIL, b""))
            kinds["smallwin"] += 1
    for literals in (b"", b"abc", b"x" * 300, bytes(range(256)) * 2):
        for pad in (1, 16):
            for code in (30, 31):
                records.append((4, -15, literals, badcode_stream(literals, "dist", code, pad), b""))
                kinds["badcode"] += 1
            for code in (286, 287):
                records.append((4, -15, literals, badcode_stream(literals, "litlen", code, pad), b""))
                kinds["badcode"] += 1
    rng.shuffle(records)
    with path.open("wb") as out:
        out.write(b"NIBL" + struct.pack("<II", 2, len(records)))
        for kind, wbits, orig, comp, zdict in records:
            out.write(struct.pack("<bbHIII", kind, wbits, 0, len(orig), len(comp), len(zdict)))
            out.write(orig); out.write(comp); out.write(zdict)
    kinds["bytes"] = path.stat().st_size
    return kinds


# --- 5. host oracles ---------------------------------------------------------

def prefix_variant(work: Path, tag: str, extra: str = "") -> Path:
    """The vendored prefix header with iz_ renamed to <tag>_ plus optional
    overrides appended (hook routing for a reference build)."""
    text = (VENDOR / "izlib114_prefix.h").read_text()
    text = text.replace(" iz_", f" {tag}_").replace("IZLIB114_PREFIX_H", f"NI_{tag.upper()}_PREFIX_H")
    if extra:
        text = text.replace("#endif /* IZLIB114_PREFIX_H */", extra + "\n#endif")
        text = text.replace(f"#endif /* NI_{tag.upper()}_PREFIX_H */", extra + "\n#endif")
    path = work / f"{tag}_prefix.h"
    path.write_text(text)
    return path


def hooked_variants(work: Path, tag: str, fast: str, flush: str, zmemcpy: str) -> tuple[Path, Path]:
    codes = work / f"{tag}_prefix_codes.h"
    codes.write_text(f'#include "{tag}_prefix.h"\n#undef inflate_fast\n#undef inflate_flush\n'
                     f"#define inflate_fast {fast}\n#define inflate_flush {flush}\n")
    util = work / f"{tag}_prefix_util.h"
    util.write_text(f'#include "{tag}_prefix.h"\n#define NO_MEMCPY 1\n#define zmemcpy {zmemcpy}\n')
    return codes, util


class Builder:
    def __init__(self, cc: list[str], work: Path, label: str) -> None:
        self.cc, self.work, self.label = cc, work, label
        self.objects: list[str] = []
        self.n = 0

    def compile(self, source: Path, flags: list[str], name: str) -> Path:
        self.n += 1
        obj = self.work / f"{self.label}_{self.n:02d}_{name}.o"
        run([*self.cc, "-c", *flags, f"-I{RUNTIME}", f"-I{VENDOR}", f"-I{self.work}",
             str(source), "-o", str(obj)])
        self.objects.append(str(obj))
        return obj

    def link(self, name: str) -> Path:
        binary = self.work / (f"{self.label}_{name}.exe" if os.name == "nt" else f"{self.label}_{name}")
        run([*self.cc, *self.objects, "-o", str(binary)])
        return binary


def add_shipped_core(b: Builder, oracle_defs: list[str]) -> None:
    """Exactly the TUs and per-source flags CMake gives the eboot (iz_ prefix,
    hooked infcodes.c through the pinning wrapper, NO_MEMCPY infutil.c)."""
    iz = VENDOR / "izlib114_prefix.h"
    b.compile(RUNTIME / "host_vita_native_inflate_zlib114_codes.c",
              ["-O2", "-w", "-include", str(VENDOR / "izlib114_prefix_codes.h"), *SEAM_DEFS, *oracle_defs], "iz_codes")
    b.compile(VENDOR / "infutil.c", ["-O2", "-w", "-include", str(VENDOR / "izlib114_prefix_util.h")], "iz_infutil")
    b.compile(VENDOR / "inffast.c", ["-O2", "-w", "-include", str(iz)], "iz_inffast")
    b.compile(VENDOR / "adler32.c", ["-O2", "-w", "-include", str(iz)], "iz_adler32")
    b.compile(RUNTIME / "host_vita_native_inflate_hooks.c",
              [*STRICT, "-include", str(iz), *SEAM_DEFS, *oracle_defs], "iz_hooks")


def add_plain_driver(b: Builder, prefix: Path, tag: str, files: list[str]) -> None:
    for name in files:
        b.compile(VENDOR / name, ["-O2", "-w", "-include", str(prefix)], f"{tag}_{name[:-2]}")


def add_reference(b: Builder) -> None:
    rf = prefix_variant(b.work, "rf")
    codes, util = hooked_variants(b.work, "rf", "ni_rf_hook_inflate_fast", "ni_rf_hook_inflate_flush",
                                  "ni_rf_hook_zmemcpy")
    add_plain_driver(b, rf, "rf", ["inflate.c", "infblock.c", "inffast.c", "inftrees.c", "adler32.c", "zutil.c"])
    b.compile(VENDOR / "infcodes.c", ["-O2", "-w", "-include", str(codes)], "rf_infcodes")
    b.compile(VENDOR / "infutil.c", ["-O2", "-w", "-include", str(util)], "rf_infutil")


def run_core_oracle(cc: list[str], work: Path, blob: Path, seeds: list[int]) -> None:
    b = Builder(cc, work, "core")
    oracle_defs = ["-DISAAC_NI_HOST_ORACLE=1"]
    add_shipped_core(b, oracle_defs)
    add_plain_driver(b, VENDOR / "izlib114_prefix.h", "iz", ["inflate.c", "infblock.c", "inftrees.c", "zutil.c"])
    add_reference(b)
    b.compile(RUNTIME / "host_vita_native_inflate_oracle.c", [*STRICT, "-DISAAC_NI_E2E=0", *SEAM_DEFS[1:]], "oracle")
    binary = b.link("oracle")
    for seed in seeds:
        out = run([str(binary), "core", str(blob), str(seed)])
        require("PASS" in out, f"core oracle failed:\n{out}")
        print(f"  seed {seed}: {out.strip()}")


def run_guest_oracle(cc: list[str], work: Path) -> None:
    b = Builder(cc, work, "guest")
    iz = VENDOR / "izlib114_prefix.h"
    oracle_defs = ["-DISAAC_NI_HOST_ORACLE=1"]
    b.compile(RUNTIME / "host_vita_native_inflate.c", [*STRICT, "-include", str(iz), *SEAM_DEFS, *oracle_defs], "seam")
    b.compile(RUNTIME / "host_vita_native_inflate_hooks.c", [*STRICT, "-include", str(iz), *SEAM_DEFS, *oracle_defs], "hooks")
    b.compile(VENDOR / "infutil.c", ["-O2", "-w", "-include", str(VENDOR / "izlib114_prefix_util.h")], "infutil")
    b.compile(VENDOR / "inffast.c", ["-O2", "-w", "-include", str(iz)], "inffast")
    b.compile(VENDOR / "adler32.c", ["-O2", "-w", "-include", str(iz)], "adler32")
    b.compile(RUNTIME / "host_vita_native_inflate_guest_oracle.c",
              [*STRICT, "-include", str(iz), *SEAM_DEFS, *oracle_defs], "oracle")
    binary = b.link("oracle")
    out = run([str(binary)])
    require("PASS" in out, f"guest oracle failed:\n{out}")
    print("  " + out.strip().replace("\n", "\n  "))


def ilp32_ok(cc: list[str], work: Path) -> bool:
    probe = work / "ilp32_probe.c"
    probe.write_text("#include <stdio.h>\nint main(void){printf(\"%d %d %d\", (int)sizeof(void*), "
                     "(int)sizeof(long), (int)sizeof(enum{A}));return 0;}\n")
    binary = work / ("ilp32_probe.exe" if os.name == "nt" else "ilp32_probe")
    try:
        run([*cc, str(probe), "-o", str(binary)])
        return run([str(binary)]).strip() == "4 4 4"
    except AssertionError:
        return False


def run_e2e_oracle(cc: list[str], work: Path, blob: Path, seeds: list[int]) -> None:
    b = Builder(cc, work, "e2e")
    iz = VENDOR / "izlib114_prefix.h"
    oracle_defs = ["-DISAAC_NI_HOST_ORACLE=1"]
    # the shipped seam + core, exactly as the eboot compiles them
    b.compile(RUNTIME / "host_vita_native_inflate.c", [*STRICT, "-include", str(iz), *SEAM_DEFS, *oracle_defs], "seam")
    add_shipped_core(b, oracle_defs)
    # sm_: pristine inflate()/inflate_blocks() whose inflate_codes call enters the seam
    sm = prefix_variant(work, "sm")
    sm_blocks = work / "sm_prefix_blocks.h"
    sm_blocks.write_text('#include "sm_prefix.h"\n#undef inflate_codes\n#define inflate_codes isaac_ni_e2e_codes_entry\n')
    add_plain_driver(b, sm, "sm", ["inflate.c", "infcodes.c", "inffast.c", "inftrees.c", "infutil.c", "adler32.c", "zutil.c"])
    b.compile(VENDOR / "infblock.c", ["-O2", "-w", "-include", str(sm_blocks)], "sm_infblock")
    add_reference(b)
    b.compile(RUNTIME / "host_vita_native_inflate_oracle.c", [*STRICT, "-DISAAC_NI_E2E=1", *SEAM_DEFS, *oracle_defs], "oracle")
    binary = b.link("oracle")
    for seed in seeds:
        out = run([str(binary), "e2e", str(blob), str(seed)])
        require("PASS" in out, f"e2e oracle failed:\n{out}")
        print(f"  seed {seed}: {out.strip()}")


# --- 6. ARM cross-compile ----------------------------------------------------

ARM_FLAGS = ["-std=gnu11", "-O2", "-mcpu=cortex-a9", "-mfpu=neon", "-mfloat-abi=softfp", "-mthumb",
             "-fno-strict-aliasing", "-ffunction-sections", "-fdata-sections", "-Wall", "-Wextra",
             "-DGUEST_IMAGE_BASE=0x98000000u", "-DGUEST_STACK_REQUIRED=1", "-DISAAC_VITA_HAS_RUNTIME=1",
             "-DISAAC_VITA_HEAP_MB=64", "-DISAAC_VITA_LUA=1", "-DISAAC_VITA_RAW_ALLOCATOR_GATE=1"]


def arm_cross(arm_cc: str, work: Path) -> None:
    cc = shlex.split(arm_cc)
    require(shutil.which(cc[0]) is not None, f"ARM compiler not found: {cc[0]}")
    tool = Path(shutil.which(cc[0]))
    prefix = tool.name[:tool.name.rindex("gcc")]
    nm, objdump, readelf = (str(tool.with_name(prefix + t)) for t in ("nm", "objdump", "readelf"))
    poison = VITA / "isaac_vita_raw_allocator_poison.h"
    common = [*ARM_FLAGS, f"-I{VITA}", f"-I{RUNTIME}", f"-I{VENDOR}"]
    if poison.exists():
        common += ["-include", str(poison)]
    iz = VENDOR / "izlib114_prefix.h"
    seam_defs = ["-DISAAC_VITA_NATIVE_INFLATE=1", "-DISAAC_VITA_NATIVE_INFLATE_WRAP=1",
                 '-DISAAC_VITA_NATIVE_INFLATE_BUILD_ID="test"']
    plan = [
        ("seam", RUNTIME / "host_vita_native_inflate.c", ["-Werror", "-include", str(iz), *seam_defs]),
        ("hooks", RUNTIME / "host_vita_native_inflate_hooks.c", ["-Werror", "-include", str(iz), *seam_defs]),
        ("codes", RUNTIME / "host_vita_native_inflate_zlib114_codes.c",
         ["-w", "-include", str(VENDOR / "izlib114_prefix_codes.h"), *seam_defs]),
        ("infutil", VENDOR / "infutil.c", ["-w", "-include", str(VENDOR / "izlib114_prefix_util.h")]),
        ("inffast", VENDOR / "inffast.c", ["-w", "-include", str(iz)]),
        ("adler32", VENDOR / "adler32.c", ["-w", "-include", str(iz)]),
    ]
    objects = {}
    for name, source, flags in plan:
        obj = work / f"arm_{name}.o"
        out = run([*cc, "-c", *common, *flags, str(source), "-o", str(obj)])
        require("warning" not in out, f"ARM compile of {name} warned:\n{out}")
        objects[name] = obj
    symbols = run([nm, *map(str, objects.values())])
    require(re.search(r"\bT __wrap_sub_005d6d80\b", symbols), "no __wrap_sub_005d6d80")
    require(re.search(r"\bU __real_sub_005d6d80\b", symbols), "no __real_sub_005d6d80")
    require(re.search(r"\bT iz_inflate_codes\b", symbols) and re.search(r"\bT iz_inflate_fast\b", symbols)
            and re.search(r"\bT iz_inflate_flush\b", symbols) and re.search(r"\bT iz_adler32\b", symbols),
            "shipped core symbols missing")
    defined = set(re.findall(r"\b[TtDdBbRr] (\S+)$", symbols, re.M))
    undefined = set(re.findall(r"\bU (\S+)$", symbols, re.M)) - defined
    for bad in ("malloc", "calloc", "realloc", "free", "zcalloc", "zcfree", "iz_zcalloc", "inflate_codes",
                "inflate_fast", "inflate_flush", "adler32", "zmemcpy"):
        require(bad not in undefined, f"shipped inflate TUs reference {bad}")
    allowed = {"__real_sub_005d6d80", "guest_fault", "gpop_generated", "isaac_vita_log", "memcpy", "strcmp",
               "guest_note_authenticated_translated_call", "guest_note_authenticated_import_call",
               "g_guest_coverage_functions", "memset", "__aeabi_uidivmod", "__aeabi_idivmod", "__aeabi_uidiv",
               "__aeabi_idiv", "__aeabi_uldivmod", "guest_stack_owner_violation", "guest_stack_fault"}
    unexpected = {u for u in undefined if u not in allowed and not u.startswith("__aeabi")}
    require(not unexpected, f"unexpected undefined symbols in the shipped TUs: {sorted(unexpected)}")
    attrs = run([readelf, "-A", str(objects["codes"])])
    require("Tag_ABI_enum_size: small" in attrs, "codes TU enum size is not the eboot's (small)")
    counts = {}
    for name, symbol in (("seam", "isaac_vita_native_inflate_codes_try"), ("wrap", "__wrap_sub_005d6d80"),
                         ("codes", "iz_inflate_codes"), ("inffast", "iz_inflate_fast"),
                         ("infutil", "iz_inflate_flush"), ("adler32", "iz_adler32")):
        obj = objects["seam" if name in ("seam", "wrap") else name]
        listing = run([objdump, "-d", str(obj)])
        match = re.search(rf"<{re.escape(symbol)}>:\n(.*?)(?:\n\n|\Z)", listing, re.S)
        require(match is not None, f"{symbol} not in objdump")
        insns = [l for l in match.group(1).splitlines() if re.match(r"\s*[0-9a-f]+:\s+[0-9a-f]{4}", l)]
        counts[symbol] = len(insns)
    sizes = run([nm, "-S", "--size-sort", str(objects["seam"]), str(objects["codes"]), str(objects["inffast"]),
                 str(objects["infutil"]), str(objects["adler32"])])
    wanted = {}
    for line in sizes.splitlines():
        parts = line.split()
        if len(parts) == 4 and parts[3] in counts:
            wanted[parts[3]] = int(parts[1], 16)
    print("ARM cross-compile (eboot flags, -Werror on the seam/hooks): OK; nm contract OK; "
          "Tag_ABI_enum_size small; insns/bytes: " +
          ", ".join(f"{k}={v}/{wanted.get(k, 0):#x}" for k, v in counts.items()))


# --- main --------------------------------------------------------------------

def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", required=True, type=Path)
    parser.add_argument("--generated", type=Path)
    parser.add_argument("--cc")
    parser.add_argument("--e2e-cc", help='ILP32 host compiler, e.g. "clang -m32"')
    parser.add_argument("--arm-cc")
    parser.add_argument("--count", type=int, default=1500)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--keep", type=Path, help="work directory to keep (default: temp)")
    parser.add_argument("--skip-core", action="store_true")
    parser.add_argument("--skip-guest", action="store_true")
    args = parser.parse_args()

    verify_pe(args.pe)
    verify_vendored()
    if args.generated:
        verify_generated(args.generated)
    else:
        print("generated corpus: skipped (no --generated)")

    tmp = None
    if args.keep:
        work = args.keep
        work.mkdir(parents=True, exist_ok=True)
    else:
        tmp = tempfile.TemporaryDirectory(prefix="isaac-native-inflate-")
        work = Path(tmp.name)
    try:
        blob = work / "streams.bin"
        kinds = write_blob(blob, args.count, args.seed)
        print(f"blob: {sum(v for k, v in kinds.items() if k != 'bytes')} streams "
              f"({', '.join(f'{k}={v}' for k, v in kinds.items())}), python zlib {zlib.ZLIB_RUNTIME_VERSION}")
        seeds = [args.seed, args.seed + 1000003]
        cc = which_cc(args.cc)
        if not args.skip_guest:
            print(f"guest oracle ({' '.join(cc)}):")
            run_guest_oracle(cc, work)
        if not args.skip_core:
            print(f"core oracle ({' '.join(cc)}):")
            run_core_oracle(cc, work, blob, seeds)
        e2e_cc = shlex.split(args.e2e_cc) if args.e2e_cc else None
        if e2e_cc is None and ilp32_ok(cc, work):
            e2e_cc = cc
        if e2e_cc is not None:
            require(ilp32_ok(e2e_cc, work), f"{' '.join(e2e_cc)} is not an ILP32 compiler")
            print(f"e2e oracle ({' '.join(e2e_cc)}):")
            run_e2e_oracle(e2e_cc, work, blob, seeds)
        else:
            print("e2e oracle: skipped (no ILP32 host compiler; pass --e2e-cc \"clang -m32\")")
        if args.arm_cc:
            arm_cross(args.arm_cc, work)
        else:
            print("ARM cross-compile: skipped (no --arm-cc)")
    finally:
        if tmp is not None:
            tmp.cleanup()
    print("Vita native inflate seam: PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
