#!/usr/bin/env python3
"""Proof of RENDER60 S6 (host_vita_post_com.c, vita_post_com_timeGetTime):
ISAAC_VITA_TIMEGETTIME_RECIPROCAL (leg 2, bit-exact u64/1000) and
ISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH=OBSERVE|ON (leg 1, timing-legal).

1. Frozen inputs: the PE and the bytes of the two KAGE Clear bodies
   (sub_00564f50, sub_00561f40) whose loops the latch keys on.
2. Cited constants: every `#define TGT_LATCH_* 0x..U` of the handler and
   every `#define OR_* 0x..U` of the oracle names the instruction it was
   copied from; the instruction must be in the corpus, verbatim, and must
   contain the value (a call site's value is the return word its GPUSH
   pushes).
3. Corpus contract: both bodies defined once, one timeGetTime call each,
   after a GPUSH of the cited return word, with a flush mask that publishes
   esp and esi; the keep path makes no guest call and no store; nothing in
   either body stores the vector's begin word; the loop's only guest calls
   are timeGetTime, FreeBatches and memmove; census of every import-294
   site in the corpus, and no other push of the two return words.
4. Import contract: row 294 of host_vita_import_id_map.inc is
   WINMM.dll!timeGetTime at 0x6064c0 in the POST_COM family, local 4, and
   handler 4 of host_vita_post_com.c is vita_post_com_timeGetTime.
5. Build gate: options default OFF, the knob block's cmake -P matrix, the
   defines reach host_vita_post_com.c and kage_vita_phase_profile.c only,
   no --wrap and no new TU, README entries.
6. B1 (leg 2): M and e re-derived with Python integers; the shipped
   isaac_vita_tgt_ms_from_us against `(uint32_t)(t / 1000)` exhaustively on
   [0, 2^32), on 2^32-long windows at 2^32, 2^53 and 2^64 - 2^32, at every
   1000 k +- 1 below 2^40 and on 10^9 random values (8 BelowNormal
   processes); mutants of each constant, of the shifts and of the carry must
   fail the quick run.
7. Post-COM import test (vita_post_com_import_test.c, MSVC x86) for OFF and
   the six knob variants of test_post_com_imports.sh; OBSERVE without
   ISAAC_VITA_PHASE_PROFILE must not compile.
8. Differential oracle (host_vita_clear_expiry_latch_oracle.c, MSVC x86,
   /LARGEADDRESSAWARE): the translated loops against the real handler in
   eight shapes, four body flavours (GPR locals with masked flushes and
   direct IAT dispatch, the same through guest_call, unmasked GPR locals,
   memory registers).  Claims (a)-(d) of its header.
9. Mutation test of the latch (every key clause, every cited constant, the
   re-arm and hit-advance stores, the return-word offset, the OBSERVE and
   counter logic, the reciprocal wiring): each must make the oracle fail.

usage: test_vita_clear_expiry_latch.py --pe PE --generated-dir DIR
       [--keep DIR] [--no-mutants] [--quick-b1]
"""
from __future__ import annotations

import argparse
import concurrent.futures
import hashlib
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
RUNTIME = HERE / "runtime"
VITA = HERE / "vita"
VITA_CMAKE = VITA / "CMakeLists.txt"
HANDLER = RUNTIME / "host_vita_post_com.c"
HANDLER_H = RUNTIME / "host_vita_post_com.h"
RECIPROCAL_H = RUNTIME / "host_vita_tgt_reciprocal.h"
RECIPROCAL_TEST = RUNTIME / "host_vita_tgt_reciprocal_test.c"
IMPORT_TEST = RUNTIME / "vita_post_com_import_test.c"
ORACLE = RUNTIME / "host_vita_clear_expiry_latch_oracle.c"
IMPORT_MAP = RUNTIME / "host_vita_import_id_map.inc"
sys.path.insert(0, str(HERE))

import build_all  # noqa: E402
from test_vita_kage_quad_fastpath import (  # noqa: E402
    BODY_RE, pe_section_reader, require, run)

BELOW_NORMAL = getattr(subprocess, "BELOW_NORMAL_PRIORITY_CLASS", 0)
MAX_PROCESSES = 8

PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
# {rva: (length, sha256)} of the frozen PE bytes (end = after the ret).
PINS = {
    0x00564F50: (0xD6,
                 "c92aef0c03f8a93ac5839c94193c88131dcef6805bee2ea0619304f275da5b00"),
    0x00561F40: (0x7B,
                 "7f0e12ef94439e66f597f39641cab965f958389139a6632fd496396546a7d233"),
}
BODIES = {"sub_00564f50": "guest_0167.c", "sub_00561f40": "guest_0166.c"}
# body -> (call address, return word, keep-path label pair, loop labels)
SITES = {
    "sub_00564f50": ("00564fd0", 0x00564FD6, ("L_00565014:", "L_0056501f:"),
                     ("L_00564fd0:", "L_00565023:")),
    "sub_00561f40": ("00561f65", 0x00561F6B, ("L_00561fa9:", "L_00561fb4:"),
                     ("L_00561f65:", "L_00561fb8:")),
}
TGT_ID, TGT_SLOT, TGT_LOCAL = 294, 0x006064C0, 4
TGT_CALL = "GUEST_IMPORT_CALL(_target, 0x006064c0U, 294U)"
# The corpus census of import-ID 294 (filled from the measured corpus).
CENSUS = ("census: import-294 sites=366 (call=365 jmp=1) units=67 "
          "site-A pushes=1 site-B pushes=1")
# The vector words as the bodies spell them: (int32_t) of 0x987e7d1c / 20.
BEGIN_SPELLINGS = ("(int32_t)(-1736540900)", "0x987e7d1cU")
END_SPELLINGS = ("(int32_t)(-1736540896)", "0x987e7d20U")

DEFINE_RE = re.compile(
    r"^#define ((?:TGT_LATCH|OR)_\w+) +0x([0-9A-F]+)U"
    r"( +/\* ([0-9a-f]{8})  (.+?) \*/)?$", re.M)
SHARED = {"SITE_A", "SITE_B", "VEC_BEGIN", "VEC_END", "STAMP"}

COMMON = ["/nologo", "/W4", "/WX", "/O2", "/Gy", "/std:c11",
          "/wd4310", "/wd4702", "/wd4996", "/DGUEST_IMAGE_BASE=0x98000000u"]
