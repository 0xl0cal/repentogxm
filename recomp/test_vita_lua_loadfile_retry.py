#!/usr/bin/env python3
"""ISAAC_VITA_LUA_LOADFILE_RETRY: host oracle, mutants, CMake negatives and
source pins.

usage: python recomp/test_vita_lua_loadfile_retry.py [--lua-source LUA-5.3.3]
           [--work DIR] [--skip-mutants] [--skip-cmake]
(--work defaults to a new temporary directory)

MSVC x86 builds of pristine Lua 5.3.3 with lauxlib.c's FILE calls routed
through runtime/vita_lua_loadfile_fault.h (a newlib-shaped stdio over a
fault-injecting fake sceIo kernel).  GNU ld --wrap is simulated by renaming
at compile time: lauxlib.c gets /DluaL_loadfilex=__real_luaL_loadfilex,
lbaselib.c, loadlib.c and the oracle main /DluaL_loadfilex=__wrap_luaL_loadfilex;
the OFF build links lauxlib unrenamed with no wrapper.  Builds: off, on3
(MAX=3), on0 (MAX=0), probe (MAX=3 + IO_PROBE + IO_INJECT=f1.lua@1), and
six mutants of on3 that must each fail a check.
"""

from __future__ import annotations

import argparse
import hashlib
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
RUNTIME = HERE / "runtime"
VITA = HERE / "vita"
TU = RUNTIME / "host_vita_lua_loadfile.c"
ORACLE = RUNTIME / "vita_lua_loadfile_retry_oracle.c"
FAULT = RUNTIME / "vita_lua_loadfile_fault.h"
CMAKELISTS = VITA / "CMakeLists.txt"
RELEASE = REPO / "release" / "v0.1.1-alpha.cmake"
BASE_COMMIT = "49859637d816488c428cb478aeac113768b860b9"
sys.path.insert(0, str(HERE))

import test_vita_lua_runtime as lua_runtime  # noqa: E402

CL = ["cl", "/nologo", "/std:c11", "/O2", "/W4", "/WX", "/MT", "/wd4310",
      "/D_CRT_SECURE_NO_WARNINGS"]
LUA_CL = ["cl", "/nologo", "/std:c11", "/O2", "/W3", "/MT",
          "/D_CRT_SECURE_NO_WARNINGS"]
WRAPPED = ("lauxlib.c", "lbaselib.c", "loadlib.c")
GROUPS = ("clean", "transient", "persistent", "budget", "open", "changed",
          "callers", "stdin", "unit")
COUNTERS = ("loads", "read_err", "open_transient", "open_err", "isdir",
            "retries", "recovered", "unrecovered", "changed", "observed",
            "budget_hit", "sleep_us", "logged")
# Spec 3.5 (1), copied.
BANNER_MAX3 = (
    "KAGE VITA LUA LOADFILE STATS: why=first-load max=3 "
    "delay_us=2000,16000,128000 budget=16/1000000 probe=0 inject=- loads=0 "
    "read_err=0 open_transient=0 open_err=0 isdir=0 retries=0 recovered=0 "
    "unrecovered=0 changed=0 observed=0 budget_hit=0 sleep_us=0 logged=0")
STATS = re.compile(
    r"^\[log\] KAGE VITA LUA LOADFILE STATS: why=(\S+) max=([0-3]) "
    r"delay_us=(\S+) budget=(\d+)/(\d+) probe=([01]) inject=(\S+) "
    + " ".join(rf"{name}=(\d+)" for name in COUNTERS) + "$")
# Spec 3.5 (3).
EVENT = re.compile(
    r"^\[log\] KAGE VITA LUA LOADFILE RETRY: n=(?P<n>\d+) "
    r"result=(?P<result>recovered|unrecovered|changed|observed|budget|isdir|open-error) "
    r"kind=(?P<kind>read|open|other) attempts=(?P<attempts>\d+) "
    r"status=(?P<status>-?\d+) fail_us=(?P<fail_us>\d+) "
    r"retry_us=(?P<retry_us>\d+) sce=(?P<sce>0x[0-9a-f]{8}|none|-) "
    r"sce_at=(?P<sce_at>\S+) sce_len=(?P<sce_len>\S+) sce_us=(?P<sce_us>\S+) "
    r"close=(?P<close>0x[0-9a-f]{8}|-) inj=(?P<inj>[01]) "
    r"path=(?P<path>.*?) why=(?P<why>.*)$")
CASE = re.compile(r"^case (\S+) fired=(\d+) opens=(\d+) delays=(\S+) :: (.*)$")
HEADER = re.compile(r"^oracle bufsiz=(\d+) eio=(\d+) enodev=(\d+) strerror0=(.*)$")

# (old, new) on host_vita_lua_loadfile.c; every one must fail a check.
MUTANTS = (
    ("read-prefix",
     ['static const char prefix_read[] = "cannot read ";'],
     ['static const char prefix_read[] = "cannot open ";']),
    ("no-pop",
     ["        lua_pop(L, 1); /* the failed attempt's single error string */\n"],
     [""]),
    ("loop-bound",
     ["for (attempt = 1U; attempt <= max; ++attempt) {"],
     ["for (attempt = 1U; attempt < max; ++attempt) {"]),
    ("swap-recovered",
     ["        result = LF_RECOVERED;\n        ++s_lf.recovered;",
      "        result = LF_UNRECOVERED;\n        ++s_lf.unrecovered;"],
     ["        result = LF_UNRECOVERED;\n        ++s_lf.unrecovered;",
      "        result = LF_RECOVERED;\n        ++s_lf.recovered;"]),
    ("no-errno-restore",
     ["    lf_arm_leave(&saved_arm);\n    errno = saved_errno;\n    return status;"],
     ["    lf_arm_leave(&saved_arm);\n    return status;"]),
    ("no-delay-cap",
     ["    while (--attempt != 0U && delay < LF_DELAY_CAP_US)",
      "    return delay < LF_DELAY_CAP_US ? delay : LF_DELAY_CAP_US;"],
     ["    while (--attempt != 0U)",
      "    return delay;"]),
)
# Same shape, built as the probe oracle and checked by probe_build: a wrapper
# frame equal to the dead marker must clear the stale arm.
PROBE_MUTANTS = (
    ("stale-gt",
     ["        (uintptr_t)frame >= s_arm.marker) {"],
     ["        (uintptr_t)frame > s_arm.marker) {"]),
)
# The probe group's stale-arm lines, exact (probe and probe0 builds alike).
STALE_PCALL = "stale pcall=4 arm_depth=1 stale_clears=0 live=0"
STALE_AFTER = "stale after arm_depth=0 stale_clears=1"
STALE_SAME_DEPTH = ("stale same-depth first=-4 arm_depth=1 second=0 arm_depth=0 "
                    "stale_clears=2 live=0")


