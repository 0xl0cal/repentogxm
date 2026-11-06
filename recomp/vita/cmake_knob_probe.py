#!/usr/bin/env python3
"""Configure only the knob block of recomp/vita/CMakeLists.txt, on any host.

A FATAL negative ("configure must fail with message M") is only a test of M
when M is the *first* FATAL the configure reaches.  CMake stops at the first
message(FATAL_ERROR), so a negative that inherits an init cache with other
dependent knobs ON fails on an earlier, unrelated message and reads as a
defect in correct code (an ISAAC_VITA_LUA_INTEGER2STR negative once
stopped at
"ISAAC_VITA_LUA_CALL_ERROR_CATCH requires ISAAC_VITA_LUA=ON", line 387).

This tool lets a lane check its negatives on a host without the Vita SDK:

    python cmake_knob_probe.py -C init.cmake -S <src>/recomp/vita -B <dir> \
        [-G Ninja] [-D KEY=VALUE ...] [--first-line N] [--last-line N]

It accepts the configure arguments of an ARM build on the Linux build host,
so a build script's neg() runs unchanged with `cmake` pointed at it.  It writes a throw-away project under
<dir>/knob-probe whose CMakeLists.txt keeps the real file's
cmake_minimum_required (line 1, so the same policies) and helper includes
(lines 3-6), then includes the real file's lines [first, last] with every
earlier line blank, so a FATAL reports the real line number.  It configures
that project in configure mode (real -C cache semantics, option(), CACHE
FORCE), with project(... NONE): no compiler, no SDK.  The output is the
CMake output with the region path rewritten to <src>/CMakeLists.txt, so it
reads like the real configure's log; the exit status is CMake's.

What it does not model, by construction:
  * lines 1-48 (the VITASDK / toolchain / PE-path prologue and project());
    the init cache's CMAKE_TOOLCHAIN_FILE / CMAKE_MAKE_PROGRAM lines are
    dropped, and -G is ignored (the host has no Ninja);
  * everything after --last-line (default: the line before the first
    include()/find_program()/add_*()/execute_process() after the region
    start, i.e. the first command that needs the SDK or the build tree).
A probe PASS is therefore "the knob block admits this configuration", not
"the ARM configure succeeds"; the ARM build stays the authority.
"""
from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
from pathlib import Path

PROLOGUE_END = re.compile(r'^\s*include\("\$\{VITASDK\}/share/vita\.cmake"\s+REQUIRED\)\s*$')
REGION_STOP = re.compile(
    r"^\s*(include|find_program|find_package|find_library|find_path|find_file|"
    r"add_library|add_executable|add_custom_command|add_custom_target|"
    r"add_subdirectory|execute_process|try_compile|try_run|ExternalProject_Add)\s*\(")
HELPER_INCLUDE = re.compile(r'^include\("\$\{CMAKE_CURRENT_LIST_DIR\}/([A-Za-z0-9_.]+\.cmake)"\)\s*$')
# init-cache keys that select a toolchain or build tool: meaningless for a
# project(NONE) probe, and a build-host path here would fail project() itself.
DROPPED_INIT_KEYS = ("CMAKE_TOOLCHAIN_FILE", "CMAKE_MAKE_PROGRAM", "CMAKE_GENERATOR")


def cmake_exe() -> str:
    for candidate in (shutil.which("cmake"), r"C:\Program Files\CMake\bin\cmake.exe"):
        if candidate and Path(candidate).is_file():
            return candidate
    raise SystemExit("cmake_knob_probe: cmake not found")


def region_bounds(lines: list[str], first: int | None, last: int | None) -> tuple[int, int]:
    """1-based inclusive [first, last] of the knob block."""
    if first is None:
        hits = [i + 1 for i, line in enumerate(lines) if PROLOGUE_END.match(line)]
        if len(hits) != 1:
            raise SystemExit(f"cmake_knob_probe: expected one vita.cmake include, found {hits}")
        first = hits[0] + 1
    if last is None:
        last = len(lines)
        for number in range(first, len(lines) + 1):
            if REGION_STOP.match(lines[number - 1]):
                last = number - 1
                break
    if not 1 < first <= last <= len(lines):
        raise SystemExit(f"cmake_knob_probe: bad region {first}..{last} of {len(lines)} lines")
    return first, last


