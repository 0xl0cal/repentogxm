#!/usr/bin/env python3
"""Proof of ISAAC_VITA_SFX_FLOOR_PRELOAD (runtime/host_vita_sfx_floor_preload.c).

1. Frozen inputs: SFXManager::Play's prefix up to its join point, the whole
   SoundEffect::Load body, Level::Init's prologue and its m_Stage/m_StageType
   log pushes, and one Play call site, pinned against the frozen PE; every
   instruction the module cites is decoded from the PE (capstone) and found
   at its cited corpus line; the three format strings are read from the PE;
   the module's #define values are compared with the listing.
2. Corpus contract: Level::Init, Load and Play defined once; no direct
   sub_00311e60(c) inside Level::Init's owner (GNU ld --wrap would miss
   it); the direct callers elsewhere and the guest_table.c slot; Play and
   Load carry no Vita seam; Load leaves with plain `ret` only (MSVC thiscall
   is callee-clean, so Load takes no stack arguments) and reads no [ebp+N]
   argument slot; 635 of the 649 Play call sites build ECX as +0x29de0.
3. Differential oracle (MSVC x86, /LARGEADDRESSAWARE): the translated Play
   prefix against the native bounds test and against the whole wrapper,
   GPR-local and memory-mode bodies, reserve 8 and 0 MiB, pinned result
   lines and branch coverage.
4. Mutants: ten single edits of the module, each must make the oracle fail.
5. Production flavours: the module as the Vita build compiles it (host
   compiler, stub SDK headers, the raw-allocator poison header), RESERVE_MB 8
   and 0, -Wall -Wextra -Werror; with --arm-cc also the ARM object and its
   symbol table (the gate of an ARM build).
6. Build gate: default OFF, prerequisites, the UPDATE_PROBE exclusion, the
   owner census and the single --wrap; OFF adds nothing.
7. ISAAC_VITA_SFX_FIGHT_PRELOAD (the extension): three more oracle legs with
   their own pins, eight fight mutants, production/ARM flavours with the
   define (the ARM object's symbol set must equal the floor-only one), the
   build gate (OFF, FATAL without the floor knob, one define on one unit),
   and the s_fight table against tools/bench/sfx_fight_evidence.json: ids,
   order and tiers re-derived by tools/bench/sfx_fight_list.py's rule, sizes,
   SFXP_FIGHT_KIB.  --packed recomputes every row of the three lists from the
   stock archives; --artifacts regenerates the evidence from the device logs
   (not in the repository: the bench and Downpour logs are found by the
   evidence's sha256 pins).

usage: test_vita_sfx_floor_preload.py --pe PE --generated-dir DIR
           [--cc "clang -m32"] [--arm-cc arm-vita-eabi-gcc] [--keep DIR]
           [--packed RESOURCES/packed] [--artifacts DEVICE_LOG_ROOT]
"""
from __future__ import annotations

import argparse
import hashlib
import os
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
RUNTIME = HERE / "runtime"
VITA = HERE / "vita"
VITA_CMAKE = VITA / "CMakeLists.txt"
MODULE = RUNTIME / "host_vita_sfx_floor_preload.c"
ORACLE = RUNTIME / "host_vita_sfx_floor_preload_oracle.c"
TOOLS_BENCH = HERE.parent / "tools" / "bench"
EVIDENCE = TOOLS_BENCH / "sfx_fight_evidence.json"
sys.path.insert(0, str(HERE))

# build_all.py:33 -> gen_all.py:43/52 -> build_one.py:15 -> emit.py:13 (`from
# capstone import x86`) and funcs.py:41-42 (`import capstone`, `import
# pefile`) run at import time, so every mode needs both modules, --skip-oracle
# included.  A Linux build host's system python3 may have neither; a venv
# with the requirements.txt pins has both.  Name the cause here instead of a
# traceback from emit.py.
try:
    import capstone  # noqa: E402
    import pefile  # noqa: E402
except ImportError as missing:
    sys.exit(f"FAIL: {sys.executable} cannot import {missing.name}: this test "
             f"needs capstone and pefile (requirements.txt) in every mode, "
             f"--skip-oracle included; run it with the python of a venv "
             f"that has the requirements.txt pins")

import build_all  # noqa: E402
from test_vita_kage_quad_fastpath import (  # noqa: E402
    BODY_RE, pe_section_reader, require, run)

PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
PE_BASE = 0x00400000
GUEST_BASE = 0x98000000
# {start_rva: (end_rva, sha256 of the PE bytes)}
PINS = {
    # Play: prologue .. L_00488ea4 (the join after the lazy load)
    0x00488DE0: (0x00488EA4,
                 "8092600074e6b380190ef9c0599b01e0ac305703ec3b06f385cfedcbcb0997d1"),
    # SoundEffect::Load, whole body
    0x00488070: (0x00488212,
                 "a462f7b39ecaa6da40a8d898f3c0ea531d1b91a1e0d17d43c8d3de1994d16d90"),
    # Level::Init prologue through mov edi, ecx
    0x00311E60: (0x00311EA0,
                 "5668a858e5eb2cd50a40387b677580af333a3c386a5f05c0ae115ba665a65050"),
    # push [ebp-0x50]; push [edi+4]; push [edi]; push "Level::Init m_Stage.."
    0x00312153: (0x00312160,
                 "2f590e486c3529cbf091b79bf0468ac9bca874154f8a52e6ff57728af12b43aa"),
    # a Play call site: mov ecx, [g_Manager]; lea ecx, [ecx+0x29de0]; call
    0x0038A4A4: (0x0038A4B5,
                 "ce2508f4aec66c6e8f392111e1b4cfa6d7b05210c40d8037c0ab24f17465908c"),
}
# (unit, line, rva, instruction as the corpus prints it): every instruction
# the module cites by address or file:line.
CITED = [
    ("guest_0139.c", 21597, 0x00488E08, "mov ebx, ecx"),
    ("guest_0139.c", 21609, 0x00488E23, "mov edi, dword ptr [ebp + 8]"),
    ("guest_0139.c", 21617, 0x00488E31, "mov eax, 0x94f2095"),
    ("guest_0139.c", 21627, 0x00488E3A, "sar edx, 4"),
    ("guest_0139.c", 21646, 0x00488E4C, "imul esi, edi, 0x1b8"),
    ("guest_0139.c", 21655, 0x00488E54, "cmp dword ptr [esi + 0x190], 0"),
    ("guest_0139.c", 21692, 0x00488E81, "cmp byte ptr [esi + 0x198], 0"),
    ("guest_0139.c", 21695, 0x00488E8A, "mov ecx, esi"),
    ("guest_0139.c", 21697, 0x00488E8C, "call 0x488070"),
    ("guest_0139.c", 21712, 0x00488EA1, "inc dword ptr [ebx + 0xc]"),
    ("guest_0139.c", 19383, 0x0048812B, "cmp byte ptr [0x987e7d44], 0"),
    ("guest_0139.c", 19537, 0x004881E3, "mov byte ptr [esi + 0x198], 1"),
    ("guest_0139.c", 19572, 0x00488205, "ret"),
    ("guest_0083.c", 22805, 0x00311E9E, "mov edi, ecx"),
    ("guest_0083.c", 23173, 0x00312156, "push dword ptr [edi + 4]"),
    ("guest_0083.c", 23175, 0x00312159, "push dword ptr [edi]"),
    ("guest_0083.c", 23189, 0x0031215B, "push 0x9874c03c"),
    ("guest_0083.c", 25377, 0x00312EA9, "ret"),
    ("guest_0103.c", 9571, 0x0038A4A4, "mov ecx, dword ptr [0x987fd680]"),
    ("guest_0103.c", 9573, 0x0038A4AA, "lea ecx, [ecx + 0x29de0]"),
    ("guest_0103.c", 9575, 0x0038A4B0, "call 0x488de0"),
]
STRINGS = {
    0x0074C03C: b"Level::Init m_Stage %d, m_StageType %d Seed %u\n",
    0x00757A6C: b"[warn] Sound %d has no samples.\n",
    0x00757A48: b"[warn] Sound %d was not preloaded.\n",
}
DEFINES = {
    "SFXP_G_MANAGER": 0x007FD680, "SFXP_SFX_MANAGER": 0x00029DE0,
    "SFXP_DIV_MAGIC": 0x094F2095, "SFXP_DIV_SHIFT": 4,
    "SFXP_ENTRY_BYTES": 0x1B8, "SFXP_ENTRY_SAMPLES": 0x190,
    "SFXP_ENTRY_LOADED": 0x198, "SFXP_LOADED_COUNT": 0xC,
    "SFXP_LOAD_RVA": 0x00488070, "SFXP_RET_LOAD": 0x00488E91,
    "SFXP_AUDIO_RUNNING": 0x007E7D44, "SFXP_LEVEL_STAGE": 0x0,
    "SFXP_LEVEL_TYPE": 0x4, "SFXP_WATER_TYPE": 4,
    "SFXP_WATER_STAGE_MIN": 1, "SFXP_WATER_STAGE_MAX": 2,
    "SFXP_HEAP_TOTAL": 0x05100000, "SFXP_POOL_RESERVE_BYTES": 0x00200000,
}
# Level::Init's direct callers outside its owner (they reach __wrap_).
# Measured: grep -c '[^_a-zA-Z0-9]sub_00311e60(c)' guest_*.c, 2026-10-25.
LEVEL_INIT_CALLERS = {"guest_0062.c": 2, "guest_0073.c": 6, "guest_0075.c": 1,
                      "guest_0140.c": 1}
PLAY_CALLEES = {"sub_003a2950", "sub_00488070", "sub_0055e330",
                "sub_0056e9a0", "sub_0056e9d0", "sub_0056eb70",
                "sub_0056f1c0", "sub_005eb079", "sub_005eb225",
                "sub_005eb26f"}

COMMON = ["/nologo", "/W4", "/WX", "/O2", "/Gy", "/std:c11",
          "/wd4310", "/wd4702", "/wd4996", "/DGUEST_IMAGE_BASE=0x98000000u"]
DISPATCH_DEFINES = ["/DGUEST_STACK_REQUIRED=1",
                    "/DISAAC_VITA_IMPORT_ID_DISPATCH=1",
                    "/DISAAC_VITA_GUEST_LOOKUP_CACHE=1",
                    "/DISAAC_VITA_PHASE_PROFILE=1",
                    "/DISAAC_VITA_PROFILE_IMPORT_KINDS=1"]
# recomp/vita/CMakeLists.txt compile definitions of a generated unit.
BODY_DEFINES = ["/DGUEST_STACK_REQUIRED=1", "/DGUEST_GENERATED_STACK_GUARD=0",
                "/DGUEST_FLAGS_LOCAL=1", "/DGUEST_COVERAGE_HOOKS=0",
                "/DISAAC_VITA_IMPORT_ID_DISPATCH=1",
                "/DISAAC_VITA_GUEST_LOOKUP_CACHE=1",
                "/DISAAC_VITA_PHASE_PROFILE=1",
                "/DISAAC_VITA_PROFILE_IMPORT_KINDS=1"]
