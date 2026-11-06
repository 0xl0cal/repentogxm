#!/usr/bin/env python3
"""Frozen-PE host/codegen oracle for the Backdrop watergfx guard.

ISAAC_VITA_BACKDROP_WATERGFX_GUARD (GitHub issue 5 items 5.4/5.17):

1. Frozen inputs: PE identity; slot 0x44 of the three concrete image vftables
   the loader constructs is sub_005b0b00 and ends in `ret 8` (the stack
   contract of the skip edge).
2. Codegen, at both image bases with the real switch discovery: each root
   emits exactly one fenced seam right after its site statement; removing
   the fence gives byte-for-byte the text rendered with the seam disabled
   (so OFF preprocesses to the frozen text); the packing delta cancels the
   seam bytes (unit membership cannot move); one-byte drift at every pinned
   instruction fails closed.
3. Host model of the generated NULL test.
4. Build switch: CMake option default OFF, the definition reaches only the
   owner block, the helper is linked only when ON.
5. MSVC x86 oracle: the two seams extracted verbatim from the Vita-base
   rendering, compiled in both GPR spellings with and without the knob, run
   against a fake Backdrop object and loader frame together with the real
   helper (runtime/kage_vita_backdrop_watergfx_oracle.c).
6. Corpus contract (--generated-dir): one owner unit per root, one fence per
   root, no fence anywhere else; the real CMake owner block run under
   `cmake -P` selects exactly those units and routes the helper, and rejects
   a corpus missing either owner.
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
sys.path.insert(0, str(HERE))

RUNTIME = HERE / "runtime"
VITA_CMAKE = HERE / "vita" / "CMakeLists.txt"
VITA_BASE = 0x98000000
FENCE = (
    "#if defined(__vita__) && "
    "defined(ISAAC_VITA_BACKDROP_WATERGFX_GUARD)"
)
OWNER_NAME = "sub_00498850"
LOADER_NAME = "sub_0056c7e0"
# Concrete ImageBase vftables constructed by the loader (0056cac2/0056cb2e/
# 0056cb92), as RVAs, and the slot-0x44 target they share.
IMAGE_VFTABLE_RVAS = (0x00766028, 0x00765E90, 0x007662F0)
IMAGE_SLOT_44_RVA = 0x005B0B00
IMAGE_SLOT_44_RET_RVA = 0x005B0B6C


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def strip_guarded(text: str) -> str:
    """Drop every Backdrop watergfx fence (the frozen, unguarded shape)."""
    kept, skipping = [], False
    for line in text.split("\n"):
        if line.strip() == FENCE:
            require(not skipping, "nested watergfx fence")
            skipping = True
            continue
        if skipping:
            require(not line.lstrip().startswith("#if"),
                    "nested preprocessor block inside the watergfx fence")
            if line.strip() == "#endif":
                skipping = False
            continue
        kept.append(line)
    require(not skipping, "unterminated watergfx fence")
    return "\n".join(kept)


# ---- 1. frozen inputs ---------------------------------------------------------

def verify_frozen_inputs(G, img) -> None:
    from capstone import CS_ARCH_X86, CS_MODE_32, Cs

    for vftable in IMAGE_VFTABLE_RVAS:
        # The Image is relocated to its own base, so the slot holds base+RVA.
        slot = struct.unpack_from("<I", bytes(img.mem), vftable + 0x44)[0]
        require((slot - img.base) & 0xFFFFFFFF == IMAGE_SLOT_44_RVA,
                "image vftable %08x slot 0x44 changed: %08x" % (vftable, slot))
    code = bytes(img.code_at(IMAGE_SLOT_44_RVA,
                             IMAGE_SLOT_44_RET_RVA + 3 - IMAGE_SLOT_44_RVA))
    decoded = list(Cs(CS_ARCH_X86, CS_MODE_32).disasm(code, IMAGE_SLOT_44_RVA))
    require(sum(ins.size for ins in decoded) == len(code),
            "slot-0x44 callee does not decode contiguously")
    rets = [ins for ins in decoded if ins.mnemonic == "ret"]
    require(len(rets) == 1 and rets[0].address == IMAGE_SLOT_44_RET_RVA and
            rets[0].op_str == "8",
            "slot-0x44 callee no longer ends in its only `ret 8`")
    # It never reads EAX from the caller's point of view: the caller
    # overwrites EAX (00498cf8) before any read after the call.
    require(G.VITA_BACKDROP_WATERGFX_GUARD_SKIP_RVA == 0x00498CEC,
            "skip label moved")


# ---- 2. codegen ----------------------------------------------------------------

def _switch_info(G, pin):
    entries, edges, rejected, tables = G.discover_jump_tables(
        pin, G.VITA_BACKDROP_WATERGFX_GUARD_ROOT_RVA)
    require(sorted(entries) == [0x498DCF, 0x498F08, 0x49909F, 0x4991A3,
                                0x4991CA, 0x49920C] and not rejected and
            sorted(tables) == [0x498DC8],
            "Backdrop::LoadGraphics switch discovery changed")
    return {
        G.VITA_BACKDROP_WATERGFX_GUARD_ROOT_RVA: {
            "entries": entries, "edges": edges,
            "rejected": rejected, "tables": tables},
    }


def _translate(G, img, pin, root, switch_info, disabled=False):
    original = G.vita_backdrop_watergfx_guard_for_body
    if disabled:
        G.vita_backdrop_watergfx_guard_for_body = (
            lambda *args, **kwargs: ())
    try:
        return G._translate_function(img, {"rva": root}, None, switch_info,
                                     pin_img=pin)
    finally:
        G.vita_backdrop_watergfx_guard_for_body = original


ROOTS = None


def _roots(G):
    return (
        (G.VITA_BACKDROP_WATERGFX_GUARD_ROOT_RVA,
         G.VITA_BACKDROP_WATERGFX_GUARD_SITE_RVA,
         G.VITA_BACKDROP_WATERGFX_GUARD_SITE_STATEMENT,
         "00498cdd"),
        (G.VITA_BACKDROP_WATERGFX_GUARD_LOADER_ROOT_RVA,
         G.VITA_BACKDROP_WATERGFX_GUARD_LOADER_SITE_RVA,
         G.VITA_BACKDROP_WATERGFX_GUARD_LOADER_SITE_STATEMENT,
         "0056c837"),
    )


def verify_codegen(G, Image, DEFAULT_BASE, legacy_text, pe_path: Path):
    """Returns {root: Vita-base rendered text} for the host oracle."""
    pin = Image(str(pe_path), DEFAULT_BASE)
    switch_info = _switch_info(G, pin)
    rendered = {}
    for base in (DEFAULT_BASE, VITA_BASE):
        emit = Image(str(pe_path), base)
        for root, site, statement, next_rva in _roots(G):
            result = _translate(G, emit, pin, root, switch_info)
            plain = _translate(G, emit, pin, root, switch_info, disabled=True)
            require(result["stub"] is None, "%08x became a stub" % root)
            require(result["vita_backdrop_watergfx_guard_sites"] == (site,),
                    "%08x seam sites: %r" % (
                        root, result["vita_backdrop_watergfx_guard_sites"]))
            require(plain["vita_backdrop_watergfx_guard_sites"] == (),
                    "disabled translation still reports a seam")
            text = result["text"]
            require(text.count(FENCE) == 1,
                    "%08x fences: %d" % (root, text.count(FENCE)))
            # (a) only the fenced lines were added: the OFF preprocessed
            #     text of the owner is the unguarded rendering.
            require(strip_guarded(text) == plain["text"],
                    "%08x: text outside the fence changed" % root)
            # (b) the unit packing is exactly that of the unguarded body.
            require(G.unit_packing_size(text) + result["legacy_size_delta"] ==
                    G.unit_packing_size(plain["text"]) +
                    plain["legacy_size_delta"],
                    "%08x: packing size moved" % root)
            require(result["legacy_size_delta"] ==
                    plain["legacy_size_delta"] -
                    G.vita_backdrop_watergfx_guard_packing_bytes((site,)),
                    "%08x: packing delta does not cancel the seam" % root)
            # (c) the block follows the site statement, before the next insn.
            legacy = legacy_text(text)
            block = "\n".join(G.render_vita_backdrop_watergfx_guard(site))
            site_at = legacy.index("    /* %08x" % site)
            statement_at = legacy.index("    " + statement, site_at)
            block_at = legacy.index(block, statement_at)
            next_at = legacy.index("    /* %s" % next_rva, block_at)
            require(site_at < statement_at < block_at < next_at,
                    "%08x: seam placement changed" % root)
            require(legacy.count(block) == 1, "%08x: block repeated" % root)
            if base == VITA_BASE:
                rendered[root] = text
    return pin, switch_info, rendered


def verify_fail_closed(G, B, pin, switch_info) -> None:
    from image import DEFAULT_BASE  # noqa: F401  (documented import)

    for root, name in ((G.VITA_BACKDROP_WATERGFX_GUARD_ROOT_RVA, OWNER_NAME),
                       (G.VITA_BACKDROP_WATERGFX_GUARD_LOADER_ROOT_RVA,
                        LOADER_NAME)):
        info = switch_info.get(root, {})
        translated = B.translate(pin, root, name, None,
                                 extra_entries=info.get("entries", ()),
                                 indirect_edges=info.get("edges", {}))
        em, insns, order, body, unsupported, _indirect, _members = translated
        require(unsupported == [], "%s unsupported: %r" % (name, unsupported))
        sites = G.vita_backdrop_watergfx_guard_for_body(
            pin, root, em, insns, order, body)
        require(len(sites) == 1, "%s: sites %r" % (name, sites))
        if root == G.VITA_BACKDROP_WATERGFX_GUARD_ROOT_RVA:
            mutations = (
                G.VITA_BACKDROP_WATERGFX_GUARD_ROOT_RVA,
                G.VITA_BACKDROP_WATERGFX_GUARD_SITE_RVA + 2,
                G.VITA_BACKDROP_WATERGFX_GUARD_PUSH_RVAS[0] + 1,
                G.VITA_BACKDROP_WATERGFX_GUARD_PUSH_RVAS[1] + 1,
                G.VITA_BACKDROP_WATERGFX_GUARD_DEREF_RVA + 1,
                G.VITA_BACKDROP_WATERGFX_GUARD_CALL_RVA + 2,
                G.VITA_BACKDROP_WATERGFX_GUARD_NEXT_RVA + 2,
                G.VITA_BACKDROP_WATERGFX_GUARD_SKIP_RVA,
                G.VITA_BACKDROP_WATERGFX_GUARD_LAST_RVA,
            )
        else:
            mutations = (
                G.VITA_BACKDROP_WATERGFX_GUARD_LOADER_PATH_RVA + 2,
                G.VITA_BACKDROP_WATERGFX_GUARD_LOADER_RESOLVE_RVA + 1,
                G.VITA_BACKDROP_WATERGFX_GUARD_LOADER_TEST_RVA + 1,
                G.VITA_BACKDROP_WATERGFX_GUARD_LOADER_BRANCH_RVA + 1,
                G.VITA_BACKDROP_WATERGFX_GUARD_LOADER_SITE_RVA + 2,
                G.VITA_BACKDROP_WATERGFX_GUARD_LOADER_LAST_RVA,
            )
        pin.mem = bytearray(pin.mem)
        for rva in mutations:
            original = pin.mem[rva]
            pin.mem[rva] ^= 0x01
            try:
                G.vita_backdrop_watergfx_guard_for_body(
                    pin, root, em, insns, order, body)
            except RuntimeError as exc:
                require("Backdrop watergfx" in str(exc), str(exc))
            else:
                raise AssertionError(
                    "mutated %s passed its fail-closed proof at %08x" %
                    (name, rva))
            finally:
                pin.mem[rva] = original
        # A foreign root that happens to reach the site must be refused.
        try:
            G.vita_backdrop_watergfx_guard_for_body(
                pin, root + 0x10, em, insns, order, body)
        except RuntimeError as exc:
            require("membership" in str(exc), str(exc))
        else:
            raise AssertionError("foreign root accepted for %s" % name)
        # A root whose order does not reach any site is untouched.
        require(G.vita_backdrop_watergfx_guard_for_body(
            pin, root, em, insns, [root], body) == (),
            "unrelated order selected a seam")


# ---- 3. host model ---------------------------------------------------------------

def verify_host_model(G) -> None:
    esp = 0x83303C34
    for holder in (1, 0x12345678, 0xFFFFFFFF):
        target, final_esp, ecx = G.vita_backdrop_watergfx_guard_host_oracle(
            holder, 30, esp)
        require(target is None and final_esp == esp and ecx == holder,
                "live holder must fall through unchanged")
    for backdrop_id in (0, 6, 30, 0x7FFFFFFF):
        target, final_esp, ecx = G.vita_backdrop_watergfx_guard_host_oracle(
            0, backdrop_id, esp)
        require(target == G.VITA_BACKDROP_WATERGFX_GUARD_SKIP_RVA and
                final_esp == esp and ecx == backdrop_id,
                "NULL holder must skip with ESP unchanged and ECX = id")
    require(G.VITA_BACKDROP_WATERGFX_GUARD_ID_OFFSET == 0x150 and
            G.VITA_BACKDROP_WATERGFX_GUARD_WATER_STRING_OFFSET == 0x170 and
            G.VITA_BACKDROP_WATERGFX_GUARD_ENTRY_STRIDE == 0x13C,
            "Backdrop layout constants changed")
    helper = (RUNTIME / "kage_vita_backdrop_watergfx.c").read_text(
        encoding="utf-8")
    for name, value in (("BACKDROP_ID_OFFSET", 0x150),
                        ("BACKDROP_ENTRY_STRIDE", 0x13C),
                        ("BACKDROP_WATER_STRING_OFFSET", 0x170)):
        match = re.search(r"#define %s\s+0x([0-9a-f]+)u" % name, helper)
        require(match is not None and int(match.group(1), 16) == value,
                "helper %s disagrees with gen_all" % name)


# ---- 4. build switch ------------------------------------------------------------

def owner_block() -> str:
    lines = VITA_CMAKE.read_text(encoding="utf-8").split("\n")
    start = lines.index("  if(ISAAC_VITA_BACKDROP_WATERGFX_GUARD)")
    depth = 0
    for index in range(start, len(lines)):
        stripped = lines[index].strip()
        if stripped.startswith("if("):
            depth += 1
        elif stripped.startswith("endif("):
            depth -= 1
            if depth == 0:
                return "\n".join(lines[start:index + 1])
    raise AssertionError("owner block has no matching endif")


def verify_build_switch() -> None:
    cmake = VITA_CMAKE.read_text(encoding="utf-8")
    option = re.search(
        r"option\(ISAAC_VITA_BACKDROP_WATERGFX_GUARD\s+.*?\s+(ON|OFF)\)",
        cmake, flags=re.S)
    require(option is not None and option.group(1) == "OFF",
            "ISAAC_VITA_BACKDROP_WATERGFX_GUARD must default OFF")
    require("ISAAC_VITA_BACKDROP_WATERGFX_GUARD requires the translated "
            "guest runtime" in cmake, "scaffold guard missing")
    require(cmake.count("ISAAC_VITA_BACKDROP_WATERGFX_GUARD=1") == 1,
            "the definition must be set in exactly one place")
    require(cmake.count("kage_vita_backdrop_watergfx.c") == 2,
            "the helper must be listed and defined only in the owner block")
    block = owner_block()
    require("ISAAC_VITA_BACKDROP_WATERGFX_GUARD=1" in block and
            block.count("kage_vita_backdrop_watergfx.c") == 2,
            "definition/helper escaped the owner block")
    require("sub_00498850 sub_0056c7e0" in block, "owner roots changed")
    # v0.1.1-alpha shipped before the guard existed; every later recipe ships
    # it ON (tools/release/check_release_recipe.py requires it).
    for recipe in sorted((HERE.parent / "release").glob("*.cmake")):
        text = recipe.read_text(encoding="utf-8")
        if recipe.name == "v0.1.1-alpha.cmake":
            require("BACKDROP_WATERGFX" not in text,
                    "the v0.1.1-alpha recipe predates the guard: %s" % recipe.name)
        else:
            require(re.search(r"^set\(ISAAC_VITA_BACKDROP_WATERGFX_GUARD ON CACHE BOOL",
                              text, flags=re.M) is not None,
                    "release recipe must ship the guard ON: %s" % recipe.name)


# ---- 5. MSVC oracle --------------------------------------------------------------

def _extract(text: str, site: int, next_rva: str) -> list[str]:
    lines = text.split("\n")
    start = lines.index(next(line for line in lines
                             if line.startswith("    /* %08x" % site)))
    end = start + 1
    while not lines[end].startswith("    /* %s" % next_rva):
        end += 1
    return lines[start:end]


def write_harness(G, rendered, work: Path) -> Path:
    owner_root, owner_site, _, owner_next = _roots(G)[0]
    loader_root, loader_site, _, loader_next = _roots(G)[1]
    owner_lines = _extract(rendered[owner_root], owner_site, owner_next)
    loader_lines = _extract(rendered[loader_root], loader_site, loader_next)
    for lines in (owner_lines, loader_lines):
        require(sum(line.strip() == FENCE for line in lines) == 1,
                "extracted seam lost its fence")
    require(any("GUEST_GPR_FLUSH(c);" in line for line in owner_lines) and
            any("{ GUEST_GPR_RELOAD(c); goto L_00498cec; }" in line
                for line in owner_lines),
            "owner seam is not bracketed for GPR locals")
    require(any("GUEST_GPR_FLUSH(c);" in line for line in loader_lines),
            "loader seam is not bracketed for GPR locals")
    source = [
        "/* Generated by test_backdrop_watergfx_guard.py: the two seams,",
        " * extracted verbatim from the Vita-base rendering. */",
        '#include "guest.h"',
        "#define HCAT2(a, b) a##b",
        "#define HCAT(a, b) HCAT2(a, b)",
        "",
        "int HCAT(watergfx_owner, HARNESS_SUFFIX)(CPU *__restrict c)",
        "{",
        "    GUEST_GPR_DECL;",
        *owner_lines,
        "    GUEST_GPR_FLUSH(c);",
        "    return 0;",
        "L_00498cec:",
        "    GUEST_GPR_FLUSH(c);",
        "    return 1;",
        "}",
        "",
        "int HCAT(watergfx_loader, HARNESS_SUFFIX)(CPU *__restrict c)",
        "{",
        "    GUEST_GPR_DECL;",
        *loader_lines,
        "    GUEST_GPR_FLUSH(c);",
        "    return 0;",
        "}",
        "",
    ]
    path = work / "watergfx_harness.c"
    path.write_text("\n".join(source), encoding="utf-8", newline="\n")
    return path


COMMON = [
    "/nologo", "/W4", "/WX", "/O2", "/std:c11",
    # C4210: the seams declare their helper at block scope (valid C, MSVC
    # warns); C4102: the OFF harness never jumps to its label.
    "/wd4102", "/wd4210", "/wd4310", "/wd4702", "/wd4996",
    "/DGUEST_IMAGE_BASE=0x98000000u",
    "/DGUEST_STACK_REQUIRED=1",
    "/DGUEST_GENERATED_STACK_GUARD=0",
    "/DGUEST_FLAGS_LOCAL=1",
    "/DGUEST_COVERAGE_HOOKS=0",
]


def run_msvc_oracle(harness: Path, work: Path) -> list[str]:
    import build_all as BA

    env, compiler = BA.msvc_env()
    results = []
    for guard in (1, 0):
        objects = []
        for tag, gpr in (("gpr", 1), ("mem", 0)):
            obj = work / ("harness_%s_%d.obj" % (tag, guard))
            defines = ["/DGUEST_GPR_LOCAL=%d" % gpr,
                       "/DHARNESS_SUFFIX=_%s" % tag]
            if guard:
                defines += ["/D__vita__=1",
                            "/DISAAC_VITA_BACKDROP_WATERGFX_GUARD=1"]
            _run([compiler] + COMMON + defines +
                 ["/I", str(RUNTIME), "/c", str(harness), "/Fo:" + str(obj)],
                 env, work)
            objects.append(str(obj))
        exe = work / ("watergfx-oracle-%d.exe" % guard)
        _run([compiler] + COMMON +
             ["/DORACLE_GUARD_ON=%d" % guard, "/I", str(RUNTIME),
              str(RUNTIME / "kage_vita_backdrop_watergfx_oracle.c")] +
             objects + ["/Fe:" + str(exe),
                        "/Fo:" + str(work / ("oracle_%d.obj" % guard)),
                        "/link", "/INCREMENTAL:NO"], env, work)
        out = _run([str(exe)], env, work)
        lines = out.strip().splitlines()
        require(lines and lines[-1].startswith(
            "watergfx oracle guard=%d:" % guard) and
            " failures=0 " in lines[-1] + " " and
            not any(line.startswith("FAIL") for line in lines),
            "MSVC oracle failed (guard=%d):\n%s" % (guard, out[-4000:]))
        results.append(lines[-1])
    return results


def _run(command, env, cwd) -> str:
    completed = subprocess.run(command, env=env, cwd=str(cwd),
                               stdout=subprocess.PIPE,
                               stderr=subprocess.STDOUT)
    out = completed.stdout.decode("utf-8", "replace")
    if completed.returncode != 0:
        raise AssertionError("command failed (%d): %s\n%s" % (
            completed.returncode, " ".join(map(str, command)), out[-4000:]))
    return out


# ---- 6. corpus contract ----------------------------------------------------------

def verify_corpus(generated_dir: Path, work: Path) -> list[str]:
    units = sorted(generated_dir.glob("guest_[0-9][0-9][0-9][0-9].c"))
    require(units, "no generated units in %s" % generated_dir)
    owners = {OWNER_NAME: [], LOADER_NAME: []}
    fenced = {}
    for unit in units:
        text = unit.read_text(encoding="utf-8")
        for name in owners:
            if "\nvoid %s(CPU *__restrict c)\n" % name in text:
                owners[name].append(unit)
        if FENCE in text:
            fenced[unit] = text.count(FENCE)
    for name, found in owners.items():
        require(len(found) == 1, "%s owners: %s" % (name, found))
    owner_units = {owners[OWNER_NAME][0], owners[LOADER_NAME][0]}
    expected_fences = {}
    for unit in owner_units:
        expected_fences[unit] = sum(
            1 for name in owners if owners[name][0] == unit)
    require(fenced == expected_fences,
            "watergfx fences outside/inside owners: %s" % {
                u.name: n for u, n in fenced.items()})
    results = ["corpus: %d units; %s in %s, %s in %s; fences only there" % (
        len(units), OWNER_NAME, owners[OWNER_NAME][0].name,
        LOADER_NAME, owners[LOADER_NAME][0].name)]

    cmake = shutil.which("cmake") or "C:/Program Files/CMake/bin/cmake.exe"
    require(Path(cmake).exists(), "cmake is required for the owner census")
    expected = ";".join(sorted(
        u.as_posix() for u in owner_units))

    def census(tag, unit_list):
        script = work / ("census_%s.cmake" % tag)
        script.write_text("\n".join([
            "cmake_minimum_required(VERSION 3.16)",
            'include("%s")' % (VITA_CMAKE.parent /
                               "generated_definitions.cmake").as_posix(),
            "function(set_property)",
            "endfunction()",
            "set(ISAAC_VITA_BACKDROP_WATERGFX_GUARD ON)",
            'set(ISAAC_RUNTIME "%s")' % RUNTIME.as_posix(),
            'set(ISAAC_GENERATED_C "%s")' % ";".join(
                u.as_posix() for u in unit_list),
            "set(ISAAC_VITA_RUNTIME_SOURCES)",
            owner_block(),
            "list(SORT ISAAC_VITA_BACKDROP_WATERGFX_GUARD_OWNERS)",
            'message(STATUS "CENSUS owners='
            '${ISAAC_VITA_BACKDROP_WATERGFX_GUARD_OWNERS} '
            'runtime=${ISAAC_VITA_RUNTIME_SOURCES}")',
            "",
        ]), encoding="utf-8", newline="\n")
        completed = subprocess.run([cmake, "-P", str(script)],
                                   stdout=subprocess.PIPE,
                                   stderr=subprocess.STDOUT)
        out = completed.stdout.decode("utf-8", "replace")
        return completed.returncode, re.sub(r"\s+", " ", out)

    rc, out = census("real", units)
    require(rc == 0 and ("CENSUS owners=%s runtime=%s/kage_vita_backdrop_"
                         "watergfx.c" % (expected, RUNTIME.as_posix())) in out,
            "census on the real corpus: %s" % out[-1500:])
    results.append("census real corpus: PASS (owners %s, helper routed)" %
                   ", ".join(sorted(u.name for u in owner_units)))
    for name in (OWNER_NAME, LOADER_NAME):
        missing = [u for u in units if u != owners[name][0]]
        rc, out = census("no_" + name, missing)
        require(rc != 0 and ("root %s must own exactly one generated source; "
                             "found 0" % name) in out,
                "corpus without %s accepted: %s" % (name, out[-1500:]))
        results.append("census without %s: rejected (found 0)" % name)
    duplicate_dir = work / "dup"
    duplicate_dir.mkdir(exist_ok=True)
    duplicate = duplicate_dir / "guest_9999.c"
    duplicate.write_text("void %s(CPU *__restrict c)\n{}\n" % OWNER_NAME,
                         encoding="utf-8", newline="\n")
    rc, out = census("dup", units + [duplicate])
    require(rc != 0 and ("root %s must own exactly one generated source; "
                         "found 2" % OWNER_NAME) in out,
            "duplicated owner accepted: %s" % out[-1500:])
    results.append("census duplicated %s: rejected (found 2)" % OWNER_NAME)
    return results


def run(pe_path: Path, generated_dir: Path | None, keep: Path | None) -> None:
    pe_path = pe_path.resolve()
    os.environ["REPENTOGXM_PE"] = str(pe_path)
    import build_one as B  # noqa: E402
    import gen_all as G  # noqa: E402
    from gpr_locals import legacy_text  # noqa: E402
    from image import DEFAULT_BASE, Image  # noqa: E402

    require(pe_path.stat().st_size == G.VITA_BACKDROP_WATERGFX_GUARD_PE_SIZE,
            "frozen PE size changed")
    require(_sha256(pe_path) == G.VITA_BACKDROP_WATERGFX_GUARD_PE_SHA256,
            "frozen PE hash changed")
    pin = Image(str(pe_path), DEFAULT_BASE)
    verify_frozen_inputs(G, pin)
    print("frozen inputs: PASS (3 image vftables -> sub_005b0b00 ret 8)")
    pin, switch_info, rendered = verify_codegen(
        G, Image, DEFAULT_BASE, legacy_text, pe_path)
    print("codegen: PASS (2 roots x 2 bases; strip(fence) == unguarded "
          "text; packing delta cancels the seam)")
    verify_fail_closed(G, B, pin, switch_info)
    print("fail-closed: PASS (15 one-byte mutations, foreign root refused)")
    verify_host_model(G)
    print("host model: PASS")
    verify_build_switch()
    print("build switch: PASS (default OFF, one definition site)")

    def body(work: Path) -> None:
        harness = write_harness(G, rendered, work)
        for line in run_msvc_oracle(harness, work):
            print("MSVC oracle: %s" % line)
        if generated_dir is not None:
            for line in verify_corpus(generated_dir.resolve(), work):
                print(line)

    if keep is not None:
        keep.mkdir(parents=True, exist_ok=True)
        body(keep)
    else:
        with tempfile.TemporaryDirectory(prefix="isaac-watergfx-") as tmp:
            body(Path(tmp))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pe", type=Path, required=True)
    parser.add_argument("--generated-dir", type=Path)
    parser.add_argument("--keep", type=Path,
                        help="keep the oracle build directory here")
    args = parser.parse_args()
    run(args.pe, args.generated_dir, args.keep)
    print("Backdrop watergfx guard host/codegen oracle: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
