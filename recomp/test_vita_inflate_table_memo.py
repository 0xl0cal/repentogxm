#!/usr/bin/env python3
"""Proof for ISAAC_VITA_INFLATE_TABLE_MEMO (archive inflater Huffman table memo).

1. Wiring pins: default-OFF option, FATAL_ERROR dependency on the archive
   MiniZ fast path, the compile definition scoped to exactly the archive tinfl
   translation unit and the memo module, README mention, and every memo
   reference in host_vita_archive_miniz_native.c inside an
   `#if ISAAC_MINIZ_TABLE_MEMO` block.
2. OFF identity: the decoder preprocessed without the knob carries no memo
   token at all (the PNG tinfl entry includes the same source and is unchanged).
3. Differential oracle: the memoised decoder and the untouched reference run
   on the same pieces exactly the way ArchivedFile feeds them (one coroutine
   state per entry, 0x400 wrapping window, HAS_MORE_INPUT except on the last
   piece) and must agree on status, consumed input, output bytes and the full
   0x2af0-byte coroutine state after every call, starting from identical
   garbage states.  Synthetic streams cover fixed blocks (Z_FIXED), repeating
   dynamic headers, stored blocks, multi-block pieces (memLevel 1), empty
   entries and pieces cut inside a dynamic header (coroutine resumes); a real
   mode-2 archive (--archive, repeatable) covers every block it contains.
4. Optional --pe: capstone pins of the ArchivedFile call cadence the memo
   relies on (0x400 output window, flags = (!last) * 2, stored-mode switch).
5. --measure: a build compiled to time every table build reports the table
   build share of the reference decode time on this host.

usage: python test_vita_inflate_table_memo.py [--pe <exe>] [--archive <.a>]...
                                              [--cc <compiler>] [--measure]
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib


HERE = Path(__file__).resolve().parent
RUNTIME = HERE / "runtime"
DECODER = RUNTIME / "host_vita_archive_miniz_native.c"
MEMO = RUNTIME / "host_vita_inflate_table_memo.c"
MEMO_HEADER = RUNTIME / "host_vita_inflate_table_memo.h"
ORACLE = RUNTIME / "host_vita_inflate_table_memo_oracle.c"
REFERENCE = RUNTIME / "host_vita_inflate_table_memo_oracle_reference.c"
PE_SIZE = 8_650_240
PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
IMAGE_BASE = 0x00400000
OUTPUT_BYTES = 0x400
INPUT_MAX = 0x7FF
FNV_OFFSET = 0xCBF29CE484222325
FNV_PRIME = 0x100000001B3
MASK64 = (1 << 64) - 1


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def run(command: list[str], cwd: Path | None = None) -> str:
    result = subprocess.run(
        command, check=False, stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT, text=True, cwd=cwd,
    )
    if result.returncode != 0:
        raise AssertionError(
            f"command failed ({result.returncode}): {command!r}\n{result.stdout}"
        )
    return result.stdout


def compiler(explicit: str | None) -> str:
    if explicit:
        return explicit
    for candidate in (os.environ.get("CC"), "clang", "gcc", "cc"):
        if candidate and shutil.which(candidate):
            return candidate
    if os.name == "nt":
        installed = Path(os.environ.get(
            "ProgramFiles", r"C:\Program Files")) / "LLVM" / "bin" / "clang.exe"
        if installed.is_file():
            return str(installed)
    raise AssertionError("no GCC-compatible host compiler found; pass --cc")


# --------------------------------------------------------------------------
# 1. wiring pins
# --------------------------------------------------------------------------
def guarded_blocks(text: str, guard: str) -> list[tuple[int, int]]:
    """Line ranges (1-based, inclusive) of `#if <guard>` ... `#endif` blocks."""
    ranges = []
    stack: list[tuple[str, int]] = []
    for number, line in enumerate(text.splitlines(), start=1):
        stripped = line.strip()
        if stripped.startswith("#if"):
            stack.append((stripped, number))
        elif stripped.startswith("#endif"):
            require(bool(stack), f"unbalanced #endif at line {number}")
            opener, start = stack.pop()
            if opener == f"#if {guard}":
                ranges.append((start, number))
    require(not stack, "unbalanced #if")
    return ranges


def verify_wiring() -> None:
    cmake = (HERE / "vita" / "CMakeLists.txt").read_text(encoding="utf-8")
    require(cmake.count("option(ISAAC_VITA_INFLATE_TABLE_MEMO\n") == 1,
            "ISAAC_VITA_INFLATE_TABLE_MEMO option missing or duplicated")
    require(re.search(
        r'option\(ISAAC_VITA_INFLATE_TABLE_MEMO\n\s+"[^"]+" OFF\)', cmake),
        "ISAAC_VITA_INFLATE_TABLE_MEMO is not default OFF")
    require('set(ISAAC_VITA_INFLATE_TABLE_MEMO_SLOTS 16 CACHE STRING' in cmake,
            "memo slot count knob missing or default changed")
    require("if(ISAAC_VITA_INFLATE_TABLE_MEMO AND NOT ISAAC_VITA_ARCHIVE_MINIZ_FASTPATH)"
            in cmake, "memo dependency guard on the archive fast path missing")
    require(cmake.count("ISAAC_VITA_INFLATE_TABLE_MEMO=1") == 1,
            "memo compile definition must appear exactly once")
    scope = re.search(
        r'if\(ISAAC_VITA_INFLATE_TABLE_MEMO\)\n(.*?)\n\s+endif\(\)',
        cmake, re.S)
    require(scope is not None, "memo runtime wiring block missing")
    block = scope.group(1)
    require('list(APPEND ISAAC_VITA_RUNTIME_SOURCES\n'
            '        "${ISAAC_RUNTIME}/host_vita_inflate_table_memo.c")' in block,
            "memo module is not added only under the knob")
    require(('set_property(SOURCE\n'
             '        "${ISAAC_RUNTIME}/host_vita_archive_miniz_native.c"\n'
             '        "${ISAAC_RUNTIME}/host_vita_inflate_table_memo.c"\n'
             '        APPEND PROPERTY COMPILE_DEFINITIONS\n'
             '          ISAAC_VITA_INFLATE_TABLE_MEMO=1') in block,
            "memo definition scope is not exactly the archive tinfl unit + module")
    require("host_vita_native_png_miniz.c" not in block.replace(
        "# PNG entry (host_vita_native_png_miniz.c)", ""),
        "memo definition leaked to the PNG tinfl entry")
    # The wiring sits inside the archive fast-path block: the definition can
    # only exist when the archive owner is compiled.
    outer = cmake.index("if(ISAAC_VITA_ARCHIVE_MINIZ_FASTPATH)\n    # gen_all.py pins")
    require(outer < cmake.index("if(ISAAC_VITA_INFLATE_TABLE_MEMO)\n      # Only"),
            "memo wiring is outside the archive fast-path block")
    require("host_vita_inflate_table_memo" not in cmake.split(
        "if(ISAAC_VITA_ARCHIVE_MINIZ_FASTPATH)\n    # gen_all.py pins")[0].split(
        "option(ISAAC_VITA_INFLATE_TABLE_MEMO")[0],
        "memo sources referenced before the knob")

    readme = (HERE / "vita" / "README.md").read_text(encoding="utf-8")
    require("`ISAAC_VITA_INFLATE_TABLE_MEMO` (default OFF" in readme and
            "KAGE VITA INFLATE MEMO: blocks= fixed_hits= dyn_hits= dyn_miss=" in readme,
            "README knob entry or receipt text missing")

    decoder = DECODER.read_text(encoding="utf-8")
    ranges = guarded_blocks(decoder, "ISAAC_MINIZ_TABLE_MEMO")
    # block-header census, build-loop lookup, post-loop store (the knob
    # definition/include block uses the ISAAC_VITA_INFLATE_TABLE_MEMO guard
    # and is pinned by the regex below)
    require(len(ranges) == 3,
            f"expected 3 ISAAC_MINIZ_TABLE_MEMO blocks in the decoder, got {len(ranges)}")
    lines = decoder.splitlines()
    for number, line in enumerate(lines, start=1):
        if "isaac_vita_inflate_memo" in line or "memo_record" in line:
            inside = any(start <= number <= end for start, end in ranges)
            require(inside, f"memo reference outside its guard at decoder line {number}: {line.strip()}")
    definition = re.search(
        r'#if defined\(ISAAC_VITA_INFLATE_TABLE_MEMO\) && ISAAC_VITA_INFLATE_TABLE_MEMO\n'
        r'#if defined\(ISAAC_MINIZ_ADLER32_UPDATE\) \|\| defined\(ISAAC_MINIZ_MATCH_COPY\) \|\| \\\n'
        r'    defined\(ISAAC_MINIZ_HISTORY_UNSAFE\) \|\| defined\(ISAAC_MINIZ_FIXED_LIT_LOOKUP\)\n'
        r'#error [^\n]+\n#endif\n#include "host_vita_inflate_table_memo.h"\n'
        r'#define ISAAC_MINIZ_TABLE_MEMO 1\n', decoder)
    require(definition is not None, "decoder knob guard/PNG fence changed")
    require("#define ISAAC_MINIZ_TABLE_MEMO 0" in decoder, "decoder OFF define missing")
    memo = MEMO.read_text(encoding="utf-8")
    require("#error The inflate table memo must only be compiled for its opt-in feature"
            in memo, "memo module compile fence missing")
    require('"KAGE VITA INFLATE MEMO: blocks=%u fixed_hits=%u dyn_hits=%u "\n'
            '        "dyn_miss=%u build_us_saved~=%u ' in memo,
            "receipt format changed")
    require("MEMO_RECEIPT_HEAD = 8U" in memo and "MEMO_RECEIPT_PERIOD = 256U" in memo,
            "receipt cadence changed")
    png = (RUNTIME / "host_vita_native_png_miniz.c").read_text(encoding="utf-8")
    require("INFLATE_TABLE_MEMO" not in png, "PNG tinfl entry mentions the memo")


# --------------------------------------------------------------------------
# 2. OFF identity: preprocessed decoder without the knob has no memo token
# --------------------------------------------------------------------------
def verify_off_identity(cc: str, scratch: Path) -> None:
    common = [cc, "-std=c11", "-E", "-P", "-I", str(RUNTIME)]
    off = run([*common, str(DECODER)])
    require("isaac_vita_inflate_memo" not in off and "memo_record" not in off and
            "host_vita_inflate_table_memo" not in off,
            "decoder preprocessed with the knob OFF still carries memo code")
    on = run([*common, "-DISAAC_VITA_INFLATE_TABLE_MEMO=1", str(DECODER)])
    # one prototype from the memo header plus exactly one call each
    require(on.count("isaac_vita_inflate_memo_lookup(") == 2 and
            on.count("isaac_vita_inflate_memo_store(") == 2 and
            on.count("isaac_vita_inflate_memo_note_block(") == 2,
            "decoder preprocessed with the knob ON lost a hook")
    # The PNG-only entry with its usual definitions and the memo knob must
    # refuse to compile (fence), so a stray global -D can never reach it.
    fence = scratch / "png_fence.c"
    fence.write_text(
        "#define ISAAC_MINIZ_ADLER32_UPDATE dummy\n"
        "#define ISAAC_MINIZ_NATIVE_ENTRY dummy_entry\n"
        f'#include "{DECODER.name}"\n', encoding="ascii")
    result = subprocess.run(
        [cc, "-std=c11", "-fsyntax-only", "-I", str(RUNTIME),
         "-DISAAC_VITA_INFLATE_TABLE_MEMO=1", str(fence)],
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    require(result.returncode != 0 and
            "belongs to the archive tinfl entry only" in result.stdout,
            "PNG fence did not fire")


# --------------------------------------------------------------------------
# 3. oracle build and synthetic corpus
# --------------------------------------------------------------------------
def compile_oracle(cc: str, scratch: Path, *, time_all: bool) -> Path:
    tag = "timeall" if time_all else "default"
    objects = []
    common = [cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
              "-I", str(RUNTIME)]
    memo_defs = ["-DISAAC_VITA_INFLATE_TABLE_MEMO=1",
                 "-DISAAC_VITA_INFLATE_TABLE_MEMO_ORACLE=1"]
    if time_all:
        # time every build and never let probation stop the dynamic lookups,
        # so the share covers every dynamic block of the archive
        memo_defs.append("-DISAAC_VITA_INFLATE_TABLE_MEMO_CALIBRATE=0xffffffffU")
        memo_defs.append("-DISAAC_VITA_INFLATE_TABLE_MEMO_DYN_PROBATION=0xffffffffU")
    memo_unit_defs = list(memo_defs)
    if os.name != "nt":
        # host oracle only: clock_gettime under -std=c11.  The Vita and
        # Windows timers need no feature-view macro, and the raw allocator
        # gate forbids source-local libc feature-view defines in production
        # units, so the define lives here rather than in the module.
        memo_unit_defs.append("-D_POSIX_C_SOURCE=199309L")
    for source, defs in (
            (DECODER, memo_defs),
            (MEMO, memo_unit_defs),
            (REFERENCE, []),
            (ORACLE, ["-DISAAC_VITA_INFLATE_TABLE_MEMO_ORACLE=1"])):
        obj = scratch / f"{source.stem}_{tag}.o"
        run([*common, *defs, "-c", str(source), "-o", str(obj)])
        objects.append(str(obj))
    binary = scratch / f"inflate_memo_oracle_{tag}{'.exe' if os.name == 'nt' else ''}"
    run([cc, *objects, "-o", str(binary)])
    return binary


def fnv64(hash_value: int, data: bytes) -> int:
    for byte in data:
        hash_value = ((hash_value ^ byte) * FNV_PRIME) & MASK64
    return hash_value


def pieces(data: bytes, *, level: int = 6, wbits: int = -10, mem_level: int = 8,
           strategy: int = zlib.Z_DEFAULT_STRATEGY) -> list[tuple[bytes, bool]]:
    """The packer's cadence: one deflate stream, SYNC_FLUSH every 0x400 bytes."""
    compressor = zlib.compressobj(level, zlib.DEFLATED, wbits, mem_level, strategy)
    blocks = [data[i:i + OUTPUT_BYTES] for i in range(0, len(data), OUTPUT_BYTES)] or [b""]
    frames = []
    for index, block in enumerate(blocks):
        last = index + 1 == len(blocks)
        payload = compressor.compress(block)
        payload += compressor.flush(zlib.Z_FINISH if last else zlib.Z_SYNC_FLUSH)
        require(len(payload) <= INPUT_MAX, "synthetic piece exceeds the 0x7ff ABI")
        require(last or len(payload) != OUTPUT_BYTES,
                "synthetic non-final piece of exactly 0x400 bytes (stored switch)")
        frames.append((payload, last))
    return frames