# Pinned on the first green run (2026-10-25); a change here is a behaviour
# change of the module or of the oracle and must be explained.
# 2026-10-26: re-pinned after the oracle's Part B pool draw was
# fixed.  It was (r >> 24) % 5u on a 24-bit r, always 0, so random worlds
# never met the pool kinds 1..4 (was: loads=10795/10905, declines(h,p)=
# 3293,1990 / 3183,1990, coverage B replayed=2144 ...).  Now pool(exact,
# short,reserve,fallback)=129,145,120,898 and 3161/3176 pool declines; the
# module is unchanged, and the fight legs (which drew lcg() % 5u already)
# reproduce their pins.
ORACLE_RESULTS = {
    8: "Vita SFX floor preload oracle: PASS; reserve_mb=8; A=11250; B=3000; "
       "loads=9095; declines(h,p)=3309,3161",
    0: "Vita SFX floor preload oracle: PASS; reserve_mb=0; A=11250; B=3000; "
       "loads=9144; declines(h,p)=3245,3176",
}
# Identical in every leg (the worlds do not depend on the reserve or on the
# body flavour): Part A classes and Part B gates.
ORACLE_COVERAGE = [
    "coverage A: cases=11250 range=7296 (neg 3294) no_samples=688 loaded=1988 "
    "load=1278 boundary=928",
    "coverage B: cases=3000 replayed=2096 fault=147 eager=151 audio_off=119 "
    "terminal=118 no_manager=369 (null 128, begin 113, reversed 128) water=671 "
    "dry=1425 rewritten=535",
]
# The same oracle with ISAAC_VITA_SFX_FIGHT_PRELOAD (its own case draws, so
# its own pins), first green run 2026-10-26.  Here the worlds do depend on
# the reserve: a whole-list decline skips the reference's Play calls and
# their random draws, so each reserve has its own B and F coverage.
FIGHT_ORACLE_RESULTS = {
    8: "Vita SFX floor preload oracle: PASS; reserve_mb=8; fight=22; A=11250; "
       "B=3000; loads=17121; declines(h,p)=2886,2480; fight(w,h,p,s)=823,624,432,182",
    0: "Vita SFX floor preload oracle: PASS; reserve_mb=0; fight=22; A=11250; "
       "B=3000; loads=17006; declines(h,p)=2892,2380; fight(w,h,p,s)=882,554,430,173",
}
FIGHT_ORACLE_COVERAGE = {
    8: [
        ORACLE_COVERAGE[0],
        "coverage B: cases=3000 replayed=2381 fault=83 eager=88 audio_off=90 "
        "terminal=94 no_manager=264 (null 99, begin 80, reversed 85) water=764 "
        "dry=1617 rewritten=577",
        "coverage F: walked=823 (nothing loadable 83) heap=624 pool=432 stopped=182 "
        "(fault 84, breach 98) loads=8734 walk_declines=88 after_floor_decline=813 "
        "heap(exact,short,reserve)=93,95,103 pool(exact,short,margin,fallback)="
        "30,32,31,370",
    ],
    0: [
        ORACLE_COVERAGE[0],
        "coverage B: cases=3000 replayed=2319 fault=102 eager=87 audio_off=87 "
        "terminal=108 no_manager=297 (null 108, begin 97, reversed 92) water=729 "
        "dry=1590 rewritten=597",
        "coverage F: walked=882 (nothing loadable 101) heap=554 pool=430 stopped=173 "
        "(fault 88, breach 85) loads=9154 walk_declines=102 after_floor_decline=750 "
        "heap(exact,short,reserve)=101,85,98 pool(exact,short,margin,fallback)="
        "30,28,45,383",
    ],
}

# (name, old, new): one edit each; every one must make the oracle fail.
MUTANTS = [
    ("no loaded-count increment",
     "        st32(sfx + SFXP_LOADED_COUNT, ld32(sfx + SFXP_LOADED_COUNT) + 1U);\n",
     "        (void)sfx;\n"),
    ("wrong return word",
     "#define SFXP_RET_LOAD 0x00488E91U", "#define SFXP_RET_LOAD 0x00488E8CU"),
    ("lazy flags not restored", "    c->fl = s->fl;\n", ""),
    ("last error not restored", "    c->last_error = s->last_error;\n", ""),
    ("range test off by one", "if (id < 0 || id >= count) {",
     "if (id < 0 || id > count) {"),
    ("water stage 3 included", "#define SFXP_WATER_STAGE_MAX 2U",
     "#define SFXP_WATER_STAGE_MAX 3U"),
    # (the per-sound rules; anchored on the line after them since the fight
    # extension's whole-list guard spells the same comparisons)
    ("headroom off by one",
     "room - reserve < need) {\n            ++s_counters.skip[SFXP_SKIP_HEADROOM];",
     "room - reserve <= need) {\n            ++s_counters.skip[SFXP_SKIP_HEADROOM];"),
    ("OpenAL pool not checked",
     "if (pool < pool_reserve || pool - pool_reserve < need) {",
     "if (0) {"),
    ("OpenAL pool margin off by one",
     "pool - pool_reserve < need) {\n            ++s_counters.skip[SFXP_SKIP_POOL];",
     "pool - pool_reserve <= need) {\n            ++s_counters.skip[SFXP_SKIP_POOL];"),
    ("stage and type swapped", "#define SFXP_LEVEL_STAGE 0x0U",
     "#define SFXP_LEVEL_STAGE 0x4U"),
    ("no ESP check after Load", "    if (c->esp != saved) {",
     "    if (0) {"),
    ("replays after a faulted Level::Init",
     "    if (c->fault) {\n        ++s_counters.level[SFXP_LEVEL_FAULT];\n        return;\n    }\n",
     "    if (0) {\n        ++s_counters.level[SFXP_LEVEL_FAULT];\n        return;\n    }\n"),
    ("loaded byte ignored", "    if (ld8(entry + SFXP_ENTRY_LOADED) != 0U) {",
     "    if (0) {"),
]
# ISAAC_VITA_SFX_FIGHT_PRELOAD: single edits of the extension, each must make
# the fight oracle (reserve 8) fail.
FIGHT_MUTANTS = [
    ("fight heap guard off by one",
     "    if (loadable != 0U && (room < reserve || room - reserve < need)) {",
     "    if (loadable != 0U && (room < reserve || room - reserve <= need)) {"),
    ("fight guard applied to an empty list",
     "    if (loadable != 0U && (room < reserve || room - reserve < need)) {",
     "    if ((room < reserve || room - reserve < need)) {"),
    ("fight pool guard dropped",
     "    if (loadable != 0U && (pool < pool_reserve || pool - pool_reserve < need)) {",
     "    if (0) {"),
    ("fight need in the wrong unit",
     "            need += (uint32_t)s_fight[i].kib * 1024U;",
     "            need += (uint32_t)s_fight[i].kib * 1000U;"),
    ("fight registers not restored",
     "    sfxp_restore(c, &saved);\n    hr1 = sfxp_headroom();",
     "    hr1 = sfxp_headroom();"),
    ("fight stop ignored",
     "    if (!sfxp_walk(c, sfx, s_fight, n)) {",
     "    if (!sfxp_walk(c, sfx, s_fight, n) && 0) {"),
    ("fight skipped-sound mask off by one", "            mask |= 1U << i;",
     "            mask |= 2U << i;"),
    ("fight decline line without the reserve",
     "(unsigned)((need + reserve) >> 10), (unsigned)loadable,",
     "(unsigned)(need >> 10), (unsigned)loadable,"),
]

# Production compile: stand-ins for the SDK headers the module includes.
STUB_HEADERS = {
    "malloc.h": "#pragma once\n#include <stddef.h>\n"
                "struct mallinfo { size_t arena, ordblks, smblks, hblks, hblkhd,"
                " usmblks, fsmblks, uordblks, fordblks, keepcost; };\n"
                "struct mallinfo mallinfo(void);\n",
    "psp2/kernel/processmgr.h": "#pragma once\n#include <stdint.h>\n"
                                "uint64_t sceKernelGetProcessTimeWide(void);\n",
}
EBOOT_FLAGS = [
    "-std=gnu11", "-O2", "-mcpu=cortex-a9", "-mfpu=neon",
    "-mfloat-abi=softfp", "-mthumb", "-fno-strict-aliasing",
    "-ffunction-sections", "-fdata-sections", "-Wall", "-Wextra", "-Werror",
    "-DGUEST_IMAGE_BASE=0x98000000u", "-DGUEST_STACK_REQUIRED=1",
    "-DISAAC_VITA_HAS_RUNTIME=1", "-DISAAC_VITA_HEAP_MB=64",
    "-DISAAC_VITA_RAW_ALLOCATOR_GATE=1",
]