def fail(message: str) -> None:
    raise SystemExit(f"FAIL: {message}")


def require(condition: bool, message: str) -> None:
    if not condition:
        fail(message)


q = lua_runtime.quote


def msvc(vcvars: Path, commands: list[str], cwd: Path, log: Path) -> None:
    line = f"call {q(vcvars)} x86 >nul && " + " && ".join(commands)
    with log.open("a", encoding="utf-8", errors="replace") as out:
        done = subprocess.run(line, cwd=cwd, shell=True, stdout=out,
                              stderr=subprocess.STDOUT)
    require(done.returncode == 0, f"MSVC step failed, see {log}")


# --------------------------------------------------------------------------
# Pins
# --------------------------------------------------------------------------
def source_pins(lua_src: Path) -> None:
    manifest = dict(lua_runtime.records())
    lauxlib = lua_src / "src" / "lauxlib.c"
    digest = hashlib.sha256(lauxlib.read_bytes()).hexdigest()
    require(digest == manifest["lauxlib.c"],
            f"lauxlib.c sha256 {digest} != lua53_vanilla.cmake pin")
    text = lauxlib.read_text(encoding="latin-1")
    for needle in ('return errfile(L, "read", fnameindex);',
                   'lua_pushfstring(L, "cannot %s %s: %s", what, filename, serr);',
                   'return errfile(L, "open", fnameindex);',
                   'return errfile(L, "reopen", fnameindex);'):
        require(text.count(needle) == 1, f"lauxlib.c lost {needle!r}")
    cmake = CMAKELISTS.read_text(encoding="utf-8").splitlines()
    wraps = [i for i, line in enumerate(cmake) if "-Wl,--wrap=luaL_loadfilex" in line]
    require(len(wraps) == 1, f"-Wl,--wrap=luaL_loadfilex appears {len(wraps)} times")
    depth, inside = 0, False
    for line in cmake[:wraps[0]]:
        stripped = line.strip()
        if re.match(r"if\s*\(ISAAC_VITA_LUA_LOADFILE_RETRY\)", stripped):
            depth, inside = 1, True
        elif inside and re.match(r"if\s*\(", stripped):
            depth += 1
        elif inside and re.match(r"endif\s*\(", stripped):
            depth -= 1
            if depth == 0:
                inside = False
    require(inside, "-Wl,--wrap=luaL_loadfilex is not inside if(ISAAC_VITA_LUA_LOADFILE_RETRY)")
    sce_wraps = [i for i, line in enumerate(cmake)
                 if re.search(r"--wrap=sceIo(Open|Read|Close)\b", line)]
    probe_block = [i for i, line in enumerate(cmake)
                   if line.strip() == "if(ISAAC_VITA_LUA_LOADFILE_IO_PROBE)"]
    require(len(probe_block) == 1, "if(ISAAC_VITA_LUA_LOADFILE_IO_PROBE) block not unique")
    mine = [i for i in sce_wraps if probe_block[0] < i < probe_block[0] + 25]
    require(len(mine) == 3, f"probe sceIo wraps not found in the probe block: {mine}")
    defs = [line for line in cmake if "ISAAC_VITA_LUA_LOADFILE_RETRY=1" in line]
    require(len(defs) == 1, f"RETRY=1 define lines: {defs}")
    print("source pins: PASS (lauxlib sha256 == lua53_vanilla.cmake:25, errfile "
          "texts present once, --wrap=luaL_loadfilex only inside "
          "if(ISAAC_VITA_LUA_LOADFILE_RETRY), sceIo wraps only in the probe block)")


def release_and_gate_pins() -> None:
    text = RELEASE.read_text(encoding="utf-8")
    for line in ('set(ISAAC_VITA_LUA_LOADFILE_RETRY OFF CACHE BOOL "")',
                 'set(ISAAC_VITA_LUA_LOADFILE_IO_PROBE OFF CACHE BOOL "")',
                 'set(ISAAC_VITA_LUA_LOADFILE_IO_INJECT "" CACHE STRING "")',
                 'set(ISAAC_VITA_LUA_LOADFILE_RETRY_MAX 3 CACHE STRING "")'):
        require(text.count(line) == 1, f"release recipe lacks {line}")
    gate = (VITA / "vita_raw_allocator_gate.py").read_text(encoding="utf-8")
    require(re.search(r'if _feature\(cache, "ISAAC_VITA_LUA_LOADFILE_RETRY"\):\n'
                      r'\s+runtime\.add\("host_vita_lua_loadfile\.c"\)', gate) is not None,
            "vita_raw_allocator_gate.py does not register host_vita_lua_loadfile.c")
    print("release/gate pins: PASS (v0.1.1-alpha pins RETRY/IO_PROBE OFF, "
          "IO_INJECT empty, MAX 3; allocator gate registers the TU)")


# --------------------------------------------------------------------------
# CMake negatives (knob block through vita/cmake_knob_probe.py)
# --------------------------------------------------------------------------
def fatal_line(cmake: list[str], message: str) -> int:
    for index, line in enumerate(cmake):
        if message in line:
            for back in range(index, max(index - 4, -1), -1):
                if "message(FATAL_ERROR" in cmake[back]:
                    return back + 1
    fail(f"FATAL text not in CMakeLists.txt: {message}")
    return 0