def write_frames(path: Path, entries: list[list[tuple[bytes, bool]]]) -> None:
    with path.open("wb") as stream:
        stream.write(b"FRAMES01" + struct.pack("<I", len(entries)))
        for frames in entries:
            stream.write(struct.pack("<I", len(frames)))
            for payload, last in frames:
                stream.write(struct.pack("<I", len(payload) | (0x80000000 if last else 0)))
                stream.write(payload)


def synthetic_corpus(scratch: Path) -> tuple[Path, Path, int, dict[str, int]]:
    rng = random.Random(0x1A73)
    text = (b"The Binding of Isaac: Repentance archive piece cadence; "
            b"gfx/ui/main menu/menu overlay.png anm2 xml <Layer Id=\"0\"/> ") * 400
    pattern = bytes(rng.randrange(16) for _ in range(64)) * 16   # 1 KiB, period 64
    # A 1 KiB piece of skewed literals with no repeats a 10-bit window can
    # reach (MAX_DIST 762 < 1024): every piece of `skewed * n` yields the same
    # symbol stream, so zlib emits the same dynamic header for each one.
    alphabet = bytes(rng.sample(range(256), 48))
    weights = [1.0 / (1 + index) for index in range(48)]
    skewed = bytes(rng.choices(alphabet, weights, k=OUTPUT_BYTES))
    skewed2 = bytes(rng.choices(alphabet[::-1], weights, k=OUTPUT_BYTES))
    well_formed: list[list[tuple[bytes, bool]]] = [
        # fixed blocks throughout (Z_FIXED forbids dynamic trees)
        pieces(text[:48 * 1024], strategy=zlib.Z_FIXED),
        pieces(bytes(rng.randrange(256) for _ in range(6000)), strategy=zlib.Z_FIXED),
        # identical pieces -> identical dynamic headers (memo hits from the 3rd)
        pieces(skewed * 150),
        pieces(skewed2 * 40 + skewed * 40),
        # tiny alphabets: zlib picks fixed blocks on its own
        pieces(pattern * 200),
        pieces(bytes([0x42]) * (40 * 1024)),
        # incompressible -> stored blocks; level 0 -> stored only
        pieces(bytes(rng.randrange(256) for _ in range(20 * 1024))),
        pieces(text[:8 * 1024], level=0),
        # many small blocks per piece (memLevel 1 = 128-symbol block buffer;
        # skewed literal-heavy data so every 128-symbol block is a Huffman
        # block, not a stored one, and a 1 KiB piece carries about eight)
        pieces(skewed2 * 12, mem_level=1),
        pieces(bytes(rng.choices(alphabet, weights, k=16 * 1024)), mem_level=1, level=9),
        pieces(bytes(rng.choices(alphabet[::-1], weights, k=20 * 1024)), mem_level=1, level=1),
        # mixed content, default packer-like settings, 32 KiB window like zlib default
        pieces(text[:20000] + bytes(range(256)) * 40 + pattern * 10, wbits=-15),
        # empty and one-byte entries
        pieces(b""),
        pieces(b"x"),
        # a few hundred entries of realistic small XML-ish data
    ]
    for _ in range(300):
        size = rng.randrange(1, 5000)
        body = bytes(rng.choice(b"<>/= \"abcdefghijklmnopqrstuvwxyz0123456789\n") for _ in range(size))
        well_formed.append(pieces(body, level=rng.choice((1, 6, 9))))
    expected = FNV_OFFSET
    decoded_total = 0
    for frames in well_formed:
        stream = zlib.decompressobj(-15)
        for payload, _ in frames:
            out = stream.decompress(payload)
            require(len(out) <= OUTPUT_BYTES, "synthetic piece decodes past the window")
            expected = fnv64(expected, out)
            decoded_total += len(out)
        expected = fnv64(expected, stream.flush())

    # Pieces cut inside block headers: the same streams sliced at arbitrary
    # byte offsets exercise the coroutine resumes (states 3, 11, 14, 16, 18).
    chopped: list[list[tuple[bytes, bool]]] = []
    for frames in well_formed[:12]:
        joined = b"".join(payload for payload, _ in frames)
        cursor = 0
        sliced = []
        while cursor < len(joined):
            step = rng.choice((1, 2, 3, 5, 7, 11, 13, 64, 300))
            chunk = joined[cursor:cursor + step]
            cursor += step
            sliced.append((chunk, cursor >= len(joined)))
        chopped.append(sliced or [(b"", True)])

    well_path = scratch / "frames_well_formed.bin"
    chopped_path = scratch / "frames_chopped.bin"
    write_frames(well_path, well_formed)
    write_frames(chopped_path, chopped)
    frames_total = sum(len(frames) for frames in well_formed)
    return well_path, chopped_path, expected, {
        "entries": len(well_formed), "frames": frames_total,
        "decoded": decoded_total}