# ---- 1. frozen inputs ---------------------------------------------------------

def capstone_text(read, rva: int) -> str:
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    insn = next(md.disasm(read(rva, 16), rva))
    return f"{insn.mnemonic} {insn.op_str}".strip()


def pe_form(text: str) -> str:
    """The corpus prints data VAs rebased to 0x98000000 and branch targets
    as RVAs; capstone at address=rva prints data VAs at the PE's base."""
    return re.sub(r"0x98([0-9a-f]{6})\b",
                  lambda m: hex(PE_BASE + int(m.group(1), 16)), text)


def verify_frozen(pe_path: Path, generated: Path, loose: bool) -> None:
    pe = pe_path.read_bytes()
    require(hashlib.sha256(pe).hexdigest() == PE_SHA256, "frozen PE changed")
    read = pe_section_reader(pe)
    for rva, (end, sha) in PINS.items():
        require(hashlib.sha256(read(rva, end - rva)).hexdigest() == sha,
                f"bytes changed at {rva:#x}")
    # capstone is importable here (checked at the top), so the decode leg
    # runs in every mode, a Linux build host's --skip-oracle run included
    units: dict[str, list[str]] = {}
    for unit, line, rva, text in CITED:
        decoded = capstone_text(read, rva)
        require(decoded == pe_form(text),
                f"{rva:#x}: PE decodes {decoded!r}, cited {text!r}")
        # "/* rva  mnemonic operands */"; an operand-less one keeps the
        # empty operand field: "/* 00488205  ret  */"
        forms = (f"/* {rva:08x}  {text} */", f"/* {rva:08x}  {text}  */")
        if loose:
            # another generation of the corpus: the comment must exist in
            # some unit; the module's file:line citations name the frozen one
            found = any(form in path.read_text(encoding="utf-8")
                        for path in generated.glob("guest_[0-9][0-9][0-9][0-9].c")
                        for form in forms)
            require(found, f"{text!r} at {rva:#x} not in the corpus")
            continue
        lines = units.setdefault(unit, (generated / unit).read_text(
            encoding="utf-8").splitlines())
        got = lines[line - 1].strip()
        require(got in forms,
                f"{unit}:{line} is {got!r}, cited {text!r} at {rva:#x}")
    for rva, value in STRINGS.items():
        require(read(rva, len(value) + 1) == value + b"\0",
                f"string at {rva:#x} changed")
    # call 0x488070 at 0x488e8c returns to 0x488e91
    b = read(0x00488E8C, 5)
    require(b[0] == 0xE8 and 0x00488E91 + int.from_bytes(
        b[1:], "little", signed=True) == 0x00488070, "Load call identity")
    text = MODULE.read_text(encoding="utf-8")
    for name, value in DEFINES.items():
        m = re.search(rf"#define {name} (0x[0-9A-Fa-f]+|[0-9]+)U$", text, re.M)
        require(m is not None and int(m.group(1), 0) == value,
                f"{name} drifted from the listing")
    print(f"frozen inputs: PE sha256, {len(PINS)} byte pins, {len(CITED)} cited "
          f"instructions (PE decode == corpus "
          f"{'comment, any line' if loose else 'line'}), "
          f"{len(STRINGS)} strings, "
          f"{len(DEFINES)} module constants: PASS")


# ---- 2. corpus contract ------------------------------------------------------

def extract(generated: Path, loose: bool) -> str:
    owners: dict[str, list[str]] = {n: [] for n in
                                    ("sub_00311e60", "sub_00488070",
                                     "sub_00488de0")}
    bodies: dict[str, str] = {}
    callers: dict[str, int] = {}
    table_slot = False
    for unit in sorted(generated.glob("guest_[0-9][0-9][0-9][0-9].c")):
        text = unit.read_text(encoding="utf-8")
        for name in owners:
            if f"\nvoid {name}(CPU *__restrict c)\n" in text:
                owners[name].append(unit.name)
                for match in BODY_RE.finditer(text):
                    if match.group(1) == name:
                        bodies[name] = match.group(0)
        n = len(re.findall(r"[^_a-zA-Z0-9]sub_00311e60\(c\)", text))
        if n:
            callers[unit.name] = n
    table = generated / "guest_table.c"
    if table.is_file():
        table_slot = "sub_00311e60" in table.read_text(encoding="utf-8")
    for name, units in owners.items():
        require(len(units) == 1 and name in bodies, f"{name}: owners {units}")
    owner = owners["sub_00311e60"][0]
    require(loose or owner == "guest_0083.c", f"Level::Init owner moved: {owner}")
    require(owner not in callers,
            "a direct sub_00311e60(c) inside its owner bypasses --wrap")
    # another corpus generation may partition the units differently; the
    # call edges themselves are the PE's
    require(callers == LEVEL_INIT_CALLERS if not loose else
            sum(callers.values()) == sum(LEVEL_INIT_CALLERS.values()),
            f"Level::Init direct callers changed: {callers}")
    require(table_slot, "guest_table.c no longer holds the Level::Init slot")
    for name in ("sub_00488070", "sub_00488de0"):
        require("__vita__" not in bodies[name], f"{name}: unexpected Vita seam")
    load = bodies["sub_00488070"]
    own = re.findall(r"/\* (00488[0-2][0-9a-f]{2})  ret(.*?) \*/", load)
    require(own and all(not op.strip() for _, op in own),
            f"Load returns: {own}")
    require(not re.search(r"\[ebp \+ (?:0x[0-9a-f]+|[0-9]+)\]",
                          "\n".join(re.findall(r"/\* 00488[0-2][0-9a-f]{2}  (.*?) \*/",
                                               load))),
            "Load reads an argument slot [ebp+N]")
    # Play call sites: how ECX is built (informational census, pinned)
    com = re.compile(r"/\* ([0-9a-f]{8})  (.*?) \*/")
    sites: dict[str, str | None] = {}
    for unit in sorted(generated.glob("guest_[0-9][0-9][0-9][0-9].c")):
        lines = [m.groups() for m in com.finditer(unit.read_text(encoding="utf-8"))]
        for i, (a, ins) in enumerate(lines):
            if ins == "call 0x488de0":
                setup = None
                for _, prev in reversed(lines[max(0, i - 14):i]):
                    if re.match(r"(lea|mov) ecx, ", prev):
                        setup = prev
                        break
                sites.setdefault(a, setup)
    plus = sum(1 for s in sites.values() if s and "+ 0x29de0]" in s)
    require((plus, len(sites)) == (635, 649),
            f"Play call sites: {plus} of {len(sites)} use +0x29de0")
    play = bodies["sub_00488de0"]
    callees = set(re.findall(r"\b(sub_[0-9a-f]{8})\(c\)", play)) - {"sub_00488de0"}
    require(callees == PLAY_CALLEES, f"Play callees changed: {sorted(callees)}")
    print(f"corpus contract: Level::Init owner {owner} with no inside call, "
          f"callers {sum(callers.values())} in {len(callers)} units + table slot; "
          f"Load plain ret x{len(own)}, no argument reads; Play sites "
          f"{plus}/{len(sites)} +0x29de0: PASS")
    return play