def cmake_negatives(work: Path) -> None:
    cmake = CMAKELISTS.read_text(encoding="utf-8").splitlines()
    init = work / "cmake-init.cmake"
    init.write_text("# empty init cache\n", encoding="utf-8")
    lua_on = ["ISAAC_VITA_LUA=ON", "ISAAC_VITA_LUA_LOADFILE_RETRY=ON"]
    cases = [
        ("retry-without-lua", ["ISAAC_VITA_LUA_LOADFILE_RETRY=ON"],
         "ISAAC_VITA_LUA_LOADFILE_RETRY requires ISAAC_VITA_LUA=ON"),
        ("max-4", lua_on + ["ISAAC_VITA_LUA_LOADFILE_RETRY_MAX=4"],
         "ISAAC_VITA_LUA_LOADFILE_RETRY_MAX must be one of 0, 1, 2, 3"),
        ("max-minus-1", lua_on + ["ISAAC_VITA_LUA_LOADFILE_RETRY_MAX=-1"],
         "ISAAC_VITA_LUA_LOADFILE_RETRY_MAX must be one of 0, 1, 2, 3"),
        ("max-x", lua_on + ["ISAAC_VITA_LUA_LOADFILE_RETRY_MAX=x"],
         "ISAAC_VITA_LUA_LOADFILE_RETRY_MAX must be one of 0, 1, 2, 3"),
        ("probe-without-retry", ["ISAAC_VITA_LUA=ON",
                                 "ISAAC_VITA_LUA_LOADFILE_IO_PROBE=ON"],
         "ISAAC_VITA_LUA_LOADFILE_IO_PROBE requires ISAAC_VITA_LUA_LOADFILE_RETRY=ON"),
        ("inject-without-probe",
         lua_on + ["ISAAC_VITA_LUA_LOADFILE_IO_INJECT=descriptions/ab+/cs_cz.lua@1"],
         "ISAAC_VITA_LUA_LOADFILE_IO_INJECT requires ISAAC_VITA_LUA_LOADFILE_IO_PROBE=ON"),
        ("inject-no-count",
         lua_on + ["ISAAC_VITA_LUA_LOADFILE_IO_PROBE=ON",
                   "ISAAC_VITA_LUA_LOADFILE_IO_INJECT=cs_cz.lua"],
         "ISAAC_VITA_LUA_LOADFILE_IO_INJECT must be <path-suffix>@<n>"),
        ("inject-zero",
         lua_on + ["ISAAC_VITA_LUA_LOADFILE_IO_PROBE=ON",
                   "ISAAC_VITA_LUA_LOADFILE_IO_INJECT=cs_cz.lua@0"],
         "ISAAC_VITA_LUA_LOADFILE_IO_INJECT must be <path-suffix>@<n>"),
        ("probe-with-io-profile",
         lua_on + ["ISAAC_VITA_LUA_LOADFILE_IO_PROBE=ON", "ISAAC_VITA_IO_PROFILE=ON"],
         "ISAAC_VITA_LUA_LOADFILE_IO_PROBE and ISAAC_VITA_IO_PROFILE/ISAAC_VITA_IO_WINDOW_PROFILE"),
        ("probe-with-io-window-profile",
         lua_on + ["ISAAC_VITA_LUA_LOADFILE_IO_PROBE=ON",
                   "ISAAC_VITA_IO_WINDOW_PROFILE=ON"],
         "ISAAC_VITA_LUA_LOADFILE_IO_PROBE and ISAAC_VITA_IO_PROFILE/ISAAC_VITA_IO_WINDOW_PROFILE"),
    ]
    positives = [
        ("default", []),
        ("on-max3", lua_on + ["ISAAC_VITA_LUA_LOADFILE_RETRY_MAX=3"]),
        ("on-max0", lua_on + ["ISAAC_VITA_LUA_LOADFILE_RETRY_MAX=0"]),
        ("on-probe-inject",
         lua_on + ["ISAAC_VITA_LUA_LOADFILE_IO_PROBE=ON",
                   "ISAAC_VITA_LUA_LOADFILE_IO_INJECT=descriptions/ab+/cs_cz.lua@1"]),
    ]

    def probe(name: str, defs: list[str]) -> tuple[int, str]:
        build = work / "cmake" / name
        if build.exists():
            shutil.rmtree(build)
        build.mkdir(parents=True)
        argv = [sys.executable, str(VITA / "cmake_knob_probe.py"), "-C", str(init),
                "-S", str(VITA), "-B", str(build)]
        for item in defs:
            argv += ["-D", item]
        done = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                              text=True, errors="replace")
        (build / "probe.log").write_text(done.stdout, encoding="utf-8")
        return done.returncode, done.stdout

    for name, defs, message in cases:
        code, out = probe(name, defs)
        # CMake wraps a long message over indented lines up to a blank line.
        errors = re.findall(r"CMake Error at (\S+):(\d+) \(message\):\n((?:  .*\n)+)", out)
        require(code != 0 and errors, f"cmake negative {name} did not fail:\n{out}")
        where, line, body = errors[0]
        text = " ".join(part.strip() for part in body.splitlines())
        want = fatal_line(cmake, message)
        require(text.startswith(message) and int(line) == want
                and where.endswith("CMakeLists.txt"),
                f"cmake negative {name}: first FATAL is line {line} {text!r}, "
                f"expected line {want} {message!r}")
        print(f"cmake negative {name}: PASS (CMakeLists.txt:{line} {text})")
    for name, defs in positives:
        code, out = probe(name, defs)
        require(code == 0 and "isaac knob probe: CMakeLists.txt lines" in out,
                f"cmake positive {name} failed:\n{out}")
    print(f"cmake positives: PASS ({', '.join(name for name, _ in positives)})")


# --------------------------------------------------------------------------
# Builds
# --------------------------------------------------------------------------
class Toolchain:
    def __init__(self, vcvars: Path, work: Path, lua_src: Path):
        self.vcvars = vcvars
        self.work = work
        self.lua_src = lua_src
        self.lua_inc = lua_src / "src"
        sources = lua_runtime.validate_source(lua_src, lua_runtime.records())
        self.shared = [p for p in sources if p.name not in WRAPPED]
        self.wrapped = {p.name: p for p in sources if p.name in WRAPPED}
        require(len(self.wrapped) == 3, "lauxlib.c/lbaselib.c/loadlib.c missing")

    def lua(self) -> None:
        shared = self.work / "lua-shared"
        shared.mkdir(parents=True, exist_ok=True)
        msvc(self.vcvars, [" ".join(LUA_CL + [f"/I{q(self.lua_inc)}", "/c"]
                                    + [q(p) for p in self.shared])],
             shared, shared / "build.log")
        for mode in ("off", "on"):
            out = self.work / f"lua-{mode}"
            out.mkdir(parents=True, exist_ok=True)
            commands = []
            for name, path in self.wrapped.items():
                defines = []
                if name == "lauxlib.c":
                    defines = ["/DVITA_LUA_LOADFILE_FAULT_STDIO=1", f"/FI{q(FAULT)}"]
                    if mode == "on":
                        defines.append("/DluaL_loadfilex=__real_luaL_loadfilex")
                elif mode == "on":
                    defines.append("/DluaL_loadfilex=__wrap_luaL_loadfilex")
                commands.append(" ".join(LUA_CL + defines + [f"/I{q(self.lua_inc)}",
                                                             "/c", q(path)]))
            msvc(self.vcvars, commands, out, out / "build.log")

    def exe(self, name: str, mode: str, tu: Path | None,
            tu_defines: list[str], oracle_defines: list[str]) -> Path:
        out = self.work / name
        if out.exists():
            shutil.rmtree(out)
        out.mkdir(parents=True)
        includes = [f"/I{q(RUNTIME)}", f"/I{q(self.lua_inc)}"]
        commands = []
        objects = [q(self.work / "lua-shared" / f"{p.stem}.obj") for p in self.shared]
        objects += [q(self.work / f"lua-{mode}" / f"{Path(n).stem}.obj")
                    for n in WRAPPED]
        if tu is not None:
            commands.append(" ".join(CL + tu_defines + includes + ["/c", q(tu)]))
            objects.append(f"{tu.stem}.obj")
        commands.append(" ".join(CL + oracle_defines + includes + ["/c", q(ORACLE)]))
        objects.append(f"{ORACLE.stem}.obj")
        exe = out / f"{name}.exe"
        commands.append(" ".join(["link", "/nologo", f"/out:{q(exe)}", *objects]))
        msvc(self.vcvars, commands, out, out / "build.log")
        warnings = [line for line in (out / "build.log").read_text(
            encoding="utf-8", errors="replace").splitlines() if "warning" in line]
        require(not warnings, f"{name}: warnings: {warnings}")
        return exe