DISPATCH_DEFINES = ["/DGUEST_STACK_REQUIRED=1",
                    "/DISAAC_VITA_IMPORT_ID_DISPATCH=1",
                    "/DISAAC_VITA_GUEST_LOOKUP_CACHE=1",
                    "/DGUEST_FLAGS_LOCAL=1"]
# recomp/vita/CMakeLists.txt compile definitions of a generated unit.
BODY_DEFINES = ["/DGUEST_STACK_REQUIRED=1", "/DGUEST_GENERATED_STACK_GUARD=0",
                "/DGUEST_FLAGS_LOCAL=1", "/DGUEST_COVERAGE_HOOKS=0",
                "/DISAAC_VITA_IMPORT_ID_DISPATCH=1",
                "/DISAAC_VITA_GUEST_LOOKUP_CACHE=1"]
INCLUDES = ["/I", str(RUNTIME), "/I", str(VITA)]

# The eight handler shapes the oracle links (tag -> definitions).
SHAPES = {
    "off": [],
    "off_p": ["/DISAAC_VITA_PHASE_PROFILE=1"],
    "rec": ["/DISAAC_VITA_TIMEGETTIME_RECIPROCAL=1"],
    "obs": ["/DISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH_MODE=1",
            "/DISAAC_VITA_PHASE_PROFILE=1"],
    "obs_rec": ["/DISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH_MODE=1",
                "/DISAAC_VITA_PHASE_PROFILE=1",
                "/DISAAC_VITA_TIMEGETTIME_RECIPROCAL=1"],
    "on": ["/DISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH_MODE=2"],
    "on_p": ["/DISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH_MODE=2",
             "/DISAAC_VITA_PHASE_PROFILE=1"],
    "on_p_rec": ["/DISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH_MODE=2",
                 "/DISAAC_VITA_PHASE_PROFILE=1",
                 "/DISAAC_VITA_TIMEGETTIME_RECIPROCAL=1"],
}
# flavour -> (body definitions, oracle uses the direct IAT table)
FLAVOURS = {
    "gpr-masked-direct": (["/DGUEST_GPR_LOCAL=1", "/DGUEST_GPR_FLUSH_MASKED=1",
                           "/DGUEST_IMPORT_DIRECT=1"], True),
    "gpr-masked-call": (["/DGUEST_GPR_LOCAL=1", "/DGUEST_GPR_FLUSH_MASKED=1",
                         "/DGUEST_IMPORT_DIRECT=0"], False),
    "gpr-call": (["/DGUEST_GPR_LOCAL=1", "/DGUEST_GPR_FLUSH_MASKED=0",
                  "/DGUEST_IMPORT_DIRECT=0"], False),
    "mem-call": (["/DGUEST_GPR_LOCAL=0", "/DGUEST_IMPORT_DIRECT=0"], False),
}
ORACLE_RESULT = ("Vita Clear expiry latch oracle: PASS; flavour={flavour}; "
                 "scenarios=25500; runs=229500; claims a,b,c,d hold")
# Identical in every flavour: the body spellings must not change a run.
ORACLE_LINES = [
    ("perturbed calls (each forced a fresh read; all 9 runs per scenario): "
     "begin=6525 end=6597 foreign=6543 site=7227 esi=6651 first=6768"),
    ("trajectory: d>0 scenarios=2579 (ON(V) != OFF(V)), d==0 scenarios=18421 "
     "(ON(V) == OFF(V)); w>0 scenarios=637; nested=1500 frozen=1500 wrap=1500"),
    ("ON (unperturbed, on_p): site calls=1014680 hits=627822 fresh=386858 "
     "clock reads=386858 tgt=1014680 (reads = tgt - hits)"),
]
# The post-COM import test variants (test_post_com_imports.sh, MSVC here).
IMPORT_VARIANTS = {
    "off": [],
    "rec": ["/DISAAC_VITA_TIMEGETTIME_RECIPROCAL=1"],
    "rec-profile": ["/DISAAC_VITA_TIMEGETTIME_RECIPROCAL=1",
                    "/DISAAC_VITA_PHASE_PROFILE=1"],
    "latch-on": ["/DISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH_MODE=2"],
    "latch-on-rec": ["/DISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH_MODE=2",
                     "/DISAAC_VITA_TIMEGETTIME_RECIPROCAL=1"],
    "latch-observe-profile": ["/DISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH_MODE=1",
                              "/DISAAC_VITA_PHASE_PROFILE=1"],
    "latch-observe-profile-rec": [
        "/DISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH_MODE=1",
        "/DISAAC_VITA_PHASE_PROFILE=1", "/DISAAC_VITA_TIMEGETTIME_RECIPROCAL=1"],
}


def run_bn(args: list[str], env, timeout: int) -> tuple[int | None, str]:
    """One process at BelowNormal priority; (rc, output) or (None, 'timeout')."""
    try:
        done = subprocess.run(args, env=env, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT, timeout=timeout,
                              creationflags=BELOW_NORMAL)
    except subprocess.TimeoutExpired:
        return None, "timeout"
    return done.returncode, done.stdout.decode("cp866", errors="replace")


# ---- 1. frozen inputs ---------------------------------------------------------

def verify_frozen(pe_path: Path) -> None:
    pe = pe_path.read_bytes()
    require(hashlib.sha256(pe).hexdigest() == PE_SHA256, "frozen PE changed")
    read = pe_section_reader(pe)
    for rva, (length, sha) in PINS.items():
        body = read(rva, length)
        require(hashlib.sha256(body).hexdigest() == sha,
                f"body changed at {rva:#x}")
        require(body[-1] == 0xC3, f"{rva:#x}: the pinned bytes do not end in ret")


# ---- 2. cited constants -------------------------------------------------------

def numbers_in(text: str) -> set[int]:
    out: set[int] = set()
    for sign, hexv in re.findall(r"(-\s*)?0x([0-9a-f]+)", text):
        v = int(hexv, 16)
        if sign:
            v = (-v) & 0xFFFFFFFF
        out.add(v)
        if v >= 0x98000000:
            out.add(v - 0x98000000)
    for dec in re.findall(r"(?<![0-9a-fx])(\d+)(?![0-9a-fx])", text):
        out.add(int(dec))
    return out