# ---- 3. oracle ----------------------------------------------------------------

def bodies_source(play: str) -> str:
    label = "\nL_00488ea4:\n"
    require(play.count(label) == 1, "Play lost its L_00488ea4 join")
    play = play.replace(label, label +
                        "    GUEST_GPR_FLUSH(c); oracle_play_ea4(c); return;\n")
    out = ["/* test_vita_sfx_floor_preload.py: SFXManager::Play from the frozen",
           " * corpus, stopped at L_00488ea4 */", '#include "guest.h"',
           "void oracle_play_ea4(CPU *__restrict c);"]
    out += [f"void {name}(CPU *__restrict c);" for name in sorted(PLAY_CALLEES)]
    out.append(play)
    return "\n".join(out) + "\n"


def compile_bodies(env, compiler: str, work: Path, play: str) -> dict[str, Path]:
    src = work / "sfxp_play_bodies.c"
    src.write_text(bodies_source(play), encoding="utf-8", newline="\n")
    objs = {}
    for flavour, gpr in (("gpr", "1"), ("mem", "0")):
        obj = work / f"sfxp_play_{flavour}.obj"
        run([compiler, "/nologo", "/W3", "/O2", "/std:c11", "/wd4310",
             "/wd4996", "/DGUEST_IMAGE_BASE=0x98000000u",
             f"/DGUEST_GPR_LOCAL={gpr}", *BODY_DEFINES,
             "/Dsub_00488de0=real_sub_00488de0",
             "/I", str(RUNTIME), "/c", str(src), "/Fo:" + str(obj)], env)
        objs[flavour] = obj
    return objs


def build_oracle(env, compiler: str, work: Path, tag: str, source_dir: Path,
                 bodies_obj: Path, reserve: int, fight: bool = False) -> Path:
    exe = work / f"sfxp-oracle-{tag}.exe"
    objdir = work / f"obj-{tag}"
    objdir.mkdir(exist_ok=True)
    run([compiler, *COMMON, *DISPATCH_DEFINES,
         "/DISAAC_VITA_SFX_FLOOR_PRELOAD=1",
         "/DISAAC_VITA_SFX_FLOOR_PRELOAD_ORACLE=1",
         f"/DISAAC_VITA_SFX_FLOOR_PRELOAD_RESERVE_MB={reserve}",
         *(["/DISAAC_VITA_SFX_FIGHT_PRELOAD=1"] if fight else []),
         "/I", str(source_dir), "/I", str(RUNTIME),
         str(source_dir / ORACLE.name), str(bodies_obj),
         "/Fe:" + str(exe), "/Fo:" + str(objdir) + os.sep,
         "/link", "/LARGEADDRESSAWARE", "/OPT:REF", "/INCREMENTAL:NO"], env)
    return exe


def run_oracle(env, exe: Path) -> tuple[int, str]:
    completed = subprocess.run([str(exe)], env=env, stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT, timeout=600)
    return completed.returncode, completed.stdout.decode(
        "cp866", errors="replace").replace("\r\n", "\n")


def oracle_legs(env, compiler: str, work: Path, objs: dict[str, Path]) -> list[str]:
    results = []
    for fight in (False, True):
        pins = FIGHT_ORACLE_RESULTS if fight else ORACLE_RESULTS
        for flavour, reserve in (("gpr", 8), ("mem", 8), ("gpr", 0)):
            cover = FIGHT_ORACLE_COVERAGE[reserve] if fight else ORACLE_COVERAGE
            tag = f"{'fight-' if fight else ''}{flavour}-r{reserve}"
            exe = build_oracle(env, compiler, work, tag, RUNTIME, objs[flavour],
                               reserve, fight)
            rc, out = run_oracle(env, exe)
            (work / f"oracle-{tag}.out").write_text(out, encoding="utf-8")
            lines = out.strip().splitlines()
            result = lines[-1] if lines else ""
            if rc != 0 or result != pins[reserve] or \
                    any(line not in lines for line in cover):
                print(out[-6000:])
                raise AssertionError(f"oracle {tag}: rc {rc:#x}, {result!r}")
            for line in lines:
                if line.startswith(("coverage", "log lines", "fight log lines")):
                    print(f"  [{tag}] {line}")
            results.append(f"[{tag}] {result}")
    return results


# ---- 4. mutants ---------------------------------------------------------------

def mutants(env, compiler: str, work: Path, objs: dict[str, Path]) -> int:
    base = MODULE.read_text(encoding="utf-8")
    killed = 0
    every = [(name, old, new, False) for name, old, new in MUTANTS] + \
            [(name, old, new, True) for name, old, new in FIGHT_MUTANTS]
    for index, (name, old, new, fight) in enumerate(every):
        require(base.count(old) == 1, f"mutant {name!r}: anchor not unique")
        mdir = work / f"mutant-{index}"
        mdir.mkdir()
        (mdir / MODULE.name).write_text(base.replace(old, new), encoding="utf-8",
                                        newline="\n")
        shutil.copyfile(ORACLE, mdir / ORACLE.name)
        exe = build_oracle(env, compiler, work, f"mutant-{index}", mdir,
                           objs["gpr"], 8, fight)
        rc, out = run_oracle(env, exe)
        lines = out.strip().splitlines()
        result = lines[-1] if lines else ""
        fails = [line for line in lines if line.startswith("FAIL ")]
        require(rc != 0 and not result.startswith(
            "Vita SFX floor preload oracle: PASS"),
            f"mutant {name!r} survived: {result!r}")
        killed += 1
        print(f"  mutant {index} ({name}): killed, first: "
              f"{fails[0] if fails else result}")
    return killed


