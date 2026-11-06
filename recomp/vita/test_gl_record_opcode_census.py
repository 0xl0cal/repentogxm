#!/usr/bin/env python3
"""RENDER60 P0-1 host gates: ISAAC_VITA_GL_RECORD_OPCODE_CENSUS (the GL
recorder's per-opcode census, ph120.ro + ph120.rop).

Legs (each prints one PASS line; any failure exits 1):
  scope     the C define ISAAC_GL_RECORD_OPCODE_CENSUS is read by exactly
            gl_record_backend.c/.h and kage_vita_phase_profile.c among the
            production runtime sources, and CMake sets it in exactly one
            statement naming exactly those two units.
  cmake     the real CMakeLists.txt blocks (the census option and its FATAL,
            the GL recorder/worker source definitions) run by `cmake -P`
            with set_property recorded: with the knob ON every unit keeps
            its definition list and the two units gain the one define;
            the knob without PHASE_PROFILE or GL_THREAD_WORKER is refused.
  identity  clang -E (__vita__, stub system headers) of gl_record_backend.c,
            kage_vita_phase_profile.c and every unit that includes
            gl_record_backend.h, at the base commit and now, without the
            define: token-equal.  Positive controls: the define changes the
            two units' text.
  oracle    the phase-profile oracle (kage_vita_phase_profile_oracle.c)
            built with clang for the gl-worker record set without and with
            the define: records 12 -> 16 (ph120.ro + three ph120.rop), the
            all-ones lengths ro=173 rop=490.  With RENDER60 S4's
            ISAAC_VITA_GL_RECORD_ATTRIB_BATCH and/or W-pkt's
            ISAAC_VITA_KAGE_BIND_PACKET too (CMake routes both to the census
            reader): the same output; the max flush 81 slots in the same 27
            records with S4 alone, 82 slots in 28 records with W-pkt.
  readme    README.md documents the knob, its records and gates.

The render-thread/game-thread counting itself is checked by the MSVC recorder
scenario (test_gl_vita_backend.ps1 with ISAAC_GL_RECORD=1, variant `census`:
same trace as the other variants, census sums equal the window's records,
ap samples and S calls) and the parser by tools/bench/bench_run_test.py.

--plant runs each leg once against a planted defect and requires every one
to fail (the test of the test).
"""
from __future__ import annotations

import argparse
from pathlib import Path
import re
import shutil
import subprocess
import sys

import test_gl_record_free_wrap as s1
from test_gl_record_free_wrap import LegFailure, need, tool

ROOT = s1.ROOT
RUNTIME = s1.RUNTIME
VITA = s1.VITA
KNOB = "ISAAC_VITA_GL_RECORD_OPCODE_CENSUS"
DEFINE = "ISAAC_GL_RECORD_OPCODE_CENSUS"
UNITS = ("gl_record_backend.c", "kage_vita_phase_profile.c")


class PlantError(Exception):
    """A plant whose anchor is gone: never counted as caught."""


# ----------------------------------------------------------------- scope ----
def matching_endif(text: str, start: int) -> int:
    """Offset of the #endif closing the #if at `start`, nested conditionals
    counted (ISAAC_VITA_KAGE_BIND_PACKET sizes the census inside the block)."""
    depth = 0
    for m in re.finditer(r"^[ \t]*#[ \t]*(if|ifdef|ifndef|endif)\b", text[start:], re.M):
        depth += -1 if m.group(1) == "endif" else 1
        if depth == 0:
            return start + m.start()
    raise LegFailure("scope: unterminated #if in gl_record_backend.h")