def on_defines(max_retry: int, probe: bool) -> tuple[list[str], list[str]]:
    common = ["/DISAAC_VITA_LUA_LOADFILE_RETRY=1", "/DISAAC_VITA_LUA_LOADFILE_ORACLE=1"]
    tu = common + [f"/DISAAC_VITA_LUA_LOADFILE_RETRY_MAX={max_retry}U"]
    oracle = common + ["/DluaL_loadfilex=__wrap_luaL_loadfilex"]
    if probe:
        tu += ["/DISAAC_VITA_LUA_LOADFILE_IO_PROBE=1",
               q('/DISAAC_VITA_LUA_LOADFILE_IO_INJECT_SUFFIX="f1.lua"'),
               "/DISAAC_VITA_LUA_LOADFILE_IO_INJECT_READ=1U"]
        oracle += ["/DISAAC_VITA_LUA_LOADFILE_IO_PROBE=1"]
    return tu, oracle


# --------------------------------------------------------------------------
# Runs and parsing
# --------------------------------------------------------------------------
class Run:
    def __init__(self, text: str, label: str):
        self.label = label
        self.text = text
        self.cases: dict[str, tuple[int, int, str, str]] = {}
        self.order: list[str] = []
        self.stats: dict[str, dict[str, int]] = {}
        self.stats_order: list[str] = []
        self.stats_head: dict[str, tuple[str, ...]] = {}
        self.events: dict[str, list[dict[str, str]]] = {}
        self.banners: list[str] = []
        self.lines: list[str] = []
        self.header: tuple[int, int, int, str] | None = None
        self.bad: list[str] = []
        pending: list[dict[str, str]] = []
        for line in text.splitlines():
            header = HEADER.match(line)
            if header:
                self.header = (int(header.group(1)), int(header.group(2)),
                               int(header.group(3)), header.group(4))
                continue
            case = CASE.match(line)
            if case:
                self.cases[case.group(1)] = (int(case.group(2)), int(case.group(3)),
                                             case.group(4), case.group(5))
                self.order.append(case.group(1))
                continue
            stats = STATS.match(line)
            if stats:
                why = stats.group(1)
                counters = dict(zip(COUNTERS, (int(v) for v in stats.groups()[7:])))
                if why == "first-load":
                    self.banners.append(line[len("[log] "):])
                    continue
                self.stats[why] = counters
                self.stats_order.append(why)
                self.stats_head[why] = stats.groups()[1:7]
                self.events[why] = pending
                pending = []
                continue
            event = EVENT.match(line)
            if event:
                pending.append(event.groupdict())
                continue
            if line.startswith("[log]") or line.startswith("FAIL"):
                self.bad.append(line)
                continue
            self.lines.append(line)
        if pending:
            self.bad.append(f"{len(pending)} RETRY lines after the last STATS line")

    def result(self, case_id: str) -> str:
        return self.cases[case_id][3]

    def delta(self, case_id: str) -> dict[str, int]:
        index = self.stats_order.index(case_id)
        now = self.stats[case_id]
        before = (self.stats[self.stats_order[index - 1]] if index
                  else dict.fromkeys(COUNTERS, 0))
        return {k: now[k] - before[k] for k in COUNTERS}

    def line(self, prefix: str) -> str:
        found = [line for line in self.lines if line.startswith(prefix)]
        return found[-1] if found else ""


def run_groups(exe: Path, fixtures: Path, groups: tuple[str, ...],
               label: str) -> dict[str, Run]:
    runs = {}
    for group in groups:
        done = subprocess.run([str(exe), fixtures.as_posix(), group],
                              stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE, text=True, errors="replace")
        text = done.stdout.replace("\r\n", "\n")
        if done.returncode != 0 or done.stderr:
            runs[group] = Run(text + f"\nFAIL exit {done.returncode} {done.stderr}", label)
        else:
            runs[group] = Run(text, label)
        (exe.parent / f"{group}.out").write_text(text, encoding="utf-8")
    return runs


def field(result: str, name: str) -> str:
    match = re.search(rf"(?:^| ){name}=(\S*)", result)
    return match.group(1) if match else ""


def msg(result: str) -> str:
    return result.split(" msg=", 1)[1] if " msg=" in result else ""


