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
5. Build gate: the CMake options are default OFF; the fast path requires
   ISAAC_VITA_GL_SHIM_FASTDISPATCH, the memo requires the fast path and
   VERIFY requires the memo.
6. CMake census: the real owner/seam census block of recomp/vita/CMakeLists.txt
   runs under `cmake -P` (set_property stubbed) against the corpus - exactly
   one owner unit and one seam line per root - and must reject a corpus
   without the seams (the frozen shape), a seam duplicated in its owner and
   a marker that escaped into another unit.
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
                         extra_defines: list[str]) -> str:
    exe = work / f"shader-attrib-oracle-{flavour}.exe"
    objdir = work / f"obj-{flavour}"
    objdir.mkdir()
    run([compiler] + COMMON + DISPATCH_DEFINES + extra_defines + [
        "/I", str(RUNTIME),
        str(RUNTIME / "host_vita_shader_attrib_fastpath_oracle.c"),
        str(RUNTIME / "gl_bridge.c"),
        str(RUNTIME / "host_vita_gl.c"),
    ] + objects + [
        "/Fe:" + str(exe), "/Fo:" + str(objdir) + os.sep,
        "/link", "/LARGEADDRESSAWARE", "/OPT:REF", "/INCREMENTAL:NO",
    ], env)
    out = run([str(exe), tables.hex()], env)
    (work / f"oracle-{flavour}.out").write_text(out, encoding="utf-8")
    return out


def run_oracle_flavours(env, compiler: str, work: Path,
                        bodies: dict[str, str], tables: bytes) -> list[str]:
    objects = build_bodies(env, compiler, work, bodies)
    results = []
    for flavour, (extra_defines, expected) in ORACLE_FLAVOURS.items():
        out = build_and_run_oracle(env, compiler, work, objects, tables,
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
    print("Vita shader attrib fast path: PASS; roots=2; owner-unit seams=2; "
          f"oracle flavours={len(results)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