def pushed_return_word(corpus: str, comment: str) -> int | None:
    m = re.search(re.escape(comment) + r"\n[^\n]*\n\s*GPUSH\(0x([0-9a-f]+)U\);",
                  corpus)
    return int(m.group(1), 16) if m else None


def cited(path: Path, corpus: str) -> dict[str, int]:
    values: dict[str, int] = {}
    for m in DEFINE_RE.finditer(path.read_text(encoding="utf-8")):
        name, value = m.group(1), int(m.group(2), 16)
        require(m.group(3) is not None, f"{path.name}: {name} cites no instruction")
        addr, insn = m.group(4), m.group(5)
        comment = f"/* {addr}  {insn} */"
        require(comment in corpus, f"{name}: {comment} is not in the bodies")
        candidates = numbers_in(insn) | {int(addr, 16)}
        if insn.startswith("call dword ptr ["):
            word = pushed_return_word(corpus, comment)
            require(word is not None, f"{name}: no GPUSH after {comment}")
            candidates.add(word)
        require(value in candidates,
                f"{name} = {value:#x} is not in `{insn}` at {addr}")
        values[name] = value
    return values


def verify_citations(bodies: dict[str, str]) -> str:
    corpus = "\n".join(bodies.values())
    runtime = cited(HANDLER, corpus)
    oracle = cited(ORACLE, corpus)
    require(set(runtime) == {"TGT_LATCH_SITE_A", "TGT_LATCH_SITE_B",
                             "TGT_LATCH_VEC_BEGIN", "TGT_LATCH_VEC_END",
                             "TGT_LATCH_STAMP", "TGT_LATCH_LIMIT_MS"},
            f"handler cited constants: {sorted(runtime)}")
    for key in SHARED:
        require(runtime[f"TGT_LATCH_{key}"] == oracle[f"OR_{key}"],
                f"oracle and handler disagree on {key}")
    require(runtime["TGT_LATCH_LIMIT_MS"] == oracle["OR_LIMIT"], "LIMIT differs")
    # the twin's instructions carry the same operands
    twin = bodies["sub_00561f40"]
    for insn in ("/* 00561f54  mov esi, dword ptr [0x987e7d1c] */",
                 "/* 00561f5a  cmp esi, dword ptr [0x987e7d20] */",
                 "/* 00561f6d  sub eax, dword ptr [ecx + 0x74] */",
                 "/* 00561f70  cmp eax, 0x1388 */",
                 "/* 00561f79  call dword ptr [eax + 0x74] */",
                 "/* 00561f51  call dword ptr [eax + 0x70] */"):
        require(insn in twin, f"twin instruction missing: {insn}")
    return f"cited constants: handler={len(runtime)} oracle={len(oracle)}"


# ---- 3. corpus contract -------------------------------------------------------

def extract(generated_dir: Path) -> tuple[dict[str, str], str]:
    bodies: dict[str, str] = {}
    owners: dict[str, list[str]] = {name: [] for name in BODIES}
    texts: dict[str, str] = {}
    for unit in sorted(generated_dir.glob("guest_[0-9][0-9][0-9][0-9].c")):
        t = unit.read_text(encoding="utf-8")
        texts[unit.name] = t
        for name in BODIES:
            if f"\nvoid {name}(CPU *__restrict c)\n" in t:
                owners[name].append(unit.name)
                for m in BODY_RE.finditer(t):
                    if m.group(1) == name:
                        bodies[name] = m.group(0)
    require(len(texts) > 150, f"only {len(texts)} generated units")
    for name, unit in BODIES.items():
        require(owners[name] == [unit] and name in bodies,
                f"{name}: owners {owners[name]}")
    for name, (addr, word, keep, loop) in SITES.items():
        body = bodies[name]
        require("#if" not in body, f"{name}: preprocessor seam in the body")
        require(body.count("294U)") == 1 and body.count(TGT_CALL) == 1,
                f"{name}: not exactly one timeGetTime call")
        m = re.search(
            re.escape(f"/* {addr}  call dword ptr [0x986064c0] */") +
            r"\n\s*\{ uint32_t _target = ld32\(\(uint32_t\)\(\(uint32_t\)\(int32_t\)"
            r"\(-1738513216\)\)\);\n\s*GPUSH\(0x([0-9a-f]+)U\);\n"
            r"\s*GUEST_STACK_CALLSITE_BARRIER\(\);\n"
            r"\s*GUEST_GPR_FLUSH_MASK\(c, 0x([0-9a-f]+)U\); " +
            re.escape(TGT_CALL) + r"; GUEST_GPR_RELOAD\(c\);", body)
        require(m is not None, f"{name}: timeGetTime site shape changed")
        require(int(m.group(1), 16) == word, f"{name}: return word moved")
        mask = int(m.group(2), 16)
        # the handler reads c->esp (return word) and c->esi (latch key)
        require(mask & 0x50 == 0x50, f"{name}: flush mask {mask:#x} lacks esp/esi")
        k0, k1 = body.index(keep[0]), body.index(keep[1])
        keep_text = body[k0:k1]
        for needle in ("guest_call", "GUEST_IMPORT", "GPUSH", "st32", "st16",
                       "st8", "GESP", "GUEST_GPR_FLUSH"):
            require(needle not in keep_text, f"{name}: keep path has {needle}")
        require(re.search(r"\bsub_[0-9a-f]{8}\(c\)", keep_text) is None,
                f"{name}: keep path calls a function")
        for reg in ("esi", "edi"):
            require(keep_text.count(
                        f"{{ uint32_t _a = GR({reg}), _b = 0x4U, "
                        f"_r = (uint32_t)(_a + _b);\n      GR({reg}) = "
                        f"(uint32_t)(_r);\n    }}") == 1,
                    f"{name}: keep path does not advance {reg} by 4")
        require(keep_text.count("GR(eax) = (uint32_t)(ld32((uint32_t)"
                                "(0x987e7d20U)));") == 1,
                f"{name}: keep path does not reload eax from the end word")
        for spelling in BEGIN_SPELLINGS:
            require(f"st32((uint32_t)((uint32_t){spelling}" not in body and
                    f"st32((uint32_t)({spelling}" not in body,
                    f"{name}: stores the vector begin word")
        require(body.count("st32((uint32_t)(0x987e7d20U)") == 1,
                f"{name}: expected one store of the end word (erase path)")
        l0, l1 = body.index(loop[0]), body.index(loop[1])
        loop_text = body[l0:l1]
        require(loop_text.count("guest_call(c,") == 1,
                f"{name}: loop guest_call count changed (FreeBatches only)")
        require(re.findall(r"\b(sub_[0-9a-f]{8})\(c\)", loop_text) ==
                ["sub_005ec358"], f"{name}: loop direct calls changed")
        require(loop_text.count("GUEST_IMPORT") == 1, f"{name}: loop imports")
    # census of import 294 and of the two return words
    sites = calls = jmps = 0
    units = set()
    pushes = {0x00564FD6: 0, 0x00561F6B: 0}
    for unit, t in texts.items():
        found = re.findall(r"GUEST_IMPORT_(CALL|JMP)\([^;\n]*?, 0x006064c0U, "
                           r"294U\)", t)
        # every (slot, 294) pair in a unit is the timeGetTime slot, inside one
        # of the two tokens
        pairs = re.findall(r"0x([0-9a-f]{8})U, 294U\)", t)
        require(set(pairs) <= {"006064c0"} and len(pairs) == len(found),
                f"{unit}: import 294 spelled outside GUEST_IMPORT_CALL/JMP")
        if found:
            units.add(unit)
        sites += len(found)
        calls += found.count("CALL")
        jmps += found.count("JMP")
        for word in pushes:
            pushes[word] += t.count(f"GPUSH({word:#x}U)")
            require(t.count(f"{word:#x}U") == t.count(f"GPUSH({word:#x}U)"),
                    f"{unit}: {word:#x} used other than by its GPUSH")
    census = (f"census: import-294 sites={sites} (call={calls} jmp={jmps}) "
              f"units={len(units)} site-A pushes={pushes[0x00564FD6]} "
              f"site-B pushes={pushes[0x00561F6B]}")
    require(pushes[0x00564FD6] == 1 and pushes[0x00561F6B] == 1,
            f"another site pushes a latch return word: {census}")
    if CENSUS != "filled-in-below":
        require(census == CENSUS, f"corpus census changed: {census}")
    return bodies, census