# --------------------------------------------------------------------------
# Checks
# --------------------------------------------------------------------------
class Checks:
    def __init__(self, label: str, fixtures: Path, off: dict[str, Run]):
        self.label = label
        self.errors: list[str] = []
        self.fixtures = fixtures.as_posix()
        self.off = off
        header = off["clean"].header
        require(header is not None, "OFF oracle printed no header")
        self.bufsiz, self.eio, self.enodev, self.strerror0 = header

    def expect(self, condition: bool, message: str) -> None:
        if not condition:
            self.errors.append(f"{self.label}: {message}")

    def path(self, name: str) -> str:
        return f"{self.fixtures}/{name}"

    def no_bad(self, runs: dict[str, Run]) -> None:
        for group, run in runs.items():
            self.expect(not run.bad, f"{group}: unexpected lines {run.bad[:3]}")

    def same(self, on: Run, on_id: str, off: Run, off_id: str, what: str) -> None:
        if on_id not in on.cases or off_id not in off.cases:
            self.expect(False, f"{what}: case {on_id}/{off_id} missing")
            return
        self.expect(on.result(on_id) == off.result(off_id),
                    f"{what}: {on_id} differs from OFF {off_id}:\n"
                    f"  on:  {on.result(on_id)}\n  off: {off.result(off_id)}")

    def event(self, run: Run, case_id: str, **want: str) -> None:
        events = run.events.get(case_id, [])
        if len(events) != 1:
            self.expect(False, f"{case_id}: {len(events)} RETRY lines, expected 1")
            return
        for key, value in want.items():
            got = events[0][key]
            ok = got.startswith(value[:-1]) if value.endswith("*") else got == value
            self.expect(ok, f"{case_id}: RETRY {key}={got}, expected {value}")

    def counters(self, run: Run, case_id: str, **want: int) -> None:
        if case_id not in run.stats:
            self.expect(False, f"{case_id}: no STATS line")
            return
        delta = run.delta(case_id)
        full = dict.fromkeys(COUNTERS, 0)
        full.update(want)
        if "logged" not in want:
            full["logged"] = delta["logged"]
        self.expect(delta == full, f"{case_id}: counter delta {delta} != {full}")

    # -- OFF facts (the reference) --
    def off_facts(self) -> None:
        clean = self.off["clean"]
        for name in ("f1.lua", "f2.lua", "f3.lua", "f4.luac"):
            result = clean.result(f"clean/{name}")
            self.expect(field(result, "status") == "0" and field(result, "delta") == "1",
                        f"OFF clean/{name}: {result}")
            self.expect(field(result, "ret") == field(clean.result("clean/f1.lua"), "ret"),
                        f"OFF clean/{name} returns differ from f1")
        self.expect(field(clean.result("clean/f1.lua"), "source") == "@" + self.path("f1.lua"),
                    "OFF clean/f1.lua source is not @path")
        self.expect(field(clean.result("clean/f5.lua"), "status") == "3",
                    "OFF clean/f5.lua is not LUA_ERRSYNTAX")
        self.expect(field(clean.result("clean/f6.lua"), "status") == "0",
                    "OFF clean/f6.lua is not LUA_OK")
        transient = self.off["transient"]
        for case_id in transient.order:
            name = case_id.split("/", 1)[1].rsplit("@", 1)[0]
            fired, _, _, result = transient.cases[case_id]
            self.expect(fired == 1, f"OFF {case_id}: fault fired {fired} times")
            self.expect(field(result, "status") == "7" and field(result, "delta") == "1"
                        and msg(result) == f"cannot read {self.path(name)}: {self.strerror0}",
                        f"OFF {case_id}: {result}")
        for name in ("f1.lua", "f2.lua", "f3.lua", "f4.luac"):
            kset = transient.line(f"kset {name} ")
            sizes = re.match(r"kset \S+ size=(\d+) (.*)$", kset)
            self.expect(sizes is not None, f"OFF kset {name} missing")
            if sizes:
                size = int(sizes.group(1))
                ks = sorted({0, 1, 2, 3, self.bufsiz - 1, self.bufsiz, self.bufsiz + 1,
                             2 * self.bufsiz + 7, size - 1, size})
                self.expect([int(k) for k in sizes.group(2).split()] == ks,
                            f"OFF kset {name}: {kset}")
                if name != "f4.luac":
                    self.expect(size > 3 * self.bufsiz, f"{name} is not > 3*BUFSIZ")
                ids = [i for i in transient.order if i.startswith(f"transient/{name}@")]
                self.expect(len(ids) == len(ks), f"OFF transient {name}: {len(ids)} cases")
        persistent = self.off["persistent"].result("persistent/f1.lua")
        self.expect(msg(persistent).startswith("cannot read "), f"OFF persistent: {persistent}")
        opens = self.off["open"]
        self.expect(msg(opens.result("open/missing")).startswith(
            f"cannot open {self.path('missing.lua')}: "), "OFF open/missing")
        self.expect(msg(opens.result("open/reopen-f4")).startswith(
            f"cannot reopen {self.path('f4.luac')}: "), "OFF open/reopen-f4")
        self.expect(msg(opens.result("open/isdir")) ==
                    f"cannot read {self.path('f7dir')}: {self.strerror0}", "OFF open/isdir")
        callers = self.off["callers"]
        for name in ("loadfile", "dofile", "require"):
            self.expect(callers.result(f"callers/{name}-clean") !=
                        callers.result(f"callers/{name}-fault"),
                        f"OFF callers/{name}: the fault did not change the result")
            self.expect(callers.cases[f"callers/{name}-fault"][0] == 1,
                        f"OFF callers/{name}-fault: fault did not fire once")

    # -- ON builds --
    def on_build(self, runs: dict[str, Run], max_retry: int) -> None:
        off = self.off
        self.no_bad(runs)
        clean = runs["clean"]
        # Fault-free identity, one banner, loads=6 and nothing else.
        for case_id in off["clean"].order:
            self.same(clean, case_id, off["clean"], case_id, "clean")
        self.expect(len(clean.banners) == 1, f"clean: {len(clean.banners)} banners")
        if max_retry == 3:
            self.expect(clean.banners == [BANNER_MAX3],
                        f"banner text differs from spec 3.5 (1): {clean.banners}")
        last = clean.stats.get("clean/f6.lua", {})
        want = dict.fromkeys(COUNTERS, 0)
        want["loads"] = 6
        self.expect(last == want, f"clean: STATS {last} != {want}")
        self.expect(all(not v for v in clean.events.values()), "clean: RETRY lines")

        # Transient read fault at byte k.
        transient = runs["transient"]
        for index, case_id in enumerate(off["transient"].order):
            name = case_id.split("/", 1)[1].rsplit("@", 1)[0]
            if max_retry:
                self.same(transient, case_id, off["clean"], f"clean/{name}", "transient")
                self.counters(transient, case_id, loads=1, read_err=1, retries=1,
                              recovered=1, sleep_us=2000)
                if index < 8:
                    self.event(transient, case_id, result="recovered", kind="read",
                               attempts="2", status="0", sce="-", inj="0",
                               why=self.strerror0, path=self.path(name))
            else:
                self.same(transient, case_id, off["transient"], case_id, "transient MAX=0")
                self.counters(transient, case_id, loads=1, read_err=1, observed=1)
                if index < 8:
                    self.event(transient, case_id, result="observed", attempts="1",
                               status="7")
        self.expect(len(transient.order) == len(off["transient"].order),
                    "transient: case count differs from OFF")

        # Persistent fault: same message after the bounded retries.
        persistent = runs["persistent"]
        cid = "persistent/f1.lua"
        self.same(persistent, cid, off["persistent"], cid, "persistent")
        if max_retry:
            self.expect(persistent.cases.get(cid, (0, 0, ""))[2] == "2000,16000,128000",
                        f"persistent delays {persistent.cases.get(cid)}")
            self.expect(persistent.cases.get(cid, (0,))[0] == 4, "persistent: 4 attempts did not fire 4 faults")
            self.event(persistent, cid, result="unrecovered", kind="read", attempts="4",
                       status="7")
            self.counters(persistent, cid, loads=1, read_err=1, retries=3, unrecovered=1,
                          sleep_us=146000)
        else:
            self.event(persistent, cid, result="observed", attempts="1")
            self.counters(persistent, cid, loads=1, read_err=1, observed=1)

        # Open, reopen, directory.
        opens = runs["open"]
        for cid in ("open/missing", "open/eacces", "open/reopen-f4", "open/isdir"):
            self.same(opens, cid, off["open"], cid, "open")
        self.event(opens, "open/missing", result="open-error", kind="open", attempts="1")
        self.counters(opens, "open/missing", loads=1, open_err=1)
        self.event(opens, "open/eacces", result="open-error", kind="open", attempts="1")
        self.counters(opens, "open/eacces", loads=1, open_err=1)
        self.event(opens, "open/reopen-f4", result="open-error", kind="other", attempts="1")
        self.counters(opens, "open/reopen-f4", loads=1, open_err=1)
        self.event(opens, "open/isdir", result="isdir", kind="read", attempts="1",
                   why=self.strerror0)
        self.counters(opens, "open/isdir", loads=1, isdir=1)
        if max_retry:
            self.same(opens, "open/transient-eio", off["clean"], "clean/f1.lua", "open")
            self.event(opens, "open/transient-eio", result="recovered", kind="open",
                       attempts="2")
            self.counters(opens, "open/transient-eio", loads=1, open_transient=1,
                          retries=1, recovered=1, sleep_us=2000)
            self.same(opens, "open/persistent-eio", off["open"], "open/persistent-eio", "open")
            self.event(opens, "open/persistent-eio", result="unrecovered", kind="open",
                       attempts="2")
            self.counters(opens, "open/persistent-eio", loads=1, open_transient=1,
                          retries=1, unrecovered=1, sleep_us=2000)
        else:
            for cid in ("open/transient-eio", "open/persistent-eio"):
                self.same(opens, cid, off["open"], cid, "open MAX=0")
                self.event(opens, cid, result="observed", kind="open", attempts="1")
                self.counters(opens, cid, loads=1, open_transient=1, observed=1)

        # changed: a retry that ends in LUA_ERRSYNTAX or "cannot reopen".
        changed = runs["changed"]
        if max_retry:
            result = changed.result("changed/syntax")
            self.expect(field(result, "status") == "3" and field(result, "delta") == "1",
                        f"changed/syntax: {result}")
            self.event(changed, "changed/syntax", result="changed", kind="read",
                       attempts="2", status="3")
            self.counters(changed, "changed/syntax", loads=1, read_err=1, retries=1,
                          changed=1, sleep_us=2000)
            result = changed.result("changed/reopen")
            self.expect(field(result, "status") == "7" and
                        msg(result).startswith(f"cannot reopen {self.path('f4.luac')}: "),
                        f"changed/reopen: {result}")
            self.event(changed, "changed/reopen", result="changed", kind="read",
                       attempts="2", status="7")
            self.counters(changed, "changed/reopen", loads=1, read_err=1, retries=1,
                          changed=1, sleep_us=2000)

        # Native callers recover (coverage; loadfile/dofile are nil in production).
        callers = runs["callers"]
        for name in ("loadfile", "dofile", "require"):
            self.same(callers, f"callers/{name}-clean", off["callers"],
                      f"callers/{name}-clean", "callers")
            if max_retry:
                self.same(callers, f"callers/{name}-fault", off["callers"],
                          f"callers/{name}-clean", "callers")
                self.counters(callers, f"callers/{name}-fault", loads=1, read_err=1,
                              retries=1, recovered=1, sleep_us=2000)
                self.event(callers, f"callers/{name}-fault", result="recovered",
                           attempts="2")
            self.counters(callers, f"callers/{name}-clean", loads=1)

        # stdin: pure pass-through.
        stdin = runs["stdin"]
        self.same(stdin, "stdin/null", off["stdin"], "stdin/null", "stdin")
        self.counters(stdin, "stdin/null", loads=1)
        self.expect(not stdin.events.get("stdin/null"), "stdin: RETRY line")

        # Budget: 16 retries / 1 s per boot.
        if max_retry == 3:
            budget = runs["budget"]
            results, attempts = [], []
            for load in range(1, 8):
                cid = f"budget/{load}"
                self.same(budget, cid, off["budget"], cid, "budget")
                events = budget.events.get(cid, [])
                results.append(events[0]["result"] if events else "?")
                attempts.append(events[0]["attempts"] if events else "?")
            self.expect(results == ["unrecovered"] * 5 + ["budget", "budget"],
                        f"budget results {results}")
            self.expect(attempts == ["4"] * 5 + ["2", "1"], f"budget attempts {attempts}")
            final = budget.stats.get("budget/7", {})
            self.expect(final.get("retries") == 16 and final.get("sleep_us") == 732000
                        and final.get("budget_hit") == 2 and final.get("unrecovered") == 5,
                        f"budget STATS {final}")
            total = re.match(r"budget sleep_total=(\d+)$", budget.line("budget sleep_total="))
            self.expect(total is not None and int(total.group(1)) == 732000
                        and int(total.group(1)) <= 1000000, f"budget sleep total {total}")

        # Delay schedule, capped.
        if max_retry:
            self.expect(runs["unit"].line("unit delay_us=") ==
                        "unit delay_us=2000,16000,128000,128000,128000,128000,128000,128000",
                        f"unit: {runs['unit'].line('unit delay_us=')}")

    def probe_build(self, runs: dict[str, Run]) -> None:
        probe = runs["probe"]
        self.no_bad(runs)
        self.expect(len(probe.banners) == 1 and " probe=1 inject=f1.lua@1 " in probe.banners[0],
                    f"probe banner: {probe.banners}")
        self.same(probe, "probe/inject", self.off["clean"], "clean/f1.lua", "probe")
        sce_eio = f"0x{0x80010000 | self.eio:08x}"
        sce_enodev = f"0x{0x80010000 | self.enodev:08x}"
        self.event(probe, "probe/inject", result="recovered", kind="read", attempts="2",
                   sce=sce_eio, sce_at="1/*", sce_len=str(self.bufsiz), sce_us="0",
                   close="0x00000000", inj="1", why=self.strerror0)
        self.same(probe, "probe/kernel-enodev", self.off["clean"], "clean/f2.lua", "probe")
        self.event(probe, "probe/kernel-enodev", result="recovered", kind="read",
                   attempts="2", sce=sce_enodev, close="0x00000000", inj="0",
                   why=self.strerror0)
        self.event(probe, "probe/isdir", result="isdir", sce="none", sce_at="0/0",
                   sce_len="0", close="-", inj="0")
        self.stale_lines(probe)

    def stale_lines(self, probe: Run) -> None:
        self.expect(probe.line("stale pcall=") == STALE_PCALL,
                    f"stale: {probe.line('stale pcall=')}")
        self.expect(probe.line("stale after") == STALE_AFTER,
                    f"stale after: {probe.line('stale after')}")
        self.expect(probe.line("stale same-depth") == STALE_SAME_DEPTH,
                    f"stale same-depth: {probe.line('stale same-depth')}")

    def probe0_build(self, runs: dict[str, Run]) -> None:
        """Device check D-A's shape: MAX=0 + probe + inject.  The injected
        read is observed, not retried, and the caller sees exactly what OFF
        returns for a read error at byte 0 (status, message, errno)."""
        probe = runs["probe"]
        self.no_bad(runs)
        self.expect(len(probe.banners) == 1 and
                    " max=0 delay_us=- " in probe.banners[0] and
                    " probe=1 inject=f1.lua@1 " in probe.banners[0],
                    f"probe0 banner: {probe.banners}")
        self.same(probe, "probe/inject", self.off["transient"],
                  "transient/f1.lua@0", "probe0")
        sce_eio = f"0x{0x80010000 | self.eio:08x}"
        self.event(probe, "probe/inject", result="observed", kind="read",
                   attempts="1", status="7", sce=sce_eio,
                   sce_at="1/*", sce_len=str(self.bufsiz), sce_us="0",
                   close="0x00000000", inj="1", why=self.strerror0)
        self.stale_lines(probe)