# ---- 5. production flavours ----------------------------------------------------

def default_cc() -> list[str]:
    if os.environ.get("CC"):
        return shlex.split(os.environ["CC"])
    for candidate in ("clang", "gcc", "cc"):
        found = shutil.which(candidate)
        if found:
            return [found, "-m32"]
    if os.name == "nt":
        installed = Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / \
            "LLVM" / "bin" / "clang.exe"
        if installed.is_file():
            return [str(installed), "-m32"]
    raise AssertionError("no host C compiler; pass --cc")


def production_flavours(cc: list[str], work: Path) -> None:
    stubs = work / "stub-include"
    for name, text in STUB_HEADERS.items():
        path = stubs / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8", newline="\n")
    for fight in (False, True):
        for reserve in (8, 0):
            run([*cc, "-std=gnu11", "-fsyntax-only", "-Wall", "-Wextra", "-Werror",
                 "-Wtype-limits", "-DGUEST_IMAGE_BASE=0x98000000u",
                 "-DGUEST_STACK_REQUIRED=1", "-DISAAC_VITA_SFX_FLOOR_PRELOAD=1",
                 f"-DISAAC_VITA_SFX_FLOOR_PRELOAD_RESERVE_MB={reserve}",
                 '-DISAAC_VITA_SFX_FLOOR_PRELOAD_BUILD_ID="sfxp:test"',
                 *(["-DISAAC_VITA_SFX_FIGHT_PRELOAD=1"] if fight else []),
                 "-I", str(stubs), "-I", str(RUNTIME), "-I", str(VITA), str(MODULE)],
                os.environ.copy())
            print(f"production flavour reserve_mb={reserve}{' fight' if fight else ''} "
                  f"({Path(cc[0]).name}, -Wall -Wextra -Wtype-limits -Werror): PASS")
    # The raw-allocator poison header only accepts the gated Vita target
    # (arm_cross force-includes it); here the identifiers are checked as text.
    code = re.sub(r"/\*.*?\*/|//[^\n]*", " ", MODULE.read_text(encoding="utf-8"),
                  flags=re.S)
    raw = re.findall(r"\b(malloc|calloc|realloc|free|memalign|aligned_alloc|"
                     r"strdup)\s*\(", code)
    require(not raw, f"the module calls raw allocators: {raw}")
    # The knob's own guard: without the define the unit refuses to compile.
    completed = subprocess.run(
        [*cc, "-std=gnu11", "-fsyntax-only", "-I", str(stubs), "-I",
         str(RUNTIME), str(MODULE)], stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT)
    require(completed.returncode != 0 and b"opt-in feature" in completed.stdout,
            "the unit compiles without ISAAC_VITA_SFX_FLOOR_PRELOAD")
    print("unit without the knob define: #error: PASS")


def arm_cross(arm_cc: str, work: Path) -> None:
    off = arm_cross_one(arm_cc, work, False)
    on = arm_cross_one(arm_cc, work, True)
    # the extension adds code and data only: no new import, no new export
    require(on == off, f"fight object changes the symbol interface: {on ^ off}")
    print(f"ARM cross-compile: fight object has the same global symbol set "
          f"(readelf -sW, {len(on)} symbols): PASS")


def readelf_globals(table: str) -> dict[str, str]:
    """{name: kind} of the GLOBAL and WEAK symbols in `readelf -sW` output:
    U undefined, T FUNC, D OBJECT, any other type by its name.  readelf
    rather than nm for Thumb objects (the workflow's rule); -W keeps GNU
    readelf from cutting names at 21 characters."""
    symbols: dict[str, str] = {}
    rows = 0
    for line in table.splitlines():
        # "   Num:    Value  Size Type    Bind   Vis      Ndx Name"; nameless
        # rows (index 0, sections) have 7 fields and carry nothing here
        parts = line.split()
        if len(parts) < 8 or not re.fullmatch(r"\d+:", parts[0]):
            continue
        rows += 1
        kind, bind, ndx, name = parts[3], parts[4], parts[-2], parts[-1]
        if bind in ("GLOBAL", "WEAK"):
            symbols[name] = "U" if ndx == "UND" else \
                {"FUNC": "T", "OBJECT": "D"}.get(kind, kind)
    require(rows > 0, "readelf -sW printed no symbol rows")
    return symbols


def arm_cross_one(arm_cc: str, work: Path, fight: bool) -> set:
    obj = work / f"host_vita_sfx_floor_preload{'-fight' if fight else ''}.o"
    run([arm_cc, *EBOOT_FLAGS, "-DISAAC_VITA_SFX_FLOOR_PRELOAD=1",
         "-DISAAC_VITA_SFX_FLOOR_PRELOAD_RESERVE_MB=8",
         '-DISAAC_VITA_SFX_FLOOR_PRELOAD_BUILD_ID="sfxp:arm"',
         *(["-DISAAC_VITA_SFX_FIGHT_PRELOAD=1"] if fight else []),
         "-include", str(VITA / "isaac_vita_raw_allocator_poison.h"),
         "-I", str(RUNTIME), "-I", str(VITA), "-c", str(MODULE), "-o", str(obj)],
        os.environ.copy())
    prefix = arm_cc[:-len("gcc")] if arm_cc.endswith("gcc") else ""
    symbols = readelf_globals(run([prefix + "readelf", "-sW", str(obj)],
                                  os.environ.copy()))
    require(symbols.get("__wrap_sub_00311e60") == "T", "__wrap_ not defined (T)")
    require(symbols.get("__real_sub_00311e60") == "U", "__real_ not undefined")
    for needed in ("sub_00488070", "isaac_vita_log", "mallinfo",
                   "sceKernelGetProcessTimeWide",
                   "isaac_vita_audio_materialization_ready",
                   "isaac_vita_guest_heap_terminal",
                   "isaac_vita_openal_pool_snapshot_get"):
        require(symbols.get(needed) == "U", f"ARM object lacks U {needed}")
    forbidden = [s for s, k in symbols.items() if k == "U" and re.fullmatch(
        r"(malloc|calloc|realloc|free|printf|sceClibPrintf|sub_00311e60|"
        r"sub_00488de0)", s)]
    require(not forbidden, f"ARM object references {forbidden}")
    print(f"ARM cross-compile{' (fight)' if fight else ''} ({arm_cc}): -Werror clean, "
          f"T __wrap_sub_00311e60, "
          f"undefined={sorted(s for s, k in symbols.items() if k == 'U')}: PASS")
    return set(symbols.items())


