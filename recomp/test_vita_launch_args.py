#!/usr/bin/env python3
"""Proof of ISAAC_VITA_LAUNCH_ARGS_FILE (host_vita_launch_args.c), including
the --vita-save-slot= token that the benchmark harness passes.

1. Frozen inputs (with --pe): the instructions the slot store relies on, read
   from the frozen isaac-ng.exe, and the header constants that copy them.
2. Host oracle: host_vita_launch_args_oracle.c with the module, -Werror; it
   must print PASS and reach every outcome class (its coverage line).
3. Mutations: each edit of the module below must make the oracle fail.
4. Build gate: the option is default OFF, the module and its compile
   definition exist only under it, and every launch-args line of
   host_vita_crt.c sits inside #if defined(ISAAC_VITA_LAUNCH_ARGS_FILE).

usage: test_vita_launch_args.py [--pe PE] [--cc CLANG_OR_GCC] [--keep DIR]
Compiler default: clang from the LLVM install dir, else gcc/cc on PATH.
"""
from __future__ import annotations

import argparse
import hashlib
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
RUNTIME = HERE / "runtime"
VITA = HERE / "vita"
MODULE = RUNTIME / "host_vita_launch_args.c"
HEADER = RUNTIME / "host_vita_launch_args.h"
ORACLE = RUNTIME / "host_vita_launch_args_oracle.c"
CRT = RUNTIME / "host_vita_crt.c"
CMAKE = VITA / "CMakeLists.txt"

PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
# rva -> (bytes, what); VA = 0x400000 + rva in the frozen PE.
PINS = {
    0x0050B973: ("a380d6bf00", "main: mov dword ptr [0xbfd680], eax (Manager)"),
    0x004AD63D: ("8b3d80d6bf00", "Manager::Init: mov edi, dword ptr [0xbfd680]"),
    0x004AD7FA: ("c7471000000000", "Manager::Init: mov dword ptr [edi + 0x10], 0"),
    0x004AD841: ("a35cd6bf00", "Manager::Init: mov dword ptr [0xbfd65c], eax (Game)"),
    0x0040D7D0: ("a180d6bf00", "mod data: mov eax, dword ptr [0xbfd680]"),
    0x0040D7D5: ("8b4010", "mod data: mov eax, dword ptr [eax + 0x10]"),
    0x0040D7DA: ("0f44c1", "mod data: cmove eax, ecx (slot 0 -> 1)"),
    0x004B4768: ("895e10", "LoadPersistentGameData: mov [esi + 0x10], ebx"),
    0x004B4781: ("895f10", "LoadPersistentGameData: mov [edi + 0x10], ebx"),
    0x0050C3A6: ("c70580d6bf0000000000", "teardown: mov dword ptr [0xbfd680], 0"),
}
CONSTANTS = {
    "ISAAC_VITA_LAUNCH_ARGS_MANAGER_RVA": 0x007FD680,
    "ISAAC_VITA_LAUNCH_ARGS_GAME_RVA": 0x007FD65C,
    "ISAAC_VITA_LAUNCH_ARGS_MANAGER_SLOT": 0x10,
}
CLASSES = ("split-comment split-over-cap absent io-open io-read too-large too-many "
           "ok-no-slot ok-slot no-slot bad-slot-value bad-slot-dup apply-no-slot "
           "apply-wait-manager apply-wait-game apply-first apply-unchanged "
           "apply-restore-logged apply-restore-quiet apply-teardown").split()

