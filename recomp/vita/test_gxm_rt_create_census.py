#!/usr/bin/env python3
"""Host proofs for ISAAC_VITA_GXM_RT_CREATE_CENSUS (measurement only, OFF).

Run from recomp/:  python -B vita/test_gxm_rt_create_census.py [--base REV]

1. sources   the knob's code in kage_vita_phase_profile.c is two whole
             "#if defined(ISAAC_VITA_GXM_RT_CREATE_CENSUS)" blocks; with them
             (and the deferred-attach blocks of the same lane, and the GL
             admit behind blocks merged at 7d23e50) removed the
             file is the base revision's text; no other C source or header
             names the knob; CMake has one OFF option, one refusal, the
             define on exactly one source and the two --wrap link options,
             and the lane deleted no CMake line.
2. off       kage_vita_phase_profile.c compiled for the host without the
             knob is byte-identical to the base revision's text compiled the
             same way (both objects are built here, every run; the ON
             objects of leg 3 must differ from it).  A host object: the ARM
             identity is the box script's gate.
3. on        the same file compiles with the knob (-Wall -Wextra -Werror),
             alone, with the render thread and with deferred attach; the
             object defines the two __wrap_ symbols and leaves the two
             __real_ ones undefined; the OFF object has neither.
4. oracle    host_tests/gxm_rt_census_oracle.c: the wrappers from the real
             header, scripted clock and scripted __real_ functions; exact
             record and receipt text for three windows.
5. mutants   the header with one edit each; every one must be rejected by
             the oracle.

Not covered here (needs the ARM toolchain): that GNU ld's --wrap routes the
references inside libvitaGL.a to the wrappers.  The box script checks the
symbols of the linked ELF; the device receipt's seen(c,...) >= 1 proves the
route at run time.  The emission block in kage_vita_phase_profile.c is
compiled here but not executed: its format, arguments and take are the
header's macros and function, which the oracle does execute.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RUNTIME = ROOT / "runtime"
VITA = ROOT / "vita"
KNOB = "ISAAC_VITA_GXM_RT_CREATE_CENSUS"
GUARDS = (f"#if defined({KNOB})", "#if defined(ISAAC_VITA_FBO_DEFERRED_ATTACH)")
HEADER = "kage_vita_gxm_rt_census.h"
PROFILE = "runtime/kage_vita_phase_profile.c"
IDENTIFIERS = re.compile(
    r"GXM_RT_CREATE_CENSUS|GXM_RT_CENSUS|gxm_rt_census|KageVitaGxmRtCensus|"
    r"ph120\.gr|__(?:wrap|real)_sceGxm(?:Create|Destroy)RenderTarget")
# Files that may name the knob (C sources and headers under recomp/).
ALLOWED = {
    "runtime/kage_vita_gxm_rt_census.h",
    "runtime/kage_vita_phase_profile.c",
    "vita/host_tests/gxm_rt_census_oracle.c",
}


class Failure(Exception):
    pass


def require(condition: bool, message: str) -> None:
    if not condition:
        raise Failure(message)


def tool(name: str) -> str:
    found = shutil.which(name)
    if found:
        return found
    candidate = (Path(os.environ.get("ProgramFiles", r"C:\Program Files")) /
                 "LLVM" / "bin" / (name + ".exe"))
    require(candidate.is_file(), f"no {name} found")
    return str(candidate)


def strip_blocks(text: str) -> tuple[str, dict[str, int]]:
    """Remove whole blocks opened at column 0 by one of GUARDS."""
    result = []
    depth = 0
    blocks = {guard: 0 for guard in GUARDS}
    for line in text.splitlines(keepends=True):
        if not depth:
            opened = [g for g in GUARDS if line.rstrip("\n") == g]
            if opened:
                depth = 1
                blocks[opened[0]] += 1
                continue
        if depth:
            if re.match(r"#\s*if", line):
                depth += 1
            elif re.match(r"#\s*endif", line):
                depth -= 1
            continue
        result.append(line)
    require(depth == 0, "unterminated knob block")
    return "".join(result), blocks


ADMIT_BEHIND_GUARD = re.compile(
    r"\s*#\s*if\s+defined\(ISAAC_VITA_GL_THREAD_ADMIT_BEHIND\)")
# Outermost GL admit behind blocks in kage_vita_phase_profile.c (the count
# test_kage_vita_fullspeed_scheduler.py verify_admit_behind_off_reading
# prints for this file).
ADMIT_BEHIND_BLOCKS = 4


def strip_admit_behind_blocks(text: str) -> tuple[str, int]:
    """Remove whole GL admit behind blocks, guard at any indentation; an
    #else/#elif arm of one is refused rather than guessed at."""
    result = []
    depth = 0
    blocks = 0
    for line in text.splitlines(keepends=True):
        if not depth:
            if ADMIT_BEHIND_GUARD.match(line):
                depth = 1
                blocks += 1
                continue
            result.append(line)
            continue
        if re.match(r"\s*#\s*if", line):
            depth += 1
        elif re.match(r"\s*#\s*endif", line):
            depth -= 1
        elif depth == 1 and re.match(r"\s*#\s*(else|elif)", line):
            raise Failure("#else on a GL admit behind block is not modelled")
    require(depth == 0, "unterminated GL admit behind block")
    return "".join(result), blocks


