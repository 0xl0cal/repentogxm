#!/usr/bin/env python3
"""Host gate for the PS Vita trophy runtime (ISAAC_VITA_TROPHIES).

1. Oracle: recomp/runtime/host_vita_trophies_oracle.c + host_vita_trophies.c
   built with clang and with gcc (WSL gcc on Windows), each with and without
   ISAAC_VITA_TROPHY_TEST_REQUEST, `-std=c11 -O2 -Wall -Wextra -Werror
   -Wundef`; every run must print `host_vita_trophies_oracle: PASS`.
2. Mapping: the oracle's `--dump-map` output must hash to the canonical
   SHA-256 of the mapping dump (recomp/vita/TROPHIES.md, "Mapping")
   and equal a Python implementation of the rule byte for byte.
3. Save checksum: synthetic saves built here with a Python copy of the
   game's save checksum (TROPHIES.md, "Manager contract") go through the C
   import path (`--save`); status, counts and checksum must agree.
4. Fences: the TU refuses to build without the knob or at a foreign
   GUEST_IMAGE_BASE, the header declares nothing without the knob, the
   observer prototype equals the one gen_all.py renders into the seam, the
   exact log formats (TROPHIES.md, "Log lines") are in the TU.
5. Optional ARM leg: with $VITASDK/bin/arm-vita-eabi-gcc the TU is compiled
   with the production flags; with --vitasdk-include DIR (vita-headers
   include/) clang checks the Vita path for armv7a.  Otherwise reported as
   skipped, never as passed.

--real-saves FILE...: not part of the default run; checks the C checksum and
import counts against the Python reference on real save files and prints
only counts and ok flags.
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import struct
import subprocess
import sys
import tempfile


HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
RUNTIME = HERE / "runtime"
VITA = HERE / "vita"
SOURCE = RUNTIME / "host_vita_trophies.c"
HEADER = RUNTIME / "host_vita_trophies.h"
ORACLE = RUNTIME / "host_vita_trophies_oracle.c"
CMAKE = VITA / "CMakeLists.txt"
GEN_ALL = HERE / "gen_all.py"

KNOB = "ISAAC_VITA_TROPHIES"
ORACLE_KNOB = "ISAAC_VITA_TROPHIES_ORACLE"
TEST_KNOB = "ISAAC_VITA_TROPHY_TEST_REQUEST"
PASS_LINE = "host_vita_trophies_oracle: PASS"
WSL_DISTRO = "Ubuntu-22.04"
LLVM_CLANG = Path("C:/Program Files/LLVM/bin/clang.exe")

# recomp/vita/TROPHIES.md ("Mapping") quotes the same value; a trophy pack
# must number its trophies by the same rule.
MAP_SHA256 = "f3b12afbc0acdbcb0fc930fee8ddd69d7bf9d96b588d04c092e16f94e07e7233"
MAP_BYTES = 6256
SETS = ((1, 89), (90, 178), (179, 276), (277, 403), (404, 520), (521, 637))
EXCLUDED = (325, 336, 354)

# Save layout and checksum of the game's save file, as the import path reads
# it (TROPHIES.md, "Manager contract").
SAVE_MAGIC = b"ISAACNGSAVE09R  "
POLY = 0xEDB88320
INIT = 0xFEDCBA76
ACH_SLOTS = 0x27E
SAVE_MAX = 1024 * 1024

# TROPHIES.md, "Log lines" (exact formats; the TU also has the module-failure
# spelling of the boot line, init=-, from the failure-policy table).
LOG_FORMATS = (
    "KAGE VITA TROPHY: boot module=0x%08x init=0x%08x title=ISAACR001 sets=6",
    "KAGE VITA TROPHY: boot module=0x%08x init=- title=ISAACR001 sets=6",
    "KAGE VITA TROPHY: ctx set=%u comm=ISAC00000_%02u rc=0x%08x ctx=%d",
    "KAGE VITA TROPHY: worker rc=0x%08x prio=%u stack=65536 affinity=0x%08x",
    "KAGE VITA TROPHY: disabled stage=%s rc=0x%08x",
    "KAGE VITA TROPHY: setup set=%u status=%s init=0x%08x result=%d "
    "frames=%u us=%u",
    "KAGE VITA TROPHY: setup done live=%u/%u dialogs=%u timeouts=%u us=%u",
    "KAGE VITA TROPHY: destroy set=%u rc=0x%08x reason=%s",
    "KAGE VITA TROPHY: deliver set=%u status=%s left=%u us=%u",
    "KAGE VITA TROPHY: pending id=%u set=%u trophy=%03u",
    "KAGE VITA TROPHY: state set=%u rc=0x%08x count=%d bits=%08x%08x%08x%08x",
    "KAGE VITA TROPHY: event id=%u ra=0x%08x",
    "KAGE VITA TROPHY: readonly id=%u ra=0x%08x",
    "KAGE VITA TROPHY: pgd-mismatch pgd=0x%08x mgr=0x%08x id=%u ra=0x%08x "
    "n=%u",
    "KAGE VITA TROPHY: id-range id=%u ra=0x%08x",
    "KAGE VITA TROPHY: unlock src=%s id=%u set=%u trophy=%03u rc=0x%08x "
    "plat=%d us=%u",
    "KAGE VITA TROPHY: skip src=%s id=%u reason=%s",
    "KAGE VITA TROPHY: set-dead set=%u rc=0x%08x",
    "KAGE VITA TROPHY: platinum set=%u trophy=%d",
    "KAGE VITA TROPHY IMPORT: slot=%u status=%s saved=%u mapped=%u "
    "unlocked=%u already=%u failed=%u noset=%u request=%s",
    "KAGE VITA TROPHY TEST: queued ids=%u",
    "KAGE VITA TROPHY TEST: done ids=%u unlocked=%u already=%u failed=%u",
    "KAGE VITA TROPHY: pending load status=%s ids=%u known=0x%02x",
    "KAGE VITA TROPHY: pending save ids=%u status=%s rc=0x%08x",
    "KAGE VITA TROPHY: pair a=%d b=%d rule=%s saves=%u known=0x%02x "
    "locked=%u,%u,%u,%u,%u,%u",
)
IMPORT_STATUSES = ("done", "partial", "bad-request", "no-save", "read-error",
                   "too-large", "bad-magic", "bad-checksum", "bad-chunk",
                   "unavailable")


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


# ---- reference implementations ------------------------------------------------

def canonical_map() -> bytes:
    lines = []
    for achievement in range(1, 638):
        if achievement in EXCLUDED:
            lines.append("%u - -\n" % achievement)
            continue
        for number, (first, last) in enumerate(SETS):
            if first <= achievement <= last:
                trophy = achievement - first + 1 - sum(
                    1 for e in EXCLUDED if first <= e < achievement)
                lines.append("%u %u %03u\n" % (achievement, number, trophy))
                break
    return "".join(lines).encode("ascii")


def _sar1(value: int) -> int:
    value &= 0xFFFFFFFF
    return ((value >> 1) | (value & 0x80000000)) & 0xFFFFFFFF


def _build_table() -> list[int]:
    table = []
    for i in range(256):
        v = ((i >> 1) ^ (POLY if (i & 1) else 0)) & 0xFFFFFFFF
        for _ in range(7):
            v = (_sar1(v) ^ (POLY if (v & 1) else 0)) & 0xFFFFFFFF
        table.append(v)
    return table


TABLE = _build_table()


def checksum(data: bytes, acc: int = INIT) -> int:
    ebx = (~acc) & 0xFFFFFFFF
    for b in data:
        ebx = ((ebx >> 8) ^ TABLE[(b ^ ebx) & 0xFF]) & 0xFFFFFFFF
    return (~ebx) & 0xFFFFFFFF


def reference_import(data: bytes) -> tuple[str, int, int]:
    """(status, saved, mapped) by the rules in TROPHIES.md, "Manager contract"."""
    size = len(data)
    if size > SAVE_MAX:
        return "too-large", 0, 0
    if size < 16 or data[:16] != SAVE_MAGIC:
        return "bad-magic", 0, 0
    if size < 0x14 or checksum(data[0x10:size - 4]) != struct.unpack_from(
            "<I", data, size - 4)[0]:
        return "bad-checksum", 0, 0
    if size < 0x24 or struct.unpack_from("<I", data, 0x14)[0] != 1:
        return "bad-chunk", 0, 0
    data_bytes, count = struct.unpack_from("<II", data, 0x18)
    count = min(count, ACH_SLOTS)
    if size < 0x20 + count + 4:
        return "bad-chunk", 0, 0
    saved = [i for i in range(1, min(count, data_bytes))
             if (data[0x20 + i] ^ 0x80) - 0x80 > 0]
    mapped = [i for i in saved if i not in EXCLUDED]
    return "ok", len(saved), len(mapped)


def synthetic_save(values: dict[int, int], chunk_id: int = 1,
                   count: int = ACH_SLOTS) -> bytes:
    body = bytearray(SAVE_MAGIC)
    body += struct.pack("<I", 0x00012345)
    body += struct.pack("<III", chunk_id, ACH_SLOTS, count)
    slots = bytearray(ACH_SLOTS)
    for index, value in values.items():
        slots[index] = value
    body += slots
    body += struct.pack("<III", 2, 0x7C0, 0x1F0) + bytes(range(40))
    body += struct.pack("<I", checksum(bytes(body[0x10:])))
    return bytes(body)


# ---- compilers -------------------------------------------------------------------

class Compiler:
    """One host compiler leg: compile + run, paths translated for WSL."""

    def __init__(self, name: str, argv: list[str], wsl: bool):
        self.name = name
        self.argv = argv
        self.wsl = wsl

    def path(self, value: Path | str) -> str:
        text = str(value)
        if not self.wsl:
            return text
        resolved = str(Path(text).resolve())
        drive, rest = resolved[0].lower(), resolved[2:].replace("\\", "/")
        return "/mnt/%s%s" % (drive, rest)

    def run(self, args: list[str], check: bool = True) -> subprocess.CompletedProcess:
        command = (["wsl", "-d", WSL_DISTRO, "--"] if self.wsl else []) + args
        result = subprocess.run(command, capture_output=True, text=True)
        if check and result.returncode != 0:
            raise AssertionError("command failed (%d): %s\n%s%s" % (
                result.returncode, " ".join(command), result.stdout,
                result.stderr))
        return result

    def compile(self, flags: list[str], sources: list[Path], output: str,
                check: bool = True) -> subprocess.CompletedProcess:
        return self.run(self.argv + flags + [self.path(s) for s in sources] +
                        ["-o", output], check=check)

    def syntax(self, flags: list[str], source: Path,
               check: bool = True) -> subprocess.CompletedProcess:
        return self.run(self.argv + flags + ["-fsyntax-only",
                                             self.path(source)], check=check)


def discover_compilers(names: list[str] | None) -> list[Compiler]:
    found: list[Compiler] = []
    wanted = names or ["clang", "gcc"]
    for name in wanted:
        if name == "clang":
            clang = shutil.which("clang")
            if clang is None and LLVM_CLANG.is_file():
                clang = str(LLVM_CLANG)
            if clang is not None:
                found.append(Compiler("clang", [clang], False))
        elif name == "gcc":
            if os.name == "nt":
                probe = subprocess.run(["wsl", "-d", WSL_DISTRO, "--", "gcc",
                                        "--version"], capture_output=True,
                                       text=True)
                if probe.returncode == 0:
                    found.append(Compiler("gcc(wsl)", ["gcc"], True))
            elif shutil.which("gcc") is not None:
                found.append(Compiler("gcc", ["gcc"], False))
        else:
            raise AssertionError("unknown compiler %r" % name)
    require(found, "no host compiler found (clang or gcc)")
    return found


STRICT = ["-std=c11", "-Wall", "-Wextra", "-Werror", "-Wundef"]


def includes(cc: Compiler) -> list[str]:
    return ["-I" + cc.path(RUNTIME), "-I" + cc.path(VITA)]


# ---- 1. oracle ------------------------------------------------------------------

def build_oracle(cc: Compiler, work: Path, test_request: bool) -> str:
    variant = "t" if test_request else "n"
    if cc.wsl:
        exe = "/tmp/isaac-trophies-%d-%s" % (os.getpid(), variant)
    else:
        exe = str(work / ("oracle-%s-%s%s" % (cc.name, variant,
                                              ".exe" if os.name == "nt" else "")))
    flags = STRICT + ["-O2", "-D%s=1" % KNOB, "-D%s=1" % ORACLE_KNOB] + (
        ["-D%s=1" % TEST_KNOB] if test_request else []) + includes(cc)
    cc.compile(flags, [SOURCE, ORACLE], exe)
    return exe


def run_oracles(compilers: list[Compiler], work: Path) -> dict[str, str]:
    """Builds and runs every leg; returns compiler name -> no-test-request
    executable (for the map and save legs)."""
    executables = {}
    for cc in compilers:
        for test_request in (True, False):
            exe = build_oracle(cc, work, test_request)
            out = cc.run([exe]).stdout
            lines = out.strip().splitlines()
            require(lines and lines[-1] == PASS_LINE,
                    "%s oracle did not pass:\n%s" % (cc.name, out))
            variant = "test-request" if test_request else "no-test-request"
            require("variant=%s" % variant in out,
                    "%s oracle ran the wrong variant" % cc.name)
            print("oracle %s %s: %s" % (cc.name, variant, lines[-2].split(
                ": ", 1)[1]))
            if not test_request:
                if not executables:
                    # Stage B time modelled with the device's measured costs
                    for line in lines:
                        if line.startswith("host_vita_trophies_oracle: model "):
                            print("oracle %s" % line.split(": ", 1)[1])
                executables[cc.name] = exe
    return executables


# ---- 2. mapping -----------------------------------------------------------------

def verify_map(compilers: list[Compiler], executables: dict[str, str],
               work: Path) -> None:
    reference = canonical_map()
    require(len(reference) == MAP_BYTES and
            hashlib.sha256(reference).hexdigest() == MAP_SHA256,
            "Python mapping rule drifted from the shared hash")
    for cc in compilers:
        target = work / ("map-%s.txt" % cc.name.replace("(", "-").rstrip(")"))
        cc.run([executables[cc.name], "--dump-map", cc.path(target)])
        dump = target.read_bytes()
        require(dump == reference, "%s --dump-map differs from the Python "
                "rule" % cc.name)
        require(hashlib.sha256(dump).hexdigest() == MAP_SHA256,
                "%s --dump-map hash" % cc.name)
    print("map: %d bytes sha256 %s == Python rule == pinned (%s)" % (
        MAP_BYTES, MAP_SHA256, ", ".join(cc.name for cc in compilers)))


# ---- 3. saves -------------------------------------------------------------------

_SAVE_LINE = re.compile(
    r"^save (?P<path>.*): size=(?P<size>\d+) crc=0x(?P<crc>[0-9a-f]{8}) "
    r"stored=0x(?P<stored>[0-9a-f]{8}) KAGE VITA TROPHY IMPORT: slot=1 "
    r"status=(?P<status>[a-z-]+) saved=(?P<saved>\d+) mapped=(?P<mapped>\d+) "
    r"unlocked=(?P<unlocked>\d+) already=(?P<already>\d+) "
    r"failed=(?P<failed>\d+) noset=(?P<noset>\d+) "
    r"request=(?P<request>deleted|kept)$")


def run_saves(cc: Compiler, exe: str, paths: list[Path]) -> list[dict]:
    result = cc.run([exe, "--save"] + [cc.path(p) for p in paths], check=False)
    rows = []
    for line in result.stdout.splitlines():
        match = _SAVE_LINE.match(line)
        require(match is not None, "%s: unexpected --save line: %r" %
                (cc.name, line))
        rows.append(match.groupdict())
    require(len(rows) == len(paths), "%s: --save printed %d of %d rows" %
            (cc.name, len(rows), len(paths)))
    return rows


def compare_save(row: dict, data: bytes, label: str) -> tuple[str, int, int]:
    status, saved, mapped = reference_import(data)
    if len(data) >= 0x14:
        require(int(row["crc"], 16) == checksum(data[0x10:len(data) - 4]),
                "%s: C checksum differs from the Python reference" % label)
        require(int(row["stored"], 16) == struct.unpack_from(
            "<I", data, len(data) - 4)[0], "%s: stored checksum" % label)
    expected = "done" if status == "ok" else status
    require(row["status"] == expected, "%s: status %s, reference %s" %
            (label, row["status"], expected))
    require(int(row["saved"]) == saved and int(row["mapped"]) == mapped,
            "%s: counts %s/%s, reference %d/%d" %
            (label, row["saved"], row["mapped"], saved, mapped))
    if status == "ok":
        require(int(row["unlocked"]) == mapped and row["request"] == "deleted",
                "%s: import result" % label)
    return expected, saved, mapped


def verify_synthetic_saves(compilers: list[Compiler],
                           executables: dict[str, str], work: Path) -> None:
    cases = {
        "standard": synthetic_save({0: 1, 1: 1, 2: 0x7F, 3: 0x80, 4: 0xFF,
                                    31: 1, 325: 1, 336: 1, 354: 1, 637: 1}),
        "all": synthetic_save({i: 1 for i in range(ACH_SLOTS)}),
        "count-clamp": synthetic_save({10: 1, 600: 1}, count=0xFFFFFFFF),
        "chunk2": synthetic_save({1: 1}, chunk_id=2),
    }
    corrupt = bytearray(cases["standard"])
    corrupt[0x40] ^= 1
    cases["corrupt"] = bytes(corrupt)
    cases["magic08"] = cases["standard"].replace(b"09R", b"08R", 1)
    expected = {"standard": ("done", 7, 4), "all": ("done", 637, 634),
                "count-clamp": ("done", 2, 2), "chunk2": ("bad-chunk", 0, 0),
                "corrupt": ("bad-checksum", 0, 0),
                "magic08": ("bad-magic", 0, 0)}
    paths = []
    for name, data in cases.items():
        path = work / ("save-%s.dat" % name)
        path.write_bytes(data)
        paths.append(path)
    for cc in compilers:
        rows = run_saves(cc, executables[cc.name], paths)
        for (name, data), row in zip(cases.items(), rows):
            got = compare_save(row, data, "%s %s" % (cc.name, name))
            require(got == expected[name], "%s %s: %s != %s" %
                    (cc.name, name, got, expected[name]))
    print("saves: %d synthetic saves (Python reference checksum) == C import "
          "path (%s)" % (len(cases), ", ".join(cc.name for cc in compilers)))


def verify_real_saves(compilers: list[Compiler], executables: dict[str, str],
                      paths: list[Path], work: Path) -> None:
    # Copies without spaces in the path (WSL argv), named by index only.
    copies = []
    for index, path in enumerate(paths):
        copy = work / ("real-%02d.dat" % index)
        copy.write_bytes(path.read_bytes())
        copies.append(copy)
    for cc in compilers:
        rows = run_saves(cc, executables[cc.name], copies)
        for index, (copy, row) in enumerate(zip(copies, rows)):
            data = copy.read_bytes()
            label = "#%02d %s" % (index, paths[index].name)
            status, saved, mapped = compare_save(row, data, label)
            crc_ok = len(data) >= 0x14 and int(row["crc"], 16) == int(
                row["stored"], 16)
            print("real save %s %s: size=%d status=%s saved=%d mapped=%d "
                  "crc_ok=%d" % (cc.name, label, len(data), status, saved,
                                 mapped, int(crc_ok)))
    print("real saves: %d files, C == Python reference (%s)" % (
        len(paths), ", ".join(cc.name for cc in compilers)))


# ---- 4. fences and source contract ------------------------------------------------

def joined_literals(text: str) -> str:
    return re.sub(r'"\s*\n\s*"', "", text)


def verify_fences(cc: Compiler, work: Path) -> None:
    out = cc.syntax(STRICT + includes(cc), SOURCE, check=False)
    require(out.returncode != 0 and "must only be compiled with "
            "ISAAC_VITA_TROPHIES" in out.stdout + out.stderr,
            "the TU builds without the knob")
    out = cc.syntax(STRICT + includes(cc) + ["-D%s=1" % KNOB,
                                             "-DGUEST_IMAGE_BASE=0x30000000u"],
                    SOURCE, check=False)
    require(out.returncode != 0 and "GUEST_IMAGE_BASE 0x98000000" in
            out.stdout + out.stderr, "the TU accepts a foreign image base")
    # Without the knob the header declares nothing: a conflicting definition
    # of every public name must compile.
    probe = work / "header_probe.c"
    probe.write_text(
        '#include "host_vita_trophies.h"\n'
        "int isaac_vita_trophies_boot;\n"
        "int isaac_vita_trophies_setup;\n"
        "int isaac_vita_trophies_try_unlock_observe;\n"
        "int isaac_vita_trophies_map;\n"
        "#ifdef ISAAC_VITA_TROPHY_TITLE_ID\n#error leaked\n#endif\n",
        encoding="ascii")
    cc.syntax(STRICT + includes(cc), probe)
    out = cc.syntax(STRICT + includes(cc) + ["-D%s=1" % KNOB], probe,
                    check=False)
    require(out.returncode != 0, "the header declares nothing with the knob")
    # Stage B hook: kage_vita_backend.c type-checks with the knob against the
    # backend oracle's vitaGL stand-in (kage_vita_backend_test_vitagl.h).
    for extra in ([], ["-DISAAC_VITA_VITAGL_STOCK_REFERENCE=1"]):
        cc.syntax(["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror"] +
                  includes(cc) + ["-DISAAC_KAGE_VITA_BACKEND_ORACLE=1",
                                  "-D%s=1" % KNOB] + extra,
                  RUNTIME / "kage_vita_backend.c")
    print("fences: TU #error without the knob and at 0x30000000; header empty "
          "without the knob; backend hook type-checks with the knob (%s)" %
          cc.name)


def verify_source_contract() -> None:
    source = SOURCE.read_text(encoding="utf-8")
    header = HEADER.read_text(encoding="utf-8")
    flat = joined_literals(source)
    uses = 0
    for line in LOG_FORMATS:
        count = flat.count('"%s"' % line)
        require(count >= 1, "log format missing in the TU: %s" % line)
        uses += count
    require(flat.count('"KAGE VITA TROPHY') == uses,
            "the TU logs a line outside the pinned formats")
    for status in IMPORT_STATUSES:
        require('"%s"' % status in source, "import status %s" % status)
    for forbidden in ("malloc", "calloc", "realloc", "free(", "printf(",
                      "fopen", "<stdio.h>", "<stdlib.h>", "<string.h>"):
        require(forbidden not in re.sub(r"/\*.*?\*/", "", source, flags=re.S),
                "the TU uses %s" % forbidden)
    title = re.search(r'#define ISAAC_VITA_TROPHY_TITLE_ID\s+"([^"]+)"', header)
    cmake = re.search(r'set\(ISAAC_TITLE_ID "([^"]+)"\)',
                      CMAKE.read_text(encoding="utf-8"))
    require(title is not None and cmake is not None and
            title.group(1) == cmake.group(1) == "ISAACR001",
            "header title id != CMake ISAAC_TITLE_ID")
    # NP comm id base: device run 2026-10-09 refused ISAACR001_0N
    # (C0-11133-9) and accepted ISAC00000_0N; header == CMake == log format.
    base = re.search(r'#define ISAAC_VITA_TROPHY_COMM_BASE\s+"([^"]+)"', header)
    cmake_base = re.findall(r'set\(ISAAC_VITA_TROPHY_COMM_BASE "([^"]+)"\)',
                            CMAKE.read_text(encoding="utf-8"))
    require(base is not None and cmake_base == [base.group(1)] and
            re.fullmatch(r"[A-Z]{4}[0-9]{5}", base.group(1)) is not None and
            base.group(1) == "ISAC00000",
            "comm id base: header %r, CMake %r" % (
                base.group(1) if base else None, cmake_base))
    require("ISAAC_VITA_TROPHY_COMM_BASE[i]" in source and
            "ISAAC_VITA_TROPHY_TITLE_ID[i]" not in source,
            "the context comm id must come from the comm base")
    # Stage A creates no context (two at most, each needs a dialog that only
    # Stage B can show; device run 2026-10-09).
    boot = source[source.index("void isaac_vita_trophies_boot(void)"):]
    boot = boot[:boot.index("\n}\n")]
    require("trophy_create_context" not in boot and
            "trophy_open_context" not in boot,
            "Stage A must not create a trophy context")
    # The prototype gen_all.py renders into the seam == the header's.
    gen_all = GEN_ALL.read_text(encoding="utf-8")
    rendered = re.search(
        r'"    extern void isaac_vita_trophies_try_unlock_observe\("\s*'
        r'"([^"]*)\);",', gen_all)
    declared = re.search(
        r"void isaac_vita_trophies_try_unlock_observe\(([^)]*)\);", header)
    require(rendered is not None and declared is not None,
            "observer prototype not found")
    norm = lambda text: re.sub(r"\s+", " ", text).strip()  # noqa: E731
    require(norm(rendered.group(1)) == norm(declared.group(1)),
            "gen_all seam prototype != header prototype")
    constants = {
        "TROPHY_MGR_PTR_RVA": "0x7fd680u", "TROPHY_PGD_OFFSET": "0x14u",
        "TROPHY_PGD_READONLY": "0x1u", "TROPHY_PGD_UNLOCKED": "0x38u",
        "TROPHY_ID_MAX": "637u", "TROPHY_UNLOCK_GAP_US": "100000u",
        # device run 2026-10-09: two live contexts at most (the third
        # CreateContext -> 0x80551606), dialogs 55-245 frames
        "TROPHY_SETUP_MAX_FRAMES": "600u",
        "TROPHY_SETUP_ABORT_FRAMES": "300u",
        "TROPHY_LIVE_MAX": "2u",
        "TROPHY_ERROR_EXCEEDS_MAX": "0x80551606u",
        "TROPHY_WAIT_TICK_US": "10000u",
        "TROPHY_DRAIN_STALL_US": "5000000u",
        "TROPHY_RETIRE_WAIT_US": "2000000u",
        "TROPHY_STORE_BYTES": "200u",
        "TROPHY_STORE_VERSION": "1u",
        "TROPHY_SAVE_MAX_BYTES": "(1024u * 1024u)",
        "TROPHY_REQUEST_MAX_BYTES": "16u", "TROPHY_TEST_MAX_IDS": "8u",
        "TROPHY_ERROR_ALREADY_UNLOCKED": "0x8055160fu",
        "TROPHY_ERROR_SETUP_REQUIRED": "0x80551612u",
        "TROPHY_CRC_POLY": "0xEDB88320u", "TROPHY_CRC_INIT": "0xFEDCBA76u",
        "TROPHY_SAVE_SLOTS": "0x27eu",
    }
    for name, value in constants.items():
        match = re.search(r"#define %s\s+(.+)" % name, source)
        require(match is not None and match.group(1).strip() == value,
                "%s = %r" % (name, match.group(1) if match else None))
    require('"ISAACNGSAVE09R  "' in source and "0xb9u, 0xddu, 0xe1u, 0x3bu, "
            "0x01u, 0x00u" in source, "save magic / comm signature")
    print("source: %d exact log formats, no allocator/stdio, title id == "
          "CMake ISAAC_TITLE_ID, comm base ISAC00000 == CMake, seam prototype "
          "== header, constants pinned" %
          len(LOG_FORMATS))


# ---- 5b. hooks and allocator gate ------------------------------------------------

ENTRY = RUNTIME / "entry_vita.c"
BACKEND = RUNTIME / "kage_vita_backend.c"
GATE = VITA / "vita_raw_allocator_gate.py"
GATE_TEST = VITA / "test_vita_raw_allocator_gate.py"
FENCE_OPEN = "#if defined(ISAAC_VITA_TROPHIES)"


def fenced_spans(text: str) -> list[tuple[int, int]]:
    """Line spans of every `#if defined(ISAAC_VITA_TROPHIES)` .. `#endif`
    (no nested conditionals allowed inside)."""
    lines = text.split("\n")
    spans = []
    index = 0
    while index < len(lines):
        if lines[index].strip() == FENCE_OPEN:
            end = index + 1
            while lines[end].strip() not in ("#endif",):
                require(not lines[end].lstrip().startswith("#if") and
                        not lines[end].lstrip().startswith("#el"),
                        "nested conditional inside a trophy fence")
                end += 1
            spans.append((index, end))
            index = end
        index += 1
    return spans


def strip_fences(text: str) -> str:
    lines = text.split("\n")
    drop = set()
    for start, end in fenced_spans(text):
        drop.update(range(start, end + 1))
    return "\n".join(line for n, line in enumerate(lines) if n not in drop)


def verify_hooks_contract() -> None:
    for path, expected_spans in ((ENTRY, 2), (BACKEND, 3)):
        text = path.read_text(encoding="utf-8")
        spans = fenced_spans(text)
        require(len(spans) == expected_spans, "%s: %d trophy fences" %
                (path.name, len(spans)))
        require("__LINE__" not in text, "%s uses __LINE__" % path.name)
        rest = strip_fences(text)
        require(not re.search(r"troph", rest, flags=re.I),
                "%s mentions trophies outside a fence" % path.name)
    entry = ENTRY.read_text(encoding="utf-8")
    call = entry.index("    isaac_vita_trophies_boot();")
    require(entry.count("isaac_vita_trophies_boot();") == 1 and
            entry.index("isaac_vita_texel_scratch_reserve(") < call <
            entry.index("    guest_register_all();") and
            entry[call:].split("\n")[1:3] == ["#endif",
                                              "    guest_register_all();"],
            "Stage A must sit right before guest_register_all()")
    backend = BACKEND.read_text(encoding="utf-8")
    call = backend.index("    isaac_vita_trophies_setup(kage_vita_trophy_present);")
    init = backend.index("int kage_vita_backend_initialize(")
    require(backend.count("isaac_vita_trophies_setup(") == 1 and
            init < backend.index('"vitaGL initialized without a GL_VERSION '
                                 'string"') < call <
            backend.index("    vglWaitVblankStart(GL_FALSE);", init) and
            "vglSwapBuffers(GL_TRUE);" in backend[:init],
            "Stage B must follow the GL_VERSION check in the cold path")
    gate = GATE.read_text(encoding="utf-8")
    require('TROPHIES_CACHE_KEY = "ISAAC_VITA_TROPHIES"' in gate and
            'TROPHIES_SOURCE = "host_vita_trophies.c"' in gate and
            "runtime.add(TROPHIES_SOURCE)" in gate,
            "raw allocator gate registration")
    gate_test = GATE_TEST.read_text(encoding="utf-8")
    require('"ISAAC_VITA_TROPHIES": "OFF",' in gate_test and
            "CMake cache is missing ISAAC_VITA_TROPHIES" in gate_test,
            "raw allocator gate test fixture/delta")
    print("hooks: Stage A before guest_register_all(), Stage B after the "
          "GL_VERSION check, every trophy line fenced; allocator gate "
          "registers host_vita_trophies.c")


RECIPE = ROOT / "release" / "v0.1.1-alpha.cmake"
RECIPE_PINS = ('set(ISAAC_VITA_TROPHIES OFF CACHE BOOL "")',
               'set(ISAAC_VITA_TROPHY_TEST_REQUEST OFF CACHE BOOL "")')
TROPHIES_DOC = VITA / "TROPHIES.md"


def verify_recipe_and_doc() -> None:
    lines = RECIPE.read_text(encoding="utf-8").split("\n")
    for pin in RECIPE_PINS:
        require(lines.count(pin) == 1, "release recipe pin: %s" % pin)
    first = lines.index(RECIPE_PINS[0])
    names = [re.match(r"set\((\w+)", line).group(1)
             for line in lines[first - 1:first + 3]]
    require(names == sorted(names) and lines[first + 1] == RECIPE_PINS[1],
            "trophy pins out of alphabetical order: %s" % names)
    require("ISAAC_VITA_TROPHY_PACK_DIR" not in "\n".join(lines),
            "the release recipe must not name a pack directory")
    doc = TROPHIES_DOC.read_text(encoding="utf-8")
    for needle in ("trophy-import.req", "trophy-import.res", "trophy-test.req",
                   "persistentgamedata", "-DISAAC_VITA_TROPHIES=ON",
                   "-DISAAC_VITA_TROPHY_TEST_REQUEST=ON",
                   "-DISAAC_VITA_TROPHY_PACK_DIR=", "achievement 4"):
        require(needle in doc, "TROPHIES.md lacks %s" % needle)
    for line in LOG_FORMATS:
        require(line in doc, "TROPHIES.md lacks the log line %s" % line)
    for status in IMPORT_STATUSES:
        require("`%s`" % status in doc, "TROPHIES.md lacks status %s" % status)
    print("recipe: both trophy knobs pinned OFF in order, no pack dir; "
          "TROPHIES.md documents the contract and every log line")


# ---- 6. CMake ------------------------------------------------------------------

def _block(lines: list[str], start: int) -> list[str]:
    """lines[start] opens an if(); return through its matching endif()."""
    depth = 0
    for index in range(start, len(lines)):
        stripped = lines[index].strip()
        if stripped.startswith("if("):
            depth += 1
        elif stripped.startswith("endif("):
            depth -= 1
            if depth == 0:
                return lines[start:index + 1]
    raise AssertionError("unterminated if() at CMake line %d" % (start + 1))


def cmake_blocks() -> dict[str, str]:
    lines = CMAKE.read_text(encoding="utf-8").split("\n")
    opens = [i for i, line in enumerate(lines)
             if line.strip() == "if(ISAAC_VITA_TROPHIES)"]
    owner = [i for i in opens if "sub_00484c80 sub_00485ab0" in
             "\n".join(_block(lines, i))]
    link = [i for i in opens if "SceNpTrophy_stub" in
            "\n".join(_block(lines, i))]
    require(len(opens) == 2 and len(owner) == 1 and len(link) == 1,
            "expected one owner block and one link block")
    fatal_start = lines.index(
        "if(ISAAC_VITA_TROPHIES AND ISAAC_VITA_SCAFFOLD_ONLY)")
    fatal_last = lines.index(
        'if(NOT "${ISAAC_VITA_TROPHY_PACK_DIR}" STREQUAL "" AND NOT '
        "ISAAC_VITA_TROPHIES)")
    fatal = lines[fatal_start:fatal_last] + _block(lines, fatal_last)
    vpk_start = lines.index("set(ISAAC_VPK_TROPHY_FILES)")
    vpk = lines[vpk_start:vpk_start + 1] + _block(lines, vpk_start + 1)
    return {"owner": "\n".join(_block(lines, owner[0])),
            "link": "\n".join(_block(lines, link[0])),
            "fatal": "\n".join(fatal), "vpk": "\n".join(vpk),
            "all": "\n".join(lines)}


def verify_cmake_contract() -> None:
    blocks = cmake_blocks()
    text = blocks["all"]
    for name in ("ISAAC_VITA_TROPHIES", "ISAAC_VITA_TROPHY_TEST_REQUEST"):
        option = re.search(r"option\(%s\s+\".*?\"\s+(ON|OFF)\)" % name, text,
                           flags=re.S)
        require(option is not None and option.group(1) == "OFF",
                "%s must default OFF" % name)
    require(text.count('set(ISAAC_VITA_TROPHY_PACK_DIR "" CACHE PATH') == 1,
            "ISAAC_VITA_TROPHY_PACK_DIR cache path")
    for message in ("ISAAC_VITA_TROPHIES requires the translated guest runtime",
                    "ISAAC_VITA_TROPHIES requires ISAAC_VITA_KAGE=ON",
                    "ISAAC_VITA_TROPHY_TEST_REQUEST requires "
                    "ISAAC_VITA_TROPHIES=ON",
                    "ISAAC_VITA_TROPHY_PACK_DIR requires ISAAC_VITA_TROPHIES=ON"):
        require(blocks["fatal"].count(message) == 1, "FATAL: %s" % message)
    code = "\n".join(line for line in text.split("\n")
                     if not line.lstrip().startswith("#"))
    # The manager UI2 block links its own SceAppUtil_stub (system language);
    # same exclusion as test_livearea_package.py's LiveArea dispatch check.
    ui2_begin = code.find("if(ISAAC_VITA_MANAGER_UI2)\n")
    ui2_block = (code[ui2_begin:code.index("\nendif()\n", ui2_begin)]
                 if ui2_begin >= 0 else "")
    require(ui2_block.count("SceAppUtil_stub") <= 1,
            "manager UI2 block links SceAppUtil_stub at most once")
    code = code.replace(ui2_block, "", 1) if ui2_block else code
    for token, count in (("ISAAC_VITA_TROPHIES=1", 1),
                         ("ISAAC_VITA_TROPHY_TEST_REQUEST=1", 1),
                         ("host_vita_trophies.c", 3),
                         ("SceNpTrophy_stub", 1),
                         ("SceAppUtil_stub", 1),
                         ("trophy_pack_check.py", 3),
                         ("${ISAAC_VPK_TROPHY_FILES}", 1)):
        require(code.count(token) == count, "CMake count of %s: %d" %
                (token, code.count(token)))
    for token in ("ISAAC_VITA_TROPHIES=1", "ISAAC_VITA_TROPHY_TEST_REQUEST=1",
                  "host_vita_trophies.c", '"${ISAAC_RUNTIME}/entry_vita.c"',
                  '"${ISAAC_RUNTIME}/kage_vita_backend.c"',
                  "must own exactly one"):
        require(token in blocks["owner"], "owner block lacks %s" % token)
    require("SceSysmodule_stub" in blocks["link"], "link block")
    # The trophy members ride only on the translated VPK, as its last
    # argument; the scaffold branch never sees them.
    create = re.findall(r"vita_create_vpk\((.*?)\)\n", text, flags=re.S)
    require(len(create) == 2 and "${ISAAC_VPK_TROPHY_FILES}" not in create[0]
            and create[1].rstrip().endswith("${ISAAC_VPK_TROPHY_FILES}") and
            "isaac-ng.exe.unpacked.exe" in create[1],
            "VPK list must be the last argument of the translated VPK only")
    require(text.index("find_package(Python3 COMPONENTS Interpreter") <
            text.index("set(ISAAC_VPK_TROPHY_FILES)"),
            "Python3 must be found before the pack check")
    print("cmake: options OFF, 4 FATAL rules, owner block (2 roots), link, VPK "
          "list last in the translated vita_create_vpk only")


def _cmake() -> str:
    found = shutil.which("cmake")
    if found is None and Path("C:/Program Files/CMake/bin/cmake.exe").is_file():
        found = "C:/Program Files/CMake/bin/cmake.exe"
    require(found is not None, "cmake is required for the CMake legs")
    return found


def run_cmake_script(work: Path, tag: str, body: list[str]) -> tuple[int, str]:
    script = work / ("trophies_%s.cmake" % tag)
    script.write_text("\n".join(["cmake_minimum_required(VERSION 3.16)"] +
                                body + [""]), encoding="utf-8", newline="\n")
    done = subprocess.run([_cmake(), "-P", str(script)],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return done.returncode, done.stdout.decode("utf-8", "replace")


def verify_cmake_scripts(work: Path, generated_dir: Path | None) -> list[str]:
    blocks = cmake_blocks()
    results = []

    # FATAL rules
    def fatal(tag, settings):
        return run_cmake_script(work, "fatal_" + tag, [
            "set(%s %s)" % kv for kv in settings] + [blocks["fatal"],
                                                    'message(STATUS "FATAL-OK")'])

    base = [("ISAAC_VITA_SCAFFOLD_ONLY", "OFF"), ("ISAAC_VITA_KAGE", "ON"),
            ("ISAAC_VITA_TROPHIES", "OFF"),
            ("ISAAC_VITA_TROPHY_TEST_REQUEST", "OFF"),
            ("ISAAC_VITA_TROPHY_PACK_DIR", '""')]

    def with_(**changes):
        return [(k, changes.get(k, v)) for k, v in base]

    accepted = [with_(), with_(ISAAC_VITA_TROPHIES="ON"),
                with_(ISAAC_VITA_TROPHIES="ON",
                      ISAAC_VITA_TROPHY_TEST_REQUEST="ON",
                      ISAAC_VITA_TROPHY_PACK_DIR='"/x/packs"'),
                with_(ISAAC_VITA_KAGE="OFF")]
    for n, settings in enumerate(accepted):
        rc, out = fatal("ok%d" % n, settings)
        require(rc == 0 and "FATAL-OK" in out, "accepted config refused: %s" %
                out[-800:])
    refused = [
        (with_(ISAAC_VITA_TROPHIES="ON", ISAAC_VITA_SCAFFOLD_ONLY="ON"),
         "requires the translated guest runtime"),
        (with_(ISAAC_VITA_TROPHIES="ON", ISAAC_VITA_KAGE="OFF"),
         "requires ISAAC_VITA_KAGE=ON"),
        (with_(ISAAC_VITA_TROPHY_TEST_REQUEST="ON"),
         "ISAAC_VITA_TROPHY_TEST_REQUEST requires ISAAC_VITA_TROPHIES=ON"),
        (with_(ISAAC_VITA_TROPHY_PACK_DIR='"/x/packs"'),
         "ISAAC_VITA_TROPHY_PACK_DIR requires ISAAC_VITA_TROPHIES=ON"),
    ]
    for n, (settings, needle) in enumerate(refused):
        rc, out = fatal("no%d" % n, settings)
        require(rc != 0 and needle in out, "refused config accepted (%s): %s" %
                (needle, out[-800:]))
    results.append("cmake FATAL rules: %d accepted, %d refused" %
                   (len(accepted), len(refused)))

    # Owner census
    def census(tag, units, test_request="OFF"):
        return run_cmake_script(work, "census_" + tag, [
            'include("%s")' % (VITA / "generated_definitions.cmake").as_posix(),
            "function(set_property)",
            '  if(ARGV0 STREQUAL "SOURCE")',
            '    message(STATUS "SETPROP ${ARGN}")',
            "  endif()",
            "endfunction()",
            "set(ISAAC_VITA_TROPHIES ON)",
            "set(ISAAC_VITA_TROPHY_TEST_REQUEST %s)" % test_request,
            'set(ISAAC_VITA_TROPHY_PACK_DIR "")',
            'set(ISAAC_RUNTIME "%s")' % RUNTIME.as_posix(),
            'set(ISAAC_GENERATED_C "%s")' % ";".join(u.as_posix()
                                                     for u in units),
            "set(ISAAC_VITA_RUNTIME_SOURCES)",
            blocks["owner"],
            'message(STATUS "CENSUS owners=${ISAAC_VITA_TROPHIES_OWNERS} '
            'runtime=${ISAAC_VITA_RUNTIME_SOURCES}")'])

    def unit(name, roots):
        path = work / "units" / name
        path.parent.mkdir(exist_ok=True)
        path.write_text("".join("void %s(CPU *__restrict c)\n{\n}\n" % r
                                for r in roots), encoding="utf-8",
                        newline="\n")
        return path

    runtime = RUNTIME.as_posix()
    filler = unit("guest_0001.c", ["sub_00401000"])
    both = unit("guest_0139.c", ["sub_00484c80", "sub_00485ab0"])
    rc, out = census("both", [filler, both], "ON")
    flat = re.sub(r"\s+", " ", out)
    require(rc == 0 and "CENSUS owners=%s runtime=%s/host_vita_trophies.c" %
            (both.as_posix(), runtime) in flat, "census (one unit): %s" %
            flat[-1200:])
    require(("SETPROP SOURCE;%s;%s/host_vita_trophies.c;%s/entry_vita.c;"
             "%s/kage_vita_backend.c;APPEND;PROPERTY;COMPILE_DEFINITIONS;"
             "ISAAC_VITA_TROPHIES=1" % (both.as_posix(), runtime, runtime,
                                         runtime)) in flat and
            ("SETPROP SOURCE;%s/host_vita_trophies.c;APPEND;PROPERTY;"
             "COMPILE_DEFINITIONS;ISAAC_VITA_TROPHY_TEST_REQUEST=1" % runtime)
            in flat and "test request ON; no packs" in flat,
            "census definitions: %s" % flat[-1500:])
    rc, out = census("both_notest", [filler, both])
    require(rc == 0 and "ISAAC_VITA_TROPHY_TEST_REQUEST=1" not in out,
            "test-request definition leaked: %s" % out[-800:])
    split_a = unit("guest_0200.c", ["sub_00484c80"])
    split_b = unit("guest_0201.c", ["sub_00485ab0"])
    rc, out = census("split", [filler, split_a, split_b])
    flat = re.sub(r"\s+", " ", out)
    require(rc == 0 and "CENSUS owners=%s;%s " % (split_a.as_posix(),
                                                   split_b.as_posix()) in flat,
            "census (split owners): %s" % flat[-1200:])
    for root, units, found in (
            ("sub_00485ab0", [filler, split_a], 0),
            ("sub_00484c80", [filler, split_b], 0),
            ("sub_00484c80", [filler, both, split_a], 2)):
        rc, out = census("bad_%s_%d" % (root, found), units)
        require(rc != 0 and ("root %s must own exactly one generated source; "
                             "found %d" % (root, found)) in re.sub(r"\s+", " ",
                                                                   out),
                "census accepted %s found %d: %s" % (root, found, out[-800:]))
    results.append("cmake census: one-unit and split owners routed; missing "
                   "and duplicated roots refused")
    if generated_dir is not None:
        units = sorted(generated_dir.resolve().glob(
            "guest_[0-9][0-9][0-9][0-9].c"))
        rc, out = census("real", units)
        flat = re.sub(r"\s+", " ", out)
        owner = (generated_dir.resolve() / "guest_0139.c").as_posix()
        require(rc == 0 and "CENSUS owners=%s runtime=" % owner in flat,
                "census on the real corpus: %s" % flat[-1200:])
        results.append("cmake census real corpus (%d units): owners "
                       "guest_0139.c" % len(units))

    # VPK block
    sys.path.insert(0, str(VITA))
    import test_trophy_pack_check as packs  # noqa: E402

    def vpk(tag, trophies, pack_dir):
        return run_cmake_script(work, "vpk_" + tag, [
            'include("%s")' % (VITA / "literal_glob_path.cmake").as_posix(),
            "set(ISAAC_VITA_TROPHIES %s)" % trophies,
            'set(ISAAC_VITA_TROPHY_PACK_DIR "%s")' % pack_dir,
            'set(ISAAC_TITLE_ID "ISAACR001")',
            'set(Python3_EXECUTABLE "%s")' % Path(sys.executable).as_posix(),
            'set(CMAKE_CURRENT_SOURCE_DIR "%s")' % VITA.as_posix(),
            blocks["vpk"],
            'message(STATUS "VPKFILES=${ISAAC_VPK_TROPHY_FILES}")'])

    good = work / "packs-good"
    packs.write_packs(good)
    rc, out = vpk("off", "OFF", "")
    require(rc == 0 and "VPKFILES=\n" in out.replace("\r", ""),
            "OFF must add no members: %s" % out[-800:])
    rc, out = vpk("empty", "ON", "")
    require(rc == 0 and "VPKFILES=\n" in out.replace("\r", "") and
            "WARNING ISAAC_VITA_TROPHY_PACK_DIR is empty" in
            re.sub(r"\s+", " ", out), "ON + empty dir: %s" % out[-800:])
    rc, out = vpk("good", "ON", good.as_posix())
    files = re.search(r"VPKFILES=(.*)", out)
    expected = ";".join("FILE;%s/ISAC00000_0%d/TROPHY.TRP;"
                        "sce_sys/trophy/ISAC00000_0%d/TROPHY.TRP" %
                        (good.as_posix(), n, n) for n in range(6))
    require(rc == 0 and files is not None and
            files.group(1).strip() == expected and
            out.count("trophy pack OK: ISAC00000_0") == 6,
            "ON + good packs: %s" % out[-1500:])
    broken = work / "packs-broken"
    packs.write_packs(broken, (3, packs.mutated(3, lambda d: d.__setitem__(
        0x30, 1))))
    rc, out = vpk("broken", "ON", broken.as_posix())
    out = re.sub(r"\s+", " ", out)
    require(rc != 0 and "rejected ISAAC_VITA_TROPHY_PACK_DIR" in out and
            "ISAC00000_03" in out, "broken pack accepted: %s" % out[-800:])
    missing = work / "packs-missing"
    packs.write_packs(missing)
    (missing / "ISAC00000_05" / "TROPHY.TRP").unlink()
    rc, out = vpk("missing", "ON", missing.as_posix())
    out = re.sub(r"\s+", " ", out)
    require(rc != 0 and "ISAC00000_05/TROPHY.TRP is not a regular file" in out,
            "missing pack accepted: %s" % out[-800:])
    rc, out = vpk("relative", "ON", "packs/relative")
    require(rc != 0 and "ISAAC_VITA_TROPHY_PACK_DIR" in out,
            "relative pack dir accepted: %s" % out[-800:])
    results.append("cmake VPK: OFF and empty dir add nothing; six good packs "
                   "-> 6 FILE pairs sce_sys/trophy/ISAC00000_0N/TROPHY.TRP; "
                   "broken, missing and relative refused")
    return results


# ---- 5. ARM -------------------------------------------------------------------

ARM_FLAGS = ["-std=gnu11", "-O2", "-mcpu=cortex-a9", "-mfpu=neon",
             "-mfloat-abi=softfp", "-mthumb", "-fno-strict-aliasing",
             "-ffunction-sections", "-fdata-sections", "-Wall", "-Wextra",
             "-Werror", "-DGUEST_IMAGE_BASE=0x98000000u",
             "-DGUEST_STACK_REQUIRED=1", "-D%s=1" % KNOB]


def verify_arm(vitasdk_include: Path | None, work: Path) -> str:
    vitasdk = os.environ.get("VITASDK")
    gcc = Path(vitasdk) / "bin" / "arm-vita-eabi-gcc" if vitasdk else None
    if gcc is not None and gcc.is_file():
        for extra in ([], ["-D%s=1" % TEST_KNOB]):
            out = subprocess.run([str(gcc)] + ARM_FLAGS + extra + [
                "-I" + str(RUNTIME), "-I" + str(VITA), "-c", str(SOURCE),
                "-o", str(work / "trophies-arm.o")], capture_output=True,
                text=True)
            require(out.returncode == 0, "ARM build failed:\n%s%s" %
                    (out.stdout, out.stderr))
        return "ARM: arm-vita-eabi-gcc production flags PASS (both variants)"
    if vitasdk_include is not None:
        clang = shutil.which("clang") or str(LLVM_CLANG)
        for extra in ([], ["-D%s=1" % TEST_KNOB]):
            out = subprocess.run([clang, "--target=armv7a-none-eabi",
                                  "-mthumb", "-mfloat-abi=softfp",
                                  "-std=gnu11", "-O2", "-Wall", "-Wextra",
                                  "-Werror", "-Wundef", "-fsyntax-only",
                                  "-D__vita__", "-DGUEST_IMAGE_BASE=0x98000000u",
                                  "-D%s=1" % KNOB] + extra + [
                                  "-I" + str(RUNTIME), "-I" + str(VITA),
                                  "-isystem", str(vitasdk_include),
                                  str(SOURCE)], capture_output=True, text=True)
            require(out.returncode == 0, "clang armv7a syntax check failed:\n"
                    "%s%s" % (out.stdout, out.stderr))
        return ("ARM: clang --target=armv7a-none-eabi -fsyntax-only against %s "
                "PASS (both variants; not a link, not the vitasdk compiler)" %
                vitasdk_include)
    return "ARM: skipped (no $VITASDK/bin/arm-vita-eabi-gcc, no --vitasdk-include)"


# ---- main -----------------------------------------------------------------------

STATIC_CHECKS = [verify_source_contract, verify_hooks_contract,
                 verify_cmake_contract, verify_recipe_and_doc]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--cc", action="append", choices=("clang", "gcc"),
                        help="compiler legs (default: every one found)")
    parser.add_argument("--vitasdk-include", type=Path,
                        help="vita-headers include/ for the clang ARM leg")
    parser.add_argument("--real-saves", type=Path, nargs="+",
                        help="real save files (not in the default run)")
    parser.add_argument("--generated-dir", type=Path,
                        help="a seam corpus for the CMake owner census")
    args = parser.parse_args()
    try:
        for check_fn in STATIC_CHECKS:
            check_fn()
        compilers = discover_compilers(args.cc)
        print("compilers: %s" % ", ".join(cc.name for cc in compilers))
        with tempfile.TemporaryDirectory(prefix="isaac-trophies-") as value:
            work = Path(value)
            executables = run_oracles(compilers, work)
            verify_map(compilers, executables, work)
            verify_synthetic_saves(compilers, executables, work)
            for cc in compilers:
                verify_fences(cc, work)
            for line in verify_cmake_scripts(work, args.generated_dir):
                print(line)
            if args.real_saves:
                verify_real_saves(compilers, executables, args.real_saves,
                                  work)
            print(verify_arm(args.vitasdk_include, work))
    except AssertionError as exc:
        print("FAIL: %s" % exc)
        return 1
    print("Vita trophies host gate: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