def probe_project(source: Path, work: Path, first: int, last: int) -> Path:
    """Write the throw-away project; return the region file's path."""
    lines = (source / "CMakeLists.txt").read_text(encoding="utf-8").splitlines()
    if not lines[0].startswith("cmake_minimum_required("):
        raise SystemExit("cmake_knob_probe: line 1 is not cmake_minimum_required")
    src = source.resolve().as_posix()
    helpers = []
    for line in lines[:first - 1]:
        match = HELPER_INCLUDE.match(line)
        if match:
            helpers.append(f'include("{src}/{match.group(1)}")')
    region = [line.replace("${CMAKE_CURRENT_LIST_DIR}", src) for line in lines[first - 1:last]]
    work.mkdir(parents=True, exist_ok=True)
    region_file = work / "region.cmake"
    region_file.write_text("\n" * (first - 1) + "\n".join(region) + "\n",
                           encoding="utf-8", newline="\n")
    top = [lines[0],
           "project(isaac_knob_probe NONE)",
           # the region reads its own sources through CMAKE_CURRENT_SOURCE_DIR
           f'set(CMAKE_CURRENT_SOURCE_DIR "{src}")',
           *helpers,
           f'include("{region_file.resolve().as_posix()}")',
           f'message(STATUS "isaac knob probe: CMakeLists.txt lines {first}..{last} passed")']
    (work / "CMakeLists.txt").write_text("\n".join(top) + "\n", encoding="utf-8", newline="\n")
    return region_file


def filtered_init(init: Path, extra: list[str], out: Path,
                  path_map: list[tuple[str, str]]) -> list[str]:
    kept, dropped = [], []
    for line in init.read_text(encoding="utf-8").splitlines():
        for old, new in path_map:
            line = line.replace(old, new)
        match = re.match(r"\s*set\(\s*([A-Za-z0-9_]+)\b", line)
        if match and match.group(1) in DROPPED_INIT_KEYS:
            dropped.append(match.group(1))
            continue
        kept.append(line)
    for item in extra:
        key, sep, value = item.partition("=")
        if not sep:
            raise SystemExit(f"cmake_knob_probe: -D {item!r} is not KEY=VALUE")
        kept.append(f'set({key} "{value}" CACHE STRING "" FORCE)')
    out.write_text("\n".join(kept) + "\n", encoding="utf-8", newline="\n")
    return dropped


def run(argv: list[str]) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("-C", dest="init", type=Path, required=True)
    parser.add_argument("-S", dest="source", type=Path, required=True)
    parser.add_argument("-B", dest="build", type=Path, required=True)
    parser.add_argument("-G", dest="generator", default=None, help="accepted and ignored")
    parser.add_argument("-D", dest="defs", action="append", default=[])
    parser.add_argument("--first-line", type=int, default=None)
    parser.add_argument("--last-line", type=int, default=None)
    parser.add_argument("--path-map", action="append", default=[], metavar="OLD=NEW",
                        help="rewrite a path prefix in the init values (a build-host checkout "
                             "path to a local one, for the region's EXISTS checks)")
    args = parser.parse_args(argv)
    lines = (args.source / "CMakeLists.txt").read_text(encoding="utf-8").splitlines()
    first, last = region_bounds(lines, args.first_line, args.last_line)
    work = args.build / "knob-probe"
    if work.exists():
        shutil.rmtree(work)
    region_file = probe_project(args.source, work, first, last)
    init = work / "init.cmake"
    path_map = []
    for item in args.path_map:
        old, sep, new = item.partition("=")
        if not sep or not old:
            raise SystemExit(f"cmake_knob_probe: --path-map {item!r} is not OLD=NEW")
        path_map.append((old, new))
    dropped = filtered_init(args.init, args.defs, init, path_map)
    print(f"-- isaac knob probe: region {first}..{last}, init {args.init}"
          f" ({len(args.defs)} -D), dropped {','.join(dropped) or 'nothing'}", flush=True)
    done = subprocess.run([cmake_exe(), "-C", str(init), "-S", str(work), "-B", str(work / "b")],
                          stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    text = done.stdout.decode("utf-8", errors="replace")
    # CMake names the region file relative to the probe project when it can
    # ("CMake Error at region.cmake:592 (message)"), absolute otherwise.
    real = (args.source / "CMakeLists.txt").resolve().as_posix()
    for spelling in sorted({region_file.resolve().as_posix(), str(region_file.resolve()),
                            region_file.name}, key=len, reverse=True):
        text = re.sub(r"(CMake (?:Error|Warning)[^\n]* at )" + re.escape(spelling) + r":",
                      lambda m: m.group(1) + real + ":", text)
    sys.stdout.write(text)
    sys.stdout.flush()
    return done.returncode


if __name__ == "__main__":
    sys.exit(run(sys.argv[1:]))