# ---- 4. import contract -------------------------------------------------------

def verify_import_contract() -> str:
    rows = re.findall(
        r"^ISAAC_VITA_IMPORT_ID_ROW\((\d+)U, 0x([0-9a-f]{8})U, \"([^\"]+)\", "
        r"(\w+), (\d+)U\)$", IMPORT_MAP.read_text(encoding="utf-8"), re.M)
    require(len(rows) == 413, f"import map rows: {len(rows)}")
    require([int(r[0]) for r in rows] == list(range(413)), "import IDs not dense")
    slots = [int(r[1], 16) for r in rows]
    require(slots == sorted(slots) and len(set(slots)) == 413, "slots unsorted")
    row = rows[TGT_ID]
    require((int(row[1], 16), row[2], row[3], int(row[4])) ==
            (TGT_SLOT, "WINMM.dll!timeGetTime", "ISAAC_VITA_IMPORT_POST_COM",
             TGT_LOCAL), f"row 294 changed: {row}")
    require(sum(1 for r in rows if r[2] == "WINMM.dll!timeGetTime") == 1,
            "timeGetTime imported twice")
    header = HANDLER_H.read_text(encoding="utf-8")
    require('#define ISAAC_VITA_POST_COM_GET_TIME_NAME "WINMM.dll!timeGetTime"'
            in header and
            "#define ISAAC_VITA_POST_COM_GET_TIME_IAT_RVA       0x006064c0U"
            in header, "post-COM header timeGetTime constants changed")
    text = HANDLER.read_text(encoding="utf-8")
    table = re.search(r"s_vita_post_com_imports\[\] = \{(.*?)\n\};", text, re.S)
    require(table is not None, "handler table missing")
    entries = re.findall(r"\{ (ISAAC_VITA_POST_COM_\w+),\s+(vita_post_com_\w+) \}",
                         table.group(1))
    require(len(entries) == 6 and entries[TGT_LOCAL] ==
            ("ISAAC_VITA_POST_COM_GET_TIME_NAME", "vita_post_com_timeGetTime"),
            f"handler index {TGT_LOCAL} is not timeGetTime: {entries}")
    require(text.count("vita_post_com_timeGetTime(") == 1,
            "timeGetTime handler defined or called elsewhere")
    return "import contract: row 294 = WINMM.dll!timeGetTime POST_COM local 4"


# ---- 5. build gate --------------------------------------------------------------

KNOB_HEAD = "# RENDER60 S6: the guest timeGetTime import handler"
KNOB_TAIL = ('  "ISAAC_VITA_TIMEGETTIME_RECIPROCAL='
             '${ISAAC_VITA_TIMEGETTIME_RECIPROCAL_NAME}")\n')
CMAKE_CASES = [
    # (latch, rec, profile, scaffold) -> expected stdout/stderr needle
    (None, None, "OFF", "OFF",
     "ISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH=OFF (mode 0)"),
    ("OFF", "OFF", "OFF", "OFF", "ISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH=OFF (mode 0)"),
    ("", "OFF", "OFF", "OFF", "ISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH=OFF (mode 0)"),
    ("off", "OFF", "OFF", "OFF", "ISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH=OFF (mode 0)"),
    ("OBSERVE", "OFF", "ON", "OFF",
     "ISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH=OBSERVE (mode 1)"),
    ("observe", "ON", "ON", "OFF",
     "ISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH=OBSERVE (mode 1)"),
    ("OBSERVE", "OFF", "OFF", "OFF",
     "FATAL:ISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH=OBSERVE requires"),
    ("ON", "OFF", "OFF", "OFF", "ISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH=ON (mode 2)"),
    ("on", "OFF", "ON", "OFF", "ISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH=ON (mode 2)"),
    ("1", "OFF", "ON", "OFF", "FATAL:must be OFF, OBSERVE or ON (got '1')"),
    ("2", "OFF", "ON", "OFF", "FATAL:must be OFF, OBSERVE or ON (got '2')"),
    ("YES", "OFF", "ON", "OFF", "FATAL:must be OFF, OBSERVE or ON (got 'YES')"),
    ("ON", "OFF", "OFF", "ON", "FATAL:require the real Vita runtime"),
    ("OFF", "ON", "OFF", "ON", "FATAL:require the real Vita runtime"),
    ("OFF", "ON", "OFF", "OFF", "ISAAC_VITA_TIMEGETTIME_RECIPROCAL=ON"),
    ("OFF", None, "OFF", "OFF", "ISAAC_VITA_TIMEGETTIME_RECIPROCAL=OFF"),
]