# (name, old, new): each must be found exactly once in the module.
MUTATIONS = [
    ("slot upper bound 3 -> 4", "digit[0] <= '3'", "digit[0] <= '4'"),
    ("slot lower bound 1 -> 0", "digit[0] >= '1'", "digit[0] >= '0'"),
    ("accept trailing characters", "&& digit[1] == '\\0')", ")"),
    ("allow two slot tokens", "seen > 1u", "seen > 2u"),
    ("--set-stage without slot accepted", "if (set_stage && seen == 0u) {",
     "if (0 && set_stage && seen == 0u) {"),
    ("slot token reaches the guest argv", "            continue;\n        }\n"
     "        if (launch_args_prefix(token, ISAAC_VITA_LAUNCH_ARGS_SET_STAGE_TOKEN))",
     "        }\n        if (launch_args_prefix(token, ISAAC_VITA_LAUNCH_ARGS_SET_STAGE_TOKEN))"),
    ("no-slot keeps the guest tokens", "        *count = 0u;\n        return \"no-slot\";",
     "        return \"no-slot\";"),
    ("bad-slot keeps the slot", "    *slot = 0u;\n    if (bad", "    *slot = value;\n    if (bad"),
    ("slot survives the next load", "    s_slot = 0u;\n    if (descriptor < 0)",
     "    if (descriptor < 0)"),
    ("store before Manager::Init cleared the slot",
     "if (manager == 0u || launch_args_ld32(game_global) == 0u) {",
     "if (manager == 0u || (game_global & 0u) != 0u) {"),
    ("store a different value", "launch_args_st32(field, (uint32_t)s_slot);\n        s_slot_applied = 1;",
     "launch_args_st32(field, (uint32_t)s_slot + 1u);\n        s_slot_applied = 1;"),
    ("wrong field offset",
     "        field = manager + ISAAC_VITA_LAUNCH_ARGS_MANAGER_SLOT;\n        was = launch_args_ld32(field);\n        launch_args_st32",
     "        field = manager + 0x14u;\n        was = launch_args_ld32(field);\n        launch_args_st32"),
    ("first store repeated", "        s_slot_applied = 1;\n", "\n"),
    ("waited not counted", "            ++s_slot_waited;\n", "\n"),
    ("restore never fires", "    if (was == (uint32_t)s_slot)\n        return 0;",
     "    if (was != (uint32_t)s_slot || 1)\n        return 0;"),
    ("restore always stores", "    if (was == (uint32_t)s_slot)\n        return 0;", ""),
    ("restore log cap off by one", "if (s_slot_restored <= ISAAC_VITA_LAUNCH_ARGS_SLOT_LOG_MAX)",
     "if (s_slot_restored < ISAAC_VITA_LAUNCH_ARGS_SLOT_LOG_MAX)"),
    ("teardown reads Manager 0", "    if (manager == 0u)\n        return 0;\n    field", "    field"),
    ("applied receipt drops the trigger", "waited=%u trigger=%s", "waited=%u trigger=%.0s"),
    ("receipt loses the slot field", "tokens=%u slot=%u argc=%u", "tokens=%u slot=%u0 argc=%u"),
]


def require(ok: bool, what: str) -> None:
    if not ok:
        raise AssertionError(what)


def find_cc(explicit: str | None) -> str:
    if explicit:
        return explicit
    for p in (r"C:\Program Files\LLVM\bin\clang.exe", r"C:\Program Files (x86)\LLVM\bin\clang.exe"):
        if Path(p).exists():
            return p
    for name in ("clang", "gcc", "cc"):
        found = shutil.which(name)
        if found:
            return found
    raise AssertionError("no C compiler: pass --cc")


def pe_reader(pe: bytes):
    e = struct.unpack_from("<I", pe, 0x3C)[0]
    count = struct.unpack_from("<H", pe, e + 6)[0]
    opt = struct.unpack_from("<H", pe, e + 20)[0]
    sections = []
    off = e + 24 + opt
    for _ in range(count):
        vsize, va, rsize, rp = struct.unpack_from("<IIII", pe, off + 8)
        sections.append((va, max(vsize, rsize), rp))
        off += 40

    def read(rva: int, size: int) -> bytes:
        for va, span, rp in sections:
            if va <= rva < va + span:
                return pe[rp + rva - va: rp + rva - va + size]
        raise AssertionError(f"RVA {rva:#x} outside every section")
    return read


def verify_frozen(pe_path: Path) -> None:
    pe = pe_path.read_bytes()
    require(hashlib.sha256(pe).hexdigest() == PE_SHA256, "frozen PE changed")
    read = pe_reader(pe)
    for rva, (hexbytes, what) in PINS.items():
        want = bytes.fromhex(hexbytes)
        require(read(rva, len(want)) == want, f"{rva:#x} changed: {what}")
    header = HEADER.read_text(encoding="utf-8")
    for name, value in CONSTANTS.items():
        m = re.search(rf"#define {name} 0x([0-9A-Fa-f]+)u", header)
        require(m is not None and int(m.group(1), 16) == value, f"{name} drifted")
    # The two globals are the absolute operands of the pinned stores.
    require(read(0x0050B974, 4) == (0x00400000 + CONSTANTS["ISAAC_VITA_LAUNCH_ARGS_MANAGER_RVA"]).to_bytes(4, "little"),
            "Manager global operand")
    require(read(0x004AD842, 4) == (0x00400000 + CONSTANTS["ISAAC_VITA_LAUNCH_ARGS_GAME_RVA"]).to_bytes(4, "little"),
            "Game global operand")
    print(f"frozen PE: {len(PINS)} instruction pins and {len(CONSTANTS)} constants OK")