def base_text(base: str, name: str) -> str:
    return subprocess.check_output(
        ["git", "show", f"{base}:recomp/{name}"], cwd=ROOT).decode("utf-8")


def check_sources(base: str) -> None:
    resolved = subprocess.run(
        ["git", "rev-parse", "--verify", "--quiet", f"{base}^{{commit}}"],
        cwd=ROOT, stdout=subprocess.PIPE)
    require(resolved.returncode == 0, f"--base {base} is not a commit")
    current = (ROOT / PROFILE).read_bytes().decode("utf-8")
    require("\r" not in current, f"{PROFILE}: CR line ending")
    # ISAAC_VITA_GL_THREAD_ADMIT_BEHIND, merged onto this file at 7d23e50,
    # lives in whole blocks of its own (some indented): taken out first, the
    # count pinned.  Its own OFF proof, against 66ce1c8, is
    # vita/test_kage_vita_fullspeed_scheduler.py verify_admit_behind_off_reading.
    current, admit = strip_admit_behind_blocks(current)
    require(admit == ADMIT_BEHIND_BLOCKS,
            f"{PROFILE}: {admit} GL admit behind blocks, not "
            f"{ADMIT_BEHIND_BLOCKS}")
    stripped, blocks = strip_blocks(current)
    require(blocks[GUARDS[0]] == 2,
            f"{PROFILE}: {blocks[GUARDS[0]]} census blocks, expected 2")
    require(stripped == base_text(base, PROFILE),
            f"{PROFILE}: differs from {base} outside the knob blocks")
    leak = IDENTIFIERS.search(stripped)
    require(leak is None, f"{PROFILE}: census identifier outside its block")

    named = set()
    for directory in (RUNTIME, VITA):
        for path in directory.rglob("*"):
            if path.suffix not in (".c", ".h", ".cpp", ".hpp", ".inc"):
                continue
            if "build" in path.parts or not path.is_file():
                continue
            text = path.read_bytes().decode("utf-8", errors="replace")
            if IDENTIFIERS.search(text):
                named.add(path.relative_to(ROOT).as_posix())
    require(named == ALLOWED,
            f"census identifiers in unexpected files: {sorted(named ^ ALLOWED)}")

    cmake = (VITA / "CMakeLists.txt").read_bytes().decode("utf-8")
    require(len(re.findall(rf"^option\({KNOB}\n[^\n]*\" OFF\)$", cmake,
                           re.M)) == 1, "CMake: option is not one OFF option")
    require(len(re.findall(
        rf"^if\({KNOB} AND\n\s+\(ISAAC_VITA_SCAFFOLD_ONLY OR NOT ISAAC_VITA_KAGE"
        rf" OR\n\s+NOT ISAAC_VITA_PHASE_PROFILE\)\)\n\s+message\(FATAL_ERROR",
        cmake, re.M)) == 1, "CMake: refusal without the phase profile")
    require(cmake.count(f"{KNOB}=1") == 1 and re.search(
        r"set_property\(SOURCE\n\s+\"\$\{ISAAC_RUNTIME\}/"
        r"kage_vita_phase_profile\.c\"\n\s+APPEND PROPERTY COMPILE_DEFINITIONS"
        rf"\n\s+{KNOB}=1\)", cmake) is not None,
            "CMake: the define is not on kage_vita_phase_profile.c alone")
    require(cmake.count("-Wl,--wrap=sceGxmCreateRenderTarget") == 1 and
            cmake.count("-Wl,--wrap=sceGxmDestroyRenderTarget") == 1,
            "CMake: the two --wrap link options")
    numstat = subprocess.check_output(
        ["git", "diff", "--numstat", base, "--", "vita/CMakeLists.txt"],
        cwd=ROOT, text=True).split()
    require(len(numstat) >= 2 and numstat[1] == "0",
            f"CMake: lines deleted against {base}: {numstat}")
    print(f"sources: PASS (2 census blocks; {PROFILE} == {base} outside the "
          f"knob blocks; {len(ALLOWED)} files name the knob; CMake "
          f"+{numstat[0]} -0)")