# ---- 6. build gate ---------------------------------------------------------------

def verify_build_gate() -> None:
    cmake = VITA_CMAKE.read_text(encoding="utf-8")
    require(re.search(r"option\(ISAAC_VITA_SFX_FLOOR_PRELOAD\s+\"[^\"]*\"\s+OFF\)",
                      cmake) is not None, "option missing or not default OFF")
    require(re.search(r"set\(ISAAC_VITA_SFX_FLOOR_PRELOAD_RESERVE_MB 8 CACHE STRING",
                      cmake) is not None, "RESERVE_MB default is not 8")
    require("ISAAC_VITA_SFX_FLOOR_PRELOAD needs the generated runtime, "
            "TRANSLATED_CPU, KAGE and AUDIO" in cmake and
            re.search(r"if\(ISAAC_VITA_SFX_FLOOR_PRELOAD AND\n"
                      r"   \(ISAAC_VITA_SCAFFOLD_ONLY OR NOT ISAAC_VITA_TRANSLATED_CPU OR\n"
                      r"    NOT ISAAC_VITA_KAGE OR NOT ISAAC_VITA_AUDIO\)\)", cmake)
            is not None, "prerequisite gate missing")
    require(re.search(r"if\(ISAAC_VITA_SFX_FLOOR_PRELOAD AND ISAAC_VITA_UPDATE_PROBE\)\n"
                      r"  message\(FATAL_ERROR", cmake) is not None,
            "UPDATE_PROBE exclusion missing")
    probe = (RUNTIME / "host_vita_update_probe.h").read_text(encoding="utf-8")
    require("X(00311e60)" in probe,
            "UPDATE_PROBE no longer wraps Level::Init: drop the exclusion")
    block = re.search(r"\n  if\(ISAAC_VITA_SFX_FLOOR_PRELOAD\)\n(.*?)\n  endif\(\)\n",
                      cmake, re.S)
    require(block is not None, "knob block missing")
    body = block.group(1)
    for needle in ('"${ISAAC_RUNTIME}/host_vita_sfx_floor_preload.c"',
                   "-Wl,--wrap=sub_00311e60",
                   "ISAAC_SFXP_OWNER_COUNT EQUAL 1",
                   "ISAAC_SFXP_LOAD_DEFS EQUAL 1",
                   "ISAAC_VITA_SFX_FLOOR_PRELOAD=1",
                   "ISAAC_VITA_SFX_FLOOR_PRELOAD_RESERVE_MB="
                   "${ISAAC_VITA_SFX_FLOOR_PRELOAD_RESERVE_MB}"):
        require(needle in body, f"knob block lacks {needle}")
    # 3 = the source list, the floor defines and the fight define (the fight
    # block nests inside this one; checked below)
    require(cmake.count('"${ISAAC_RUNTIME}/host_vita_sfx_floor_preload.c"') == 3 and
            body.count('"${ISAAC_RUNTIME}/host_vita_sfx_floor_preload.c"') == 3 and
            cmake.count("--wrap=sub_00311e60") == 2 and
            body.count("--wrap=sub_00311e60") == 1,
            "the seam must be reachable from the knob block only")
    gate = (VITA / "vita_raw_allocator_gate.py").read_text(encoding="utf-8")
    require('("ISAAC_VITA_SFX_FLOOR_PRELOAD", "host_vita_sfx_floor_preload.c")'
            in gate, "raw allocator gate does not register the new unit")
    readme = (VITA / "README.md").read_text(encoding="utf-8")
    require("ISAAC_VITA_SFX_FLOOR_PRELOAD" in readme, "README lacks the knob")
    # ISAAC_VITA_SFX_FIGHT_PRELOAD: default OFF, refused without the floor
    # knob, one compile definition on the floor unit inside the floor block,
    # no source, link option or other unit of its own.
    require(re.search(r"option\(ISAAC_VITA_SFX_FIGHT_PRELOAD\s+\"[^\"]*\"\s+OFF\)",
                      cmake) is not None, "fight option missing or not default OFF")
    require(re.search(r"if\(ISAAC_VITA_SFX_FIGHT_PRELOAD AND NOT "
                      r"ISAAC_VITA_SFX_FLOOR_PRELOAD\)\n  message\(FATAL_ERROR", cmake)
            is not None, "fight knob does not refuse a build without the floor knob")
    inner = re.search(r"\n    if\(ISAAC_VITA_SFX_FIGHT_PRELOAD\)\n(.*?)\n    endif\(\)\n",
                      body + "\n", re.S)
    require(inner is not None, "fight block missing inside the floor knob block")
    require("ISAAC_VITA_SFX_FIGHT_PRELOAD=1" in inner.group(1) and
            '"${ISAAC_RUNTIME}/host_vita_sfx_floor_preload.c"' in inner.group(1) and
            "target_link_options" not in inner.group(1) and
            "ISAAC_VITA_RUNTIME_SOURCES" not in inner.group(1),
            "fight block must only add the define to the floor unit")
    require(cmake.count("ISAAC_VITA_SFX_FIGHT_PRELOAD=1") == 1,
            "the fight define reaches another unit")
    require("ISAAC_VITA_SFX_FIGHT_PRELOAD" in readme, "README lacks the fight knob")
    print("build gate: option OFF, reserve 8, prerequisites, UPDATE_PROBE "
          "exclusion, owner census, single --wrap in the knob block, raw "
          "allocator registration, README; fight option OFF, FATAL without the "
          "floor knob, define on the floor unit only: PASS")


# ---- 7. the fight list ---------------------------------------------------------

def module_tables() -> dict[str, list[tuple[int, int, int, int]]]:
    """{table: [(id, kib, comment bytes, comment files)]} from the module."""
    text = MODULE.read_text(encoding="utf-8")
    out = {}
    for name in ("s_water", "s_generic", "s_fight"):
        m = re.search(rf"static const sfxp_sound {name}\[\] = \{{\n(.*?)\n\}};", text, re.S)
        require(m is not None, f"{name} not found")
        rows = []
        for line in m.group(1).splitlines():
            r = re.fullmatch(r"\s*\{ (\d+), (\d+) \},\s*/\*[^:]*: ([\d,]+) B"
                             r"(?:, (\d+) files)? \*/", line)
            require(r is not None, f"{name}: unparsed row {line!r}")
            rows.append((int(r.group(1)), int(r.group(2)),
                         int(r.group(3).replace(",", "")), int(r.group(4) or 1)))
        out[name] = rows
    return out