def cmake_exe() -> str:
    for candidate in (shutil.which("cmake"), r"C:\Program Files\CMake\bin\cmake.exe"):
        if candidate and Path(candidate).is_file():
            return candidate
    raise SystemExit("cmake not found")


def verify_build_gate(work: Path) -> str:
    cmake = VITA_CMAKE.read_text(encoding="utf-8")
    require(re.search(r"option\(ISAAC_VITA_TIMEGETTIME_RECIPROCAL\s+\"[^\"]*\"\s+OFF\)",
                      cmake) is not None, "reciprocal option missing or not OFF")
    require(re.search(r"set\(ISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH OFF CACHE STRING",
                      cmake) is not None, "latch cache string missing or not OFF")
    # the definitions reach exactly the two TUs
    for define, sources in (
            ("ISAAC_VITA_TIMEGETTIME_RECIPROCAL=1",
             ["host_vita_post_com.c"]),
            ("ISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH_MODE="
             "${ISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH_MODE}",
             ["host_vita_post_com.c", "kage_vita_phase_profile.c"])):
        found = []
        for m in re.finditer(r"set_property\(SOURCE\s+\"\$\{ISAAC_RUNTIME\}/(\w+\.c)\""
                             r"\s+APPEND PROPERTY COMPILE_DEFINITIONS\s+([^)]*)\)",
                             cmake):
            if define in m.group(2):
                found.append(m.group(1))
        require(found == sources and cmake.count(define) == len(sources),
                f"{define} reaches {found}")
    require("timeGetTime" not in "".join(re.findall(r"--wrap=[^\s\")]+", cmake)),
            "a --wrap names timeGetTime")
    for forbidden in ("host_vita_tgt_reciprocal_test.c",
                      "host_vita_clear_expiry_latch_oracle.c"):
        require(forbidden not in cmake, f"{forbidden} is in the Vita build")
    require(cmake.count("host_vita_post_com.c") >= 1, "handler TU missing")
    readme = (VITA / "README.md").read_text(encoding="utf-8")
    for needle in ("`ISAAC_VITA_TIMEGETTIME_RECIPROCAL` (default OFF",
                   "`ISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH=OBSERVE|ON` (default OFF",
                   "recomp/test_vita_clear_expiry_latch.py"):
        require(needle in readme, f"README lacks {needle!r}")
    # the knob block alone, in cmake -P
    start = cmake.index(KNOB_HEAD)
    end = cmake.index(KNOB_TAIL, start) + len(KNOB_TAIL)
    script = work / "s6_knobs.cmake"
    script.write_text(cmake[start:end], encoding="utf-8", newline="\n")
    exe = cmake_exe()
    for latch, rec, profile, scaffold, needle in CMAKE_CASES:
        args = [exe]
        if latch is not None:
            args.append(f"-DISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH={latch}")
        if rec is not None:
            args.append(f"-DISAAC_VITA_TIMEGETTIME_RECIPROCAL={rec}")
        args += [f"-DISAAC_VITA_PHASE_PROFILE={profile}",
                 f"-DISAAC_VITA_SCAFFOLD_ONLY={scaffold}", "-P", str(script)]
        done = subprocess.run(args, stdout=subprocess.PIPE,
                              stderr=subprocess.STDOUT)
        out = done.stdout.decode("utf-8", errors="replace")
        flat = " ".join(out.split())
        if needle.startswith("FATAL:"):
            require(done.returncode != 0 and needle[6:] in flat,
                    f"cmake case {latch}/{rec}/{profile}/{scaffold}: {flat[:300]}")
        else:
            require(done.returncode == 0 and needle in flat,
                    f"cmake case {latch}/{rec}/{profile}/{scaffold}: {flat[:300]}")
    return f"build gate: cmake -P cases={len(CMAKE_CASES)}"


# ---- 6. B1: the reciprocal ----------------------------------------------------

RECIPROCAL_MUTANTS = {
    "m_lo+1": ("#define ISAAC_VITA_TGT_RECIPROCAL_M_LO 0xE353F7CFU",
               "#define ISAAC_VITA_TGT_RECIPROCAL_M_LO 0xE353F7D0U"),
    "m_lo-1": ("#define ISAAC_VITA_TGT_RECIPROCAL_M_LO 0xE353F7CFU",
               "#define ISAAC_VITA_TGT_RECIPROCAL_M_LO 0xE353F7CEU"),
    "m_hi-1": ("#define ISAAC_VITA_TGT_RECIPROCAL_M_HI 0x20C49BA5U",
               "#define ISAAC_VITA_TGT_RECIPROCAL_M_HI 0x20C49BA4U"),
    "preshift2": ("#define ISAAC_VITA_TGT_RECIPROCAL_PRESHIFT 3U",
                  "#define ISAAC_VITA_TGT_RECIPROCAL_PRESHIFT 2U"),
    "postshift3": ("#define ISAAC_VITA_TGT_RECIPROCAL_POSTSHIFT 4U",
                   "#define ISAAC_VITA_TGT_RECIPROCAL_POSTSHIFT 3U"),
    "no-mid-carry": ("p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);",
                     "p11 + (p01 >> 32) + (p10 >> 32);"),
    "no-p00-carry": ("(p00 >> 32) + (uint32_t)p01 + (uint32_t)p10;",
                     "(uint32_t)p01 + (uint32_t)p10;"),
    "no-p10-hi": ("p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);",
                  "p11 + (p01 >> 32) + (mid >> 32);"),
}