PROFILE_FLAGS = [
    "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
    "-DISAAC_KAGE_VITA_PHASE_PROFILE_ORACLE=1",
    "-DISAAC_VITA_PHASE_PROFILE=1",
    "-DISAAC_VITA_PHASE_PROFILE_BUILD_ID=\"gxm:census\"",
]
if os.name == "nt":
    # clang's COFF writer stamps the object with the time of day otherwise
    # (measured: two compiles of one file differed in header byte 5 only).
    PROFILE_FLAGS.append("-mno-incremental-linker-compatible")


def compile_profile(cc: str, source: Path, out: Path, extra: list[str],
                    cwd: Path) -> None:
    # The source is named relative to cwd so that the object carries no
    # absolute path of the directory it was built in.
    done = subprocess.run(
        [cc, *PROFILE_FLAGS, *extra, f"-I{RUNTIME}", f"-I{VITA}", "-c",
         source.name, "-o", str(out)], cwd=cwd, text=True,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    require(done.returncode == 0,
            f"compile {source.name} {extra}: rc {done.returncode}\n"
            f"{done.stdout[-3000:]}")


def symbols(nm: str, obj: Path) -> dict[str, str]:
    out = subprocess.check_output([nm, str(obj)], text=True)
    table = {}
    for line in out.splitlines():
        parts = line.split()
        if len(parts) >= 2:
            table[parts[-1]] = parts[-2]
    return table


def kind(table: dict[str, str], name: str) -> str:
    # i386 objects spell C symbols with one more leading underscore.
    return table.get(name) or table.get("_" + name) or ""


def check_objects(cc: str, base: str, work: Path) -> None:
    nm = tool("llvm-nm")
    name = Path(PROFILE).name
    now_dir, base_dir = work / "now", work / "base"
    for directory in (now_dir, base_dir):
        directory.mkdir()
    shutil.copyfile(ROOT / PROFILE, now_dir / name)
    (base_dir / name).write_bytes(base_text(base, PROFILE).encode("utf-8"))
    for directory in (now_dir, base_dir):
        compile_profile(cc, directory / name, directory / "off.o", [],
                        directory)
    off_now = (now_dir / "off.o").read_bytes()
    require(off_now == (base_dir / "off.o").read_bytes(),
            f"OFF object differs from {base}")
    off = symbols(nm, now_dir / "off.o")
    require(not any("sceGxm" in key or "gxm_rt_census" in key for key in off),
            "OFF object names a census symbol")
    print(f"off: PASS ({name} without the knob == {base}, "
          f"{len(off_now)} bytes, no census symbol)")

    variants = (
        ("knob", []),
        ("knob+worker", ["-DISAAC_VITA_GL_THREAD_WORKER=1"]),
        ("knob+worker+deferred-attach",
         ["-DISAAC_VITA_GL_THREAD_WORKER=1",
          "-DISAAC_VITA_FBO_DEFERRED_ATTACH=1"]),
    )
    for label, extra in variants:
        obj = now_dir / f"{label}.o"
        compile_profile(cc, now_dir / name, obj, [f"-D{KNOB}=1", *extra],
                        now_dir)
        table = symbols(nm, obj)
        for function in ("sceGxmCreateRenderTarget",
                         "sceGxmDestroyRenderTarget"):
            require(kind(table, "__wrap_" + function) == "T",
                    f"{label}: __wrap_{function} is not defined")
            require(kind(table, "__real_" + function) == "U",
                    f"{label}: __real_{function} is not an undefined reference")
            require(not kind(table, function),
                    f"{label}: direct reference to {function}")
        raw = obj.read_bytes()
        # The comparison of leg 2 can see a difference at all.
        require(raw != off_now, f"{label}: object equals the OFF object")
        require(raw.count(b"ph120.gr bid=") == 1 and
                raw.count(b"KAGE VITA GXM RT CREATE CENSUS: on") == 1,
                f"{label}: record or receipt literal is not there once")
    print(f"on: PASS ({len(variants)} configurations compile with -Werror; "
          "__wrap_ defined, __real_ undefined, record and receipt literal once)")


# name -> (anchor, replacement) in kage_vita_gxm_rt_census.h
MUTANTS = {
    # a refused create is counted as a successful one
    "failed-as-created": ("    unsigned slot = rc < 0 ? 1u : 0u;\n",
                          "    unsigned slot = rc < 0 ? 0u : 0u;\n"),
    # a positive return value is counted as a failure
    "positive-as-failed": ("    unsigned slot = rc < 0 ? 1u : 0u;\n",
                           "    unsigned slot = rc != 0 ? 1u : 0u;\n"),
    # the take reads the create counter without zeroing it
    "take-keeps-count": (
        "    out->created =\n        __atomic_exchange_n("
        "&s_gxm_rt_census_window[0], 0u, __ATOMIC_RELAXED);\n",
        "    out->created =\n        __atomic_load_n("
        "&s_gxm_rt_census_window[0], __ATOMIC_RELAXED);\n"),
    # the maximum is carried into the next window
    "max-carried": (
        "    out->create_max_us =\n        __atomic_exchange_n("
        "&s_gxm_rt_census_window[3], 0u, __ATOMIC_RELAXED);\n",
        "    out->create_max_us =\n        __atomic_load_n("
        "&s_gxm_rt_census_window[3], __ATOMIC_RELAXED);\n"),
    # the maximum is the last call, not the longest
    "max-is-last": (
        "    if (spent > __atomic_load_n(&s_gxm_rt_census_window[3], "
        "__ATOMIC_RELAXED))\n", "    if (spent)\n"),
    # a 64-bit duration is truncated, not clamped
    "no-clamp": ("    return us > 0xffffffffull ? 0xffffffffu : (uint32_t)us;\n",
                 "    return (uint32_t)us;\n"),
    # the create wrapper swallows the real return value
    "create-rc-dropped": (
        "    kage_vita_gxm_rt_census_note_create(\n"
        "        rc, sceKernelGetProcessTimeWide() - start);\n    return rc;\n",
        "    kage_vita_gxm_rt_census_note_create(\n"
        "        rc, sceKernelGetProcessTimeWide() - start);\n    return 0;\n"),
    # the destroy wrapper swallows the real return value
    "destroy-rc-dropped": (
        "    kage_vita_gxm_rt_census_note_destroy(\n"
        "        sceKernelGetProcessTimeWide() - start);\n    return rc;\n",
        "    kage_vita_gxm_rt_census_note_destroy(\n"
        "        sceKernelGetProcessTimeWide() - start);\n    return rc - rc;\n"),
    # the destroy wrapper counts but never calls the real function
    "destroy-not-forwarded": (
        "    int rc = __real_sceGxmDestroyRenderTarget(target);\n",
        "    int rc = 0;\n    (void)target;\n"),
    # destroys are missing from the totals since start
    "destroy-not-seen": (
        "    __atomic_fetch_add(&s_gxm_rt_census_seen[2], 1u, "
        "__ATOMIC_RELAXED);\n", ""),
    # the clock is read once: every call costs 0
    "no-time": (
        "    kage_vita_gxm_rt_census_note_create(\n"
        "        rc, sceKernelGetProcessTimeWide() - start);\n",
        "    kage_vita_gxm_rt_census_note_create(rc, start - start);\n"),
}


def build_oracle(cc: str, include: Path, out: Path) -> subprocess.CompletedProcess:
    return subprocess.run(
        [cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", f"-I{include}",
         str(VITA / "host_tests" / "gxm_rt_census_oracle.c"), "-o", str(out)],
        text=True, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)


def check_oracle(cc: str, work: Path) -> None:
    suffix = ".exe" if os.name == "nt" else ""
    exe = work / ("oracle" + suffix)
    built = build_oracle(cc, RUNTIME, exe)
    require(built.returncode == 0, f"oracle build:\n{built.stdout[-3000:]}")
    ran = subprocess.run([str(exe)], text=True, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT)
    require(ran.returncode == 0 and
            "gxm rt census oracle: PASS" in ran.stdout,
            f"oracle: rc {ran.returncode}\n{ran.stdout}")
    print("oracle: " + ran.stdout.strip().splitlines()[-1])

    header = (RUNTIME / HEADER).read_bytes().decode("utf-8")
    for name, (anchor, replacement) in MUTANTS.items():
        require(header.count(anchor) == 1, f"mutant anchor: {name}")
        directory = work / name
        directory.mkdir()
        (directory / HEADER).write_bytes(
            header.replace(anchor, replacement).encode("utf-8"))
        mutant = directory / ("oracle" + suffix)
        built = build_oracle(cc, directory, mutant)
        require(built.returncode == 0,
                f"mutant {name} does not build:\n{built.stdout[-3000:]}")
        ran = subprocess.run([str(mutant)], text=True, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT)
        require(ran.returncode != 0 and "FAIL" in ran.stdout,
                f"mutant {name} SURVIVED:\n{ran.stdout}")
        first = next(line for line in ran.stdout.splitlines()
                     if line.startswith("FAIL"))
        print(f"mutant {name}: rejected ({first.strip()})")
    print(f"mutants: PASS ({len(MUTANTS)} rejected)")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", default="22327fa",
                        help="revision the lane started from")
    parser.add_argument("--skip", action="append", default=[],
                        choices=("sources", "objects", "oracle"))
    args = parser.parse_args()
    try:
        cc = tool("clang")
        if "sources" not in args.skip:
            check_sources(args.base)
        with tempfile.TemporaryDirectory(prefix="isaac-gxm-census-") as tmp:
            work = Path(tmp)
            if "objects" not in args.skip:
                check_objects(cc, args.base, work)
            if "oracle" not in args.skip:
                check_oracle(cc, work)
    except Failure as failure:
        print(f"FAIL: {failure}")
        return 1
    print("test_gxm_rt_create_census: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
