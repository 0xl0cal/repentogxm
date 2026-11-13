#!/usr/bin/env python3
"""cmake_knob_probe.py, and the Lua tranche-2 FATAL negatives it was written for.

CMake stops at the first message(FATAL_ERROR).  A build-host negative
("configure must fail with M") from an init cache that keeps other dependent
levers ON stops at an earlier FATAL and FAILs on correct code: the
ISAAC_VITA_LUA_INTEGER2STR negative once stopped at
"ISAAC_VITA_LUA_CALL_ERROR_CATCH requires ISAAC_VITA_LUA=ON".  This test:

1. checks the probe: an empty init cache passes the knob block, and a FATAL is
   reported at the CMakeLists.txt line of its message(FATAL_ERROR) command,
   under the real path;
2. reproduces the defect: the negatives with the original key lists stop at the earlier FATAL (CALL_ERROR_CATCH; with S8 in the
   base, BINDING_REPLAY);
3. checks the corrected negatives (verbatim key lists below): the first FATAL is the knob's own, at its own
   line, and the positive control (only the knob under test flipped back)
   passes the whole knob block;
4. does 2 and 3 over four bases: recipe A's Lua levers, the same plus S8
   (ISAAC_VITA_LUA_BINDING_REPLAY + _VERIFY ON), plus the lua-gc lever GC_ON
   (GC_FINALIZER_PACE ON with its allocation latch), and plus both.

The base is MODELLED by default: the Lua keys of the int2j device build's
init cache (plus the four runtime keys S8 requires) and the Lua keys
recipe A adds (its COMMON/PROFILE/LEVERS_ON/GC_HOLD groups), copied below.
--init-cache FILE replaces the model with a real -C file (for example the
init.cmake of an ARM build; give --path-map for its EXISTS checks).
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

VITA = Path(__file__).resolve().parent
PROBE = VITA / "cmake_knob_probe.py"
CMAKE_LISTS = VITA / "CMakeLists.txt"

# Lua keys of the int2j device build's init cache
INIT_LUA = {
    "ISAAC_VITA_LUA": "ON",
    "ISAAC_VITA_LUA_ARENA_MB": "33",
    "ISAAC_VITA_LUA_GCCOLLECT_CLAMP": "ON",
    "ISAAC_VITA_LUA_GCCOLLECT_STEP_KB": "256",
    "ISAAC_VITA_LUA_GCSTEP_CLAMP_KB": "128",
    "ISAAC_VITA_LUA_GC_PROFILE": "OFF",
    "ISAAC_VITA_LUA_IMPORT_FASTDISPATCH": "ON",
    "ISAAC_VITA_LUA_NATIVE_GETCLASS": "ON",
    "ISAAC_VITA_LUA_NATIVE_GETCLASS_VERIFY": "OFF",
    "ISAAC_VITA_LUA_NATIVE_INDEX": "ON",
    "ISAAC_VITA_LUA_NATIVE_INDEX_VERIFY": "OFF",
    "ISAAC_VITA_LUA_OFF_CLOSE_NOOP": "OFF",
    "ISAAC_VITA_LUA_RECEIPT": "OFF",
    "ISAAC_VITA_LUA_SCOPE_FASTPATH": "ON",
    # not Lua keys: the runtime modes S8 requires (CMakeLists.txt
    # BINDING_REPLAY runtime FATAL), from the same init cache
    "ISAAC_VITA_GUEST_STACK_GUARD": "OFF",
    "ISAAC_VITA_TRANSLATED_CPU": "ON",
    "ISAAC_VITA_TRANSLATED_CPU_FLAGS_LOCAL": "ON",
    "ISAAC_VITA_TRANSLATED_CPU_GPR_LOCAL": "ON",
}
# Recipe A of the 2026-10-25 device build, its Lua keys in order (groups
# COMMON, PROFILE, LEVERS_ON, GC_HOLD)
RECIPE_A_LUA = [
    ("ISAAC_VITA_LUA_CALL_ERROR_CATCH", "ON"),
    ("ISAAC_VITA_LUA_COST_PROFILE", "ON"),
    ("ISAAC_VITA_LUA_GC_PROFILE", "ON"),
    ("ISAAC_VITA_LUA_CORE_ARM", "ON"),
    ("ISAAC_VITA_LUA_GC_FINALIZER_PACE", "OFF"),
    ("ISAAC_VITA_LUA_GC_SWEEP_COST", "5"),
    ("ISAAC_VITA_LUA_GCSTEP_CLAMP_KB", "128"),
]
# The S8 verify build of 2026-10-26
S8_VERIFY = [("ISAAC_VITA_LUA_BINDING_REPLAY", "ON"),
             ("ISAAC_VITA_LUA_BINDING_REPLAY_VERIFY", "ON")]
# The lua-gc lever as the r5 pacer takes it (r60-b1-lua-20261111 build
# PACE_ON).  int-final-20261025 build GC_ON (:98: SWEEP_COST 32, CLAMP_KB
# 40) no longer configures: r5 retired ISAAC_VITA_LUA_GC_SWEEP_COST and
# refuses any value but 5 (gc_on_retired_test below).  With the lever in the
# base, PACE=OFF alone trips "..._ALLOC_LATCH_BYTES takes effect only with
# ISAAC_VITA_LUA_GC_FINALIZER_PACE=ON" before INTEGER2STR's FATAL, hence the
# budget in the i2s-lua key list.
GC_ON = [("ISAAC_VITA_LUA_GC_FINALIZER_PACE", "ON"),
         ("ISAAC_VITA_LUA_GC_FINALIZER_ALLOC_LATCH_BYTES", "65536")]
GC_ON_RETIRED = [("ISAAC_VITA_LUA_GC_FINALIZER_PACE", "ON"),
                 ("ISAAC_VITA_LUA_GC_SWEEP_COST", "32"),
                 ("ISAAC_VITA_LUA_GCSTEP_CLAMP_KB", "40")]
MSG_RETIRED = "ISAAC_VITA_LUA_GC_SWEEP_COST is retired"

MSG_LGCD = "ISAAC_VITA_LUA_GETCLASS_DIRECT requires ISAAC_VITA_LUA_NATIVE_GETCLASS=ON"
MSG_I2S = "ISAAC_VITA_LUA_INTEGER2STR requires ISAAC_VITA_LUA=ON"
# The key lists the original negatives used: the defect.
ORIGINAL = {
    "lgcd-native": ["ISAAC_VITA_LUA_GETCLASS_DIRECT=ON", "ISAAC_VITA_LUA_NATIVE_GETCLASS=OFF"],
    "i2s-lua": ["ISAAC_VITA_LUA_INTEGER2STR=ON", "ISAAC_VITA_LUA=OFF"],
}
# The corrected negatives: every lever whose FATAL
# precedes the one under test and depends on what the negative removes.
FIXED = {
    "lgcd-native": (MSG_LGCD, "ISAAC_VITA_LUA_GETCLASS_DIRECT=OFF", [
        "ISAAC_VITA_LUA_GETCLASS_DIRECT=ON", "ISAAC_VITA_LUA_NATIVE_GETCLASS=OFF",
        "ISAAC_VITA_LUA_NATIVE_GETCLASS_VERIFY=OFF", "ISAAC_VITA_LUA_BINDING_REPLAY=OFF",
        "ISAAC_VITA_LUA_BINDING_REPLAY_VERIFY=OFF"]),
    "i2s-lua": (MSG_I2S, "ISAAC_VITA_LUA_INTEGER2STR=OFF", [
        "ISAAC_VITA_LUA_INTEGER2STR=ON", "ISAAC_VITA_LUA=OFF",
        "ISAAC_VITA_LUA_RECEIPT=OFF", "ISAAC_VITA_LUA_CALL_ERROR_CATCH=OFF",
        "ISAAC_VITA_LUA_NATIVE_INDEX=OFF", "ISAAC_VITA_LUA_NATIVE_INDEX_VERIFY=OFF",
        "ISAAC_VITA_LUA_ARENA_MB=0", "ISAAC_VITA_LUA_IMPORT_FASTDISPATCH=OFF",
        "ISAAC_VITA_LUA_GCCOLLECT_CLAMP=OFF", "ISAAC_VITA_LUA_GC_PROFILE=OFF",
        "ISAAC_VITA_LUA_CORE_ARM=OFF", "ISAAC_VITA_LUA_COST_PROFILE=OFF",
        "ISAAC_VITA_LUA_GC_FINALIZER_PACE=OFF", "ISAAC_VITA_LUA_GC_FINALIZER_MAX=8",
        "ISAAC_VITA_LUA_GC_FINALIZER_COST=512", "ISAAC_VITA_LUA_GC_SWEEP_COST=5",
        "ISAAC_VITA_LUA_GC_FINALIZER_ALLOC_LATCH_BYTES=0",
        "ISAAC_VITA_LUA_NATIVE_GETCLASS=OFF", "ISAAC_VITA_LUA_NATIVE_GETCLASS_VERIFY=OFF",
        "ISAAC_VITA_LUA_BINDING_REPLAY=OFF", "ISAAC_VITA_LUA_BINDING_REPLAY_VERIFY=OFF"]),
}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(f"FAIL: {message}")


def fatal_line(text: str) -> int:
    """CMakeLists.txt line of the message(FATAL_ERROR ...) whose text holds `text`."""
    lines = CMAKE_LISTS.read_text(encoding="utf-8").splitlines()
    hits = [i for i, line in enumerate(lines) if text in line]
    require(len(hits) == 1, f"message text {text!r} found on lines {[h + 1 for h in hits]}")
    for i in range(hits[0], max(hits[0] - 4, -1), -1):
        if "message(FATAL_ERROR" in lines[i]:
            return i + 1
    raise SystemExit(f"FAIL: no message(FATAL_ERROR above line {hits[0] + 1}")


def first_error(output: str) -> tuple[str, int, str] | None:
    """(file, line, whitespace-collapsed message) of the first CMake Error."""
    match = re.search(r"^CMake Error at (.+?):(\d+) \(message\):\n((?:  .*\n|\n)+?)(?:Call Stack|\S)",
                      output, re.M)
    if match is None:
        return None
    return match.group(1), int(match.group(2)), " ".join(match.group(3).split())


def probe(work: Path, init: Path, keys: list[str], path_map: list[str]) -> tuple[int, str]:
    args = [sys.executable, str(PROBE), "-C", str(init), "-S", str(VITA), "-B", str(work),
            "-G", "Ninja"]
    for key in keys:
        args += ["-D", key]
    for item in path_map:
        args += ["--path-map", item]
    done = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    return done.returncode, done.stdout.decode("utf-8", errors="replace").replace("\r\n", "\n")


def write_init(path: Path, pairs: list[tuple[str, str]]) -> Path:
    path.write_text("".join(f'set({k} "{v}" CACHE STRING "" FORCE)\n' for k, v in pairs),
                    encoding="utf-8", newline="\n")
    return path


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--init-cache", type=Path, default=None,
                        help="a real -C file to use as the base instead of the model")
    parser.add_argument("--path-map", action="append", default=[], metavar="OLD=NEW")
    args = parser.parse_args()
    real = CMAKE_LISTS.resolve().as_posix()
    line_lgcd, line_i2s = fatal_line(MSG_LGCD), fatal_line(MSG_I2S)
    line_cec = fatal_line("ISAAC_VITA_LUA_CALL_ERROR_CATCH requires ISAAC_VITA_LUA=ON")
    line_lbr = fatal_line("ISAAC_VITA_LUA_BINDING_REPLAY requires ISAAC_VITA_LUA_NATIVE_GETCLASS=ON")
    with tempfile.TemporaryDirectory(prefix="knob-probe-") as tmp:
        work = Path(tmp)
        # 1. fidelity
        empty = write_init(work / "empty.cmake", [])
        rc, out = probe(work / "p", empty, [], [])
        require(rc == 0 and "isaac knob probe: CMakeLists.txt lines" in out and
                first_error(out) is None, f"empty init does not pass the knob block:\n{out[-1500:]}")
        region = re.search(r"lines (\d+)\.\.(\d+) passed", out)
        require(region is not None and int(region.group(1)) < line_lbr and
                line_i2s < line_lgcd <= int(region.group(2)),
                f"knob region {region.group(0) if region else '?'} misses the Lua FATALs")
        rc, out = probe(work / "p", empty, ["ISAAC_VITA_LUA_INTEGER2STR=ON"], [])
        err = first_error(out)
        require(rc != 0 and err == (real, line_i2s, MSG_I2S),
                f"probe error report {err} != {(real, line_i2s, MSG_I2S)}")
        print(f"probe fidelity: empty init passes {region.group(0)}; "
              f"INTEGER2STR alone fails at CMakeLists.txt:{line_i2s} with its own message")
        # 2-4 over the bases
        if args.init_cache is not None:
            bases = {"init-cache": (args.init_cache, [])}
        else:
            model = write_init(work / "recipe-a-lua.cmake", list(INIT_LUA.items()) + RECIPE_A_LUA)
            bases = {"recipe-A-lua": (model, [])}
        name, (init, extra) = next(iter(bases.items()))
        s8v = [f"{k}={v}" for k, v in S8_VERIFY]
        gc_on = [f"{k}={v}" for k, v in GC_ON]
        bases[name + "+S8v"] = (init, s8v)
        bases[name + "+GC_ON"] = (init, gc_on)
        bases[name + "+S8v+GC_ON"] = (init, s8v + gc_on)
        for base, (init, extra) in bases.items():
            rc, out = probe(work / "p", init, extra, args.path_map)
            require(rc == 0 and first_error(out) is None,
                    f"[{base}] the base itself does not pass the knob block: {first_error(out)}")
            # 2. the defect, reproduced with the original key lists
            rc, out = probe(work / "p", init, extra + ORIGINAL["i2s-lua"], args.path_map)
            err = first_error(out)
            require(rc != 0 and err is not None and err[1] == line_cec and
                    "ISAAC_VITA_LUA_CALL_ERROR_CATCH requires" in err[2],
                    f"[{base}] original i2s-lua did not stop at CALL_ERROR_CATCH: {err}")
            rc, out = probe(work / "p", init, extra + ORIGINAL["lgcd-native"], args.path_map)
            err = first_error(out)
            if s8v[0] in extra:
                require(rc != 0 and err is not None and err[1] == line_lbr,
                        f"[{base}] original lgcd-native did not stop at BINDING_REPLAY: {err}")
            else:
                require(rc != 0 and err == (real, line_lgcd, MSG_LGCD),
                        f"[{base}] original lgcd-native: {err}")
            original = (f"original i2s-lua stops at :{line_cec} (CALL_ERROR_CATCH), "
                        f"original lgcd-native at :{err[1]}")
            # 3. the corrected negatives and their positive controls
            for neg, (msg, flip, keys) in FIXED.items():
                rc, out = probe(work / "p", init, extra + keys, args.path_map)
                err = first_error(out)
                want = (real, line_i2s if neg == "i2s-lua" else line_lgcd, msg)
                require(rc != 0 and err == want, f"[{base}] fixed {neg}: {err} != {want}")
                rc, out = probe(work / "p", init, extra + keys + [flip], args.path_map)
                require(rc == 0 and first_error(out) is None,
                        f"[{base}] {neg} control ({flip}) does not pass: {first_error(out)}")
            print(f"[{base}] {original}; fixed negatives stop at :{line_i2s}/:{line_lgcd} "
                  f"with their own messages; both controls pass the block")
        # 5. the frozen recipe's GC_ON no longer configures (r5 retired
        # SWEEP_COST); GC_HOLD, which every device recipe carries, does
        # (it is in the base above)
        name, (init, extra) = next(iter(bases.items()))
        rc, out = probe(work / "p", init,
                        [f"{k}={v}" for k, v in GC_ON_RETIRED], args.path_map)
        err = first_error(out)
        require(rc != 0 and err is not None and MSG_RETIRED in err[2],
                f"int-final GC_ON (SWEEP_COST 32) was not refused as retired: {err}")
        print(f"[{name}+int-final GC_ON] refused at :{err[1]}: {MSG_RETIRED}")
    print("cmake knob probe + Lua tranche-2 FATAL negatives: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