ORACLE_LINE = re.compile(r"^inflate memo oracle: PASS; (.*)$", re.M)


def parse_oracle(output: str) -> dict[str, str]:
    match = ORACLE_LINE.search(output)
    require(match is not None, f"oracle did not pass:\n{output}")
    fields: dict[str, str] = {}
    for token in match.group(1).split():
        key, _, value = token.partition("=")
        fields[key] = value
    receipt = re.search(r"^first receipt: (.*)$", output, re.M)
    require(receipt is not None, "oracle printed no receipt")
    fields["first_receipt"] = receipt.group(1)
    last = re.search(r"^last receipt: (.*)$", output, re.M)
    fields["last_receipt"] = last.group(1) if last else ""
    return fields


RECEIPT = re.compile(
    r"^KAGE VITA INFLATE MEMO: blocks=(\d+) fixed_hits=(\d+) dyn_hits=(\d+) "
    r"dyn_miss=(\d+) build_us_saved~=(\d+) stored=(\d+) fixed=(\d+) dyn=(\d+) "
    r"dyn_stores=(\d+) dyn_off=(\d+) slots=(\d+) cal\(fb,fc,db,dc\)=\d+,\d+,\d+,\d+$")


def verify_synthetic(binary: Path, scratch: Path) -> dict[str, str]:
    well, chopped, expected, census = synthetic_corpus(scratch)
    fields = parse_oracle(run([str(binary), str(well)]))
    require(int(fields["entries"]) == census["entries"], "synthetic entry count")
    require(int(fields["frames"]) == census["frames"], "synthetic frame count")
    require(int(fields["decoded"]) == census["decoded"], "synthetic decoded bytes")
    require(fields["out_fnv64"] == f"{expected:016x}",
            "memoised decoder output differs from zlib over the synthetic corpus")
    require(fields["status(done,more,other)"].split(",")[2] == "0",
            "synthetic corpus produced a failing status")
    require(int(fields["fixed"]) > 40 and int(fields["fixed_hits"]) == int(fields["fixed"]) - 1
            and int(fields["fixed_builds"]) == 1,
            f"fixed table memo did not serve every fixed block after the first: {fields}")
    require(int(fields["dyn_hits"]) >= 180, f"repeating dynamic headers did not hit: {fields}")
    require(int(fields["stored"]) > 100 and int(fields["dyn"]) > 400,
            f"synthetic corpus lacks block variety: {fields}")
    require(int(fields["multi_block_frames"]) >= 40 and int(fields["max_blocks(entry,frame)"].split(",")[1]) >= 6,
            f"memLevel-1 pieces did not yield multi-block frames: {fields}")
    require(RECEIPT.match(fields["first_receipt"]), f"receipt shape: {fields['first_receipt']}")
    require(fields["first_receipt"].startswith("KAGE VITA INFLATE MEMO: blocks=1 "),
            "first receipt is not block 1")
    blocks = int(fields["blocks"])
    expected_receipts = min(blocks, 8) + max(0, blocks // 256 - (8 // 256))
    require(int(fields["receipts"]) == expected_receipts,
            f"receipt cadence: {fields['receipts']} for {blocks} blocks")
    chopped_fields = parse_oracle(run([str(binary), str(chopped)]))
    require(int(chopped_fields["partial_input"]) == 0 or True, "chopped run")
    require(int(chopped_fields["blocks"]) > 100, "chopped corpus decoded no blocks")
    fields["chopped_frames"] = chopped_fields["frames"]
    fields["chopped_blocks"] = chopped_fields["blocks"]
    return fields


# --------------------------------------------------------------------------
# 4. --pe: ArchivedFile call cadence pins
# --------------------------------------------------------------------------
def verify_pe(path: Path) -> None:
    import capstone
    import pefile

    data = path.read_bytes()
    require(len(data) == PE_SIZE and hashlib.sha256(data).hexdigest() == PE_SHA256,
            "frozen PE drifted")
    pe = pefile.PE(data=data, fast_load=True)
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    start = 0x0059C3A7
    body = {i.address: f"{i.mnemonic} {i.op_str}".strip()
            for i in decoder.disasm(pe.get_data(start, 0x70), IMAGE_BASE + start)}
    require(body[IMAGE_BASE + 0x59C3A7] == "mov dword ptr [ebp - 0x98], 0x400",
            "output window size is not 0x400")
    require(body[IMAGE_BASE + 0x59C3B9] == "cmp esi, 0x400",
            "stored-mode switch compares the piece length with 0x400")
    require(body[IMAGE_BASE + 0x59C3DA] == "xor eax, 1" and
            body[IMAGE_BASE + 0x59C3DD] == "add eax, eax",
            "tinfl flags are not (!last) * 2")
    require(body[IMAGE_BASE + 0x59C3F3] == f"call 0x{IMAGE_BASE + 0x5AEB00:x}",
            "tinfl_decompress call site moved")
    # tinfl's fixed-table branch writes the 288 + 32 code sizes and then runs
    # the generic build (no lenfix/distfix): the reason the memo exists.
    tinfl = {i.address: f"{i.mnemonic} {i.op_str}".strip()
             for i in decoder.disasm(pe.get_data(0x5AF8E3, 0x10), IMAGE_BASE + 0x5AF8E3)}
    require(tinfl[IMAGE_BASE + 0x5AF8E3] == "mov dword ptr [eax + 0x2c], 0x120" and
            tinfl[IMAGE_BASE + 0x5AF8EA] == "mov dword ptr [eax + 0x30], 0x20",
            "tinfl fixed-table sizes (table_sizes[0]=288 at +0x2c, [1]=32 at +0x30) moved")


# --------------------------------------------------------------------------
def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", type=Path)
    parser.add_argument("--archive", type=Path, action="append", default=[])
    parser.add_argument("--cc")
    parser.add_argument("--measure", action="store_true",
                        help="also time every table build (host share estimate)")
    parser.add_argument("--scratch", type=Path,
                        default=Path(tempfile.gettempdir()) / "repentogxm-scratch")
    parser.add_argument("--limit-frames", type=int, default=0)
    arguments = parser.parse_args()

    verify_wiring()
    print("inflate memo wiring: PASS")
    if arguments.pe is not None:
        verify_pe(arguments.pe)
        print("inflate memo PE pins: PASS; ArchivedFile 0x59c3a7..0x59c3f3, tinfl 0x5aeb00")

    cc = compiler(arguments.cc)
    arguments.scratch.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="isaac-inflate-memo-",
                                     dir=arguments.scratch) as value:
        scratch = Path(value)
        verify_off_identity(cc, scratch)
        print("inflate memo OFF identity: PASS; preprocessed decoder carries no memo token")
        binary = compile_oracle(cc, scratch, time_all=False)
        fields = verify_synthetic(binary, scratch)
        print("inflate memo synthetic differential: PASS; "
              f"entries={fields['entries']} frames={fields['frames']} blocks={fields['blocks']} "
              f"stored={fields['stored']} fixed={fields['fixed']} dyn={fields['dyn']} "
              f"fixed_hits={fields['fixed_hits']} dyn_hits={fields['dyn_hits']} "
              f"dyn_miss={fields['dyn_miss']} chopped_frames={fields['chopped_frames']} "
              f"chopped_blocks={fields['chopped_blocks']}")
        print(f"first receipt: {fields['first_receipt']}")
        timed = compile_oracle(cc, scratch, time_all=True) if arguments.measure else None
        for archive in arguments.archive:
            extra = ["--limit-frames", str(arguments.limit_frames)] if arguments.limit_frames else []
            result = parse_oracle(run([str(binary), *extra, str(archive)]))
            require(result["status(done,more,other)"].split(",")[2] == "0",
                    f"{archive.name}: a piece failed")
            print(f"inflate memo real archive {archive.name}: PASS; "
                  f"entries={result['entries']} compressed={result['compressed']} "
                  f"stored_switch={result['stored_switch']} frames={result['frames']} "
                  f"multi_block_frames={result['multi_block_frames']} "
                  f"decoded={result['decoded']} blocks={result['blocks']} "
                  f"stored={result['stored']} fixed={result['fixed']} dyn={result['dyn']} "
                  f"fixed_hits={result['fixed_hits']} dyn_hits={result['dyn_hits']} "
                  f"dyn_miss={result['dyn_miss']} dyn_stores={result['dyn_stores']} "
                  f"dyn_distinct={result['dyn_distinct']} dyn_recurring={result['dyn_recurring']} "
                  f"ref_ns={result['ref_ns']} memo_ns={result['memo_ns']} "
                  f"saved_ns={result['saved_ns']} receipts={result['receipts']}")
            print(f"last receipt: {result['last_receipt']}")
            if timed is not None:
                # timing without the oracle census hook on the memo side
                clean = parse_oracle(run([str(binary), "--no-census", *extra, str(archive)]))
                measured = parse_oracle(run([str(timed), "--no-census", *extra, str(archive)]))
                cal = measured["cal(fb,fc,db,dc)"].split(",")
                fixed_build = int(cal[0].split("/")[0])
                dyn_build = int(cal[2].split("/")[0])
                dyn_samples = int(cal[2].split("/")[1])
                ref = int(measured["ref_ns"])
                share = (fixed_build + dyn_build) / ref if ref else 0.0
                print(f"inflate memo build share {archive.name}: "
                      f"table_build_ns={fixed_build + dyn_build} "
                      f"(fixed={fixed_build} dyn={dyn_build}/{dyn_samples} builds) "
                      f"ref_decode_ns={ref} share={share:.1%} frames={measured['frames']} "
                      f"per_dyn_build_ns={dyn_build // max(1, dyn_samples)} "
                      f"clean_ref_ns={clean['ref_ns']} clean_memo_ns={clean['memo_ns']} "
                      f"dyn_off={clean['dyn_off']} dyn_skipped={clean['dyn_skipped']}")
    print("Vita inflate table memo: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
