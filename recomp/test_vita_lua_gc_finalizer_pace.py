#!/usr/bin/env python3
"""Host oracle for ISAAC_VITA_LUA_GC_FINALIZER_PACE (no Vita needed).

1. The derivation (recomp/vita/lua53_gc_finalizer_pace.py) pins the
   manifest's pristine lgc.c, changes exactly lines 45 and 848 (and 39 with
   ISAAC_VITA_LUA_GC_SWEEP_COST other than the pristine 5), refuses a
   non-pristine input and out-of-range constants, and its CLI writes the same
   bytes and leaves an up-to-date output untouched.
2. recomp/runtime/vita_lua_gc_finalizer_pace_oracle.c is built 32-bit (the
   Vita ABI: sizeof(TString) = 16, GCSTEPSIZE = 1600) five times against the
   manifest-verified Lua 5.3.3 sources: pristine lgc.c, the derived lgc.c
   (sweep cost 5 and SWEEP), and two mutants that keep only one of the two
   finalizer lines.  Every build
   runs the same EID-shaped workloads (K = paced step KB, FIN finalizable
   userdata and GARB garbage tables per loop):
     - all builds: no finalizer runs early, no finalized object is still a
       weak value, finalizer batches keep tobefnz order, every object is
       finalized exactly once (also after lua_close), weak keys are cleared;
     - derived build: every luaC_step stays within the pacing bound
       (budget + GCSTEPSIZE + 200) / COST + 2 * MAX; pristine and both mutants
       must violate it somewhere, which shows the check has teeth for each
       derived line;
     - a __gc that raises inside a paced step surfaces the identical
       LUA_ERRGCMM message to the protected caller in every build and the
       remaining finalizers still run;
     - the derived builds cover both finalizer lines: gcfinnum doubling below
       MAX, saturating at MAX, reset on drain, finalizers in GCScallfin and in
       the tail runafewfinalizers;
     - the sweep line is live: with SWEEP the K=40 cycle needs >= 4x the
       sweep steps of the sweep-5 build for about the same objects.
3. The real CMake blocks of recomp/vita/CMakeLists.txt (knob validation and
   the isaac_lua53 source swap), extracted verbatim and run under cmake -P:
   OFF leaves the 33-source list untouched; ON swaps exactly lgc.c, in place,
   for <build>/lua53-gc-finalizer-pace/lgc.c holding the derived bytes (for
   the default and for SWEEP); out of range constants, PACE without
   ISAAC_VITA_LUA and non-default sub-knobs with PACE OFF are refused.
The device projection lines (gc ms/s, worst frame, heap peak) are printed for
reference; they are estimates from console constants, not pass criteria.

usage: python recomp/test_vita_lua_gc_finalizer_pace.py
           [--lua-source DIR] [--cc CLANG] [--work DIR]
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

import test_vita_lua_runtime as R

ROOT = Path(__file__).resolve().parent
ORACLE = ROOT / "runtime" / "vita_lua_gc_finalizer_pace_oracle.c"
VITA_CMAKE = ROOT / "vita" / "CMakeLists.txt"
sys.path.insert(0, str(ROOT / "vita"))
import lua53_gc_finalizer_pace as pace  # noqa: E402

MAX, COST = 8, 512           # the CMake defaults under test
SWEEP = 32                   # the proposed ISAAC_VITA_LUA_GC_SWEEP_COST
DERIVED_SHA256 = "590b05c4ef62feca1d24adb422af6c6bec07dfef747182adedb17b5f56a2abec"
DERIVED_SWEEP_SHA256 = (
    "2c6c2c0783eca2925d55bed5fca3e29ec4de75ddafd5a67eea1379bc6ecd2508")
PRISTINE_LINE_39 = b"#define GCSWEEPCOST\t((sizeof(TString) + 4) / 4)\n"
PRISTINE_LINE_45 = b"#define GCFINALIZECOST\tGCSWEEPCOST\n"
PRISTINE_LINE_848 = (b"                    : g->gcfinnum * 2;  "
                     b"/* else call a few more next time */\n")
LIVE_KB = 11300              # EID heap (P2b ph120.mem lua(l) 11223..11674 KiB)
GARB = 35
# name, K, finalizers per loop, frames
SCENARIOS = (
    ("k128", 128, 8, 4000),
    ("k40", 40, 8, 6000),
    ("k40-heavy", 40, 30, 4000),
    ("k16", 16, 8, 6000),
)
VIOLATION = re.compile(
    r"violations early=(\d+) weak=(\d+) order=(\d+) twice=(\d+) never=(\d+) "
    r"keys=(\d+) bound=(\d+)")
COVERAGE = re.compile(
    r"coverage double=(\d+) cap=(\d+) drain=(\d+) callfin=(\d+) tail=(\d+)")
SWEEP_LINE = re.compile(
    r"sweep cost=(\d+) steps=(\d+) per_cycle=([0-9.]+) max_step_objs=(\d+) "
    r"objs_per_cycle=(\d+)")
ERROR_LINE = re.compile(r"^error-case .*$", re.M)


def fail(message: str) -> None:
    raise SystemExit(f"FAIL: {message}")


def need(ok: bool, message: str) -> None:
    if not ok:
        fail(message)


def derivation_tests(pristine: bytes, work: Path) -> None:
    need(hashlib.sha256(pristine).hexdigest() == pace.PRISTINE_LGC_SHA256,
         "PRISTINE_LGC_SHA256 differs from the manifest-verified lgc.c")
    derived = pace.derive(pristine, MAX, COST)
    need(hashlib.sha256(derived).hexdigest() == DERIVED_SHA256,
         f"derived lgc.c sha changed: {hashlib.sha256(derived).hexdigest()}")
    old_lines = pristine.splitlines(keepends=True)
    new_lines = derived.splitlines(keepends=True)
    need(len(old_lines) == len(new_lines), "derivation changed the line count")
    changed = [i + 1 for i, (a, b) in enumerate(zip(old_lines, new_lines))
               if a != b]
    need(changed == [45, 848], f"derivation changed lines {changed}")
    need(old_lines[44] == PRISTINE_LINE_45 and old_lines[847] == PRISTINE_LINE_848,
         "pristine anchors moved")
    need(new_lines[44].startswith(b"#define GCFINALIZECOST\t512 "),
         f"line 45: {new_lines[44]!r}")
    need(b"(g->gcfinnum * 2 > 8u ? 8u : g->gcfinnum * 2);" in new_lines[847],
         f"line 848: {new_lines[847]!r}")
    need(pace.derive(pristine, MAX, COST, 5) == derived,
         "sweep cost 5 must leave lgc.c:39 pristine")
    swept = pace.derive(pristine, MAX, COST, SWEEP)
    need(hashlib.sha256(swept).hexdigest() == DERIVED_SWEEP_SHA256,
         f"sweep-derived lgc.c sha changed: {hashlib.sha256(swept).hexdigest()}")
    swept_lines = swept.splitlines(keepends=True)
    need(len(swept_lines) == len(old_lines), "sweep derivation changed the line count")
    swept_changed = [i + 1 for i, (a, b) in enumerate(zip(old_lines, swept_lines))
                     if a != b]
    need(swept_changed == [39, 45, 848],
         f"sweep derivation changed lines {swept_changed}")
    need(old_lines[38] == PRISTINE_LINE_39, "pristine sweep anchor moved")
    need(swept_lines[38].startswith(b"#define GCSWEEPCOST\t32 "),
         f"line 39: {swept_lines[38]!r}")
    need(swept_lines[44] == new_lines[44] and swept_lines[847] == new_lines[847],
         "the sweep cost changed a finalizer line")
    for bad in ((0, COST), (4097, COST), (MAX, 0), (MAX, 65537),
                (MAX, COST, 0), (MAX, COST, 401)):
        try:
            pace.derive(pristine, *bad)
        except pace.DeriveError:
            pass
        else:
            fail(f"derive accepted out-of-range constants {bad}")
    try:
        pace.derive(pristine + b"\n", MAX, COST)
    except pace.DeriveError as exc:
        need("not pristine" in str(exc), f"wrong refusal: {exc}")
    else:
        fail("derive accepted a non-pristine lgc.c")
    # CLI: same bytes; an up-to-date output is not rewritten; bad input -> 1.
    src = work / "cli" / "pristine-lgc.c"
    out = work / "cli" / "out" / "lgc.c"
    src.parent.mkdir(parents=True, exist_ok=True)
    src.write_bytes(pristine)
    script = ROOT / "vita" / "lua53_gc_finalizer_pace.py"
    cmd = [sys.executable, str(script), "--input", str(src), "--output", str(out),
           "--max", str(MAX), "--cost", str(COST)]
    first = subprocess.run(cmd, capture_output=True, text=True)
    need(first.returncode == 0 and out.read_bytes() == derived,
         f"CLI derivation failed: {first.stdout}{first.stderr}")
    need(f"sha256={DERIVED_SHA256}" in first.stdout, f"CLI receipt: {first.stdout}")
    out_sweep = work / "cli" / "out-sweep" / "lgc.c"
    sweep_cmd = [sys.executable, str(script), "--input", str(src), "--output",
                 str(out_sweep), "--max", str(MAX), "--cost", str(COST),
                 "--sweep-cost", str(SWEEP)]
    second = subprocess.run(sweep_cmd, capture_output=True, text=True)
    need(second.returncode == 0 and out_sweep.read_bytes() == swept and
         f"sweep_cost={SWEEP} sha256={DERIVED_SWEEP_SHA256}" in second.stdout,
         f"CLI sweep derivation failed: {second.stdout}{second.stderr}")
    stamp = out.stat().st_mtime_ns
    os.utime(out, ns=(stamp - 10**9, stamp - 10**9))
    stamp = out.stat().st_mtime_ns
    again = subprocess.run(cmd, capture_output=True, text=True)
    need(again.returncode == 0 and out.stat().st_mtime_ns == stamp,
         "CLI rewrote an up-to-date output")
    src.write_bytes(pristine.replace(b"GCSWEEPCOST", b"GCSWEEPCOSX", 1))
    bad = subprocess.run(cmd, capture_output=True, text=True)
    need(bad.returncode == 1 and "not pristine" in bad.stderr,
         f"CLI accepted a non-pristine input: {bad.returncode} {bad.stderr}")
    print(f"derivation: lines {changed} changed, sha {DERIVED_SHA256[:16]}...; "
          f"sweep {SWEEP}: lines {swept_changed}, sha "
          f"{DERIVED_SWEEP_SHA256[:16]}...; range/pristine/CLI refusals OK")


def cmake_block(first: str, blocks: int) -> str:
    """`blocks` consecutive if()/endif() blocks of CMakeLists.txt, verbatim."""
    lines = VITA_CMAKE.read_text(encoding="utf-8").split("\n")
    need(lines.count(first) == 1, f"CMake anchor not unique: {first!r}")
    start = lines.index(first)
    depth = done = 0
    for index in range(start, len(lines)):
        stripped = lines[index].strip()
        if stripped.startswith("if("):
            depth += 1
        elif stripped.startswith("endif("):
            depth -= 1
            if depth == 0:
                done += 1
                if done == blocks:
                    return "\n".join(lines[start:index + 1])
    fail(f"CMake block {first!r} has no matching endif")
    raise AssertionError


def posix(path: Path) -> str:
    return str(path).replace("\\", "/")


def cmake_tests(lua_root: Path, pristine: bytes, work: Path) -> None:
    cmake = (shutil.which("cmake") or "C:/Program Files/CMake/bin/cmake.exe")
    need(Path(cmake).exists() or shutil.which(cmake) is not None,
         "cmake is required for the CMake leg")
    validation = cmake_block(
        "if(ISAAC_VITA_LUA_GC_FINALIZER_PACE AND NOT ISAAC_VITA_LUA)", 5)
    need("ISAAC_VITA_LUA_GC_SWEEP_COST must be" in validation,
         "the extracted CMake validation lost the sweep-cost check")
    swap = cmake_block("  if(ISAAC_VITA_LUA_GC_FINALIZER_PACE)", 1)
    derived = pace.derive(pristine, MAX, COST)

    def configure(tag: str, pace_on: str, lua_on: str = "ON",
                  max_text: str = str(MAX), cost_text: str = str(COST),
                  sweep_text: str = "5") -> tuple[int, str, Path]:
        build = work / "cmake" / tag
        build.mkdir(parents=True, exist_ok=True)
        script = build / "probe.cmake"
        script.write_text("\n".join([
            "cmake_minimum_required(VERSION 3.16)",
            "# set_property(DIRECTORY ...) is not scriptable; the probe only",
            "# needs the source list and the derived file.",
            "function(set_property)",
            "endfunction()",
            f'set(ISAAC_VITA_LUA {lua_on})',
            f'set(ISAAC_VITA_LUA_GC_FINALIZER_PACE {pace_on})',
            f'set(ISAAC_VITA_LUA_GC_FINALIZER_MAX "{max_text}")',
            f'set(ISAAC_VITA_LUA_GC_FINALIZER_COST "{cost_text}")',
            f'set(ISAAC_VITA_LUA_GC_SWEEP_COST "{sweep_text}")',
            f'set(ISAAC_LUA53_SOURCE_DIR "{posix(lua_root)}")',
            f'set(CMAKE_CURRENT_SOURCE_DIR "{posix(VITA_CMAKE.parent)}")',
            f'set(CMAKE_BINARY_DIR "{posix(build)}")',
            f'set(Python3_EXECUTABLE "{posix(Path(sys.executable))}")',
            validation,
            f'include("{posix(VITA_CMAKE.parent / "lua53_vanilla.cmake")}")',
            'set(PROBE_BEFORE "${ISAAC_LUA53_C_SOURCES}")',
            swap,
            'message(STATUS "PROBE_BEFORE=${PROBE_BEFORE}")',
            'message(STATUS "PROBE_AFTER=${ISAAC_LUA53_C_SOURCES}")',
            "",
        ]), encoding="utf-8", newline="\n")
        done = subprocess.run([cmake, "-P", str(script)], capture_output=True,
                              text=True)
        return done.returncode, done.stdout + done.stderr, build

    def lists(out: str) -> tuple[list[str], list[str]]:
        before = re.search(r"PROBE_BEFORE=(.*)", out)
        after = re.search(r"PROBE_AFTER=(.*)", out)
        need(before is not None and after is not None, f"probe output: {out}")
        assert before is not None and after is not None
        return before.group(1).strip().split(";"), after.group(1).strip().split(";")

    rc, out, build = configure("off", "OFF")
    need(rc == 0, f"CMake OFF probe failed: {out}")
    before, after = lists(out)
    need(len(before) == 33 and after == before, "OFF changed the Lua source list")
    need(not (build / "lua53-gc-finalizer-pace").exists(),
         "OFF wrote a derived lgc.c")
    rc, out, build = configure("on", "ON")
    need(rc == 0, f"CMake ON probe failed: {out}")
    before, after = lists(out)
    target = posix(build / "lua53-gc-finalizer-pace" / "lgc.c")
    index = [Path(item).name for item in before].index("lgc.c")
    need(after == before[:index] + [target] + before[index + 1:],
         f"ON did not swap exactly lgc.c in place: {after}")
    need((build / "lua53-gc-finalizer-pace" / "lgc.c").read_bytes() == derived,
         "ON derived bytes differ from lua53_gc_finalizer_pace.derive")
    need(f"sha256={DERIVED_SHA256}" in out, f"ON status receipt missing: {out}")
    rc, out, build = configure("on-sweep", "ON", sweep_text=str(SWEEP))
    need(rc == 0, f"CMake ON sweep probe failed: {out}")
    unused, after = lists(out)
    need([Path(item).name for item in after].count("lgc.c") == 1 and
         posix(build / "lua53-gc-finalizer-pace" / "lgc.c") in after,
         f"ON sweep did not swap lgc.c: {after}")
    need((build / "lua53-gc-finalizer-pace" / "lgc.c").read_bytes() ==
         pace.derive(pristine, MAX, COST, SWEEP),
         "ON sweep derived bytes differ from lua53_gc_finalizer_pace.derive")
    need(f"sweep_cost={SWEEP} sha256={DERIVED_SWEEP_SHA256}" in out,
         f"ON sweep status receipt missing: {out}")
    refusals = (
        ("ON", "max0", {"max_text": "0"}, "ISAAC_VITA_LUA_GC_FINALIZER_MAX must"),
        ("ON", "max-big", {"max_text": "4097"}, "ISAAC_VITA_LUA_GC_FINALIZER_MAX must"),
        ("ON", "cost-text", {"cost_text": "fast"}, "ISAAC_VITA_LUA_GC_FINALIZER_COST must"),
        ("ON", "cost-big", {"cost_text": "65537"}, "ISAAC_VITA_LUA_GC_FINALIZER_COST must"),
        ("ON", "sweep0", {"sweep_text": "0"}, "ISAAC_VITA_LUA_GC_SWEEP_COST must"),
        ("ON", "sweep-big", {"sweep_text": "401"}, "ISAAC_VITA_LUA_GC_SWEEP_COST must"),
        ("ON", "no-lua", {"lua_on": "OFF"}, "requires ISAAC_VITA_LUA=ON"),
        ("OFF", "sweep-pace-off", {"sweep_text": str(SWEEP)}, "take effect only with"),
        ("OFF", "cost-pace-off", {"cost_text": "1024"}, "take effect only with"))
    for pace_on, tag, kwargs, needle in refusals:
        rc, out, unused = configure(tag, pace_on, **kwargs)
        # cmake wraps long FATAL_ERROR text
        need(rc != 0 and needle in " ".join(out.split()),
             f"CMake accepted {tag}: {out}")
    print("cmake: OFF list untouched, ON swaps lgc.c in place with the derived "
          f"bytes (sweep 5 and {SWEEP}), {len(refusals)} refusals OK")


def find_cc(explicit: Path | None) -> Path:
    candidates = [explicit] if explicit else []
    env = os.environ.get("ISAAC_CLANG")
    if env:
        candidates.append(Path(env))
    candidates.append(Path("C:/Program Files/LLVM/bin/clang.exe"))
    which = shutil.which("clang")
    if which:
        candidates.append(Path(which))
    for candidate in candidates:
        if candidate and candidate.is_file():
            return candidate
    fail("clang not found: pass --cc or set ISAAC_CLANG")
    raise AssertionError


def run(cmd: list[str], cwd: Path) -> str:
    done = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    if done.returncode:
        fail(f"command failed ({done.returncode}): {' '.join(cmd)}\n"
             f"{done.stdout}{done.stderr}")
    return done.stdout + done.stderr


def build_variants(cc: Path, lua_src: Path, sources: list[Path],
                   pristine: bytes, work: Path) -> dict[str, Path]:
    common = work / "common"
    common.mkdir(parents=True, exist_ok=True)
    flags = ["-m32", "-O2", "-w", f"-I{lua_src}"]
    objects = []
    for source in sources:
        if source.name == "lgc.c":
            continue
        obj = common / (source.stem + ".o")
        run([str(cc), *flags, "-c", str(source), "-o", str(obj)], work)
        objects.append(str(obj))
    derived = pace.derive(pristine, MAX, COST)
    cost_line = derived.splitlines(keepends=True)[44]
    cap_line = derived.splitlines(keepends=True)[847]
    variants = {
        # name: (lgc bytes, oracle defines)
        "pristine": (pristine, []),
        "paced": (derived, [f"-DORACLE_FIN_MAX={MAX}", f"-DORACLE_FIN_COST={COST}"]),
        "paced-sweep": (pace.derive(pristine, MAX, COST, SWEEP),
                        [f"-DORACLE_FIN_MAX={MAX}", f"-DORACLE_FIN_COST={COST}",
                         f"-DORACLE_SWEEP_COST={SWEEP}"]),
        "mutant-cost-only": (derived.replace(cap_line, PRISTINE_LINE_848),
                             [f"-DORACLE_FIN_COST={COST}"]),
        "mutant-cap-only": (derived.replace(cost_line, PRISTINE_LINE_45),
                            [f"-DORACLE_FIN_MAX={MAX}"]),
    }
    exes = {}
    for name, (text, defines) in variants.items():
        directory = work / name
        directory.mkdir(parents=True, exist_ok=True)
        lgc = directory / "lgc.c"
        lgc.write_bytes(text)
        lgc_obj = directory / "lgc.o"
        # every luaC_step goes through the oracle's wrapper
        run([str(cc), *flags, "-DluaC_step=oracle_real_luaC_step", "-c",
             str(lgc), "-o", str(lgc_obj)], work)
        exe = directory / "oracle.exe"
        run([str(cc), *flags, *defines, str(ORACLE), *objects, str(lgc_obj),
             "-o", str(exe)], work)
        exes[name] = exe
    return exes


def oracle(exe: Path, args: list[str]) -> tuple[int, str]:
    done = subprocess.run([str(exe), *args], capture_output=True, text=True)
    return done.returncode, done.stdout + done.stderr


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--lua-source", type=Path)
    parser.add_argument("--cc", type=Path)
    parser.add_argument("--work", type=Path)
    args = parser.parse_args()
    lua_root = R.discover_source(args.lua_source)
    sources = R.validate_source(lua_root, R.records())
    need(len(sources) == 33, f"expected the 33-C closure, got {len(sources)}")
    lua_src = lua_root / "src"
    manifest_lgc = dict(R.records())["lgc.c"]
    need(manifest_lgc == pace.PRISTINE_LGC_SHA256,
         "lua53_gc_finalizer_pace.PRISTINE_LGC_SHA256 != lua53_vanilla.cmake lgc.c")
    pristine = (lua_src / "lgc.c").read_bytes()
    cc = find_cc(args.cc)
    holder = None
    if args.work is None:
        holder = tempfile.TemporaryDirectory(prefix="isaac-lua-gc-pace-")
        work = Path(holder.name)
    else:
        work = args.work.resolve()
        work.mkdir(parents=True, exist_ok=True)
    try:
        derivation_tests(pristine, work)
        cmake_tests(lua_root, pristine, work)
        exes = build_variants(cc, lua_src, sources, pristine, work)
        bound_bad: dict[str, list[str]] = {name: [] for name in exes}
        coverage: dict[str, list[int]] = {name: [0] * 5 for name in exes}
        sweeps: dict[tuple[str, str], tuple[int, float, int, int]] = {}
        for scenario, k, fin, frames in SCENARIOS:
            for name, exe in exes.items():
                rc, out = oracle(exe, ["run", str(k), str(LIVE_KB), str(fin),
                                       str(GARB), str(frames), "60",
                                       str(COST), str(MAX)])
                violations = VIOLATION.search(out)
                cov = COVERAGE.search(out)
                sweep = SWEEP_LINE.search(out)
                need(violations is not None and cov is not None and
                     sweep is not None,
                     f"{name} {scenario}: unparsable oracle output\n{out}")
                assert violations is not None and cov is not None
                assert sweep is not None
                sweeps[(scenario, name)] = (
                    int(sweep.group(1)), float(sweep.group(3)),
                    int(sweep.group(4)), int(sweep.group(5)))
                early, weak, order, twice, never, keys, bound = (
                    int(v) for v in violations.groups())
                need(rc == 0 and not (early or weak or order or twice or never
                                      or keys),
                     f"{name} {scenario}: finalizer semantics violated "
                     f"(rc={rc})\n{out}")
                if bound:
                    bound_bad[name].append(scenario)
                for i, value in enumerate(cov.groups()):
                    coverage[name][i] += int(value)
                device = out.strip().splitlines()[-1].strip()
                worst = [line.strip() for line in out.splitlines()
                         if "worst-step" in line]
                print(f"{scenario:9s} {name:17s} bound_violations={bound:4d} "
                      f"{device}")
                if name.startswith("paced") and worst:
                    print(f"{'':27s}{worst[0]}")
                    print(f"{'':27s}{sweep.group(0)}")
        for name in ("paced", "paced-sweep"):
            need(not bound_bad[name],
                 f"{name} build exceeded the pacing bound in {bound_bad[name]}")
        five = sweeps[("k40", "paced")]
        charged = sweeps[("k40", "paced-sweep")]
        need(five[0] == 5 and charged[0] == SWEEP,
             f"oracle sweep cost defines: {five} {charged}")
        # The oracle derives swept objects as units / ORACLE_SWEEP_COST, so the
        # objects-per-step ratio (~SWEEP / 5 = 6.4) only shows the define is
        # consistent.  The teeth: with the line live a cycle needs >= 4x the
        # sweep steps, and the objects per cycle stay the same population
        # (a longer cycle sweeps somewhat more garbage).  Built with the
        # define but without lgc.c:39, k40 gives 41 steps/cycle and 23k
        # "objects" per cycle against 36 and 132k: both checks fail.
        ratio = five[2] / max(charged[2], 1)
        population = charged[3] / max(five[3], 1)
        need(charged[1] >= 4 * five[1] and 5.0 <= ratio <= 8.0 and
             0.6 <= population <= 2.5,
             "sweep line not live: k40 sweep steps/cycle "
             f"{five[1]} -> {charged[1]}, max objects/step {five[2]} -> "
             f"{charged[2]} (ratio {ratio:.2f}, expected ~{SWEEP / 5:.1f}), "
             f"objects/cycle {five[3]} -> {charged[3]}")
        print(f"sweep line: k40 sweep steps/cycle {five[1]} -> {charged[1]}, "
              f"max objects/step {five[2]} -> {charged[2]}, objects/cycle "
              f"{five[3]} -> {charged[3]}")
        for name in ("pristine", "mutant-cost-only", "mutant-cap-only"):
            need(bool(bound_bad[name]),
                 f"{name} never violated the pacing bound: the check has no teeth")
        need(bound_bad["pristine"] == [s[0] for s in SCENARIOS],
             f"pristine must violate the bound in every scenario: "
             f"{bound_bad['pristine']}")
        for name in ("paced", "paced-sweep"):
            double, cap, drain, callfin, tail = coverage[name]
            need(double and cap and drain and callfin and tail,
                 f"{name} build coverage incomplete: double={double} cap={cap} "
                 f"drain={drain} callfin={callfin} tail={tail}")
        need(coverage["pristine"][1] == 0 and coverage["pristine"][0] > 0,
             f"pristine coverage implausible: {coverage['pristine']}")
        for name, values in coverage.items():
            print(f"coverage {name:17s} double={values[0]} cap={values[1]} "
                  f"drain={values[2]} callfin={values[3]} tail={values[4]} "
                  f"bound_violations_in={bound_bad[name]}")
        error_lines = {}
        for name, exe in exes.items():
            rc, out = oracle(exe, ["error"])
            match = ERROR_LINE.search(out)
            need(rc == 0 and match is not None,
                 f"{name}: __gc error case failed (rc={rc})\n{out}")
            assert match is not None
            error_lines[name] = match.group(0)
        need(len(set(error_lines.values())) == 1,
             f"__gc error behaviour differs between builds: {error_lines}")
        print(f"error case (all builds identical): {error_lines['paced']}")
    finally:
        if holder is not None:
            holder.cleanup()
    print("PASS: test_vita_lua_gc_finalizer_pace "
          f"(derivation + CMake + {len(SCENARIOS)} workloads x {len(exes)} "
          "builds + __gc error case)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