def derive_reciprocal() -> str:
    m = -(-(1 << 68) // 125)
    e = 125 * m - (1 << 68)
    require(m == 0x20C49BA5E353F7CF and e == 19, f"M={m:#x} e={e}")
    # n < 2^61: n e / 2^68 < 2^61 * 19 / 2^68 = 19/128 < 1
    require((1 << 61) * e < (1 << 68), "error bound")
    require(((1 << 61) - 1) * m < (1 << 123), "product bound")
    text = RECIPROCAL_H.read_text(encoding="utf-8")
    lo = int(re.search(r"M_LO 0x([0-9A-F]{8})U", text).group(1), 16)
    hi = int(re.search(r"M_HI 0x([0-9A-F]{8})U", text).group(1), 16)
    require((hi << 32 | lo) == m, "header M differs from ceil(2^68/125)")
    require("PRESHIFT 3U" in text and "POSTSHIFT 4U" in text, "header shifts")
    # the four-product formula in Python integers, on the edge values
    def formula(t: int) -> int:
        n = t >> 3
        n0, n1 = n & 0xFFFFFFFF, n >> 32
        p00, p01, p10, p11 = n0 * lo, n0 * hi, n1 * lo, n1 * hi
        mid = (p00 >> 32) + (p01 & 0xFFFFFFFF) + (p10 & 0xFFFFFFFF)
        return ((p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32)) >> 4) & 0xFFFFFFFF
    for t in [0, 1, 999, 1000, 1001, 7999, 8000, 2**32 - 1, 2**32,
              4294967295999, 4294967296000, 2**53, 2**61 - 1, 2**63,
              2**64 - 1001, 2**64 - 1000, 2**64 - 999, 2**64 - 1]:
        require(formula(t) == (t // 1000) & 0xFFFFFFFF, f"formula at {t}")
    return f"reciprocal: M={m:#x} e={e} (python re-derivation)"


def build_reciprocal_test(env, cl: str, work: Path, header_dir: Path | None,
                          tag: str) -> Path:
    exe = work / f"tgt-reciprocal-{tag}.exe"
    source = RECIPROCAL_TEST
    if header_dir is not None:
        # the test includes "host_vita_tgt_reciprocal.h" relative to its own
        # directory first: copy it next to the mutated header
        source = header_dir / RECIPROCAL_TEST.name
        shutil.copyfile(RECIPROCAL_TEST, source)
    # a mutant may leave a product unused (C4189): no /WX for mutants
    flags = COMMON if header_dir is None else [f for f in COMMON if f != "/WX"]
    run([cl] + flags + ["/I", str(header_dir or RUNTIME), str(source),
                         "/Fo:" + str(work / f"tgt-reciprocal-{tag}.obj"),
                         "/Fe:" + str(exe)], env)
    return exe


def run_reciprocal(exe: Path, env, mode: str, parts: int,
                   timeout: int) -> list[tuple[int | None, str]]:
    with concurrent.futures.ThreadPoolExecutor(max_workers=MAX_PROCESSES) as pool:
        return list(pool.map(
            lambda part: run_bn([str(exe), str(part), str(parts), mode], env,
                                timeout), range(parts)))


def b1(env, cl: str, work: Path, quick: bool) -> list[str]:
    out = [derive_reciprocal()]
    exe = build_reciprocal_test(env, cl, work, None, "shipped")
    mode = "quick" if quick else "full"
    results = run_reciprocal(exe, env, mode, MAX_PROCESSES, 3600)
    checked = 0
    for rc, text in results:
        line = text.strip().splitlines()[-1] if text.strip() else text
        m = re.match(r"tgt-reciprocal part=\d+/\d+ mode=(\w+) checked=(\d+) "
                     r"mismatches=(\d+) first=\d+$", line)
        require(rc == 0 and m is not None and m.group(1) == mode and
                m.group(3) == "0", f"reciprocal part failed: rc={rc} {line!r}")
        checked += int(m.group(2))
    if not quick:
        expected = 4 * 2**32 + 3 * ((1 << 40) // 1000) + 10**9
        require(checked == expected, f"checked {checked}, expected {expected}")
    out.append(f"B1 {mode}: processes={MAX_PROCESSES} checked={checked} "
               "mismatches=0")
    # mutants: each must fail its quick run
    header = RECIPROCAL_H.read_text(encoding="utf-8")
    mutdir = work / "b1-mutants"
    mutdir.mkdir(exist_ok=True)

    def one(item):
        name, (old, new) = item
        require(header.count(old) == 1, f"B1 mutant {name}: anchor not unique")
        d = mutdir / name
        d.mkdir()
        (d / RECIPROCAL_H.name).write_text(header.replace(old, new),
                                           encoding="utf-8", newline="\n")
        mexe = build_reciprocal_test(env, cl, d, d, name)
        res = run_reciprocal(mexe, env, "quick", 1, 600)
        rc, text = res[0]
        return name, rc, (text.strip().splitlines() or [""])[-1]

    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
        outcomes = list(pool.map(one, RECIPROCAL_MUTANTS.items()))
    for name, rc, line in outcomes:
        print(f"  B1 mutant {name}: {'killed' if rc else 'SURVIVED'} ({line[:90]})")
        require(rc != 0, f"B1 mutant {name} survived")
    out.append(f"B1 mutants={len(outcomes)} killed={len(outcomes)}")
    return out


# ---- 7. post-COM import test ---------------------------------------------------

# guest.h's out-of-line stack fault recorders (guest.c on the device): the
# unbound CPU of vita_post_com_import_test.c never reaches them, a call would
# fail the test through c->fault.
IMPORT_TEST_STUBS = """#include "guest.h"
int guest_stack_violation(CPU *__restrict c, uint32_t pc, uint32_t kind,
                          uint32_t address, uint32_t size)
{
    (void)pc; (void)kind; (void)address; (void)size;
    c->fault = "post-COM host stub: stack violation";
    return 0;
}
int guest_stack_owner_violation(CPU *__restrict c, uint32_t pc)
{
    (void)pc;
    c->fault = "post-COM host stub: stack owner violation";
    return 0;
}
"""


def import_tests(env, cl: str, work: Path) -> str:
    stubs = work / "post_com_host_stubs.c"
    stubs.write_text(IMPORT_TEST_STUBS, encoding="utf-8", newline="\n")
    for name, defs in IMPORT_VARIANTS.items():
        exe = work / f"post-com-{name}.exe"
        objdir = work / f"post-com-{name}"
        objdir.mkdir(exist_ok=True)
        run([cl] + COMMON + defs + INCLUDES +
            [str(HANDLER), str(IMPORT_TEST), str(stubs),
             "/Fo:" + str(objdir) + "\\", "/Fe:" + str(exe)], env)
        rc, text = run_bn([str(exe)], env, 60)
        require(rc == 0, f"post-COM import test {name}: rc={rc} {text[-400:]}")
    done = subprocess.run(
        [cl] + COMMON + ["/DISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH_MODE=1"] +
        INCLUDES + ["/c", str(HANDLER), "/Fo:" + str(work / "observe-np.obj")],
        env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    text = done.stdout.decode("cp866", errors="replace")
    require(done.returncode != 0 and "needs ISAAC_VITA_PHASE_PROFILE" in text,
            "OBSERVE without ISAAC_VITA_PHASE_PROFILE compiled")
    return f"post-COM import test: variants={len(IMPORT_VARIANTS)} rc=0; " \
           "OBSERVE without PHASE_PROFILE refused"


# ---- 8. oracle ------------------------------------------------------------------

def bodies_source(bodies: dict[str, str]) -> str:
    out = ["/* test_vita_clear_expiry_latch.py: frozen corpus bodies */",
           '#include "guest.h"',
           "void sub_00561ff0(CPU *__restrict c);",
           "void sub_005ec358(CPU *__restrict c);"]
    for name in BODIES:
        out.append(f"void {name}(CPU *__restrict c);")
    for name in BODIES:
        out.append(bodies[name])
    return "\n".join(out) + "\n"


def shape_renames(tag: str) -> list[str]:
    return [f"/Disaac_vita_post_com_import_indexed={tag}_indexed",
            f"/Disaac_vita_post_com_import_name={tag}_name",
            f"/Disaac_vita_post_com_import_counted={tag}_import_counted",
            f"/Disaac_vita_post_com_import={tag}_import",
            f"/Dg_isaac_vita_post_com_time_get_time_calls={tag}_tgt",
            f"/Dg_isaac_vita_clear_latch_counters={tag}_counters"]


class Build:
    def __init__(self, env, cl: str, work: Path, bodies: dict[str, str]):
        self.env, self.cl, self.work = env, cl, work
        src = work / "latch_bodies.c"
        src.write_text(bodies_source(bodies), encoding="utf-8", newline="\n")
        self.bodies: dict[str, Path] = {}
        for flavour, (defines, _direct) in FLAVOURS.items():
            obj = work / f"latch_bodies_{flavour}.obj"
            run([cl, "/nologo", "/W3", "/O2", "/std:c11", "/wd4310", "/wd4996",
                 "/DGUEST_IMAGE_BASE=0x98000000u"] + BODY_DEFINES + defines +
                ["/I", str(RUNTIME), "/c", str(src), "/Fo:" + str(obj)], env)
            self.bodies[flavour] = obj
        self.oracle: dict[bool, Path] = {}
        for direct in (False, True):
            obj = work / f"latch_oracle{'_direct' if direct else ''}.obj"
            run([cl] + COMMON + DISPATCH_DEFINES +
                (["/DISAAC_VITA_IMPORT_DIRECT=1"] if direct else []) + INCLUDES +
                ["/c", str(ORACLE), "/Fo:" + str(obj)], env)
            self.oracle[direct] = obj
        self.shapes = self.compile_shapes(HANDLER, "shipped")

    def compile_shapes(self, source: Path, tag: str,
                       strict: bool = True) -> list[Path]:
        # a mutant may leave a variable unused: no /WX for mutants
        flags = COMMON if strict else [f for f in COMMON if f != "/WX"]
        objs = []
        for shape, defines in SHAPES.items():
            obj = self.work / f"shape_{tag}_{shape}.obj"
            run([self.cl] + flags + DISPATCH_DEFINES + defines +
                shape_renames(shape) + INCLUDES +
                ["/c", str(source), "/Fo:" + str(obj)], self.env)
            objs.append(obj)
        return objs

    def link(self, tag: str, flavour: str, shapes: list[Path]) -> Path:
        exe = self.work / f"latch-oracle-{tag}-{flavour}.exe"
        direct = FLAVOURS[flavour][1]
        run([self.cl, "/nologo", str(self.oracle[direct]),
             str(self.bodies[flavour])] + [str(s) for s in shapes] +
            ["/Fe:" + str(exe), "/link", "/LARGEADDRESSAWARE", "/OPT:REF",
             "/INCREMENTAL:NO"], self.env)
        return exe


def oracle_flavours(build: Build) -> list[str]:
    exes = {f: build.link("shipped", f, build.shapes) for f in FLAVOURS}
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(FLAVOURS)) as pool:
        outs = dict(zip(FLAVOURS, pool.map(
            lambda f: run_bn([str(exes[f]), "full", f], build.env, 1800),
            FLAVOURS)))
    results = []
    for flavour, (rc, out) in outs.items():
        (build.work / f"oracle-{flavour}.out").write_text(out, encoding="utf-8")
        lines = out.strip().splitlines()
        result = lines[-1] if lines else ""
        print(f"[{flavour}] rc={rc}")
        for line in lines[-4:]:
            print(f"  {line}")
        if rc != 0 or not result.startswith("Vita Clear expiry latch oracle: PASS"):
            print(out[-6000:])
            raise AssertionError(f"oracle failed ({flavour}, rc {rc}): {result!r}")
        if ORACLE_RESULT != "filled-in-below":
            require(result == ORACLE_RESULT.replace("{flavour}", flavour),
                    f"oracle result changed: {result!r}")
            for line in ORACLE_LINES:
                require(line in lines, f"oracle line missing: {line!r}")
        results.append(f"[{flavour}] {result}")
    return results


# ---- 9. mutation test -----------------------------------------------------------

def latch_mutants(text: str) -> dict[str, tuple[str, str]]:
    def key(name: str) -> tuple[str, str]:
        m = re.search(r"^( +)(.+?)( &&|;)( +/\* key: " + name + r" \*/)$", text, re.M)
        require(m is not None, f"key clause {name} not found")
        prefix = "eligible = " if m.group(2).startswith("eligible = ") else ""
        return m.group(0), f"{m.group(1)}{prefix}1{m.group(3)}{m.group(4)}"

    def define(name: str, new: int) -> tuple[str, str]:
        m = re.search(rf"^#define {name} 0x([0-9A-F]+)U", text, re.M)
        require(m is not None, f"{name} not found")
        return m.group(0), f"#define {name} 0x{new:X}U"

    def value(name: str) -> int:
        return int(re.search(rf"^#define {name} 0x([0-9A-F]+)U", text,
                             re.M).group(1), 16)

    hit_store = "    if (eligible) {\n        latch->esi = c->esi;\n"
    rearm_store = "        latch->end = end;\n        latch->ms = ms;\n"
    observe_read = ("#else\n        ms = vita_post_com_fresh_ms();\n"
                    "        if (ms != latch->ms)\n")
    mutants = {f"key-{k}": key(k)
               for k in ("cpu", "site", "esi", "first", "begin", "end")}
    mutants.update({
        "SITE_A-based": define("TGT_LATCH_SITE_A",
                               value("TGT_LATCH_SITE_A") + 0x98000000),
        "SITE_B-based": define("TGT_LATCH_SITE_B",
                               value("TGT_LATCH_SITE_B") + 0x98000000),
        "SITE_A+1": define("TGT_LATCH_SITE_A", value("TGT_LATCH_SITE_A") + 1),
        "VEC_BEGIN+4": define("TGT_LATCH_VEC_BEGIN",
                              value("TGT_LATCH_VEC_BEGIN") + 4),
        "VEC_END+4": define("TGT_LATCH_VEC_END", value("TGT_LATCH_VEC_END") + 4),
        "STAMP-4": define("TGT_LATCH_STAMP", value("TGT_LATCH_STAMP") - 4),
        "LIMIT-1": define("TGT_LATCH_LIMIT_MS", value("TGT_LATCH_LIMIT_MS") - 1),
        "LIMIT+1": define("TGT_LATCH_LIMIT_MS", value("TGT_LATCH_LIMIT_MS") + 1),
        "no-hit-advance": (hit_store, "    if (eligible) {\n"),
        "no-rearm-ms": (rearm_store, "        latch->end = end;\n"),
        "return-word+4": ("        return 0U;\n    return ld32(c->esp);\n}",
                          "        return 0U;\n    return ld32(c->esp + 4U);\n}"),
        "observe-returns-latch": (observe_read,
                                  "#else\n        ms = latch->ms;\n"
                                  "        if (ms != latch->ms)\n"),
        "ticks-inverted": ("        if (ms != latch->ms)\n            ++k->ticks;",
                           "        if (ms == latch->ms)\n            ++k->ticks;"),
        "flips-inverted": ("            ((uint32_t)(latch->ms - stamp) > "
                           "TGT_LATCH_LIMIT_MS))\n            ++k->flips;",
                           "            ((uint32_t)(latch->ms - stamp) >= "
                           "TGT_LATCH_LIMIT_MS))\n            ++k->flips;"),
        "no-fresh-count": ("        ++k->fresh;\n", "\n"),
        "ahead-sign": ("        if ((int32_t)(stamp - latch->ms) > 0)",
                       "        if ((int32_t)(stamp - latch->ms) >= 0)"),
        "reciprocal-wiring": ("        return isaac_vita_tgt_ms_from_us(t);\n    }",
                              "        return isaac_vita_tgt_ms_from_us(t + 1000U);"
                              "\n    }"),
    })
    for name, (old, _new) in mutants.items():
        require(text.count(old) == 1, f"mutant {name}: anchor not unique")
    return mutants


EQUIVALENT: dict[str, str] = {}


def mutation_test(build: Build) -> str:
    text = HANDLER.read_text(encoding="utf-8")
    mutants = latch_mutants(text)
    mutdir = build.work / "latch-mutants"
    mutdir.mkdir()

    def one(item):
        name, (old, new) = item
        d = mutdir / name
        d.mkdir()
        src = d / HANDLER.name
        src.write_text(text.replace(old, new), encoding="utf-8", newline="\n")
        shapes = build.compile_shapes(src, f"mut_{name}", strict=False)
        exe = build.link(f"mut_{name}", "gpr-masked-call", shapes)
        rc, out = run_bn([str(exe), "fast", "gpr-masked-call"], build.env, 600)
        first = out.strip().splitlines()[0] if out.strip() else str(rc)
        return name, rc != 0, first

    with concurrent.futures.ThreadPoolExecutor(max_workers=6) as pool:
        outcomes = list(pool.map(one, mutants.items()))
    survivors = []
    for name, killed, first in outcomes:
        print(f"  mutant {name}: {'killed' if killed else 'SURVIVED'} "
              f"({first[:100]})")
        if not killed:
            survivors.append(name)
    unexpected = [s for s in survivors if s not in EQUIVALENT]
    stale = [s for s in EQUIVALENT if s not in survivors]
    require(not unexpected, f"mutants survived: {unexpected}")
    require(not stale, f"listed as equivalent but killed: {stale}")
    for s in survivors:
        print(f"  equivalent: {s}: {EQUIVALENT[s]}")
    return (f"latch mutants={len(outcomes)} killed={len(outcomes) - len(survivors)}"
            f" equivalent={len(survivors)}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", type=Path, required=True)
    parser.add_argument("--generated-dir", type=Path, required=True)
    parser.add_argument("--keep", type=Path)
    parser.add_argument("--no-mutants", action="store_true")
    parser.add_argument("--quick-b1", action="store_true",
                        help="B1 on 2^24-value windows (development only)")
    arguments = parser.parse_args()

    verify_frozen(arguments.pe)
    bodies, census = extract(arguments.generated_dir)
    lines = [census, verify_citations(bodies), verify_import_contract()]
    env, cl = build_all.msvc_env()

    def go(work: Path) -> None:
        lines.append(verify_build_gate(work))
        lines.extend(b1(env, cl, work, arguments.quick_b1))
        lines.append(import_tests(env, cl, work))
        build = Build(env, cl, work, bodies)
        lines.extend(oracle_flavours(build))
        if not arguments.no_mutants:
            lines.append("mutation: " + mutation_test(build))

    if arguments.keep:
        shutil.rmtree(arguments.keep, ignore_errors=True)
        arguments.keep.mkdir(parents=True)
        go(arguments.keep)
    else:
        with tempfile.TemporaryDirectory() as tmp:
            go(Path(tmp))
    for line in lines:
        print(line)
    print("Vita Clear expiry latch + timeGetTime reciprocal: PASS; "
          f"pins={len(PINS)} bodies, flavours={len(FLAVOURS)}, "
          f"shapes={len(SHAPES)}{'' if not arguments.quick_b1 else ' (B1 quick)'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