def write_fixtures(fixtures: Path) -> None:
    if fixtures.exists():
        shutil.rmtree(fixtures)
    fixtures.mkdir(parents=True)
    lines = ["-- F1: plain text, larger than 3*BUFSIZ; a cut inside the table",
             "-- constructor is a syntax error", "local t = {"]
    lines += [f'  "value{index:03d}",' for index in range(1, 201)]
    lines += ["}", "return #t, t[1], t[#t]", ""]
    f1 = "\n".join(lines).encode("ascii")
    (fixtures / "f1.lua").write_bytes(f1)
    (fixtures / "f2.lua").write_bytes(b"\xef\xbb\xbf" + f1)
    (fixtures / "f3.lua").write_bytes(b"#!/usr/bin/env lua\n" + f1)
    (fixtures / "f5.lua").write_bytes(b"local x = = 1\n")
    (fixtures / "f6.lua").write_bytes(b"")
    (fixtures / "f7dir").mkdir()


def oracle_matrix(vcvars: Path, work: Path, lua_src: Path, mutants: bool) -> None:
    tools = Toolchain(vcvars, work, lua_src)
    tools.lua()
    fixtures = work / "fixtures"
    write_fixtures(fixtures)
    off_exe = tools.exe("off", "off", None, [], [])
    prep = run_groups(off_exe, fixtures, ("prep",), "off")["prep"]
    require(not prep.bad and prep.line("prep f4.luac size="),
            f"prep failed: {prep.text}")
    off = run_groups(off_exe, fixtures, GROUPS, "off")
    for group, run in off.items():
        require(not run.bad and not run.banners and not run.stats,
                f"OFF {group}: wrapper output or failures: {run.bad[:3]}")
    checks = Checks("off", fixtures, off)
    checks.off_facts()
    require(not checks.errors, "\n".join(checks.errors))
    total = sum(len(run.order) for run in off.values())
    print(f"OFF oracle: PASS ({total} cases; every transient fault fired once and "
          f"gave 'cannot read <path>: {checks.strerror0}' with LUA_ERRFILE)")

    builds = {}
    for name, max_retry, probe in (("on3", 3, False), ("on0", 0, False),
                                   ("probe", 3, True), ("probe0", 0, True)):
        tu, oracle = on_defines(max_retry, probe)
        builds[name] = tools.exe(name, "on", TU, tu, oracle)
    for name, max_retry in (("on3", 3), ("on0", 0)):
        runs = run_groups(builds[name], fixtures, GROUPS, name)
        checks = Checks(name, fixtures, off)
        checks.on_build(runs, max_retry)
        require(not checks.errors, "\n".join(checks.errors[:20]))
        transient = runs["transient"]
        print(f"{name}: PASS ({sum(len(r.order) for r in runs.values())} cases; "
              f"clean identical to OFF; transient {len(transient.order)} cases "
              + ("recovered to the clean chunk/chunkname" if max_retry else
                 "observed, identical to OFF")
              + f"; persistent: {runs['persistent'].events['persistent/f1.lua'][0]['result']} "
              f"attempts={runs['persistent'].events['persistent/f1.lua'][0]['attempts']})")
        if name == "on3":
            sample = transient.events[transient.order[0]][0]
            print("  receipt: KAGE VITA LUA LOADFILE RETRY: " + " ".join(
                f"{k}={v}" for k, v in sample.items()))
            stats = [line for line in runs["budget"].text.splitlines()
                     if "STATS: why=budget/7 " in line]
            print("  budget: " + (stats[0][len("[log] "):] if stats else "missing"))
    probe_runs = run_groups(builds["probe"], fixtures, ("probe",), "probe")
    checks = Checks("probe", fixtures, off)
    checks.probe_build(probe_runs)
    require(not checks.errors, "\n".join(checks.errors))
    for case_id in ("probe/inject", "probe/kernel-enodev", "probe/isdir"):
        event = probe_runs["probe"].events[case_id][0]
        print(f"probe {case_id}: PASS (result={event['result']} sce={event['sce']} "
              f"sce_at={event['sce_at']} sce_len={event['sce_len']} close={event['close']} "
              f"inj={event['inj']})")
    print("probe stale arm: PASS (" + probe_runs["probe"].line("stale pcall=") + "; "
          + probe_runs["probe"].line("stale after") + "; "
          + probe_runs["probe"].line("stale same-depth") + ")")
    probe0_runs = run_groups(builds["probe0"], fixtures, ("probe",), "probe0")
    checks = Checks("probe0", fixtures, off)
    checks.probe0_build(probe0_runs)
    require(not checks.errors, "\n".join(checks.errors))
    event = probe0_runs["probe"].events["probe/inject"][0]
    print("probe0 (D-A shape: MAX=0 + probe + inject) probe/inject: PASS "
          "(caller result identical to OFF transient/f1.lua@0; RETRY: "
          + " ".join(f"{k}={event[k]}" for k in ("result", "kind", "attempts", "status",
                                                 "retry_us", "sce", "sce_at", "sce_len",
                                                 "close", "inj", "why")) + ")")

    if not mutants:
        return
    source = TU.read_text(encoding="utf-8")
    killed = 0
    every = [(name, olds, news, False) for name, olds, news in MUTANTS]
    every += [(name, olds, news, True) for name, olds, news in PROBE_MUTANTS]
    for name, olds, news, probe in every:
        text = source
        for old in olds:
            require(text.count(old) == 1, f"mutant {name}: anchor not unique: {old!r}")
        # Apply all replacements simultaneously (the swap needs it).
        marks = [f"\x00{index}\x00" for index in range(len(olds))]
        for old, mark in zip(olds, marks):
            text = text.replace(old, mark)
        for mark, new in zip(marks, news):
            text = text.replace(mark, new)
        require(text != source, f"mutant {name}: no change")
        mutant_dir = work / f"mutant-src-{name}"
        mutant_dir.mkdir(exist_ok=True)
        path = mutant_dir / "host_vita_lua_loadfile.c"
        path.write_text(text, encoding="utf-8")
        tu, oracle = on_defines(3, probe)
        exe = tools.exe(f"mutant-{name}", "on", path, tu, oracle)
        checks = Checks(f"mutant-{name}", fixtures, off)
        if probe:
            checks.probe_build(run_groups(exe, fixtures, ("probe",), f"mutant-{name}"))
        else:
            checks.on_build(run_groups(exe, fixtures, GROUPS, f"mutant-{name}"), 3)
        require(checks.errors, f"mutant {name} survived")
        killed += 1
        print(f"mutant {name}: killed ({len(checks.errors)} failed checks; first: "
              f"{checks.errors[0].splitlines()[0]})")
    print(f"mutants: {killed}/{len(every)} killed")