def leg_scope(runtime: Path, cmake_text: str) -> None:
    readers = sorted(p.name for p in runtime.iterdir()
                     if p.suffix in (".c", ".h", ".inc") and not p.name.endswith("_oracle.c")
                     and DEFINE in p.read_text(encoding="utf-8", errors="replace"))
    need(readers == ["gl_record_backend.c", "gl_record_backend.h", "kage_vita_phase_profile.c"],
         f"scope: production units reading {DEFINE}: {readers}")
    sets = [m.group(0) for m in re.finditer(r"set_property\(SOURCE[^)]*\)", cmake_text) if DEFINE in m.group(0)]
    need(len(sets) == 1, f"scope: {len(sets)} CMake statements set {DEFINE}")
    named = re.findall(r'"\$\{ISAAC_RUNTIME\}/([^"]+)"', sets[0])
    need(sorted(named) == sorted(UNITS) and f"COMPILE_DEFINITIONS {DEFINE}=1)" in " ".join(sets[0].split()),
         f"scope: the CMake statement names {named}")
    header = (runtime / "gl_record_backend.h").read_text(encoding="utf-8")
    start = header.index(f"#if defined({DEFINE})")
    end = matching_endif(header, start)
    for symbol in ("IsaacGlRecordOpcodeCensus", "isaac_gl_record_opcode_census", "ISAAC_GL_RECORD_CENSUS_OPS"):
        need(header.count(symbol) == header[start:end].count(symbol) > 0,
             f"scope: {symbol} declared outside the {DEFINE} block of gl_record_backend.h")
    print(f"PASS scope: {DEFINE} read by {', '.join(readers)}; one CMake statement names "
          f"{' and '.join(UNITS)}; header declarations inside its #if")


# ----------------------------------------------------------------- cmake ----
SIM_TAIL = r'''
get_property(files GLOBAL PROPERTY sim_files)
list(REMOVE_DUPLICATES files)
list(SORT files)
foreach(f IN LISTS files)
  get_property(d GLOBAL PROPERTY "sim_defs_${f}")
  message(STATUS "SIM defs ${f} = ${d}")
endforeach()
'''
COMMON = dict(s1.COMMON, ISAAC_VITA_PHASE_PROFILE="ON", ISAAC_VITA_HEAP_CENSUS="ON")
RUNS = {
    "A": dict(COMMON),
    "A+census": dict(COMMON, **{KNOB: "ON"}),
    "A-proof+census": dict(COMMON, ISAAC_VITA_GL_THREAD_WORKER_PROOF="ON", **{KNOB: "ON"}),
    # refused
    "B+census": dict(COMMON, ISAAC_VITA_PHASE_PROFILE="OFF", **{KNOB: "ON"}),
    "noworker+census": dict(COMMON, ISAAC_VITA_GL_THREAD_WORKER="OFF", **{KNOB: "ON"}),
}
REFUSED = {
    "B+census": "GL_RECORD_OPCODE_CENSUS requires PHASE_PROFILE and GL_THREAD_WORKER",
    "noworker+census": "GL_RECORD_OPCODE_CENSUS requires PHASE_PROFILE and GL_THREAD_WORKER",
}


def cmake_blocks(text: str) -> str:
    o = text.index(f"option({KNOB}\n")
    need(o > text.index("option(ISAAC_VITA_PHASE_PROFILE\n") and o > text.index("option(ISAAC_VITA_GL_THREAD_WORKER\n"),
         "cmake: the census option precedes an option its FATAL reads")
    check = text.index(f"if({KNOB} AND\n", o)
    options = text[o:s1.endif_after(text, check, "")]
    r = text.index("  if(ISAAC_VITA_GL_THREAD_RECORD)\n    list(APPEND ISAAC_VITA_RUNTIME_SOURCES\n"
                   "      \"${ISAAC_RUNTIME}/gl_record_backend.c\"")
    r_end = text.index("  if(ISAAC_VITA_GL_OWNER_CHECK)\n", r)
    record = text[r:r_end]
    need(f"  if({KNOB})\n" in record, "cmake: the census block is not inside the recorder/worker section")
    body = "\n".join((options, record)).replace("option(", "sim_option(")
    body = re.sub(r"\bset_property\(SOURCE", "sim_set_property(SOURCE", body)
    need("set_property(" not in body.replace("sim_set_property(", ""), "cmake: unsimulated set_property")
    head = s1.SIM_HEAD.replace(
        '    set_property(GLOBAL APPEND PROPERTY "sim_defs_${file}" ${values})\n',
        '    set_property(GLOBAL APPEND PROPERTY "sim_defs_${file}" ${values})\n'
        '    set_property(GLOBAL APPEND PROPERTY sim_files "${file}")\n')
    need(head != s1.SIM_HEAD, "cmake: the S1 simulation head changed shape")
    return head + body + SIM_TAIL