def build_and_run(cc: str, module: Path, work: Path, tag: str) -> tuple[int, str]:
    exe = work / f"oracle-{tag}.exe"
    cmd = [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-O1",
           "-DISAAC_VITA_LAUNCH_ARGS_ORACLE=1", f"-I{RUNTIME}", f"-I{VITA}",
           str(ORACLE), str(module), "-o", str(exe)]
    built = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    if built.returncode != 0:
        return -1, built.stdout.decode(errors="replace")
    ran = subprocess.run([str(exe)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
    return ran.returncode, ran.stdout.decode(errors="replace")


def verify_oracle(cc: str, work: Path) -> None:
    rc, out = build_and_run(cc, MODULE, work, "base")
    print(out.strip())
    require(rc == 0 and "launch args oracle: PASS" in out, "oracle failed")
    cov = re.search(r"^coverage: (.*)$", out, re.M)
    require(cov is not None, "no coverage line")
    got = dict(kv.split("=") for kv in cov.group(1).split())
    require(list(got) == CLASSES, f"outcome classes changed: {list(got)}")
    require(all(int(v) > 0 for v in got.values()), "an outcome class was not reached")


def verify_mutations(cc: str, work: Path) -> None:
    source = MODULE.read_text(encoding="utf-8")
    killed = 0
    for i, (name, old, new) in enumerate(MUTATIONS):
        require(source.count(old) == 1, f"mutation anchor not unique/found: {name}")
        mutant = work / f"mutant{i:02d}.c"
        mutant.write_text(source.replace(old, new), encoding="utf-8", newline="\n")
        rc, out = build_and_run(cc, mutant, work, f"m{i:02d}")
        dead = rc != 0
        killed += dead
        why = "build" if rc == -1 else ("rc=%d" % rc)
        print(f"  mutation {i:2d} {'killed' if dead else 'SURVIVED'} ({why}): {name}")
        if rc == -1:
            # a mutant that does not compile proves nothing about the oracle
            raise AssertionError(f"mutant did not compile: {name}\n{out[-2000:]}")
    print(f"mutations: {killed}/{len(MUTATIONS)} killed")
    require(killed == len(MUTATIONS), "a mutation survived")


def verify_build_gate() -> None:
    cmake = CMAKE.read_text(encoding="utf-8")
    m = re.search(r'option\(ISAAC_VITA_LAUNCH_ARGS_FILE\s+"[^"]*"\s+(ON|OFF)\)', cmake)
    require(m is not None and m.group(1) == "OFF", "option missing or not default OFF")
    blocks = re.findall(r"if\(ISAAC_VITA_LAUNCH_ARGS_FILE\)(.*?)endif\(\)", cmake, re.S)
    require(len(blocks) == 1, f"expected one if(ISAAC_VITA_LAUNCH_ARGS_FILE) block, got {len(blocks)}")
    require("host_vita_launch_args.c" in blocks[0] and "ISAAC_VITA_LAUNCH_ARGS_FILE=1" in blocks[0],
            "module or define outside the gated block")
    outside = cmake.replace(blocks[0], "")
    require("host_vita_launch_args.c" not in outside.replace("host_vita_launch_args_oracle.c", ""),
            "module listed outside the gate")
    require("ISAAC_VITA_LAUNCH_ARGS_FILE=1" not in outside, "define set outside the gate")
    # Every launch-args line of the CRT is inside #if defined(ISAAC_VITA_LAUNCH_ARGS_FILE).
    stack: list[bool] = []
    hits = 0
    for n, line in enumerate(CRT.read_text(encoding="utf-8").splitlines(), 1):
        s = line.strip()
        if s.startswith("#if"):
            stack.append("ISAAC_VITA_LAUNCH_ARGS_FILE" in s and "!" not in s)
            continue
        if s.startswith("#else") or s.startswith("#elif"):
            if stack:
                stack[-1] = False
            continue
        if s.startswith("#endif"):
            stack.pop()
            continue
        if re.search(r"launch_args|LAUNCH_ARGS", line):
            hits += 1
            require(any(stack), f"host_vita_crt.c:{n} uses launch args outside the gate: {s}")
    require(hits >= 6, f"expected the gated CRT uses, found {hits}")
    print(f"build gate: option default OFF, one gated CMake block, {hits} gated CRT lines")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--pe", type=Path)
    ap.add_argument("--cc")
    ap.add_argument("--keep", type=Path)
    a = ap.parse_args()
    if a.pe:
        verify_frozen(a.pe)
    else:
        print("frozen PE: skipped (no --pe)")
    verify_build_gate()
    cc = find_cc(a.cc)
    work = a.keep or Path(tempfile.mkdtemp(prefix="launch-args-"))
    work.mkdir(parents=True, exist_ok=True)
    try:
        verify_oracle(cc, work)
        verify_mutations(cc, work)
    finally:
        if not a.keep:
            shutil.rmtree(work, ignore_errors=True)
    print("test_vita_launch_args: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