# --------------------------------------------------------------------------
# OFF identity (kage_vita_loading.c is the only existing TU that changed)
# --------------------------------------------------------------------------
def off_identity(vcvars: Path, work: Path) -> None:
    if subprocess.run(["git", "cat-file", "-e", BASE_COMMIT + "^{commit}"],
                      cwd=HERE, capture_output=True).returncode != 0:
        print(f"OFF identity: SKIP (base commit {BASE_COMMIT[:7]} is not in this "
              "repository's history)")
        return
    base = work / "base_kage_vita_loading.c"
    blob = subprocess.run(["git", "show", f"{BASE_COMMIT}:recomp/runtime/kage_vita_loading.c"],
                          cwd=HERE, capture_output=True, check=True).stdout
    base.write_bytes(blob)
    # Same directory as the real file, so "..." includes resolve alike.
    local_base = RUNTIME / ".off-identity-base-kage_vita_loading.c"
    local_base.write_bytes(blob)
    try:
        sets = ("/DISAAC_KAGE_VITA_LOADING_ORACLE=1",
                "/DISAAC_KAGE_VITA_LOADING_ORACLE=1 /DISAAC_VITA_CRT_SEEK_SHADOW=1 "
                "/DISAAC_VITA_IMAGE_RETAIN=1")
        for index, defines in enumerate(sets):
            outputs = []
            for label, path in (("base", local_base), ("new", RUNTIME / "kage_vita_loading.c")):
                target = work / f"pp-{index}-{label}.i"
                msvc(vcvars, [" ".join(["cl", "/nologo", "/EP", defines,
                                        f"/I{q(RUNTIME)}", f"/I{q(VITA)}",
                                        q(path), ">", q(target)])],
                     work, work / "pp.log")
                outputs.append([line for line in target.read_bytes().splitlines()
                                if line.strip()])
            require(outputs[0] == outputs[1],
                    f"kage_vita_loading.c OFF preprocessing differs for set {index}")
            require(len(outputs[0]) > 200, f"preprocessed output too small ({len(outputs[0])})")
    finally:
        local_base.unlink(missing_ok=True)
    print(f"OFF identity: PASS (kage_vita_loading.c preprocesses to {BASE_COMMIT[:7]}'s "
          f"tokens for {len(sets)} define sets; only blank lines differ)")


