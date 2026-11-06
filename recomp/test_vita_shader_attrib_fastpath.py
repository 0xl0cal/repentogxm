#!/usr/bin/env python3
"""Prove the Shader::EnableAttribs/DisableAttribs host fast path (wf/opt-attrib).

1. Frozen inputs: PE identity, the two format jump tables, the unique Shader
   vtable slots and the four function-pointer slots the generator pins.
2. Codegen: gen_all renders both roots (with the real jump-table discovery)
   at both image bases and emits exactly one authenticated body marker per
   root - the extern declaration plus the bracketed seam
   `GUEST_GPR_FLUSH(c); if (helper(c)) return; GUEST_GPR_RELOAD(c);` - and
   nothing else changes in the body.
3. Corpus contract (--generated-dir): exactly one unit defines each root,
   with exactly one marker each; the emitted RET sequences publish no flags
   (the replay leaves the lazy flag state alone).
4. Differential oracle (MSVC x86, /LARGEADDRESSAWARE): the corpus bodies,
   extracted verbatim, linked with and without their seam in both GPR
   spellings against the production dispatcher, gl_bridge and host_vita_gl;
   see host_vita_shader_attrib_fastpath_oracle.c for the case list (the
   review added: every slot word corrupted with zero/near/foreign-registered
   tokens including the glVertexAttribPointer slot, a frame straddling the
   stack ceiling, ESP below the floor).  Three flavours of the same oracle:
   the frozen build, ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO (frozen cases
   against an emptied memo, then fill/hit/miss/relink/reuse/unmemoisable/
   decline cases whose hit streams are the reference minus its
   glGetAttribLocation lines) and the VERIFY variant (every lookup still
   performed; a forced stale entry counts one mismatch per attribute).
   RENDER60 S2 adds the memo behind the real typed GL recorder and its
   consumer thread (memo-recorder, memo-recorder-verify, memo-recorder-64),
   the 64-entry memo inline (memo-64), and three mutants that must FAIL:
   an asynchronous FENCE (generated from a monkeypatched surface), a memo
   without the generation compare and a memo without the name-bytes compare.
5. Build gate: the CMake options are default OFF; the fast path requires
   ISAAC_VITA_GL_SHIM_FASTDISPATCH, the memo requires the fast path and
   VERIFY requires the memo; ..._MEMO_ENTRIES is a STRING defaulting to 16
   whose definition reaches the fast-path TU only when it is not 16.
6. CMake census: the real owner/seam census block of recomp/vita/CMakeLists.txt
   runs under `cmake -P` (set_property stubbed) against the corpus - exactly
   one owner unit and one seam line per root - and must reject a corpus
   without the seams (the frozen shape), a seam duplicated in its owner and
   a marker that escaped into another unit.  The S2 option checks run the
   same way (ENTRIES values, ENTRIES without the memo, MEMO_VERIFY with
   GL_LOCATION_CACHE_VERIFY).
7. Static S2 pins: the recorder surface (FENCES are the 7 program mutators,
   disjoint from FIRE; every FENCE body resets the producer location cache
   and ends in record_sync), the 9 generation bump sites, install/uninstall
   only through the synchronous worker call, the only worker_post, no raw
   vitaGL program mutator outside its wrapper, and the PE census of static
   Shader objects behind the memo geometry.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import build_all as BA  # noqa: E402

RUNTIME = HERE / "runtime"
VITA_CMAKE = HERE / "vita" / "CMakeLists.txt"
PE_SIZE = 8_650_240
PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
ENABLE_ROOT = 0x005672D0
ENABLE_END = 0x005673A9
DISABLE_ROOT = 0x005673F0
TABLES_RVA = 0x005673AC
TABLES_BYTES = 64
GUARD = "#if defined(__vita__) && defined(ISAAC_VITA_SHADER_ATTRIB_FASTPATH)"
ROOTS = {
    ENABLE_ROOT: ("enable", "isaac_vita_shader_attribs_enable_try",
                  "Authenticated Shader::EnableAttribs host fast path"),
    DISABLE_ROOT: ("disable", "isaac_vita_shader_attribs_disable_try",
                   "Authenticated Shader::DisableAttribs host fast path"),
}
ORACLE_RESULT = (
    "Vita shader attrib fast path oracle: PASS; cases=150; "
    "handled=80; rejected=70; backend=1360; probes=5"
)
# ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO flavours of the same oracle: the
# frozen cases run against an emptied memo (identical streams), then the memo
# section (fill/hit/miss/relink/reuse/unmemoisable/decline); VERIFY performs
# every lookup and counts the forced stale entry (6 attributes) exactly once.
# The recorder flavours compile the real recorder units next to the oracle.
RECORDER_DEFINES = [
    "/DISAAC_VITA_GL_THREAD_RECORD=1",
    "/DISAAC_VITA_GL_THREAD_WORKER=1",
    "/DISAAC_GL_RECORD_WORKER_JITTER=1",
    "/DISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO=1",
    "/DORACLE_ATTRIB_RECORDER=1",
]
RECORDER_SOURCES = ("gl_record_backend.c", "gl_record_ring.c",
                    "gl_record_locations.c")
# Printed by every flavour before its result line: location uses
# checked against the current link epoch, none stale; primed lookups behind
# the recorder.
EPOCH_LINE = re.compile(
    r"^location epoch check: uses=(\d+) stale=0 exempt=(\d+)(?: primed=(\d+))?$")
# The cases whose "locations" are no glGetAttribLocation answers (lookup slot
# = the glCullFace token, an import table owning the token family), each in
# both GPR spellings.
EPOCH_EXEMPT = 6
ORACLE_FLAVOURS = {
    "base": ([], ORACLE_RESULT),
    "memo": (["/DISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO=1"],
             "Vita shader attrib fast path oracle: PASS; cases=186; "
             "handled=115; rejected=71; backend=2021; probes=5; "
             "memo-mismatches=0"),
    "memo-verify": (["/DISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO=1",
                     "/DISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO_VERIFY=1"],
                    "Vita shader attrib fast path oracle: PASS; cases=188; "
                    "handled=117; rejected=71; backend=2057; probes=5; "
                    "memo-mismatches=6"),
    # ISAAC_VITA_SHADER_ATTRIB_DIRECT_STATE: the same cases with the replay
    # handing its attribute set to the (recorded) backend batch; every
    # handled replay with attributes takes exactly one batch and the streams
    # stay identical.  direct-decline forces the batch's declined verdict, so
    # every case proves the fail-closed per-call fallback (one decline per
    # handled replay with attributes, same streams).
    "direct": (["/DISAAC_VITA_SHADER_ATTRIB_DIRECT_STATE=1"],
               "Vita shader attrib fast path oracle: PASS; cases=150; "
               "handled=80; rejected=70; backend=1360; probes=5; "
               "direct(batch,decl)=78,0"),
    "direct-decline": (["/DISAAC_VITA_SHADER_ATTRIB_DIRECT_STATE=1",
                        "/DORACLE_ATTRIB_DIRECT_DECLINE=1"],
                       "Vita shader attrib fast path oracle: PASS; cases=150; "
                       "handled=80; rejected=70; backend=1360; probes=5; "
                       "direct(batch,decl)=0,78"),
    "direct-memo": (["/DISAAC_VITA_SHADER_ATTRIB_DIRECT_STATE=1",
                     "/DISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO=1"],
                    "Vita shader attrib fast path oracle: PASS; cases=186; "
                    "handled=115; rejected=71; backend=2021; probes=5; "
                    "memo-mismatches=0; direct(batch,decl)=112,0"),
    "direct-memo-verify": (["/DISAAC_VITA_SHADER_ATTRIB_DIRECT_STATE=1",
                            "/DISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO=1",
                            "/DISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO_VERIFY=1"],
                           "Vita shader attrib fast path oracle: PASS; cases=188; "
                           "handled=117; rejected=71; backend=2057; probes=5; "
                           "memo-mismatches=6; direct(batch,decl)=114,0"),
    # ISAAC_VITA_GL_TIME_SDK_SPARSE_ATTRIB (ph120.gt mode=sdk32 scope=attrib):
    # the same frozen cases with the 1/32 replay timer compiled in; every
    # HANDLED replay is counted per kind, residue 0 times the ordinals 0, 32,
    # 64, ... (ceil(replays / 32) per kind), the streams and every other
    # census stay identical, and the clock-read total is pinned.
    "sparse-attrib": (["/DISAAC_VITA_GL_TIME_SDK_SPARSE_ATTRIB=1"],
                      "Vita shader attrib fast path oracle: PASS; cases=150; "
                      "handled=80; rejected=70; backend=1360; probes=5; "
                      "sparse(en,di,ten,tdi,clocks)=54,26,2,1,6"),
    "direct-sparse-attrib": (["/DISAAC_VITA_SHADER_ATTRIB_DIRECT_STATE=1",
                              "/DISAAC_VITA_GL_TIME_SDK_SPARSE_ATTRIB=1"],
                             "Vita shader attrib fast path oracle: PASS; cases=150; "
                             "handled=80; rejected=70; backend=1360; probes=5; "
                             "direct(batch,decl)=78,0; "
                             "sparse(en,di,ten,tdi,clocks)=54,26,2,1,6"),
    # RENDER60 S2: the memo behind the REAL typed recorder and its consumer
    # thread (recipes A and B run ISAAC_VITA_GL_THREAD_RECORD +
    # ISAAC_VITA_GL_THREAD_WORKER): every backend call and every generation
    # write runs on the consumer, the seven FENCE mutators are real typed
    # records whose consumer sleeps before it bumps the word, the per-run
    # ring record count is pinned, two real static Shader objects exercise
    # the memo slot alias, and the nested-callback case is skipped.
    "memo-recorder": (RECORDER_DEFINES,
                      "Vita shader attrib fast path oracle: PASS; cases=202; "
                      "handled=133; rejected=69; backend=2337; probes=5; "
                      "memo-mismatches=0; recorder(records,saved,mutators,"
                      "gen-writes,skipped)=2337,153,10,307,2; alias=collide"),
    "memo-recorder-verify": (RECORDER_DEFINES +
                             ["/DISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO_VERIFY=1"],
                             "Vita shader attrib fast path oracle: PASS; cases=204; "
                             "handled=135; rejected=69; backend=2373; probes=5; "
                             "memo-mismatches=6; recorder(records,saved,mutators,"
                             "gen-writes,skipped)=2373,0,10,307,2; alias=collide"),
    # S2b: ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO_ENTRIES=64 ((self >> 2)
    # index).  Inline, every frozen and memo case keeps its 16-entry verdict
    # (same line as "memo"); behind the recorder the alias pair stays
    # resident (alias=distinct, two more hits).
    "memo-64": (["/DISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO=1",
                 "/DISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO_ENTRIES=64"],
                "Vita shader attrib fast path oracle: PASS; cases=186; "
                "handled=115; rejected=71; backend=2021; probes=5; "
                "memo-mismatches=0"),
    "memo-recorder-64": (RECORDER_DEFINES +
                         ["/DISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO_ENTRIES=64"],
                         "Vita shader attrib fast path oracle: PASS; cases=202; "
                         "handled=133; rejected=69; backend=2337; probes=5; "
                         "memo-mismatches=0; recorder(records,saved,mutators,"
                         "gen-writes,skipped)=2337,165,10,307,2; alias=distinct"),
    # RENDER60 S4: ISAAC_VITA_GL_RECORD_ATTRIB_BATCH on top of the memo.  A
    # memo-hit replay with attributes hands its set to the recorder in one
    # call.  The S4 section (counts 1/8/16 in both spellings, Enable and
    # Disable, the {2,2,4,4,3,1,3} component mix with a duplicated and two -1
    # locations, cancel pairs, negative stride, stride 0 at base 0, a
    # decline between batches, a relink between Enable and Disable) runs
    # first in the memo section, then every memo case runs as before.
    # record-batch-memo: stand-ins of the recorder API expanding to the
    # per-call sequence (streams identical to the reference);
    # record-batch-decline: ready() forced to 0 on every hit (the per-call
    # fallback, same streams).  The oracle requires one batch (or one forced
    # decline) per seamed memo-hit replay with attributes, per case and in
    # total, and one banner (none when declined).
    # S4 section: 27 cases (26 handled, 1 declined); batches: 28 Enable and
    # 10 Disable hits = 13 + 8 in the S4 section, 15 + 2 in the memo section
    # (counted by hand against the case list, equal to the oracle's own
    # per-case expectation).
    "record-batch-memo": (["/DISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO=1",
                           "/DISAAC_VITA_GL_RECORD_ATTRIB_BATCH=1"],
                          "Vita shader attrib fast path oracle: PASS; cases=213; "
                          "handled=141; rejected=72; backend=2577; probes=5; "
                          "memo-mismatches=0; batch(en,di,decl)=28,10,0"),
    "record-batch-decline": (["/DISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO=1",
                              "/DISAAC_VITA_GL_RECORD_ATTRIB_BATCH=1",
                              "/DORACLE_RECORD_BATCH_DECLINE=1"],
                             "Vita shader attrib fast path oracle: PASS; cases=213; "
                             "handled=141; rejected=72; backend=2577; probes=5; "
                             "memo-mismatches=0; batch(en,di,decl)=0,0,38"),
    # The real API behind the real ring and consumer thread: on top of the
    # memo-recorder checks, each hit removes its lookups and turns 2n
    # (Enable) or n (Disable) records into one, both attribute shadows end
    # byte-identical to the reference run's from the same poisoned start,
    # and the recorder's own batch counters equal the per-case census.
    # Against memo-recorder: +27 cases, +7 FENCE-loop Enable hits batched
    # (35,10), one more mutator (the S4 relink) and two more generation
    # writes (that relink and the closing bump); saved 825 = lookups +
    # (2n-1) per Enable hit + (n-1) per Disable hit, checked per case.
    "memo-recorder-batch": (RECORDER_DEFINES +
                            ["/DISAAC_VITA_GL_RECORD_ATTRIB_BATCH=1"],
                            "Vita shader attrib fast path oracle: PASS; cases=229; "
                            "handled=159; rejected=70; backend=2893; probes=5; "
                            "memo-mismatches=0; recorder(records,saved,mutators,"
                            "gen-writes,skipped)=2893,825,11,309,2; alias=collide; "
                            "batch(en,di,decl)=35,10,0"),
    "memo-recorder-batch-64": (RECORDER_DEFINES +
                               ["/DISAAC_VITA_GL_RECORD_ATTRIB_BATCH=1",
                                "/DISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO_ENTRIES=64"],
                               "Vita shader attrib fast path oracle: PASS; cases=229; "
                               "handled=159; rejected=70; backend=2893; probes=5; "
                               "memo-mismatches=0; recorder(records,saved,mutators,"
                               "gen-writes,skipped)=2893,859,11,309,2; alias=distinct; "
                               "batch(en,di,decl)=37,10,0"),
}

COMMON = [
    "/nologo", "/W4", "/WX", "/O2", "/Gy", "/std:c11",
    "/wd4310", "/wd4702", "/wd4996",
    "/DGUEST_IMAGE_BASE=0x98000000u",
]
DISPATCH_DEFINES = [
    "/DGUEST_STACK_REQUIRED=1",
    "/DISAAC_VITA_IMPORT_ID_DISPATCH=1",
    "/DISAAC_VITA_GUEST_LOOKUP_CACHE=1",
    "/DISAAC_VITA_PHASE_PROFILE=1",
    "/DISAAC_VITA_GL_SHIM_FASTDISPATCH=1",
    "/DISAAC_VITA_SHADER_ATTRIB_FASTPATH=1",
]
# The production compile definitions of a generated unit that matter to
# these two bodies (recomp/vita/CMakeLists.txt: GUEST_GENERATED_STACK_GUARD=0,
# GUEST_FLAGS_LOCAL=1, GUEST_COVERAGE_HOOKS=0; GUEST_GPR_LOCAL per variant).
BODY_DEFINES = [
    "/DGUEST_STACK_REQUIRED=1",
    "/DGUEST_GENERATED_STACK_GUARD=0",
    "/DGUEST_FLAGS_LOCAL=1",
    "/DGUEST_COVERAGE_HOOKS=0",
    "/DISAAC_VITA_IMPORT_ID_DISPATCH=1",
    "/DISAAC_VITA_GUEST_LOOKUP_CACHE=1",
    "/DISAAC_VITA_PHASE_PROFILE=1",
    "/DISAAC_VITA_GL_SHIM_FASTDISPATCH=1",
]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def digest(path: Path) -> str:
    value = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            value.update(block)
    return value.hexdigest()


def run(command: list[str], env: dict[str, str], cwd: Path | None = None) -> str:
    completed = subprocess.run(
        command, cwd=cwd or HERE, env=env,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
    )
    out = completed.stdout.decode("cp866", errors="replace")
    if completed.returncode != 0:
        print(out[-6000:])
        raise AssertionError(
            f"command failed ({completed.returncode}): {command[:3]}...")
    return out.replace("\r\n", "\n")


# ---- 1. frozen inputs --------------------------------------------------------

def pe_section_reader(pe: bytes):
    e_lfanew = struct.unpack_from("<I", pe, 0x3C)[0]
    count = struct.unpack_from("<H", pe, e_lfanew + 6)[0]
    optional = struct.unpack_from("<H", pe, e_lfanew + 20)[0]
    sections = []
    offset = e_lfanew + 24 + optional
    for _ in range(count):
        vsize, va, rsize, rp = struct.unpack_from("<IIII", pe, offset + 8)
        sections.append((va, max(vsize, rsize), rp))
        offset += 40

    def read(rva: int, size: int) -> bytes:
        for va, span, rp in sections:
            if va <= rva < va + span:
                return pe[rp + rva - va: rp + rva - va + size]
        raise AssertionError(f"RVA {rva:#x} outside every section")
    return read


def verify_frozen_inputs(pe_path: Path, gen_all) -> bytes:
    pe = pe_path.read_bytes()
    read = pe_section_reader(pe)
    tables = read(TABLES_RVA, TABLES_BYTES)
    require(hashlib.sha256(tables).hexdigest() ==
            gen_all.VITA_SHADER_ATTRIB_FASTPATH_TABLES_SHA256,
            "format jump tables changed")
    words = struct.unpack("<16I", tables)
    components = {0x567313: 1, 0x56731A: 2, 0x567321: 3, 0x567328: 4}
    advances = {0x567369: 4, 0x567370: 8, 0x567377: 12, 0x56737E: 16}
    ncomp = [components[w - 0x400000] for w in words[:8]]
    adv = [advances[w - 0x400000] for w in words[8:]]
    require(ncomp == [1, 2, 3, 4, 3, 4, 2, 1], f"component table {ncomp}")
    require(adv == [4 * n for n in ncomp], f"advance table {adv}")
    vtable = read(gen_all.VITA_SHADER_ATTRIB_FASTPATH_VTABLE_RVA,
                  gen_all.VITA_SHADER_ATTRIB_FASTPATH_VTABLE_BYTES)
    require(hashlib.sha256(vtable).hexdigest() ==
            gen_all.VITA_SHADER_ATTRIB_FASTPATH_VTABLE_SHA256,
            "Shader vtable changed")
    slots = struct.unpack("<9I", vtable)
    require(slots[5] - 0x400000 == ENABLE_ROOT and
            slots[6] - 0x400000 == DISABLE_ROOT, "vtable slots 5/6 moved")
    for root, spec in gen_all.VITA_SHADER_ATTRIB_FASTPATH_ROOTS.items():
        body = read(root, spec["end_rva"] - root)
        require(hashlib.sha256(body).hexdigest() == spec["body_sha256"],
                f"body bytes changed at {root:#x}")
        require(body[-3:] == b"\xc2\x0c\x00", f"ret 0xc missing at {root:#x}")
    # The fast path's format table must be the frozen one.
    source = (RUNTIME / "host_vita_shader_attrib_fastpath.c").read_text(
        encoding="utf-8")
    require("= { 1u, 2u, 3u, 4u, 3u, 4u, 2u, 1u };" in source,
            "fast path component table drifted from the jump table")
    return tables


# ---- 2. codegen ----------------------------------------------------------------

def verify_codegen(pe_path: Path, gen_all, Image, DEFAULT_BASE) -> None:
    pin = Image(str(pe_path), DEFAULT_BASE)
    entries, edges, rejected, tables = gen_all.discover_jump_tables(
        pin, ENABLE_ROOT, owner_end=ENABLE_END)
    require(len(entries) == 8 and not rejected and
            sorted(edges) == [0x56730C, 0x567362] and
            all(len(t) == 8 for t in tables.values()),
            f"switch discovery changed: {entries!r} {edges!r} {rejected!r}")
    switch_info = {
        ENABLE_ROOT: {"entries": entries, "edges": edges,
                      "rejected": rejected, "tables": tables},
        DISABLE_ROOT: {"entries": (), "edges": {}, "rejected": {},
                       "tables": {}},
    }
    for base in (DEFAULT_BASE, 0x98000000):
        image = Image(str(pe_path), base)
        for root, (kind, helper, marker) in ROOTS.items():
            result = gen_all._translate_function(
                image, {"rva": root}, None, switch_info, pin_img=pin)
            require(result["stub"] is None, f"{root:#x} became a stub")
            require(result["vita_shader_attrib_fastpath"] == kind,
                    f"fast path not selected for {root:#x} at {base:#x}")
            text = result["text"]
            check_body_shape(text, root, helper, marker, base)
            require(result.get("legacy_size_delta", 0) < 0,
                    "packing delta does not subtract the seam bytes")
            stripped = strip_guarded(text)
            require(gen_all.unit_packing_size(stripped) ==
                    gen_all.unit_packing_size(text) +
                    result["legacy_size_delta"],
                    "packing size does not equal the pre-seam size")


def strip_guarded(text: str) -> str:
    out = []
    skipping = False
    for line in text.split("\n"):
        if line == GUARD:
            skipping = True
            continue
        if skipping:
            if line == "#endif":
                skipping = False
            continue
        out.append(line)
    return "\n".join(out)


def check_body_shape(text: str, root: int, helper: str, marker: str,
                     base: int) -> None:
    require(text.count(GUARD) == 2, f"{root:#x}: guard count {text.count(GUARD)}")
    require(text.count(helper + "(") == 2, f"{root:#x}: helper spelled twice")
    require(text.count(marker) == 1, f"{root:#x}: marker once")
    require(f"    extern int {helper}(CPU *__restrict);" in text,
            f"{root:#x}: extern declaration missing")
    seam = (f"    /* {marker}; exact translated fallback follows. */\n"
            f"    GUEST_GPR_FLUSH(c);\n"
            f"    if ({helper}(c))\n"
            f"        return;\n"
            f"    GUEST_GPR_RELOAD(c);\n"
            f"#endif\n")
    require(seam in text, f"{root:#x}: bracketed seam shape changed")
    # The seam sits between the coverage hook and the first x86 statement.
    first = f"    /* {root:08x}  push e"
    require(text.index(seam) < text.index(first),
            f"{root:#x}: seam not at the root instruction")
    # A full generation assigns coverage ids; the hook then precedes the seam
    # (the focused render of verify_codegen has no ids and no hook).
    if "guest_coverage_function(" in text:
        require(text.index("guest_coverage_function(") < text.index(seam),
                f"{root:#x}: seam precedes the coverage hook")
    # Flags are dead at the RETs: no flag flush is published before them.
    for match in re.finditer(r"    /\* [0-9a-f]{8}  ret 0xc \*/\n((?:.*\n){1,4})",
                             text):
        require("GUEST_FLAGS_FLUSH" not in match.group(1),
                f"{root:#x}: RET publishes flags")
    slots = {0x5672D0: ("0x%08x" % ((base + 0x7C294C) & 0xFFFFFFFF),),
             0x5673F0: ("0x%08x" % ((base + 0x7C294C) & 0xFFFFFFFF),)}
    for spelled in slots[root]:
        require(f"call dword ptr [{spelled}]" in text,
                f"{root:#x}: slot call spelling at base {base:#x}")


# ---- 3. corpus contract -------------------------------------------------------

BODY_RE = re.compile(
    r"^/\* (sub_[0-9a-f]{8})  RVA [0-9a-f]{8}  \d+ insns, \d+ switch entries \*/\n"
    r"void \1\(CPU \*__restrict c\)\n\{\n.*?^\}\n", re.S | re.M)


def extract_bodies(generated_dir: Path) -> tuple[Path, dict[str, str]]:
    owners: dict[str, list[Path]] = {"sub_005672d0": [], "sub_005673f0": []}
    bodies: dict[str, str] = {}
    marker_units: dict[str, list[Path]] = {m: [] for _, _, m in ROOTS.values()}
    for unit in sorted(generated_dir.glob("guest_[0-9][0-9][0-9][0-9].c")):
        text = unit.read_text(encoding="utf-8")
        for _, _, marker in ROOTS.values():
            if marker in text:
                marker_units[marker].append(unit)
        for name in owners:
            if f"\nvoid {name}(CPU *__restrict c)\n" in text:
                owners[name].append(unit)
                for match in BODY_RE.finditer(text):
                    if match.group(1) == name:
                        bodies[name] = match.group(0)
    for name, units in owners.items():
        require(len(units) == 1, f"{name}: owners {units}")
    for marker, units in marker_units.items():
        require(len(units) == 1, f"marker escaped/missing: {marker} {units}")
    require(owners["sub_005672d0"] == owners["sub_005673f0"] ==
            marker_units[ROOTS[ENABLE_ROOT][2]],
            "both roots and their markers must share one owner unit")
    owner = owners["sub_005672d0"][0]
    text = owner.read_text(encoding="utf-8")
    for root, (kind, helper, marker) in ROOTS.items():
        require(text.count(marker) == 1, f"{marker}: {text.count(marker)}")
        require(text.count(helper + "(") == 2, f"{helper}: spelled twice")
        body = bodies[f"sub_{root:08x}"]
        check_body_shape(body, root, helper, marker, 0x98000000)
    require(text.count(GUARD) == 4, f"guard lines in owner: {text.count(GUARD)}")
    return owner, bodies


# ---- 4. oracle -----------------------------------------------------------------

def build_bodies(env, compiler: str, work: Path,
                 bodies: dict[str, str]) -> list[str]:
    """The four link-time variants of the two translated bodies; shared by
    every oracle flavour (the memo define never reaches generated units)."""
    source = ["/* Generated by test_vita_shader_attrib_fastpath.py from the",
              " * frozen corpus unit; the only edit is the seam guard. */",
              "#include <math.h>", '#include "guest.h"',
              "void sub_0055e330(CPU *__restrict c);", ""]
    for name in ("sub_005672d0", "sub_005673f0"):
        body = bodies[name]
        require(body.count(GUARD) == 2, "guard count in extracted body")
        source.append(body.replace(GUARD, "#if defined(ORACLE_ATTRIB_SEAM)"))
    bodies_c = work / "attrib_bodies.c"
    bodies_c.write_text("\n".join(source), encoding="utf-8", newline="\n")
    variants = {
        "seam_gpr": ["/DGUEST_GPR_LOCAL=1", "/DORACLE_ATTRIB_SEAM=1"],
        "ref_gpr": ["/DGUEST_GPR_LOCAL=1",
                    "/Dsub_005672d0=ref_sub_005672d0",
                    "/Dsub_005673f0=ref_sub_005673f0"],
        "seam_mem": ["/DGUEST_GPR_LOCAL=0", "/DORACLE_ATTRIB_SEAM=1",
                     "/Dsub_005672d0=mem_sub_005672d0",
                     "/Dsub_005673f0=mem_sub_005673f0"],
        "ref_mem": ["/DGUEST_GPR_LOCAL=0",
                    "/Dsub_005672d0=mem_ref_sub_005672d0",
                    "/Dsub_005673f0=mem_ref_sub_005673f0"],
    }
    objects = []
    for tag, extra in variants.items():
        obj = work / f"attrib_bodies_{tag}.obj"
        run([compiler, "/nologo", "/W3", "/O2", "/std:c11", "/wd4310",
             "/wd4996", "/DGUEST_IMAGE_BASE=0x98000000u"] + BODY_DEFINES +
            extra + ["/I", str(RUNTIME), "/c", str(bodies_c),
                     "/Fo:" + str(obj)], env)
        objects.append(str(obj))
    return objects


def build_and_run_oracle(env, compiler: str, work: Path, objects: list[str],
                         tables: bytes, flavour: str,
                         extra_defines: list[str],
                         recorder_backend: Path | None = None,
                         may_fail: bool = False,
                         oracle_source: Path | None = None) -> tuple[int, str]:
    """Build one flavour and run it; returns (exit code, output).  The
    recorder flavours add the three real recorder units; recorder_backend
    replaces gl_record_backend.c (the async-fence mutant compiles a copy that
    sits next to its generated surface); oracle_source replaces the oracle
    (the S4 replay mutants compile a copy that sits next to a patched
    host_vita_shader_attrib_fastpath.c, which its quoted include finds
    first)."""
    exe = work / f"shader-attrib-oracle-{flavour}.exe"
    objdir = work / f"obj-{flavour}"
    objdir.mkdir()
    recorder = []
    if "/DORACLE_ATTRIB_RECORDER=1" in extra_defines:
        recorder = [str(RUNTIME / unit) for unit in RECORDER_SOURCES]
        if recorder_backend is not None:
            recorder[0] = str(recorder_backend)
    run([compiler] + COMMON + DISPATCH_DEFINES + extra_defines + [
        "/I", str(RUNTIME),
        str(oracle_source or
            RUNTIME / "host_vita_shader_attrib_fastpath_oracle.c"),
        str(RUNTIME / "gl_bridge.c"),
        str(RUNTIME / "host_vita_gl.c"),
    ] + recorder + objects + [
        "/Fe:" + str(exe), "/Fo:" + str(objdir) + os.sep,
        "/link", "/LARGEADDRESSAWARE", "/OPT:REF", "/INCREMENTAL:NO",
    ], env)
    completed = subprocess.run([str(exe), tables.hex()], cwd=HERE, env=env,
                               stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT)
    out = completed.stdout.decode("cp866", errors="replace").replace(
        "\r\n", "\n")
    (work / f"oracle-{flavour}.out").write_text(out, encoding="utf-8")
    if completed.returncode != 0 and not may_fail:
        print(out[-8000:])
        raise AssertionError(
            f"oracle {flavour} exited {completed.returncode}")
    return completed.returncode, out


def run_oracle_flavours(env, compiler: str, work: Path,
                        bodies: dict[str, str], tables: bytes) -> list[str]:
    objects = build_bodies(env, compiler, work, bodies)
    results = []
    for flavour, (extra_defines, expected) in ORACLE_FLAVOURS.items():
        _, out = build_and_run_oracle(env, compiler, work, objects, tables,
                                      flavour, extra_defines)
        lines = out.strip().splitlines()
        result = lines[-1] if lines else ""
        failures = [line for line in lines if line.startswith("FAIL")]
        if failures or result != expected:
            print(out[-8000:])
            raise AssertionError(
                f"oracle result changed ({flavour}): {result!r}")
        if flavour in RECORDER_ALIAS_ENTRIES:
            # The C slot computation must agree with the census model below
            # (memo_slot), which is what the PE census pins.
            alias = [line for line in lines if line.startswith("alias ")]
            want = alias_line(RECORDER_ALIAS_ENTRIES[flavour])
            require(alias == [want],
                    f"alias slot line ({flavour}): {alias!r} != {want!r}")
        # The backend-side epoch check ran on every location use
        # (and, behind the recorder, the mutator cases primed the producer
        # location cache), so a stale location fails even when it reaches
        # both bodies alike.
        epoch = [m for m in (EPOCH_LINE.match(line) for line in lines) if m]
        recorder = "/DORACLE_ATTRIB_RECORDER=1" in extra_defines
        require(len(epoch) == 1 and int(epoch[0].group(1)) > 0 and
                int(epoch[0].group(2)) == EPOCH_EXEMPT and
                (epoch[0].group(3) is not None) == recorder and
                (not recorder or int(epoch[0].group(3)) > 0),
                f"location epoch check line ({flavour}): "
                f"{[m.group(0) for m in epoch]!r}")
        results.append(f"[{flavour}] {result}; {epoch[0].group(0)}")
    results += run_oracle_mutants(env, compiler, work, objects, tables)
    return results


# ---- 7. RENDER60 S2: the memo behind the typed recorder ---------------------
#
# Static half of the S2 proof (the recorder flavours above are the dynamic
# half).  The recorder flavours compile gl_record_surface.inc, an artefact of
# recomp/vita/gl_record_surface.py, so the generator runs here and its output
# must equal the checked-in file.

GL_RECORD_SURFACE_PY = HERE / "vita" / "gl_record_surface.py"
GL_RECORD_SURFACE_INC = RUNTIME / "gl_record_surface.inc"
GL_SURFACE_HEADER = RUNTIME / "gl_surface_generated.h"
GL_VITA_BACKEND_C = RUNTIME / "gl_vita_backend.c"
KAGE_VITA_BACKEND_C = RUNTIME / "kage_vita_backend.c"
FASTPATH_H = RUNTIME / "host_vita_shader_attrib_fastpath.h"
FASTPATH_C = RUNTIME / "host_vita_shader_attrib_fastpath.c"
# The seven program-state mutators of the guest GL surface: the recorder's
# FENCES, the shim location cache's invalidation set and the memo's bumps.
PROGRAM_MUTATORS = frozenset((
    "glAttachShader", "glCompileShader", "glCreateProgram", "glDeleteProgram",
    "glDeleteShader", "glLinkProgram", "glShaderSource"))
# Program-state entry points outside the guest surface; the port must not
# call them raw either.
OTHER_PROGRAM_MUTATORS = ("glBindAttribLocation", "glDetachShader",
                          "glProgramBinary", "glShaderBinary",
                          "glProgramParameteri")
# The oracle's alias objects (ORACLE_ALIAS_A/B in the oracle): ColorOffset
# and the KAGE Position+Color+TexCoord built-in, committed image addresses.
ALIAS_A = 0x987C4F3C
ALIAS_B = 0x987C7A9C
GUEST_IMAGE_BASE = 0x98000000      # COMMON /DGUEST_IMAGE_BASE
RECORDER_ALIAS_ENTRIES = {"memo-recorder": 16, "memo-recorder-verify": 16,
                          "memo-recorder-64": 64, "memo-recorder-batch": 16,
                          "memo-recorder-batch-64": 64}


def memo_slot(address: int, entries: int) -> int:
    """attrib_memo_slot (16 entries, host_vita_shader_attrib_fastpath.c) and
    ISAAC_VITA_SHADER_ATTRIB_MEMO_SLOT (other sizes, the header's end); the
    source spellings are pinned in verify_memo_census and the C slots are
    compared with this model through the oracle's alias line."""
    if entries == 16:
        return ((address >> 4) ^ (address >> 9)) & 15
    return (address >> 2) & (entries - 1)


def alias_line(entries: int) -> str:
    a, b = memo_slot(ALIAS_A, entries), memo_slot(ALIAS_B, entries)
    return (f"alias entries={entries} slot({ALIAS_A:08x})={a} "
            f"slot({ALIAS_B:08x})={b} {'collide' if a == b else 'distinct'}; "
            f"legacy16={memo_slot(ALIAS_A, 16)},{memo_slot(ALIAS_B, 16)}")


def load_record_surface():
    import importlib.util
    spec = importlib.util.spec_from_file_location("gl_record_surface_s2",
                                                  GL_RECORD_SURFACE_PY)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def strip_c_comments(text: str) -> str:
    """Blank comments and string/char literals; every newline survives, so
    line numbers stay valid."""
    out = []
    i, n = 0, len(text)
    while i < n:
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif text[i] in "\"'":
            quote, j = text[i], i + 1
            while j < n and text[j] != quote and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            j = min(j + 1, n)
            out.append(quote + " " * (j - i - 2) + quote)
            i = j
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def c_functions(text: str) -> list[tuple[str, int, int]]:
    """(name, header line, closing line) of every function definition whose
    braces sit in column 0 (the style of every file pinned here)."""
    lines = text.split("\n")
    found = []
    index = 0
    while index < len(lines):
        line = lines[index]
        match = re.match(r"[A-Za-z_][^;#=]*?\b(\w+)\s*\(", line)
        if not match or line.startswith(("typedef", "struct", "enum")):
            index += 1
            continue
        body = None
        for probe in range(index, min(index + 16, len(lines))):
            if lines[probe].startswith("{"):
                body = probe
                break
            if ";" in lines[probe] or lines[probe].startswith("#"):
                break
        if body is None:
            index += 1
            continue
        close = next(k for k in range(body + 1, len(lines))
                     if lines[k].startswith("}"))
        found.append((match.group(1), index, close))
        index = close + 1
    return found


def owner_of(functions: list[tuple[str, int, int]], line: int) -> str | None:
    for name, start, end in functions:
        if start <= line <= end:
            return name
    return None


def call_sites(text: str, pattern: str) -> list[tuple[int, str | None]]:
    """(line, enclosing function) of every non-preprocessor match."""
    functions = c_functions(text)
    sites = []
    for number, line in enumerate(text.split("\n")):
        if line.lstrip().startswith("#"):
            continue
        for _ in re.finditer(pattern, line):
            sites.append((number + 1, owner_of(functions, number)))
    return sites


def function_text(text: str, name: str) -> str:
    lines = text.split("\n")
    hits = [(start, end) for fn, start, end in c_functions(text) if fn == name]
    require(len(hits) == 1, f"{name}: {len(hits)} definitions")
    start, end = hits[0]
    return "\n".join(lines[start:end + 1])


def production_sources() -> list[Path]:
    """Every C source under recomp/runtime and recomp/vita, recursively
    (vendor/, third_party/, vitagl-overlay/, openal-overlay/ and
    vitagl-stock-reference/ included), except the host oracles and their
    stand-in headers (*oracle* in the file or directory name, *_test_vitagl*,
    host_tests/), the desktop backends (gl_pc_backend*, kage_pc_backend*: not
    in the Vita target) and the host link probes (*_link_probe.c and
    vitagl-overlay/link_probe.c, a host main() that links vitaGL)."""
    paths = []
    for directory in (RUNTIME, HERE / "vita"):
        for path in sorted(directory.rglob("*")):
            if path.suffix not in (".c", ".h", ".inc") or not path.is_file():
                continue
            parts = [part.lower() for part in
                     path.relative_to(directory).parts[:-1]]
            if any("oracle" in part or part == "host_tests" for part in parts):
                continue
            name = path.name.lower()
            if ("oracle" in name or "_test_vitagl" in name or
                    name.startswith(("gl_pc_backend", "kage_pc_backend")) or
                    name == "link_probe.c" or name.endswith("_link_probe.c")):
                continue
            paths.append(path)
    return paths


def function_index(paths: list[Path]) -> dict[str, list[tuple[str, str]]]:
    """{function name: [(file, comment-stripped definition text), ...]} of
    every column-0 function definition (c_functions) in `paths`; a name with
    #if variants keeps every definition."""
    index: dict[str, list[tuple[str, str]]] = {}
    for path in paths:
        text = strip_c_comments(path.read_text(encoding="utf-8",
                                               errors="replace"))
        lines = text.split("\n")
        for name, start, end in c_functions(text):
            index.setdefault(name, []).append(
                (path.name, "\n".join(lines[start:end + 1])))
    return index


def call_closure(index: dict, root: str) -> set[str]:
    """Every indexed function reachable from `root` by direct calls, every
    #if variant followed.  Calls through function pointers and macro bodies
    are not followed (an under-approximation, stated by the caller)."""
    seen, todo = {root}, [root]
    while todo:
        for _, body in index.get(todo.pop(), []):
            for callee in re.findall(r"\b([A-Za-z_]\w*)\s*\(", body):
                if callee in index and callee not in seen:
                    seen.add(callee)
                    todo.append(callee)
    return seen


def verify_recorder_pins() -> list[str]:
    surface = load_record_surface()
    generated = surface.emit()
    require(GL_RECORD_SURFACE_INC.read_text(encoding="utf-8") == generated,
            "gl_record_surface.inc is stale: run recomp/vita/"
            "gl_record_surface.py --write")
    fences, fire = set(surface.FENCES), set(surface.FIRE)
    custom = set(surface.CUSTOM)
    require(fences == PROGRAM_MUTATORS,
            f"recorder FENCES drifted: {sorted(fences ^ PROGRAM_MUTATORS)}")
    require(not fences & fire, f"FENCES and FIRE overlap: {sorted(fences & fire)}")
    require(not fences & custom,
            f"FENCES and CUSTOM overlap: {sorted(fences & custom)}")
    header = GL_SURFACE_HEADER.read_text(encoding="utf-8")
    surface_names = set(re.findall(r"^\s+\w+ \(\*(gl\w+)\)\(", header, re.M))
    require(len(surface_names) == 73, "guest GL surface is not 73 entries")
    require(not surface_names & set(OTHER_PROGRAM_MUTATORS),
            "a program-state entry point outside the seven reached the guest "
            f"surface: {sorted(surface_names & set(OTHER_PROGRAM_MUTATORS))}")
    results = ["pin recorder surface: generator output == gl_record_surface.inc; "
               "FENCES == the 7 program mutators; FENCES & FIRE = {}; "
               "FENCES & CUSTOM = {}"]

    # Every FENCE body: reset the producer location cache, enqueue, wait.
    bodies = dict(re.findall(
        r"^static [^\n]*\brecord_(gl\w+)\([^\n]*\)\n\{\n(.*?)\n\}$",
        generated, re.M | re.S))
    for name in sorted(fences):
        require(name in bodies, f"no generated body for record_{name}")
        statements = [line.strip() for line in bodies[name].split("\n")]
        require(statements[0].startswith(f"Record_{name} a = "),
                f"record_{name}: first statement {statements[0]!r}")
        require(statements[1:4] == [
            "isaac_gl_record_locations_reset(&s_locations);",
            f"record_enqueue(REC_{name}, &a, sizeof a, 0u, 0u);",
            "record_sync();"], f"record_{name} is not reset+enqueue+sync: "
            f"{statements!r}")
        require(statements[4:] in ([], ["return (guest_gl_uint)s_result;"]),
                f"record_{name}: unexpected tail {statements[4:]!r}")
    resetters = {name for name, body in bodies.items()
                 if "isaac_gl_record_locations_reset" in body}
    require(resetters == fences,
            f"producer location-cache resets outside FENCES: "
            f"{sorted(resetters ^ fences)}")
    results.append("pin FENCE bodies: 7 x reset + enqueue + record_sync; no "
                   "other generated body resets the producer location cache")

    # The nine bump sites of the generation word.
    backend = strip_c_comments(GL_VITA_BACKEND_C.read_text(encoding="utf-8"))
    invalidate = call_sites(backend,
                            r"\bgl_vita_location_cache_invalidate\(\)\s*;")
    reset = call_sites(backend, r"\bgl_vita_location_cache_reset\(\)\s*;")
    bump = call_sites(backend, r"\bGL_VITA_LOCATION_GENERATION_BUMP\(\)\s*;")
    require(sorted(owner for _, owner in invalidate) ==
            sorted(f"vita_{name}" for name in fences),
            f"invalidate callers drifted: {invalidate!r}")
    require(sorted(owner for _, owner in reset) ==
            ["gl_vita_backend_install", "gl_vita_backend_uninstall"],
            f"location-cache reset callers drifted: {reset!r}")
    require(sorted(owner for _, owner in bump) ==
            ["gl_vita_location_cache_invalidate",
             "gl_vita_location_cache_reset"],
            f"generation bump outside the two entry points: {bump!r}")
    for alias in ("invalidate", "reset"):
        require(re.search(r"#\s*define gl_vita_location_cache_" + alias +
                          r"\(\) GL_VITA_LOCATION_GENERATION_BUMP\(\)",
                          backend) is not None,
                f"the no-location-cache {alias} no longer bumps")
    writes = re.compile(r"(\+\+|--)\s*g_isaac_vita_gl_location_generation\b|"
                        r"\bg_isaac_vita_gl_location_generation\s*"
                        r"(\+\+|--|[-+*/|&^]?=(?!=))")
    write_lines = []
    for path in production_sources():
        text = strip_c_comments(path.read_text(encoding="utf-8",
                                               errors="replace"))
        for number, line in enumerate(text.split("\n"), 1):
            if writes.search(line):
                write_lines.append((path.name, line.strip()))
    require(write_lines == [
        ("gl_vita_backend.c", "uint32_t g_isaac_vita_gl_location_generation = 1u;"),
        ("gl_vita_backend.c",
         "if (++g_isaac_vita_gl_location_generation == 0u) \\"),
        ("gl_vita_backend.c", "g_isaac_vita_gl_location_generation = 1u; \\"),
    ], f"generation word written outside GL_VITA_LOCATION_GENERATION_BUMP: "
       f"{write_lines!r}")
    results.append(
        "pin generation bumps: 9 sites = invalidate in the 7 wrappers "
        f"(lines {','.join(str(l) for l, _ in invalidate)}) + reset in "
        f"install/uninstall (lines {','.join(str(l) for l, _ in reset)}); the "
        "bump only inside the two entry points; the word written nowhere else")

    # install/uninstall reach the consumer only through worker_call.
    for function, closure in (("gl_vita_backend_install",
                               "gl_vita_install_closure"),
                              ("gl_vita_backend_uninstall",
                               "gl_vita_uninstall_closure")):
        body = [line.strip() for line in
                function_text(backend, function).split("\n") if line.strip()]
        guard = body.index("#if defined(ISAAC_VITA_GL_THREAD_WORKER) && "
                           "ISAAC_VITA_GL_THREAD_WORKER")
        require(body[guard + 1:guard + 3] == [
            "if (isaac_gl_record_worker_active() && "
            "!isaac_gl_record_on_render_thread()) {",
            f"isaac_gl_record_worker_call({closure}, NULL);"],
            f"{function} does not start with its synchronous worker_call")
        require(guard < body.index("gl_vita_location_cache_reset();"),
                f"{function} resets before its worker guard")
        uses = re.findall(r"\b" + closure + r"\b", backend)
        require(len(uses) == 2, f"{closure} used {len(uses)} times "
                "(definition + one worker_call expected)")
    posts = []
    for path in production_sources():
        text = strip_c_comments(path.read_text(encoding="utf-8",
                                               errors="replace"))
        # A call passes "<closure>,"; the prototype and the definition
        # spell "void (*fn)(void *)" and do not match.
        for match in re.finditer(
                r"\bisaac_gl_record_worker_post\(\s*(\w+)\s*,", text):
            posts.append((path.name, match.group(1)))
    require(posts == [("kage_vita_backend.c", "kvb_present_closure")],
            f"worker_post call sites drifted: {posts!r}")
    # Transitively, over every production function definition
    # (every #if variant), the posted closure reaches no install, no
    # location cache, no generation write and no program-state wrapper.
    index = function_index(production_sources())
    reach = call_closure(index, "kvb_present_closure")
    banned = ({"gl_vita_backend_install", "gl_vita_backend_uninstall",
               "gl_vita_install_closure", "gl_vita_uninstall_closure",
               "gl_vita_location_cache_invalidate",
               "gl_vita_location_cache_reset"} |
              {f"vita_{name}" for name in fences})
    require(not reach & banned,
            f"the posted present closure reaches {sorted(reach & banned)}")
    for name in sorted(reach):
        for file_name, body in index[name]:
            for forbidden in ("GL_VITA_LOCATION_GENERATION_BUMP",
                              "g_isaac_vita_gl_location_generation",
                              "gl_vita_location_cache_",
                              "isaac_gl_record_wrap"):
                require(forbidden not in body,
                        f"{name} ({file_name}), reached from the posted "
                        f"present closure, mentions {forbidden}")
    reached_files = sorted({f for name in reach for f, _ in index[name]})
    results.append("pin install/uninstall: synchronous worker_call guard first, "
                   "closures used once; the only worker_post is "
                   "kvb_present_closure, whose direct-call closure "
                   f"({len(reach)} functions in {len(reached_files)} files) "
                   "reaches no install, location cache, generation write or "
                   "program-state wrapper")

    # No raw vitaGL program mutator outside its own wrapper.
    raw = re.compile(r"(?<![\w.>])(" + "|".join(
        sorted(PROGRAM_MUTATORS | set(OTHER_PROGRAM_MUTATORS))) + r")\s*\(")
    raw_sites = []
    for path in production_sources():
        text = strip_c_comments(path.read_text(encoding="utf-8",
                                               errors="replace"))
        functions = c_functions(text)
        for number, line in enumerate(text.split("\n")):
            if line.lstrip().startswith("#"):
                continue
            for match in raw.finditer(line):
                raw_sites.append((path.name, match.group(1),
                                  owner_of(functions, number), number + 1))
    stray = [site for site in raw_sites
             if site[0] != "gl_vita_backend.c" or site[2] != f"vita_{site[1]}"]
    require(not stray, f"raw program mutator outside its wrapper: {stray!r}")
    require({site[1] for site in raw_sites} == fences,
            f"a wrapper lost its raw call: {raw_sites!r}")
    # ...and each wrapper is reached only through the installed backend table
    # (so under the recorder only from a replayed FENCE record), never by a
    # direct call from another backend path.
    for name in sorted(fences):
        refs = re.findall(r"\bvita_" + name + r"\b", backend)
        table = re.findall(r"^\s*backend\." + name + r" = vita_" + name + r";$",
                           backend, re.M)
        require(len(refs) == 2 and len(table) == 1,
                f"vita_{name}: {len(refs)} references, {len(table)} table "
                "assignments (definition + one table entry expected)")
    results.append(f"pin raw program mutators: {len(raw_sites)} raw calls in "
                   f"{len(production_sources())} production sources, every one "
                   "inside its own vita_gl* wrapper; each wrapper referenced "
                   "only by its definition and its backend table entry")
    return results


def static_shader_objects(pe: bytes) -> dict[int, list[int]]:
    """Every `mov dword [imm32], <Shader vtable VA>` (C7 05) in an executable
    section: {committed guest address: [constructor VA, ...]}."""
    header = FASTPATH_H.read_text(encoding="utf-8")
    vtable_rva = int(re.search(
        r"#define ISAAC_VITA_SHADER_ATTRIB_VTABLE_RVA\s+(0x[0-9A-Fa-f]+)u",
        header).group(1), 16)
    e_lfanew = struct.unpack_from("<I", pe, 0x3C)[0]
    count = struct.unpack_from("<H", pe, e_lfanew + 6)[0]
    optional = struct.unpack_from("<H", pe, e_lfanew + 20)[0]
    image_base = struct.unpack_from("<I", pe, e_lfanew + 24 + 28)[0]
    sections = []
    offset = e_lfanew + 24 + optional
    for _ in range(count):
        _, rva, raw_size, raw_pointer = struct.unpack_from("<IIII", pe,
                                                           offset + 8)
        flags = struct.unpack_from("<I", pe, offset + 36)[0]
        sections.append((rva, raw_size, raw_pointer, flags))
        offset += 40
    needle = struct.pack("<I", image_base + vtable_rva)
    objects: dict[int, list[int]] = {}
    position = pe.find(needle)
    while position >= 0:
        if pe[position - 6:position - 4] == b"\xc7\x05":
            for rva, raw_size, raw_pointer, flags in sections:
                if (raw_pointer <= position - 6 < raw_pointer + raw_size and
                        flags & 0x20000000):
                    va = struct.unpack_from("<I", pe, position - 4)[0]
                    guest = GUEST_IMAGE_BASE + (va - image_base)
                    objects.setdefault(guest, []).append(
                        image_base + rva + position - 6 - raw_pointer)
        position = pe.find(needle, position + 1)
    return objects


def extra_occupants(addresses, entries: int) -> int:
    slots: dict[int, int] = {}
    for address in addresses:
        slot = memo_slot(address, entries)
        slots[slot] = slots.get(slot, 0) + 1
    return sum(n - 1 for n in slots.values() if n > 1)


def verify_memo_census(pe_path: Path) -> list[str]:
    """The S2b question, answered from the frozen PE: which static Shader
    objects share a memo slot at 16 and 64 entries, under both indexes."""
    source = FASTPATH_C.read_text(encoding="utf-8")
    header = FASTPATH_H.read_text(encoding="utf-8")
    require("return &s_attrib_memo[((self >> 4) ^ (self >> 9)) &" in source,
            "legacy fold drifted from memo_slot")
    require("(&s_attrib_memo[((uint32_t)(self) >> 2) & "
            "(ATTRIB_MEMO_ENTRIES - 1u)])" in header,
            "(self >> 2) index drifted from memo_slot")
    objects = static_shader_objects(pe_path.read_bytes())
    addresses = sorted(objects)
    require(len(addresses) == 23, f"static Shader census: {len(addresses)}")
    require(ALIAS_A in objects and ALIAS_B in objects,
            "alias objects are not static Shader objects")
    # The estimate verifier's claim (render-60 lever S2b): ColorOffset
    # 0x987c4f3c, water_v2 0x987c4e34 and colormod 0x987c4d2c share slot 4.
    slot4 = [a for a in addresses if memo_slot(a, 16) == 4]
    require(slot4 == [0x987C4D2C, 0x987C4E34, 0x987C4F3C, 0x987C7A9C],
            f"16-entry slot 4: {[hex(a) for a in slot4]}")
    legacy64 = {}
    for address in addresses:
        legacy64.setdefault(((address >> 4) ^ (address >> 9)) & 63,
                            []).append(address)
    pairs64 = sorted(v for v in legacy64.values() if len(v) > 1)
    require(pairs64 == [[0x987C4F3C, 0x987C7A9C], [0x987C4F68, 0x987C7AC8]],
            f"legacy fold at 64: {pairs64!r}")
    counts = {n: extra_occupants(addresses, n) for n in (16, 32, 64, 128)}
    require(counts == {16: 10, 32: 1, 64: 0, 128: 0},
            f"memo slot census changed: {counts!r}")
    return [
        f"memo census: {len(addresses)} static Shader objects; 16-entry slot 4 "
        "= colormod 0x987c4d2c, water_v2 0x987c4e34, ColorOffset 0x987c4f3c, "
        "KAGE textured 0x987c7a9c",
        "memo census: legacy fold at 64 entries still pairs 0x987c4f3c/"
        "0x987c7a9c and 0x987c4f68/0x987c7ac8 (2 extra occupants); the "
        "(self >> 2) index leaves extra occupants "
        f"16/32/64/128 = {counts[16]}/{counts[32]}/{counts[64]}/{counts[128]}"
        " (16 = the legacy fold)",
    ]


# Must-FAIL legs.  Each must exit non-zero, end on the oracle's FAIL line and
# fail every case in its required set (the cases the mutation is designed to
# break); other cases may fail too.
_RECORDER_MISSES = {f"memo-recorder-{name}-miss" for name in PROGRAM_MUTATORS}
_BATCH_INLINE_DEFINES = ORACLE_FLAVOURS["record-batch-memo"][0]
_BATCH_RECORDER_DEFINES = ORACLE_FLAVOURS["memo-recorder-batch"][0]
ORACLE_MUTANTS = {
    # The six void FENCES emitted as FIRE (no record_sync): the replay reads
    # the generation before the consumer bumped it.  glCreateProgram returns
    # a value and stays synchronous, so its case and the reuse case (whose
    # glCreateProgram waits for the glDeleteProgram before it) still pass.
    "mutant-async-fence": (RECORDER_DEFINES, "async-fence",
                           {"memo-relink-miss"} |
                           {f"memo-recorder-{name}-miss" for name in
                            PROGRAM_MUTATORS - {"glCreateProgram"}}),
    "mutant-memo-no-generation": (
        RECORDER_DEFINES + ["/DORACLE_MUTATE_MEMO_NO_GENERATION=1"], None,
        {"memo-relink-miss", "memo-reuse-miss"} | _RECORDER_MISSES),
    "mutant-memo-no-name-bytes": (
        RECORDER_DEFINES + ["/DORACLE_MUTATE_MEMO_NO_NAME_BYTES=1"], None,
        {"memo-mutable-rewrite-miss", "memo-mutable-shorter-miss",
         "memo-mutable-longer-miss"}),
    # The seven
    # FENCE bodies without their producer location-cache reset.  The memo
    # misses (its generation moved), the lookups then hit the producer cache
    # primed with the previous link's answers, and stale locations reach the
    # backend in BOTH bodies, so only the backend-side epoch check sees it.
    # Every case that runs a FENCE mutator must fail.
    "mutant-no-fence-reset": (RECORDER_DEFINES, "no-fence-reset",
                              {"memo-relink-miss", "memo-reuse-miss"} |
                              _RECORDER_MISSES),
    # RENDER60 S4.  Replay side, text-patched
    # copies of host_vita_shader_attrib_fastpath.c under the inline
    # record-batch-memo flavour: [ebp+8] from the FIRST remembered location
    # (visible wherever the first and last locations differ: the frame
    # compare), and last_format never updated (EAX/ECX of the retire).  The
    # stand-in without its base advance shows the stream compare sees a
    # wrong running pointer.  Recorder side, oracle defines in
    # gl_record_backend.c under the real recorder: the producer notes base
    # for every element (producer shadow), the consumer skips its shadow
    # updates (consumer shadow), the consumer issues all toggles before all
    # pointers (stream).  Count-1 cases cannot see the first two and the
    # order mutant, so every required case has at least 2 attributes.
    "mutant-batch-esp-first-location": (
        _BATCH_INLINE_DEFINES, "batch-esp-first",
        {"batch-count-8-hit", "batch-count-16-hit-mem", "memo-enable-hit"}),
    "mutant-batch-stale-last-format": (
        _BATCH_INLINE_DEFINES, "batch-last-format",
        {"batch-count-1-hit", "batch-count-8-hit", "batch-mix-enable-hit",
         "memo-enable-hit"}),
    "mutant-batch-standin-no-advance": (
        _BATCH_INLINE_DEFINES +
        ["/DORACLE_MUTATE_BATCH_STANDIN_NO_ADVANCE=1"], None,
        {"batch-count-8-hit", "batch-count-16-hit", "memo-enable-hit"}),
    "mutant-batch-producer-no-advance": (
        _BATCH_RECORDER_DEFINES +
        ["/DISAAC_GL_RECORD_ORACLE_MUTATE_ATTRIB_BATCH_NO_ADVANCE=1"], None,
        {"batch-count-8-hit", "batch-count-16-hit", "batch-mix-enable-hit"}),
    "mutant-batch-consumer-no-shadow": (
        _BATCH_RECORDER_DEFINES +
        ["/DISAAC_GL_RECORD_ORACLE_MUTATE_ATTRIB_BATCH_NO_CONSUME=1"], None,
        {"batch-count-1-hit", "batch-count-8-disable-hit",
         "batch-mix-enable-hit"}),
    "mutant-batch-consumer-order": (
        _BATCH_RECORDER_DEFINES +
        ["/DISAAC_GL_RECORD_ORACLE_MUTATE_ATTRIB_BATCH_ORDER=1"], None,
        {"batch-count-8-hit", "batch-count-16-hit-mem",
         "batch-mix-enable-hit", "memo-enable-hit"}),
}
RESET_LINE = "    isaac_gl_record_locations_reset(&s_locations);"


def async_fence_backend(work: Path) -> Path:
    """gl_record_backend.c next to a generated surface whose void FENCES are
    FIRE (enqueue, no wait); returns the copy to compile."""
    import shutil
    surface = load_record_surface()
    header = GL_SURFACE_HEADER.read_text(encoding="utf-8")
    returns = {name: ret for ret, name, _ in re.findall(
        r"^\s+(\w+) \(\*(gl\w+)\)\(([^\n]*)\);", header, re.M)}
    void_fences = {name for name in surface.FENCES if returns[name] == "void"}
    require(void_fences == PROGRAM_MUTATORS - {"glCreateProgram"},
            f"void FENCES: {sorted(void_fences)}")
    real = surface.emit()
    saved = surface.FIRE
    surface.FIRE = set(saved) | void_fences
    try:
        mutant = surface.emit()
    finally:
        surface.FIRE = saved
    import difflib
    delta = [line for line in difflib.unified_diff(
        real.split("\n"), mutant.split("\n"), lineterm="", n=0)
        if line[:1] in "+-" and not line.startswith(("+++", "---"))]
    require(delta == ["-    record_sync();"] * len(void_fences),
            f"async-fence surface changed more than the waits: {delta!r}")
    directory = work / "mutant-async-fence-src"
    directory.mkdir()
    (directory / "gl_record_surface.inc").write_text(mutant, encoding="utf-8",
                                                    newline="\n")
    shutil.copyfile(RUNTIME / "gl_record_backend.c",
                    directory / "gl_record_backend.c")
    return directory / "gl_record_backend.c"


def no_fence_reset_backend(work: Path) -> Path:
    """gl_record_backend.c next to the generated surface minus every producer
    location-cache reset (exactly the 7 FENCE bodies, pin 2); returns the copy
    to compile."""
    import difflib
    import shutil
    surface = load_record_surface()
    real = surface.emit()
    mutant = "\n".join(line for line in real.split("\n") if line != RESET_LINE)
    delta = [line for line in difflib.unified_diff(
        real.split("\n"), mutant.split("\n"), lineterm="", n=0)
        if line[:1] in "+-" and not line.startswith(("+++", "---"))]
    require(delta == ["-" + RESET_LINE] * len(PROGRAM_MUTATORS),
            f"no-fence-reset surface changed more than the resets: {delta!r}")
    directory = work / "mutant-no-fence-reset-src"
    directory.mkdir()
    (directory / "gl_record_surface.inc").write_text(mutant, encoding="utf-8",
                                                    newline="\n")
    shutil.copyfile(RUNTIME / "gl_record_backend.c",
                    directory / "gl_record_backend.c")
    return directory / "gl_record_backend.c"


MUTANT_BACKENDS = {"async-fence": async_fence_backend,
                   "no-fence-reset": no_fence_reset_backend}

# The S4 memo-hit branch of Shader::EnableAttribs, as the replay TU spells
# it; each mutant replaces exactly one line of it (pinned here, so a drift
# of the branch fails the copy instead of silently mutating nothing).
_BATCH_ESP_LINE = ("        st32(c->esp + 4u, (uint32_t)locations[count - 1u]);\n"
                   "        goto attrib_batch_retire;\n")
_BATCH_FORMAT_LINES = ("            components[i] = k_format_components[format - 1u];\n"
                       "            last_format = format;\n"
                       "        }\n"
                       "        ATTRIB_SAMPLER_PUBLISH(s_token_enable);\n"
                       "        isaac_gl_record_attribs_enable(")
FASTPATH_MUTATIONS = {
    "batch-esp-first": (_BATCH_ESP_LINE, _BATCH_ESP_LINE.replace(
        "locations[count - 1u]", "locations[0]")),
    "batch-last-format": (_BATCH_FORMAT_LINES, _BATCH_FORMAT_LINES.replace(
        "            last_format = format;\n", "")),
}


def fastpath_mutant_oracle(work: Path, tag: str) -> Path:
    """The oracle next to a copy of the replay TU with one S4 line changed;
    returns the oracle copy to compile."""
    import shutil
    old, new = FASTPATH_MUTATIONS[tag]
    text = FASTPATH_C.read_text(encoding="utf-8")
    require(text.count(old) == 1, f"S4 mutant anchor drifted: {tag}")
    mutant = text.replace(old, new)
    require(mutant != text, f"S4 mutant changed nothing: {tag}")
    directory = work / f"mutant-{tag}-src"
    directory.mkdir()
    (directory / FASTPATH_C.name).write_text(mutant, encoding="utf-8",
                                             newline="\n")
    oracle = directory / "host_vita_shader_attrib_fastpath_oracle.c"
    shutil.copyfile(RUNTIME / oracle.name, oracle)
    return oracle


def run_oracle_mutants(env, compiler: str, work: Path, objects: list[str],
                       tables: bytes) -> list[str]:
    results = []
    for mutant, (defines, special, required) in ORACLE_MUTANTS.items():
        backend = MUTANT_BACKENDS[special](work) if special in \
            MUTANT_BACKENDS else None
        oracle = fastpath_mutant_oracle(work, special) if special in \
            FASTPATH_MUTATIONS else None
        require(special is None or backend is not None or oracle is not None,
                f"unknown mutant source {special!r}")
        rc, out = build_and_run_oracle(env, compiler, work, objects, tables,
                                       mutant, defines,
                                       recorder_backend=backend,
                                       may_fail=True, oracle_source=oracle)
        lines = out.strip().splitlines()
        failed = {line.split()[1] for line in lines if line.startswith("FAIL ")}
        missing = sorted(required - failed)
        if (rc == 0 or not lines or not lines[-1].startswith(
                "Vita shader attrib fast path oracle: FAIL") or missing):
            print(out[-8000:])
            raise AssertionError(f"{mutant} did not fail as designed: rc={rc} "
                                 f"missing={missing}")
        results.append(f"[{mutant}] must-FAIL: rc={rc}; {lines[-1]}; "
                       f"{len(required)}/{len(required)} designed cases failed")
    return results


# ---- 5. build gate --------------------------------------------------------------

def verify_build_gate() -> None:
    cmake = VITA_CMAKE.read_text(encoding="utf-8")
    require(re.search(
        r"option\(ISAAC_VITA_SHADER_ATTRIB_FASTPATH\s+\"[^\"]+\"\s+OFF\)",
        cmake) is not None, "shader attrib fast path is not default OFF")
    require("ISAAC_VITA_SHADER_ATTRIB_FASTPATH requires "
            "ISAAC_VITA_GL_SHIM_FASTDISPATCH" in cmake,
            "GL_SHIM_FASTDISPATCH prerequisite gate missing")
    for option in ("ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO",
                   "ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO_VERIFY"):
        require(re.search(
            r"option\(" + option + r"\s+\"[^\"]+\"\s+OFF\)", cmake) is not None,
            f"{option} is not default OFF")
    require("ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO requires "
            "ISAAC_VITA_SHADER_ATTRIB_FASTPATH" in cmake,
            "memo prerequisite gate missing")
    require("ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO_VERIFY requires "
            "ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO" in cmake,
            "memo VERIFY prerequisite gate missing")
    require(cmake.count("ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO=1") == 1 and
            cmake.count("ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO_VERIFY=1") == 1,
            "memo compile definitions have more than one owner list each")
    # ISAAC_VITA_SHADER_ATTRIB_DIRECT_STATE: default OFF, needs the fast
    # path, rejects the per-attribute wrapper-time split and the stall-probe
    # breadcrumbs, one owner list (replay TU + typed backend) and the sampler
    # define reaching the backend only under it.
    require(re.search(
        r"option\(ISAAC_VITA_SHADER_ATTRIB_DIRECT_STATE\s+\"[^\"]+\"\s+OFF\)",
        cmake) is not None, "direct state is not default OFF")
    require("ISAAC_VITA_SHADER_ATTRIB_DIRECT_STATE requires "
            "ISAAC_VITA_SHADER_ATTRIB_FASTPATH" in cmake,
            "direct state prerequisite gate missing")
    require(re.search(
        r"if\(ISAAC_VITA_SHADER_ATTRIB_DIRECT_STATE AND ISAAC_VITA_GL_WRAPPER_TIME\)"
        r"\s*message\(FATAL_ERROR", cmake) is not None,
        "direct state does not reject GL_WRAPPER_TIME")
    require(re.search(
        r"if\(ISAAC_VITA_SHADER_ATTRIB_DIRECT_STATE AND ISAAC_VITA_STALL_PROBE\)"
        r"\s*message\(FATAL_ERROR", cmake) is not None,
        "direct state does not reject STALL_PROBE")
    require(cmake.count("ISAAC_VITA_SHADER_ATTRIB_DIRECT_STATE=1") == 1,
            "direct state compile definition has more than one owner list")
    require(re.search(
        r"if\(ISAAC_VITA_SHADER_ATTRIB_DIRECT_STATE\)\s*(?:#[^\n]*\n\s*)*"
        r"set_property\(SOURCE\s+"
        r"\"\$\{ISAAC_RUNTIME\}/host_vita_shader_attrib_fastpath\.c\"\s+"
        r"\"\$\{ISAAC_RUNTIME\}/gl_vita_backend\.c\"\s+"
        r"APPEND PROPERTY COMPILE_DEFINITIONS\s+"
        r"ISAAC_VITA_SHADER_ATTRIB_DIRECT_STATE=1\)\s*"
        r"if\(ISAAC_VITA_GUEST_SAMPLER\)\s*set_property\(SOURCE\s+"
        r"\"\$\{ISAAC_RUNTIME\}/gl_vita_backend\.c\"\s+"
        r"APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_GUEST_SAMPLER=1\)",
        cmake) is not None, "direct state owner list drifted")
    # RENDER60 S2b: the entry-count STRING knob, default 16, and its one
    # owner list (the replay TU only, only when it is not 16).
    require(re.search(
        r"set\(ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO_ENTRIES 16 CACHE STRING",
        cmake) is not None, "memo entry count is not a STRING defaulting to 16")
    require(re.search(
        r"if\(NOT ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO_ENTRIES STREQUAL \"16\"\)"
        r"\s*set_property\(SOURCE\s+"
        r"\"\$\{ISAAC_RUNTIME\}/host_vita_shader_attrib_fastpath\.c\"\s+"
        r"APPEND PROPERTY COMPILE_DEFINITIONS\s+"
        r"ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO_ENTRIES="
        r"\$\{ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO_ENTRIES\}\)",
        cmake) is not None, "memo entry count owner list drifted")
    require(cmake.count("ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO_ENTRIES="
                        "${ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO_ENTRIES})") == 1,
            "memo entry count definition has more than one owner list")
    # RENDER60 S4: ISAAC_VITA_GL_RECORD_ATTRIB_BATCH, default OFF, its two
    # FATAL checks (run under cmake -P by verify_s4_option_checks), one owner
    # list (replay TU + recorder) inside the fast-path block, and the census
    # reader's copy of the define only with the opcode census.
    require(re.search(
        r"option\(ISAAC_VITA_GL_RECORD_ATTRIB_BATCH\s+\"[^\"]+\"\s+OFF\)",
        cmake) is not None, "record attrib batch is not default OFF")
    require(cmake.count("ISAAC_VITA_GL_RECORD_ATTRIB_BATCH=1") == 2,
            "record attrib batch definition owner lists drifted")
    require(re.search(
        r"if\(ISAAC_VITA_GL_RECORD_ATTRIB_BATCH\)\s*(?:#[^\n]*\n\s*)*"
        r"set_property\(SOURCE\s+"
        r"\"\$\{ISAAC_RUNTIME\}/host_vita_shader_attrib_fastpath\.c\"\s+"
        r"\"\$\{ISAAC_RUNTIME\}/gl_record_backend\.c\"\s+"
        r"APPEND PROPERTY COMPILE_DEFINITIONS\s+"
        r"ISAAC_VITA_GL_RECORD_ATTRIB_BATCH=1\)\s*"
        r"if\(ISAAC_VITA_GL_RECORD_OPCODE_CENSUS\)\s*set_property\(SOURCE\s+"
        r"\"\$\{ISAAC_RUNTIME\}/kage_vita_phase_profile\.c\"\s+"
        r"APPEND PROPERTY COMPILE_DEFINITIONS\s+"
        r"ISAAC_VITA_GL_RECORD_ATTRIB_BATCH=1\)\s*endif\(\)",
        cmake) is not None, "record attrib batch owner list drifted")
    require("if(ISAAC_VITA_GL_RECORD_ATTRIB_BATCH)" in census_block(),
            "record attrib batch is routed outside the fast-path block")
    for marker in (ROOTS[ENABLE_ROOT][2], ROOTS[DISABLE_ROOT][2]):
        require(marker in cmake, f"CMake census lacks marker {marker!r}")
    require('"${ISAAC_RUNTIME}/host_vita_shader_attrib_fastpath.c"' in cmake,
            "replay TU not routed")


# ---- 6. CMake census -------------------------------------------------------

def census_block() -> str:
    """The real owner/seam census of recomp/vita/CMakeLists.txt, verbatim."""
    lines = VITA_CMAKE.read_text(encoding="utf-8").split("\n")
    start = lines.index("  if(ISAAC_VITA_SHADER_ATTRIB_FASTPATH)")
    depth = 0
    for index in range(start, len(lines)):
        stripped = lines[index].strip()
        if stripped.startswith("if("):
            depth += 1
        elif stripped.startswith("endif("):
            depth -= 1
            if depth == 0:
                return "\n".join(lines[start:index + 1])
    raise AssertionError("census block has no matching endif")


def posix(path: Path) -> str:
    return str(path).replace("\\", "/")


def strip_guarded(text: str) -> str:
    """Drop every `#if defined(__vita__) && defined(ISAAC_VITA_SHADER_ATTRIB_
    FASTPATH)` block (the frozen, seamless corpus shape)."""
    kept, skipping = [], False
    for line in text.split("\n"):
        if line.strip() == GUARD:
            skipping = True
            continue
        if skipping:
            if line.strip().startswith("#endif"):
                skipping = False
            continue
        kept.append(line)
    return "\n".join(kept)


def run_census(cmake: str, work: Path, tag: str, units: list[Path],
               env: dict[str, str]) -> tuple[int, str]:
    script = work / f"census_{tag}.cmake"
    script.write_text("\n".join([
        "cmake_minimum_required(VERSION 3.16)",
        "# The census reads definitions through the configure-local index",
        "# CMakeLists.txt includes before the block.",
        f'include("{posix(VITA_CMAKE.parent / "generated_definitions.cmake")}")',
        "# set_property(SOURCE ...) is not scriptable; the census only needs",
        "# its verdicts, the owner list and the routed replay TU.",
        "function(set_property)",
        "endfunction()",
        "set(ISAAC_VITA_SHADER_ATTRIB_FASTPATH ON)",
        f'set(ISAAC_RUNTIME "{posix(RUNTIME)}")',
        'set(ISAAC_GENERATED_CANONICAL_C "'
        + ";".join(posix(unit) for unit in units) + '")',
        "set(ISAAC_VITA_RUNTIME_SOURCES)",
        "set(ISAAC_VITA_GUEST_SAMPLER OFF)",
        "set(ISAAC_VITA_STALL_PROBE OFF)",
        census_block(),
        'message(STATUS "CENSUS owners=${ISAAC_VITA_SHADER_ATTRIB_FASTPATH_OWNERS}'
        ' runtime=${ISAAC_VITA_RUNTIME_SOURCES}")',
        "",
    ]), encoding="utf-8", newline="\n")
    completed = subprocess.run([cmake, "-P", str(script)], env=env,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = completed.stdout.decode("utf-8", "replace")
    return completed.returncode, re.sub(r"\s+", " ", out)


def s2_option_block() -> str:
    """The RENDER60 S2 option checks of recomp/vita/CMakeLists.txt, verbatim."""
    text = VITA_CMAKE.read_text(encoding="utf-8")
    start = text.index("# ---- RENDER60 S2 option checks")
    end = text.index("# ---- end of the RENDER60 S2 option checks ----")
    return text[start:end]


def verify_s2_option_checks(cmake: str, work: Path,
                            env: dict[str, str]) -> list[str]:
    """Run the real S2 option checks under cmake -P: the accepted shapes
    configure, every refused one stops with its own FATAL_ERROR."""
    names = {"ENTRIES": "ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO_ENTRIES",
             "MEMO": "ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO",
             "MEMO_VERIFY": "ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO_VERIFY",
             "CACHE_VERIFY": "ISAAC_VITA_GL_LOCATION_CACHE_VERIFY"}
    cases = [
        ("default", {"ENTRIES": "16"}, None),
        ("memo", {"ENTRIES": "16", "MEMO": "ON"}, None),
        ("memo-64", {"ENTRIES": "64", "MEMO": "ON"}, None),
        ("memo-128-verify", {"ENTRIES": "128", "MEMO": "ON",
                             "MEMO_VERIFY": "ON"}, None),
        ("cache-verify", {"ENTRIES": "16", "CACHE_VERIFY": "ON"}, None),
        ("64-without-memo", {"ENTRIES": "64"},
         "ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO_ENTRIES=64 requires "
         "ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO"),
        ("48", {"ENTRIES": "48", "MEMO": "ON"}, "must be 16, 32, 64 or 128"),
        ("16u", {"ENTRIES": "16u", "MEMO": "ON"}, "must be 16, 32, 64 or 128"),
        ("both-verify", {"ENTRIES": "16", "MEMO": "ON", "MEMO_VERIFY": "ON",
                         "CACHE_VERIFY": "ON"},
         "both count ph120.a loc(...,m), from two threads; enable one per build"),
    ]
    block = s2_option_block()
    accepted = refused = 0
    for tag, values, fatal in cases:
        script = work / f"s2_options_{tag}.cmake"
        script.write_text("\n".join(
            ["cmake_minimum_required(VERSION 3.16)"] +
            [f'set({names[key]} "{value}")' for key, value in values.items()] +
            [block, 'message(STATUS "S2 OPTIONS ACCEPTED")', ""]),
            encoding="utf-8", newline="\n")
        completed = subprocess.run([cmake, "-P", str(script)], env=env,
                                   stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT)
        out = re.sub(r"\s+", " ", completed.stdout.decode("utf-8", "replace"))
        if fatal is None:
            require(completed.returncode == 0 and "S2 OPTIONS ACCEPTED" in out,
                    f"S2 option checks refused {tag}: {out[-800:]}")
            accepted += 1
        else:
            require(completed.returncode != 0 and fatal in out and
                    "S2 OPTIONS ACCEPTED" not in out,
                    f"S2 option checks accepted {tag}: {out[-800:]}")
            refused += 1
    return [f"S2 option checks (cmake -P, verbatim block): {accepted} shapes "
            f"accepted, {refused} refused (entries outside 16|32|64|128, "
            "entries without the memo, MEMO_VERIFY with LOCATION_CACHE_VERIFY)"]


def s4_option_block() -> str:
    """The RENDER60 S4 option checks of recomp/vita/CMakeLists.txt, verbatim."""
    text = VITA_CMAKE.read_text(encoding="utf-8")
    start = text.index("# ---- RENDER60 S4 option checks")
    end = text.index("# ---- end of the RENDER60 S4 option checks ----")
    return text[start:end]


def verify_s4_option_checks(cmake: str, work: Path,
                            env: dict[str, str]) -> list[str]:
    """Run the real S4 option checks under cmake -P: the knob OFF and the
    knob with its four prerequisites configure; each missing prerequisite,
    MEMO_VERIFY and DIRECT_STATE stop with their FATAL_ERROR."""
    full = {"ISAAC_VITA_GL_RECORD_ATTRIB_BATCH": "ON",
            "ISAAC_VITA_GL_THREAD_RECORD": "ON",
            "ISAAC_VITA_GL_THREAD_WORKER": "ON",
            "ISAAC_VITA_SHADER_ATTRIB_FASTPATH": "ON",
            "ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO": "ON"}
    requires = ("ISAAC_VITA_GL_RECORD_ATTRIB_BATCH requires "
                "ISAAC_VITA_GL_THREAD_RECORD, ISAAC_VITA_GL_THREAD_WORKER, "
                "ISAAC_VITA_SHADER_ATTRIB_FASTPATH and "
                "ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO")
    exclusive = ("ISAAC_VITA_GL_RECORD_ATTRIB_BATCH batches memo hits behind "
                 "the recorder: ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO_VERIFY "
                 "and ISAAC_VITA_SHADER_ATTRIB_DIRECT_STATE must be OFF")
    cases = [("default", {}, None),
             ("prerequisites-without-batch",
              {k: v for k, v in full.items()
               if k != "ISAAC_VITA_GL_RECORD_ATTRIB_BATCH"}, None),
             ("batch", dict(full), None)]
    for missing in list(full)[1:]:
        cases.append((f"batch-without-{missing}",
                      {k: v for k, v in full.items() if k != missing},
                      requires))
    cases.append(("batch-memo-verify",
                  dict(full, ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO_VERIFY="ON"),
                  exclusive))
    cases.append(("batch-direct-state",
                  dict(full, ISAAC_VITA_SHADER_ATTRIB_DIRECT_STATE="ON"),
                  exclusive))
    block = s4_option_block()
    accepted = refused = 0
    for tag, values, fatal in cases:
        script = work / f"s4_options_{tag}.cmake"
        script.write_text("\n".join(
            ["cmake_minimum_required(VERSION 3.16)"] +
            [f'set({key} "{value}")' for key, value in values.items()] +
            [block, 'message(STATUS "S4 OPTIONS ACCEPTED")', ""]),
            encoding="utf-8", newline="\n")
        completed = subprocess.run([cmake, "-P", str(script)], env=env,
                                   stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT)
        out = re.sub(r"\s+", " ", completed.stdout.decode("utf-8", "replace"))
        if fatal is None:
            require(completed.returncode == 0 and "S4 OPTIONS ACCEPTED" in out,
                    f"S4 option checks refused {tag}: {out[-800:]}")
            accepted += 1
        else:
            require(completed.returncode != 0 and fatal in out and
                    "S4 OPTIONS ACCEPTED" not in out,
                    f"S4 option checks accepted {tag}: {out[-800:]}")
            refused += 1
    return [f"S4 option checks (cmake -P, verbatim block): {accepted} shapes "
            f"accepted, {refused} refused (each of the four prerequisites "
            "missing, MEMO_VERIFY, DIRECT_STATE)"]


def verify_census(generated_dir: Path, owner: Path, work: Path,
                  env: dict[str, str]) -> list[str]:
    import shutil
    cmake = (shutil.which("cmake", path=env.get("PATH")) or shutil.which("cmake")
             or "C:/Program Files/CMake/bin/cmake.exe")
    require(Path(cmake).exists(), "cmake is required for the census proof")
    units = sorted(generated_dir.glob("guest_[0-9][0-9][0-9][0-9].c"))
    other = next(unit for unit in units if unit.name != owner.name)
    marker = ROOTS[ENABLE_ROOT][2]
    text = owner.read_text(encoding="utf-8")
    results = []

    # (a) the real corpus: one owner and one seam per root, replay TU routed.
    rc, out = run_census(cmake, work, "real", units, env)
    require(rc == 0, f"census rejected the real corpus: {out[-1500:]}")
    require(f"CENSUS owners={posix(owner)} runtime="
            f"{posix(RUNTIME)}/host_vita_shader_attrib_fastpath.c" in out,
            f"census owner/routing drifted: {out[-1500:]}")
    results.append(f"census real corpus: PASS ({len(units)} units, owner "
                   f"{owner.name}, replay TU routed)")

    corrupt = work / "corrupt"
    corrupt.mkdir(exist_ok=True)

    def variant(tag: str, name: str, body: str) -> Path:
        directory = corrupt / tag
        directory.mkdir(exist_ok=True)
        path = directory / name
        path.write_text(body, encoding="utf-8", newline="\n")
        return path

    # (b) the frozen shape (owner without any seam) must be rejected.
    seamless = strip_guarded(text)
    require(marker not in seamless and GUARD not in seamless, "strip failed")
    rc, out = run_census(cmake, work, "noseam",
                         [variant("b", owner.name, seamless), other], env)
    require(rc != 0 and "missing or duplicate seam" in out,
            f"seamless owner accepted: {out[-1500:]}")
    results.append("census owner without seams: rejected (missing or duplicate seam)")

    # (c) a seam duplicated inside the owner must be rejected.
    line = next(l for l in text.split("\n") if marker in l)
    rc, out = run_census(cmake, work, "dup", [
        variant("c", owner.name, text.replace(line, line + "\n" + line, 1)),
        other], env)
    require(rc != 0 and "missing or duplicate seam" in out,
            f"duplicated seam accepted: {out[-1500:]}")
    results.append("census duplicated seam: rejected (missing or duplicate seam)")

    # (d) a marker that escaped into another unit must be rejected.
    rc, out = run_census(cmake, work, "escaped", [owner, variant(
        "d", other.name,
        other.read_text(encoding="utf-8") + "\n/* " + marker + " */\n")], env)
    require(rc != 0 and "escaped its frozen owner" in out,
            f"escaped marker accepted: {out[-1500:]}")
    results.append("census escaped marker: rejected (escaped its frozen owner)")

    # (e) a corpus without the owner unit must be rejected by the totals.
    rc, out = run_census(cmake, work, "noowner",
                         [unit for unit in units if unit != owner], env)
    require(rc != 0 and "owners=0, definitions=0, seams=0" in out,
            f"ownerless corpus accepted: {out[-1500:]}")
    results.append("census without the owner unit: rejected "
                   "(owners=0, definitions=0, seams=0)")
    results += verify_s2_option_checks(cmake, work, env)
    results += verify_s4_option_checks(cmake, work, env)
    return results


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", required=True, type=Path)
    parser.add_argument("--generated-dir", required=True, type=Path)
    parser.add_argument("--keep", type=Path,
                        help="keep the oracle build directory here")
    arguments = parser.parse_args()

    require(arguments.pe.stat().st_size == PE_SIZE, "frozen PE size changed")
    require(digest(arguments.pe) == PE_SHA256, "frozen PE hash changed")
    os.environ["REPENTOGXM_PE"] = str(arguments.pe)
    import gen_all  # noqa: E402
    from image import DEFAULT_BASE, Image  # noqa: E402

    tables = verify_frozen_inputs(arguments.pe, gen_all)
    verify_codegen(arguments.pe, gen_all, Image, DEFAULT_BASE)
    owner, bodies = extract_bodies(arguments.generated_dir)
    print(f"corpus owner: {owner.name} (one owner, one seam per root)")
    verify_build_gate()
    for line in verify_recorder_pins() + verify_memo_census(arguments.pe):
        print(line)

    env, compiler = BA.msvc_env()
    if arguments.keep:
        arguments.keep.mkdir(parents=True, exist_ok=True)
        for line in verify_census(arguments.generated_dir, owner,
                                  arguments.keep, env):
            print(line)
        results = run_oracle_flavours(env, compiler, arguments.keep, bodies,
                                      tables)
    else:
        with tempfile.TemporaryDirectory(prefix="isaac-shader-attrib-") as tmp:
            for line in verify_census(arguments.generated_dir, owner,
                                      Path(tmp), env):
                print(line)
            results = run_oracle_flavours(env, compiler, Path(tmp), bodies,
                                          tables)
    for result in results:
        print(result)
    flavours = sum(1 for result in results if "must-FAIL" not in result)
    print("Vita shader attrib fast path: PASS; roots=2; owner-unit seams=2; "
          f"oracle flavours={flavours}; must-FAIL mutants="
          f"{len(results) - flavours}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