def verify_fight_table(evidence: Path, packed: Path | None,
                       artifacts: Path | None) -> None:
    import json
    sys.path.insert(0, str(TOOLS_BENCH))
    import sfx_fight_list as fl  # noqa: E402
    tables = module_tables()
    fight = tables["s_fight"]
    floor = [r[0] for r in tables["s_water"] + tables["s_generic"]]
    ids = [r[0] for r in fight]
    require(len(set(ids)) == len(ids) and not set(ids) & set(floor),
            "fight ids repeat or overlap the floor lists")
    require(sorted(floor) == sorted(fl.FLOOR18), "the tool's floor ids drifted")
    text = MODULE.read_text(encoding="utf-8")
    kib = re.search(r"#define SFXP_FIGHT_KIB (\d+)U$", text, re.M)
    require(kib is not None and int(kib.group(1)) == sum(r[1] for r in fight),
            "SFXP_FIGHT_KIB is not the sum of the fight list")
    for _, k, nbytes, files in fight + [r for t in ("s_water", "s_generic")
                                        for r in tables[t]]:
        require(k == -(-nbytes // 1024) + files, f"kib bound of {nbytes} B/{files}")
    ev = json.loads(evidence.read_text(encoding="utf-8"))
    derived = fl.derive(ev)
    require([(r["id"], r["tier"]) for r in derived] ==
            [(r["id"], r["tier"]) for r in ev["list"]],
            "the selection rule no longer reproduces the frozen list")
    require(ids == [r["id"] for r in ev["list"]],
            f"module order {ids} != evidence order {[r['id'] for r in ev['list']]}")
    for i, r in zip(fight, ev["list"]):
        z = ev["sizes"][str(i[0])]
        require((i[1], i[2], i[3]) == (z["kib"], z["bytes"], z["files"]),
                f"sound {i[0]}: module {i[1:]} vs evidence {z}")
        tier = re.search(rf"\{{ {i[0]}, {i[1]} \}},\s*/\* ([FDR]) ", text)
        require(tier is not None and tier.group(1) == r["tier"], f"sound {i[0]} tier")
    print(f"fight list: {len(ids)} ids unique and disjoint from the floor 18, "
          f"{sum(r[1] for r in fight)} KiB == SFXP_FIGHT_KIB, order/tiers/sizes == "
          f"{evidence.name} (rule re-applied: {len(derived)} ids): PASS")
    if packed is not None:
        sizes = fl.Sizes(packed)
        for name, rows in tables.items():
            for sid, k, nbytes, files in rows:
                z = sizes.of(sid)
                require((z["bytes"], z["files"], z["kib"]) == (nbytes, files, k),
                        f"{name} {sid}: archives {z['bytes']} B/{z['files']} files/"
                        f"{z['kib']} KiB, module {nbytes}/{files}/{k}")
        print(f"sizes: all {sum(len(r) for r in tables.values())} rows of "
              f"s_water/s_generic/s_fight == the stock archives: PASS")
    if artifacts is not None:
        fresh = fl.extract(artifacts, None, *fl.locate(artifacts, ev))
        for key in ("bench", "downpour", "sessions"):
            require(fresh[key] == ev[key], f"evidence '{key}' differs from the logs")
        print(f"evidence: re-extracted from {artifacts} (3 bench, 3 Downpour, "
              f"{len(fresh['sessions']['real'])} real sessions) == {evidence.name}: PASS")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", type=Path, required=True)
    parser.add_argument("--generated-dir", type=Path, required=True)
    parser.add_argument("--cc", help='host compiler for the production '
                        'flavours, e.g. "clang -m32"')
    parser.add_argument("--arm-cc")
    parser.add_argument("--keep", type=Path)
    parser.add_argument("--skip-oracle", action="store_true",
                        help="hosts without MSVC (a Linux build host, under a "
                        "venv: capstone and pefile are still needed): pins, "
                        "decode, corpus, flavours, ARM, build gate")
    parser.add_argument("--any-line", action="store_true",
                        help="a corpus other than the frozen camera corpus "
                        "(a build host's own generation): cited instructions may "
                        "sit on other lines, units may be partitioned "
                        "differently")
    parser.add_argument("--evidence", type=Path, default=EVIDENCE,
                        help="the fight list's frozen evidence (default: "
                        "tools/bench/sfx_fight_evidence.json)")
    parser.add_argument("--packed", type=Path,
                        help="stock resources/packed: recompute every list "
                        "row's bytes, files and kib from the archives")
    parser.add_argument("--artifacts", type=Path,
                        help="device log root: regenerate the evidence from "
                        "the logs and compare it with the frozen file")
    args = parser.parse_args()

    print(f"interpreter: {sys.executable} (python {sys.version.split()[0]}, "
          f"capstone {capstone.__version__}, pefile {pefile.__version__})")
    verify_frozen(args.pe, args.generated_dir, args.any_line)
    play = extract(args.generated_dir, args.any_line)
    verify_build_gate()
    verify_fight_table(args.evidence, args.packed, args.artifacts)

    def body(work: Path) -> list[str]:
        out = []
        if not args.skip_oracle:
            env, compiler = build_all.msvc_env()
            objs = compile_bodies(env, compiler, work, play)
            out += oracle_legs(env, compiler, work, objs)
            out.append(f"mutants killed: {mutants(env, compiler, work, objs)}"
                       f"/{len(MUTANTS) + len(FIGHT_MUTANTS)}")
        production_flavours(shlex.split(args.cc) if args.cc else default_cc(),
                            work)
        if args.arm_cc:
            arm_cross(args.arm_cc, work)
        else:
            print("ARM cross-compile: skipped (no --arm-cc)")
        return out

    if args.keep:
        shutil.rmtree(args.keep, ignore_errors=True)
        args.keep.mkdir(parents=True)
        results = body(args.keep)
    else:
        with tempfile.TemporaryDirectory() as tmp:
            results = body(Path(tmp))
    for line in results:
        print(line)
    print(f"Vita SFX floor preload: PASS; pins={len(PINS)}+{len(CITED)} cited; "
          f"oracle legs={sum(1 for r in results if r.startswith('['))}"
          f"{'' if args.skip_oracle else f'; mutants={len(MUTANTS)}+{len(FIGHT_MUTANTS)}'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