def fault_header_guard(vcvars: Path, work: Path) -> None:
    """vita_lua_loadfile_fault.h is a host-oracle header: with __vita__ set
    the preprocessor must stop at its #error, and the same probe without it
    must compile (so the failure is the guard, not the probe)."""
    lines = FAULT.read_text(encoding="utf-8").splitlines()
    guard = [index + 1 for index, line in enumerate(lines)
             if line.startswith("#error") and "host-oracle header" in line]
    require(len(guard) == 1, f"fault header #error guard lines: {guard}")
    probe = work / "fault_guard_probe.c"
    probe.write_text('#include "vita_lua_loadfile_fault.h"\nint fault_guard_probe;\n',
                     encoding="ascii")
    results = {}
    for label, extra in (("host", []), ("vita", ["/D__vita__"])):
        log = work / f"fault-guard-{label}.log"
        line = (f"call {q(vcvars)} x86 >nul && "
                + " ".join(LUA_CL + extra + [f"/I{q(RUNTIME)}", "/Zs", q(probe)]))
        with log.open("w", encoding="utf-8", errors="replace") as out:
            done = subprocess.run(line, cwd=work, shell=True, stdout=out,
                                  stderr=subprocess.STDOUT)
        results[label] = (done.returncode,
                          log.read_text(encoding="utf-8", errors="replace"))
    require(results["host"][0] == 0, f"fault header host probe failed:\n{results['host'][1]}")
    code, text = results["vita"]
    where = f"vita_lua_loadfile_fault.h({guard[0]})"
    require(code != 0 and where in text and "C1189" in text,
            f"fault header with __vita__: rc={code}, expected the #error at {where}:\n{text}")
    print(f"fault header guard: PASS (host /Zs rc=0; /D__vita__ rc={code} at "
          f"{where} C1189 #error)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--lua-source", type=Path)
    parser.add_argument("--work", type=Path,
                        help="scratch directory (default: a new temporary directory)")
    parser.add_argument("--skip-mutants", action="store_true")
    parser.add_argument("--skip-cmake", action="store_true")
    args = parser.parse_args()
    work = (args.work or Path(tempfile.mkdtemp(prefix="isaac-lua-loadfile-retry-"))).resolve()
    if work.exists():
        shutil.rmtree(work)
    work.mkdir(parents=True)
    lua_src = lua_runtime.discover_source(args.lua_source)
    source_pins(lua_src)
    release_and_gate_pins()
    if not args.skip_cmake:
        cmake_negatives(work)
    vcvars = lua_runtime.find_vcvars()
    off_identity(vcvars, work)
    fault_header_guard(vcvars, work)
    oracle_matrix(vcvars, work, lua_src, not args.skip_mutants)
    print("PASS: test_vita_lua_loadfile_retry")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