def run_shape(cmake: str, script: Path, values: dict) -> dict:
    command = [cmake, *(f"-D{k}={v}" for k, v in sorted(values.items())), "-P", str(script)]
    done = subprocess.run(command, capture_output=True, text=True)
    result = {"rc": done.returncode, "text": done.stdout + done.stderr, "defs": {}}
    for line in result["text"].splitlines():
        m = re.match(r"-- SIM defs (\S+) = (.*)$", line)
        if m:
            result["defs"][m.group(1)] = [v for v in m.group(2).split(";") if v]
    return result


def leg_cmake(cmake_text: str, out: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    script = out / "census_cmake_sim.cmake"
    script.write_text(cmake_blocks(cmake_text), encoding="utf-8")
    cmake = shutil.which("cmake")
    need(cmake is not None, "cmake not on PATH")
    results = {name: run_shape(cmake, script, values) for name, values in RUNS.items()}
    for name, r in results.items():
        if name in REFUSED:
            need(r["rc"] != 0 and " ".join(REFUSED[name].split()) in " ".join(r["text"].split()),
                 f"cmake {name}: not refused:\n{r['text']}")
            print(f"  cmake {name}: refused ({REFUSED[name]})")
            continue
        need(r["rc"] == 0, f"cmake {name}: failed:\n{r['text']}")
        print(f"  cmake {name}:")
        for unit in UNITS:
            print(f"    {unit}: {' '.join(r['defs'].get(unit, [])) or '-'}")
    for off, on in (("A", "A+census"),):
        a, c = results[off]["defs"], results[on]["defs"]
        need(sorted(a) == sorted(c), f"cmake {on}: unit set {sorted(c)} vs {sorted(a)}")
        for unit, defs in a.items():
            want = defs + [f"{DEFINE}=1"] if unit in UNITS else defs
            need(c[unit] == want, f"cmake {on}: {unit} defs {c[unit]} (want {want})")
        print(f"  cmake {on} vs {off}: {len(a)} units in the recorder/worker section, only "
              f"{' and '.join(UNITS)} gain {DEFINE}=1")
    need(f"{DEFINE}=1" in results["A-proof+census"]["defs"]["gl_record_backend.c"], "cmake: proof shape")
    print(f"PASS cmake: {KNOB} adds {DEFINE}=1 to exactly {' and '.join(UNITS)}; refused without "
          "PHASE_PROFILE or GL_THREAD_WORKER")


# -------------------------------------------------------------- identity ----
REC, WRK = s1.REC, s1.WRK
PP = "ISAAC_VITA_PHASE_PROFILE=1"
OFF_SETS = {
    "gl_record_backend.c": {"record": [REC], "B": [REC, WRK], "A": [REC, WRK, s1.CACHE],
                            "A-proof": [REC, WRK, "ISAAC_VITA_GL_THREAD_WORKER_PROOF=1", s1.CACHE]},
    "kage_vita_phase_profile.c": {
        "profile": [PP], "worker": [PP, WRK],
        "A": [PP, WRK, "ISAAC_VITA_LOG_ASYNC=1", "ISAAC_VITA_HEAP_CENSUS=1", "ISAAC_VITA_GPU_FRAME_PROFILE=1"],
        "oracle": [PP, WRK, "ISAAC_KAGE_VITA_PHASE_PROFILE_ORACLE=1", 'ISAAC_VITA_PHASE_PROFILE_BUILD_ID="x"'],
    },
}
# Every other unit that includes gl_record_backend.h (the header's census block).
HEADER_UNITS = ("gl_bridge.c", "gl_vita_backend.c", "host_vita_debug_overlay.c", "kage_vita_backend.c",
                "kage_vita_loading.c", "manual_kage_vita.c")
# Units that include it only under their own knob: RENDER60 S4's replay TU
# (ISAAC_VITA_GL_RECORD_ATTRIB_BATCH), compared here without that knob.
COND_UNITS = {"host_vita_shader_attrib_fastpath.c": {
    "fastpath": ["ISAAC_VITA_SHADER_ATTRIB_FASTPATH=1", "ISAAC_VITA_GL_SHIM_FASTDISPATCH=1", REC, WRK]}}
CONTROLS = (("gl_record_backend.c", "A", "isaac_gl_record_opcode_census"),
            ("kage_vita_phase_profile.c", "A", "ph120.ro bid="))


def leg_identity(cur_runtime: Path, base_runtime: Path, out: Path, plant: dict | None = None) -> None:
    stubs = out / "stub-include"
    stubs.mkdir(parents=True, exist_ok=True)
    planted = out / "planted"
    includers = sorted(p.name for p in cur_runtime.glob("*.c")
                       if re.search(r'#\s*include "gl_record_backend.h"', p.read_text(encoding="utf-8", errors="replace"))
                       and not p.name.endswith("_oracle.c") and p.name not in UNITS)
    # gl_record_free_wrap.c (S1) and host_vita_kage_bind_packet.c (W-pkt,
    # compiled only with ISAAC_VITA_KAGE_BIND_PACKET) are new since the base.
    need(sorted(includers) == sorted(HEADER_UNITS + ("gl_record_free_wrap.c", "host_vita_kage_bind_packet.c")
                                     + tuple(COND_UNITS)),
         f"identity: gl_record_backend.h includers changed: {includers}")
    sets = dict(OFF_SETS)
    sets.update(COND_UNITS)
    for unit in HEADER_UNITS:   # gl_record_free_wrap.c is new since the base (S1)
        sets[unit] = {"worker": [REC, WRK, PP]}
    # the overlay TU is empty without its own knob
    sets["host_vita_debug_overlay.c"] = {"worker": [REC, WRK, PP, "ISAAC_VITA_DEBUG_OVERLAY=1"]}
    compared = 0
    for name, variants in sets.items():
        source = None
        if plant and name in plant:
            planted.mkdir(parents=True, exist_ok=True)
            source = planted / name
            source.write_text(plant[name]((cur_runtime / name).read_text(encoding="utf-8")), encoding="utf-8")
        for label, defines in variants.items():
            base = s1.preprocess(base_runtime, name, defines, stubs).split()
            cur = s1.preprocess(cur_runtime, name, defines, stubs, source).split()
            need(base == cur, f"identity {name} [{label}]: preprocessed tokens differ from the base "
                 f"({len(base)} vs {len(cur)} tokens)")
            need(len(cur) > 1000, f"identity {name} [{label}]: only {len(cur)} tokens (a vacuous comparison)")
            compared += 1
            print(f"  identity {name} [{label}]: {len(cur)} tokens = base")
    for name, label, needle in CONTROLS:
        base = s1.preprocess(base_runtime, name, OFF_SETS[name][label], stubs).split()
        on = s1.preprocess(cur_runtime, name, OFF_SETS[name][label] + [f"{DEFINE}=1"], stubs)
        need(on.split() != base and needle in on, f"identity control {name} +{DEFINE}: no effect")
        print(f"  control {name} [{label}] + {DEFINE}=1: differs from base, contains {needle!r}")
    print(f"PASS identity: {compared} TU/define-set pairs token-equal to the base without {DEFINE}; "
          f"{len(CONTROLS)} positive controls change the text")


# ---------------------------------------------------------------- oracle ----
GL_WORKER = ("-DISAAC_VITA_GFUEL_SEED_NATIVE=1", "-DISAAC_VITA_CAMERA_RING_NATIVE=1",
             "-DISAAC_VITA_GL_THREAD_WORKER=1")


def build_phase_oracle(runtime: Path, out: Path, name: str, extra: tuple) -> str:
    out.mkdir(parents=True, exist_ok=True)
    exe = out / f"phase-{name}.exe"
    command = [tool("clang"), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wno-deprecated-non-prototype",
               "-DISAAC_KAGE_VITA_PHASE_PROFILE_ORACLE=1", "-DISAAC_VITA_PHASE_PROFILE=1",
               '-DISAAC_VITA_PHASE_PROFILE_BUILD_ID="phase:oracle"', *GL_WORKER, *extra,
               "-I", str(runtime), "-I", str(runtime.parent / "vita"),
               str(runtime / "kage_vita_phase_profile.c"), str(runtime / "kage_vita_phase_profile_oracle.c"),
               "-o", str(exe)]
    done = subprocess.run(command, capture_output=True, text=True)
    need(done.returncode == 0, f"oracle {name}: build failed:\n{done.stdout}{done.stderr}")
    run = subprocess.run([str(exe)], capture_output=True, text=True)
    need(run.returncode == 0, f"oracle {name}: FAIL:\n{run.stdout}{run.stderr}")
    return run.stdout.strip()


def leg_oracle(runtime: Path, out: Path) -> None:
    off = build_phase_oracle(runtime, out, "gl-worker", ())
    on = build_phase_oracle(runtime, out, "gl-worker-census", (f"-D{DEFINE}=1",))
    print(f"  oracle gl-worker: {off}")
    print(f"  oracle gl-worker-census: {on}")
    need("records=12;" in off and "; ro=" not in off, f"oracle gl-worker: {off}")
    want = off.replace("records=12;", "records=16;").replace("rw=265; ", "rw=265; ro=173; rop=490; ")
    need(on == want, f"oracle gl-worker-census: {on!r} != {want!r}")
    # RENDER60 S4: opcodes 79/80 widen the census to 81 slots (the oracle's
    # max flush checks ops=81 and slot 80 in part 26 under the define).
    batch = build_phase_oracle(runtime, out, "gl-worker-census-batch",
                               (f"-D{DEFINE}=1", "-DISAAC_VITA_GL_RECORD_ATTRIB_BATCH=1"))
    print(f"  oracle gl-worker-census-batch: {batch}")
    need(batch == on, f"oracle gl-worker-census-batch: {batch!r} != {on!r}")
    # RENDER60 W-pkt: opcode 81 widens the census to 82 slots (the oracle's
    # max flush checks ops=82 lines=28 and slot 81 alone in part 27 under the
    # define), with and without S4; the window records are unchanged.
    for name, extra in (("gl-worker-census-pkt", ()),
                        ("gl-worker-census-batch-pkt", ("-DISAAC_VITA_GL_RECORD_ATTRIB_BATCH=1",))):
        pkt = build_phase_oracle(runtime, out, name,
                                 (f"-D{DEFINE}=1", "-DISAAC_VITA_KAGE_BIND_PACKET=1", *extra))
        print(f"  oracle {name}: {pkt}")
        need(pkt == on, f"oracle {name}: {pkt!r} != {on!r}")
    print("PASS oracle: ph120.ro + three ph120.rop after rw (records 12 -> 16), exact window-1/2 lines, "
          "all-ones ro=173 rop=490 (<= 512), the rest of the record set unchanged; "
          "+ATTRIB_BATCH: 81 slots in the same 27 records; +KAGE_BIND_PACKET (with or without "
          "ATTRIB_BATCH): 82 slots in 28 records")


def leg_readme(readme: str) -> None:
    for needle in (KNOB, DEFINE, "ph120.ro", "ph120.rop", "test_gl_record_opcode_census.py",
                   "gl-worker-census", "ISAAC_GL_RECORD=1"):
        need(needle in readme, f"readme: {needle} not documented")
    print("PASS readme: the knob, its define, both records and the gates are documented")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--base", default="e6b69f7", help="knobs-OFF identity base commit")
    parser.add_argument("--output", type=Path, default=ROOT / "build/gl-record-opcode-census")
    parser.add_argument("--plant", action="store_true", help="every leg must FAIL on a planted defect")
    parser.add_argument("--legs", default="scope,cmake,identity,oracle,readme")
    args = parser.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    legs = args.legs.split(",")
    cmake_text = (VITA / "CMakeLists.txt").read_text(encoding="utf-8")
    readme = (VITA / "README.md").read_text(encoding="utf-8")
    have_base = s1.base_available(args.base)
    if not have_base:
        print(f"SKIP identity: base commit {args.base} is not in this checkout's history "
              "(the leg needs it; give --base)")
    if not args.plant:
        try:
            if "scope" in legs:
                leg_scope(RUNTIME, cmake_text)
            if "cmake" in legs:
                leg_cmake(cmake_text, out / "cmake")
            if "identity" in legs and have_base:
                leg_identity(RUNTIME, s1.export_base(args.base, out), out / "identity")
            if "oracle" in legs:
                leg_oracle(RUNTIME, out / "oracle")
            if "readme" in legs:
                leg_readme(readme)
        except LegFailure as error:
            print(f"FAIL {error}")
            return 1
        print("PASS test_gl_record_opcode_census: " + ",".join(
            leg for leg in legs if leg != "identity" or have_base))
        return 0

    planted_runtime = out / "plant-runtime"
    if planted_runtime.exists():
        shutil.rmtree(planted_runtime)
    shutil.copytree(RUNTIME, planted_runtime, ignore=shutil.ignore_patterns("*.obj", "*.o", "*.exe"))
    shutil.copytree(VITA, planted_runtime.parent / "vita", dirs_exist_ok=True,
                    ignore=shutil.ignore_patterns("build*", "*.obj", "*.o", "*.exe", "__pycache__"))
    pp_text = (RUNTIME / "kage_vita_phase_profile.c").read_text(encoding="utf-8")

    def plant_runtime(name: str, old: str, new: str) -> Path:
        text = (RUNTIME / name).read_text(encoding="utf-8")
        if text.count(old) != 1:
            raise PlantError(f"plant anchor missing in {name}: {old[:60]!r}")
        (planted_runtime / name).write_text(text.replace(old, new), encoding="utf-8")
        return planted_runtime

    def restore(name: str) -> None:
        shutil.copy2(RUNTIME / name, planted_runtime / name)

    def oracle_plant(name: str, old: str, new: str) -> None:
        try:
            leg_oracle(plant_runtime(name, old, new), out / "plant-oracle")
        finally:
            restore(name)

    def scope_plant() -> None:
        try:
            text = (RUNTIME / "gl_record_ring.c").read_text(encoding="utf-8")
            (planted_runtime / "gl_record_ring.c").write_text(
                text + f"#if defined({DEFINE})\n#endif\n", encoding="utf-8")
            leg_scope(planted_runtime, cmake_text)
        finally:
            restore("gl_record_ring.c")

    unguarded_call = (f"# if defined({DEFINE})\n    kage_vita_profile_log_opcode_census(\n"
                      "        ISAAC_VITA_PHASE_PROFILE_BUILD_ID,")
    plants = {
        "scope: a third unit reads the define": scope_plant,
        "scope: CMake sets it on a third unit": lambda: leg_scope(RUNTIME, cmake_text.replace(
            '      "${ISAAC_RUNTIME}/kage_vita_phase_profile.c"\n      APPEND PROPERTY COMPILE_DEFINITIONS '
            f'{DEFINE}=1)',
            '      "${ISAAC_RUNTIME}/kage_vita_phase_profile.c"\n      "${ISAAC_RUNTIME}/gl_record_ring.c"\n'
            f'      APPEND PROPERTY COMPILE_DEFINITIONS {DEFINE}=1)')),
        "cmake: the FATAL dropped": lambda: leg_cmake(cmake_text.replace(
            "  message(FATAL_ERROR \"GL_RECORD_OPCODE_CENSUS requires PHASE_PROFILE and GL_THREAD_WORKER\")",
            "  message(STATUS \"census without its dependencies\")"), out / "plant-cmake1"),
        "cmake: a second unit set": lambda: leg_cmake(cmake_text.replace(
            '      "${ISAAC_RUNTIME}/kage_vita_phase_profile.c"\n      APPEND PROPERTY COMPILE_DEFINITIONS '
            f'{DEFINE}=1)',
            '      "${ISAAC_RUNTIME}/kage_vita_phase_profile.c"\n      "${ISAAC_RUNTIME}/gl_record_ring.c"\n'
            f'      APPEND PROPERTY COMPILE_DEFINITIONS {DEFINE}=1)'), out / "plant-cmake2"),
        "identity: emission unguarded": lambda: leg_identity(RUNTIME, s1.export_base(args.base, out),
            out / "plant-id1", {"kage_vita_phase_profile.c": lambda t: t.replace(
                unguarded_call, unguarded_call.replace(f"# if defined({DEFINE})", "# if 1"), 1)}),
        "identity: the census sampler unguarded": lambda: leg_identity(RUNTIME, s1.export_base(args.base, out),
            out / "plant-id2", {"gl_record_backend.c": lambda t: t.replace(
                f"# if defined({DEFINE})\n    s_pw.census_last_op = opcode;",
                "# if 0\n# endif\n# if 1\n    s_pw.census_last_op = opcode;", 1)}),
        "oracle: four slots per ph120.rop": lambda: oracle_plant(
            "kage_vita_phase_profile.c", "#  define KVPP_CENSUS_SLOTS_PER_LINE 3u",
            "#  define KVPP_CENSUS_SLOTS_PER_LINE 4u"),
        "oracle: an inactive slot printed": lambda: oracle_plant(
            "kage_vita_phase_profile.c", "        if (!kage_vita_profile_census_active(o)) continue;\n",
            "        if (!kage_vita_profile_census_active(o) && slot != 3u) continue;\n"),
        "oracle: the S4 opcodes left out of the census bound": lambda: oracle_plant(
            "gl_record_backend.h", "# define ISAAC_GL_RECORD_OPCODE_SLOTS 81u",
            "# define ISAAC_GL_RECORD_OPCODE_SLOTS 79u"),
        "oracle: the W-pkt opcode left out of the census bound": lambda: oracle_plant(
            "gl_record_backend.h", "# define ISAAC_GL_RECORD_OPCODE_SLOTS 82u",
            "# define ISAAC_GL_RECORD_OPCODE_SLOTS 81u"),
        "readme: entry missing": lambda: leg_readme(readme.replace(KNOB, "ISAAC_VITA_SOMETHING_ELSE")),
    }
    if pp_text.count(unguarded_call) != 1:
        raise PlantError("plant anchor: the emission call")
    if not have_base:
        for name in [name for name in plants if name.startswith("identity:")]:
            del plants[name]
            print(f"PLANT {name}: SKIP (no base commit)")
    caught = 0
    for name, plant in plants.items():
        try:
            plant()
        except (LegFailure, subprocess.CalledProcessError) as error:
            caught += 1
            print(f"PLANT {name}: caught ({str(error).splitlines()[0][:160]})")
        else:
            print(f"PLANT {name}: NOT CAUGHT")
    print(f"{'PASS' if caught == len(plants) else 'FAIL'} --plant: {caught}/{len(plants)} planted defects caught")
    return 0 if caught == len(plants) else 1


if __name__ == "__main__":
    sys.exit(main())
