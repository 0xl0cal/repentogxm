#!/usr/bin/env python3
"""Prove the Image::PushQuad host fast path (wf/cpu-render).

1. Frozen inputs: PE identity, the root body (sub_0055f990), the three callee
   bodies the replay reproduces (sub_0055ee80 Ring::Alloc, sub_00561e20
   attribute size table with its 561e58 jump table, sub_0055e590 snapped-quad
   rebuild), the three tail callee bodies the replay enters by direct call
   (sub_005607a0 blend/batch select, sub_00561ff0 blend reset, sub_00026fb0
   dirty-vector growth; the growing Ring::Alloc is the first callee), the
   four direct callers, the .rdata constants and blend descriptors the
   bodies read and the replay's frozen size table.
2. Codegen: gen_all renders the root at both image bases and emits exactly
   one authenticated body marker - the extern declaration plus the bracketed
   seam `GUEST_GPR_FLUSH(c); if (isaac_vita_kage_quad_try(c)) return;
   GUEST_GPR_RELOAD(c);` - and nothing else changes in the body (the packing
   delta subtracts exactly the seam bytes so unit membership is unchanged).
3. Corpus contract (--generated-dir): exactly one unit defines the root with
   exactly one marker; both `ret 0x18` epilogues publish no flags.
4. Differential oracle (MSVC x86, /LARGEADDRESSAWARE): the ten corpus
   bodies (root, the three replayed callees, the floor thunk, the tail
   callees sub_005607a0/sub_00561ff0/sub_0055ed30/sub_00562030/sub_00026fb0),
   extracted verbatim, linked with and without the seam in both GPR
   spellings against the production dispatcher; see
   host_vita_kage_quad_fastpath_oracle.c for the case list (hand-written
   handled shapes incl. the unbatched select paths, ring/dirty growth and
   fresh ring records, 160 LCG-generated numeric cases incl. NaN/inf/-0/huge
   coordinates and every format layout, every decline reason, null-pointer
   probes, post-select fault probes).  Two flavours: the production build
   and the OBSERVE build, which must count exactly the same verdicts while
   the translated body runs.
5. Build gate: the CMake options are default OFF; the fast path requires
   ISAAC_VITA_TRANSLATED_CPU and ISAAC_VITA_FLOOR_THUNK_FASTPATH, OBSERVE
   requires the fast path; single owner lists for every definition.
6. CMake census: the real owner/seam census block of recomp/vita/CMakeLists.txt
   runs under `cmake -P` (set_property stubbed) against the corpus - exactly
   one owner unit and one seam line - and must reject a corpus without the
   seam (the frozen shape), a seam duplicated in its owner, a marker that
   escaped into another unit and a corpus without the owner unit.
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
ROOT = 0x0055F990
ROOT_END = 0x00560253
CALLEES = (0x0055EE80, 0x00561E20, 0x0055E590)
# Translated callees the replay enters by direct call (gen_all.py pins their
# bodies by hash: their `ret N` and memory effects are the replay's contract).
TAIL_CALLEES = (0x005607A0, 0x00561FF0, 0x00026FB0)
FLOOR_THUNK = 0x005EC3B2
TABLE_RVA = 0x00561E58
TABLE_BYTES = 32
GUARD = "#if defined(__vita__) && defined(ISAAC_VITA_KAGE_QUAD_FASTPATH)"
HELPER = "isaac_vita_kage_quad_try"
MARKER = "Authenticated Image::PushQuad host fast path"
BODIES = ["sub_0055f990", "sub_0055ee80", "sub_00561e20", "sub_0055e590",
          "sub_005ec3b2", "sub_005607a0", "sub_00561ff0", "sub_0055ed30",
          "sub_00562030", "sub_00026fb0"]
# .rdata words the body reads (RVA, expected float) plus the u32->f64 table.
CONSTANTS = [
    (0x0076A18C, 0.0), (0x0076A480, 0.5), (0x0076A214, 0.01),
    (0x0076A8CC, 4.0), (0x007AA92C, 480.0), (0x007AA930, 270.0),
    (0x00802FEC, None), (0x00802FF0, None),
]
CVT_TABLE_RVA = 0x0076C940
# The 16-byte blend descriptors the select compares (default) and installs
# (premultiplied translucent / translucent): sub_0055ed30 builds the same
# words, so they are pinned.
BLEND_DESCRIPTORS = [
    (0x0076B060, (1, 0, 1, 0)), (0x0076B3B0, (1, 7, 1, 7)),
    (0x0076B3C0, (6, 7, 1, 7)),
]
BLOB_BYTES = 128
ORACLE_RESULT = (
    "Vita kage quad fast path oracle: PASS; cases=614; handled=582; "
    "declined=32; culled=38; snapped=286; floors=1240; getters=458; "
    "unbatched=212; ring-growths=208; dirty-growths=32; faults=8; probes=20"
)
# OBSERVE never replays, so nothing is culled by the fast path, "snapped"
# and "unbatched" count the candidates (predicate true / flag 0x20 clear)
# rather than the paths taken after the cull exit, and the growth counters
# are the entry snapshot (batched images only).
ORACLE_RESULT_OBSERVE = (
    "Vita kage quad fast path oracle: PASS; cases=614; handled=582; "
    "declined=32; culled=0; snapped=296; floors=1240; getters=458; "
    "unbatched=226; ring-growths=114; dirty-growths=34; faults=8; probes=20"
)
ORACLE_FLAVOURS = {
    "base": ([], ORACLE_RESULT),
    "observe": (["/DISAAC_VITA_KAGE_QUAD_FASTPATH_OBSERVE=1"],
                ORACLE_RESULT_OBSERVE),
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
    "/DISAAC_VITA_PROFILE_IMPORT_KINDS=1",
    "/DISAAC_VITA_FLOOR_THUNK_FASTPATH=1",
    "/DISAAC_VITA_KAGE_QUAD_FASTPATH=1",
]
# The production compile definitions of a generated unit that matter to
# these bodies (recomp/vita/CMakeLists.txt: GUEST_GENERATED_STACK_GUARD=0,
# GUEST_FLAGS_LOCAL=1, GUEST_COVERAGE_HOOKS=0; GUEST_GPR_LOCAL per variant).
BODY_DEFINES = [
    "/DGUEST_STACK_REQUIRED=1",
    "/DGUEST_GENERATED_STACK_GUARD=0",
    "/DGUEST_FLAGS_LOCAL=1",
    "/DGUEST_COVERAGE_HOOKS=0",
    "/DISAAC_VITA_IMPORT_ID_DISPATCH=1",
    "/DISAAC_VITA_GUEST_LOOKUP_CACHE=1",
    "/DISAAC_VITA_PHASE_PROFILE=1",
    "/DISAAC_VITA_PROFILE_IMPORT_KINDS=1",
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
    """Returns the 128-byte blob the oracle installs at the image addresses."""
    pe = pe_path.read_bytes()
    read = pe_section_reader(pe)
    body = read(ROOT, ROOT_END - ROOT)
    require(hashlib.sha256(body).hexdigest() ==
            gen_all.VITA_KAGE_QUAD_FASTPATH_ROOT_BODY_SHA256,
            "root body bytes changed")
    require(body[-3:] == b"\xc2\x18\x00", "ret 0x18 missing at the root end")
    require(gen_all.VITA_KAGE_QUAD_FASTPATH_ROOT_END_RVA == ROOT_END and
            tuple(sorted(gen_all.VITA_KAGE_QUAD_FASTPATH_CALLEES)) ==
            tuple(sorted(CALLEES)), "callee set drifted")
    for callee, (end, sha) in gen_all.VITA_KAGE_QUAD_FASTPATH_CALLEES.items():
        require(hashlib.sha256(read(callee, end - callee)).hexdigest() == sha,
                f"callee body changed at {callee:#x}")
    require(tuple(sorted(gen_all.VITA_KAGE_QUAD_FASTPATH_TAIL_CALLEES)) ==
            tuple(sorted(TAIL_CALLEES)), "tail callee set drifted")
    for callee, (end, sha) in \
            gen_all.VITA_KAGE_QUAD_FASTPATH_TAIL_CALLEES.items():
        tail = read(callee, end - callee)
        require(hashlib.sha256(tail).hexdigest() == sha,
                f"tail callee body changed at {callee:#x}")
        # thiscall `ret 4` for the select and the blend reset; the dirty
        # growth returns with `ret 8` at 0x27073 and ends in the length-error
        # `int3` (the replay's ESP contract).
        if callee == 0x00026FB0:
            require(tail[0x27073 - callee:0x27076 - callee] == b"\xc2\x08\x00"
                    and tail[-1:] == b"\xcc",
                    "dirty growth ret 8 / int3 tail changed")
        else:
            require(tail[-3:] == b"\xc2\x04\x00",
                    f"tail callee {callee:#x} does not end in ret 4")
    table = read(TABLE_RVA, TABLE_BYTES)
    require(hashlib.sha256(table).hexdigest() ==
            gen_all.VITA_KAGE_QUAD_FASTPATH_SIZE_TABLE_SHA256,
            "attribute size jump table changed")
    words = struct.unpack("<8I", table)
    sizes = {0x561E2D: 4, 0x561E33: 8, 0x561E39: 12, 0x561E3F: 16}
    require([sizes[w - 0x400000] for w in words] == [4, 8, 12, 16, 12, 16, 8, 4],
            f"size table {words}")
    blob = b""
    for rva, expected in CONSTANTS:
        word = read(rva, 4)
        if expected is not None:
            require(abs(struct.unpack("<f", word)[0] - expected) < 1e-9,
                    f"constant at {rva:#x} is not {expected}")
        blob += word
    cvt = read(CVT_TABLE_RVA, 16)
    require(struct.unpack("<dd", cvt) == (0.0, 4294967296.0),
            "u32->f64 table changed")
    blob += cvt + table
    for rva, expected in BLEND_DESCRIPTORS:
        descriptor = read(rva, 16)
        require(struct.unpack("<4I", descriptor) == expected,
                f"blend descriptor at {rva:#x} is "
                f"{struct.unpack('<4I', descriptor)}, not {expected}")
        blob += descriptor
    require(len(blob) == BLOB_BYTES, "blob size")
    # The fast path's size table must be the frozen one.
    source = (RUNTIME / "host_vita_kage_quad_fastpath.c").read_text(
        encoding="utf-8")
    require("    4u, 8u, 12u, 16u, 12u, 16u, 8u, 4u\n" in source,
            "fast path size table drifted from the jump table")
    header = (RUNTIME / "host_vita_kage_quad_fastpath.h").read_text(
        encoding="utf-8")
    for name, value in (("ROOT_RVA", ROOT), ("RING_ALLOC_RVA", CALLEES[0]),
                        ("FORMAT_SIZE_RVA", CALLEES[1]),
                        ("SNAP_REBUILD_RVA", CALLEES[2]),
                        ("FLOOR_THUNK_RVA", FLOOR_THUNK),
                        ("FLOOR_IAT_RVA",
                         gen_all.VITA_FLOOR_THUNK_FASTPATH_IAT_RVA),
                        ("BATCH_SELECT_RVA", TAIL_CALLEES[0]),
                        ("BLEND_RESET_RVA", TAIL_CALLEES[1]),
                        ("DIRTY_GROW_RVA", TAIL_CALLEES[2]),
                        ("RET_GET_WIDTH", 0x0055F9F5),
                        ("RET_GET_HEIGHT", 0x0055FA1E),
                        # The return words of the direct call sites the
                        # replay pushes (gen_all VITA_KAGE_QUAD_FASTPATH_
                        # CALL_SITES: call at site, return word = site + 5).
                        ("RET_BATCH_SELECT", 0x0055FB74),
                        ("RET_RING_INDEX", 0x0055FD59),
                        ("RET_RING_VERTEX", 0x0055FD6C),
                        ("RET_DIRTY_GROW", 0x005601F3),
                        ("RET_BLEND_RESET", 0x00560218),
                        ("G_RENDERER", 0x007C7A30),
                        ("C_ALPHA_OPAQUE", 0x0076A8CC),
                        ("C_BLEND_DEFAULT", BLEND_DESCRIPTORS[0][0]),
                        ("C_BLEND_PREMULTIPLIED", BLEND_DESCRIPTORS[1][0]),
                        ("C_BLEND_TRANSLUCENT", BLEND_DESCRIPTORS[2][0]),
                        ("G_RESET_OFFSET_X", 0x00802FEC),
                        ("G_RESET_OFFSET_Y", 0x00802FF0)):
        require(f"#define ISAAC_VITA_KAGE_QUAD_{name} " in header and
                re.search(rf"ISAAC_VITA_KAGE_QUAD_{name}\s+0x{value:08X}u",
                          header) is not None,
                f"header constant {name} drifted")
    require(f"ISAAC_VITA_KAGE_QUAD_FLOOR_IMPORT_ID      "
            f"{gen_all.VITA_FLOOR_THUNK_FASTPATH_IMPORT_ID}u" in header,
            "floor import id drifted")
    return blob


# ---- 2. codegen ----------------------------------------------------------------

def verify_codegen(pe_path: Path, gen_all, Image, DEFAULT_BASE) -> None:
    pin = Image(str(pe_path), DEFAULT_BASE)
    entries, edges, rejected, tables = gen_all.discover_jump_tables(
        pin, CALLEES[1], owner_end=0x00561E57)
    require(len(entries) == 4 and not rejected and
            sorted(edges) == [0x561E26] and
            all(len(t) == 8 for t in tables.values()),
            f"switch discovery changed: {entries!r} {edges!r} {rejected!r}")
    switch_info = {
        ROOT: {"entries": (), "edges": {}, "rejected": {}, "tables": {}},
        CALLEES[1]: {"entries": entries, "edges": edges,
                     "rejected": rejected, "tables": tables},
    }
    for base in (DEFAULT_BASE, 0x98000000):
        image = Image(str(pe_path), base)
        result = gen_all._translate_function(
            image, {"rva": ROOT}, None, switch_info, pin_img=pin)
        require(result["stub"] is None, "root became a stub")
        require(result["vita_kage_quad_fastpath"] is True,
                f"fast path not selected at {base:#x}")
        text = result["text"]
        check_body_shape(text, base)
        require(result.get("legacy_size_delta", 0) < 0,
                "packing delta does not subtract the seam bytes")
        stripped = strip_guarded(text)
        require(gen_all.unit_packing_size(stripped) ==
                gen_all.unit_packing_size(text) + result["legacy_size_delta"],
                "packing size does not equal the pre-seam size")
        # The callees carry no seam.
        for callee in CALLEES:
            result = gen_all._translate_function(
                image, {"rva": callee}, None, switch_info, pin_img=pin)
            require(result["stub"] is None and
                    result["vita_kage_quad_fastpath"] is False and
                    GUARD not in result["text"],
                    f"callee {callee:#x} grew a seam")


def strip_guarded(text: str) -> str:
    out = []
    skipping = False
    for line in text.split("\n"):
        if line.strip() == GUARD:
            skipping = True
            continue
        if skipping:
            if line.strip().startswith("#endif"):
                skipping = False
            continue
        out.append(line)
    return "\n".join(out)


def check_body_shape(text: str, base: int) -> None:
    require(text.count(GUARD) == 2, f"guard count {text.count(GUARD)}")
    require(text.count(HELPER + "(") == 2, "helper spelled twice")
    require(text.count(MARKER) == 1, "marker once")
    require(f"    extern int {HELPER}(CPU *__restrict);" in text,
            "extern declaration missing")
    seam = (f"    /* {MARKER}; exact translated fallback follows. */\n"
            f"    GUEST_GPR_FLUSH(c);\n"
            f"    if ({HELPER}(c))\n"
            f"        return;\n"
            f"    GUEST_GPR_RELOAD(c);\n"
            f"#endif\n")
    require(seam in text, "bracketed seam shape changed")
    first = f"    /* {ROOT:08x}  push ebp */"
    require(text.index(seam) < text.index(first),
            "seam not at the root instruction")
    if "guest_coverage_function(" in text:
        require(text.index("guest_coverage_function(") < text.index(seam),
                "seam precedes the coverage hook")
    rets = list(re.finditer(r"    /\* [0-9a-f]{8}  ret 0x18 \*/\n((?:.*\n){1,4})",
                            text))
    require(len(rets) == 2, f"ret 0x18 sites: {len(rets)}")
    for match in rets:
        require("GUEST_FLAGS_FLUSH" not in match.group(1),
                "RET publishes flags")
    require(f"{base:#x}" is not None, "unused")


# ---- 3. corpus contract ----------------------------------------------------

BODY_RE = re.compile(
    r"^/\* (sub_[0-9a-f]{8})  RVA [0-9a-f]{8}  \d+ insns, \d+ switch entries \*/\n"
    r"void \1\(CPU \*__restrict c\)\n\{\n.*?^\}\n", re.M | re.S)


def extract_bodies(generated_dir: Path) -> tuple[Path, dict[str, str]]:
    owners: dict[str, list[Path]] = {name: [] for name in BODIES}
    bodies: dict[str, str] = {}
    marker_units: list[Path] = []
    for unit in sorted(generated_dir.glob("guest_[0-9][0-9][0-9][0-9].c")):
        text = unit.read_text(encoding="utf-8")
        if MARKER in text:
            marker_units.append(unit)
        for name in owners:
            if f"\nvoid {name}(CPU *__restrict c)\n" in text:
                owners[name].append(unit)
                for match in BODY_RE.finditer(text):
                    if match.group(1) == name:
                        bodies[name] = match.group(0)
    for name, units in owners.items():
        require(len(units) == 1, f"{name}: owners {units}")
        require(name in bodies, f"{name}: body not extracted")
    require(len(marker_units) == 1, f"marker escaped/missing: {marker_units}")
    require(owners["sub_0055f990"] == marker_units,
            "the root and its marker must share one owner unit")
    owner = owners["sub_0055f990"][0]
    text = owner.read_text(encoding="utf-8")
    require(text.count(MARKER) == 1, f"{MARKER}: {text.count(MARKER)}")
    require(text.count(HELPER + "(") == 2, f"{HELPER}: spelled twice")
    require(text.count(GUARD) == 2, f"guard lines in owner: {text.count(GUARD)}")
    check_body_shape(bodies["sub_0055f990"], 0x98000000)
    for name in BODIES[1:]:
        require(GUARD not in bodies[name], f"{name}: unexpected seam")
    # The frozen thunk shape: one guest_call on the IAT slot word (no floor
    # seam in this corpus; with GUEST_FLOOR_THUNK_FASTPATH=1 the seam adds the
    # direct helper, whose receipts the replay reproduces either way).
    thunk = bodies["sub_005ec3b2"]
    require("guest_call(c, ld32((uint32_t)((uint32_t)(int32_t)(-1738513116))))"
            in thunk or "0x00606524U" in thunk, "floor thunk shape changed")
    return owner, bodies


# ---- 4. oracle -----------------------------------------------------------------

def build_bodies(env, compiler: str, work: Path,
                 bodies: dict[str, str]) -> list[str]:
    """The four link-time variants of the ten translated bodies."""
    source = ["/* Generated by test_vita_kage_quad_fastpath.py from the",
              " * frozen corpus units; the only edit is the seam guard. */",
              "#include <math.h>", '#include "guest.h"',
              "/* Stand-ins of the oracle (callees outside the linked set). */",
              "void sub_005603f0(CPU *__restrict c);",
              "void sub_0008d900(CPU *__restrict c);",
              "void sub_00018920(CPU *__restrict c);",
              "void sub_000188c0(CPU *__restrict c);",
              "void sub_005ec358(CPU *__restrict c);",
              "void sub_00010b40(CPU *__restrict c);",
              "void sub_005ec14c(CPU *__restrict c);",
              "void sub_0055e330(CPU *__restrict c);", ""]
    for name in BODIES:
        source.append(f"void {name}(CPU *__restrict c);")
    source.append("")
    for name in BODIES:
        body = bodies[name]
        source.append(body.replace(GUARD, "#if defined(ORACLE_KAGE_QUAD_SEAM)"))
    bodies_c = work / "kage_quad_bodies.c"
    bodies_c.write_text("\n".join(source), encoding="utf-8", newline="\n")

    def renames(prefix: str) -> list[str]:
        return [f"/D{name}={prefix}{name}" for name in BODIES]

    variants = {
        "seam_gpr": ["/DGUEST_GPR_LOCAL=1", "/DORACLE_KAGE_QUAD_SEAM=1"],
        "ref_gpr": ["/DGUEST_GPR_LOCAL=1"] + renames("ref_"),
        "seam_mem": ["/DGUEST_GPR_LOCAL=0", "/DORACLE_KAGE_QUAD_SEAM=1"]
                    + renames("mem_"),
        "ref_mem": ["/DGUEST_GPR_LOCAL=0"] + renames("mem_ref_"),
    }
    objects = []
    for tag, extra in variants.items():
        obj = work / f"kage_quad_bodies_{tag}.obj"
        run([compiler, "/nologo", "/W3", "/O2", "/std:c11", "/wd4310",
             "/wd4996", "/DGUEST_IMAGE_BASE=0x98000000u"] + BODY_DEFINES +
            extra + ["/I", str(RUNTIME), "/c", str(bodies_c),
                     "/Fo:" + str(obj)], env)
        objects.append(str(obj))
    return objects


def build_and_run_oracle(env, compiler: str, work: Path, objects: list[str],
                         blob: bytes, flavour: str,
                         extra_defines: list[str]) -> str:
    exe = work / f"kage-quad-oracle-{flavour}.exe"
    objdir = work / f"obj-{flavour}"
    objdir.mkdir()
    run([compiler] + COMMON + DISPATCH_DEFINES + extra_defines + [
        "/I", str(RUNTIME),
        str(RUNTIME / "host_vita_kage_quad_fastpath_oracle.c"),
    ] + objects + [
        "/Fe:" + str(exe), "/Fo:" + str(objdir) + os.sep,
        "/link", "/LARGEADDRESSAWARE", "/OPT:REF", "/INCREMENTAL:NO",
    ], env)
    out = run([str(exe), blob.hex()], env)
    (work / f"oracle-{flavour}.out").write_text(out, encoding="utf-8")
    return out


def run_oracle_flavours(env, compiler: str, work: Path,
                        bodies: dict[str, str], blob: bytes) -> list[str]:
    objects = build_bodies(env, compiler, work, bodies)
    results = []
    for flavour, (extra_defines, expected) in ORACLE_FLAVOURS.items():
        out = build_and_run_oracle(env, compiler, work, objects, blob,
                                   flavour, extra_defines)
        lines = out.strip().splitlines()
        result = lines[-1] if lines else ""
        failures = [line for line in lines if line.startswith("FAIL")]
        if failures or result != expected:
            print(out[-8000:])
            raise AssertionError(
                f"oracle result changed ({flavour}): {result!r}")
        results.append(f"[{flavour}] {result}")
    return results


# ---- 5. build gate --------------------------------------------------------------

def verify_build_gate() -> None:
    cmake = VITA_CMAKE.read_text(encoding="utf-8")
    for option in ("ISAAC_VITA_KAGE_QUAD_FASTPATH",
                   "ISAAC_VITA_KAGE_QUAD_FASTPATH_OBSERVE"):
        require(re.search(
            r"option\(" + option + r"\s+\"[^\"]+\"\s+OFF\)", cmake) is not None,
            f"{option} is not default OFF")
    require("ISAAC_VITA_KAGE_QUAD_FASTPATH requires generated guest runtime"
            in cmake, "scaffold gate missing")
    require("ISAAC_VITA_KAGE_QUAD_FASTPATH requires ISAAC_VITA_TRANSLATED_CPU"
            in cmake, "TRANSLATED_CPU prerequisite gate missing")
    require(re.search(
        r"if\(ISAAC_VITA_KAGE_QUAD_FASTPATH AND NOT ISAAC_VITA_FLOOR_THUNK_FASTPATH\)"
        r"\s*message\(FATAL_ERROR", cmake) is not None,
        "FLOOR_THUNK_FASTPATH prerequisite gate missing")
    require("ISAAC_VITA_KAGE_QUAD_FASTPATH_OBSERVE requires "
            "ISAAC_VITA_KAGE_QUAD_FASTPATH" in cmake,
            "OBSERVE prerequisite gate missing")
    require(cmake.count("ISAAC_VITA_KAGE_QUAD_FASTPATH=1") == 1 and
            cmake.count("ISAAC_VITA_KAGE_QUAD_FASTPATH_OBSERVE=1") == 1,
            "compile definitions have more than one owner list each")
    require(re.search(
        r"set_property\(SOURCE\s+\$\{ISAAC_VITA_KAGE_QUAD_FASTPATH_OWNER\}\s+"
        r"\"\$\{ISAAC_RUNTIME\}/host_vita_kage_quad_fastpath\.c\"\s+"
        r"\"\$\{ISAAC_RUNTIME\}/kage_vita_phase_profile\.c\"\s+"
        r"APPEND PROPERTY COMPILE_DEFINITIONS\s+"
        r"ISAAC_VITA_KAGE_QUAD_FASTPATH=1\)", cmake) is not None,
        "fast path owner list drifted")
    require(re.search(
        r"if\(ISAAC_VITA_KAGE_QUAD_FASTPATH_OBSERVE\)\s*set_property\(SOURCE\s+"
        r"\"\$\{ISAAC_RUNTIME\}/host_vita_kage_quad_fastpath\.c\"\s+"
        r"APPEND PROPERTY COMPILE_DEFINITIONS\s+"
        r"ISAAC_VITA_KAGE_QUAD_FASTPATH_OBSERVE=1\)", cmake) is not None,
        "OBSERVE owner list drifted")
    require(MARKER in cmake, "CMake census lacks the marker")
    require('"${ISAAC_RUNTIME}/host_vita_kage_quad_fastpath.c"' in cmake,
            "replay TU not routed")


# ---- 6. CMake census -------------------------------------------------------

def census_block() -> str:
    lines = VITA_CMAKE.read_text(encoding="utf-8").split("\n")
    start = lines.index("  if(ISAAC_VITA_KAGE_QUAD_FASTPATH)")
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


def run_census(cmake: str, work: Path, tag: str, units: list[Path],
               env: dict[str, str]) -> tuple[int, str]:
    script = work / f"census_{tag}.cmake"
    script.write_text("\n".join([
        "cmake_minimum_required(VERSION 3.16)",
        f'include("{posix(VITA_CMAKE.parent / "generated_definitions.cmake")}")',
        "function(set_property)",
        "endfunction()",
        "set(ISAAC_VITA_KAGE_QUAD_FASTPATH ON)",
        "set(ISAAC_VITA_KAGE_QUAD_FASTPATH_OBSERVE OFF)",
        "set(ISAAC_VITA_PHASE_PROFILE OFF)",
        f'set(ISAAC_RUNTIME "{posix(RUNTIME)}")',
        'set(ISAAC_GENERATED_CANONICAL_C "'
        + ";".join(posix(unit) for unit in units) + '")',
        "set(ISAAC_VITA_RUNTIME_SOURCES)",
        census_block(),
        'message(STATUS "CENSUS owners=${ISAAC_VITA_KAGE_QUAD_FASTPATH_OWNER}'
        ' runtime=${ISAAC_VITA_RUNTIME_SOURCES}")',
        "",
    ]), encoding="utf-8", newline="\n")
    completed = subprocess.run([cmake, "-P", str(script)], env=env,
                               stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = completed.stdout.decode("utf-8", "replace")
    return completed.returncode, re.sub(r"\s+", " ", out)


def verify_census(generated_dir: Path, owner: Path, work: Path,
                  env: dict[str, str]) -> list[str]:
    import shutil
    cmake = (shutil.which("cmake", path=env.get("PATH")) or shutil.which("cmake")
             or "C:/Program Files/CMake/bin/cmake.exe")
    require(Path(cmake).exists(), "cmake is required for the census proof")
    units = sorted(generated_dir.glob("guest_[0-9][0-9][0-9][0-9].c"))
    other = next(unit for unit in units if unit.name != owner.name)
    text = owner.read_text(encoding="utf-8")
    results = []

    rc, out = run_census(cmake, work, "real", units, env)
    require(rc == 0, f"census rejected the real corpus: {out[-1500:]}")
    require(f"CENSUS owners={posix(owner)} runtime="
            f"{posix(RUNTIME)}/host_vita_kage_quad_fastpath.c" in out,
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

    seamless = strip_guarded(text)
    require(MARKER not in seamless and GUARD not in seamless, "strip failed")
    rc, out = run_census(cmake, work, "noseam",
                         [variant("b", owner.name, seamless), other], env)
    require(rc != 0 and "missing or duplicate seam" in out,
            f"seamless owner accepted: {out[-1500:]}")
    results.append("census owner without seam: rejected (missing or duplicate seam)")

    line = next(l for l in text.split("\n") if MARKER in l)
    rc, out = run_census(cmake, work, "dup", [
        variant("c", owner.name, text.replace(line, line + "\n" + line, 1)),
        other], env)
    require(rc != 0 and "missing or duplicate seam" in out,
            f"duplicated seam accepted: {out[-1500:]}")
    results.append("census duplicated seam: rejected (missing or duplicate seam)")

    rc, out = run_census(cmake, work, "escaped", [owner, variant(
        "d", other.name,
        other.read_text(encoding="utf-8") + "\n/* " + MARKER + " */\n")], env)
    require(rc != 0 and "escaped its frozen owner" in out,
            f"escaped marker accepted: {out[-1500:]}")
    results.append("census escaped marker: rejected (escaped its frozen owner)")

    rc, out = run_census(cmake, work, "noowner",
                         [unit for unit in units if unit != owner], env)
    require(rc != 0 and "owners=0, definitions=0, seams=0" in out,
            f"ownerless corpus accepted: {out[-1500:]}")
    results.append("census without the owner unit: rejected "
                   "(owners=0, definitions=0, seams=0)")
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

    blob = verify_frozen_inputs(arguments.pe, gen_all)
    verify_codegen(arguments.pe, gen_all, Image, DEFAULT_BASE)
    owner, bodies = extract_bodies(arguments.generated_dir)
    print(f"corpus owner: {owner.name} (one owner, one seam)")
    verify_build_gate()

    env, compiler = BA.msvc_env()
    if arguments.keep:
        arguments.keep.mkdir(parents=True, exist_ok=True)
        for line in verify_census(arguments.generated_dir, owner,
                                  arguments.keep, env):
            print(line)
        results = run_oracle_flavours(env, compiler, arguments.keep, bodies,
                                      blob)
    else:
        with tempfile.TemporaryDirectory(prefix="isaac-kage-quad-") as tmp:
            for line in verify_census(arguments.generated_dir, owner,
                                      Path(tmp), env):
                print(line)
            results = run_oracle_flavours(env, compiler, Path(tmp), bodies,
                                          blob)
    for result in results:
        print(result)
    print("Vita kage quad fast path: PASS; roots=1; owner-unit seams=1; "
          f"oracle flavours={len(results)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
