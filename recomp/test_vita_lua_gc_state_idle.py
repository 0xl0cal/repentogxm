#!/usr/bin/env python3
"""Host proofs for ISAAC_VITA_LUA_GC_STATE_PROFILE and ISAAC_VITA_LUA_GC_IDLE_STEP.

Both knobs are default OFF.  What is proven here, on the host, against the
real Lua 5.3.3 with the lgc.c / lbaselib.c the Vita build derives:

 derivation   --state-profile changes exactly STATE_PROFILE_LINES of the
              derived lgc.c; without the flag the output is the pinned R5
              default byte for byte.
 state        (runtime/vita_lua_gc_state_idle_oracle.c, virtual clock)
              * neutrality: the collector's transcript (count, gcstate,
                finalizers per frame) and every non-time field of the
                "gc window" lines are those of the build without the marks;
              * accounting: per window the classes sum to the origins to
                the microsecond; the atomic crossings the unit attributes
                to the Update site and to allocation-driven steps are the
                ones an outside observer of G(L)->gcstate counts;
              * every atomic sub-phase is charged;
              * real clock: a heap with a large weak table shows its atomic
                in the `weak` sub-phase, the same heap held strongly does
                not -- the probe can tell a 13 ms step's cause apart.
 idle         scripted scheduler answers over a real collector: no step
              is lost, duplicated, split, merged or reordered; a step is
              carried only from a BUSY tail and only when it cannot run
              atomic() there (sweep, callfin, or propagate with the gray
              list proven longer than its budget); the carried step runs
              in the next LuaEngine::Update's LUA_GCCOUNT import, before
              the count; kb balances in every window, also across a
              window that closes with a step owed; UNKNOWN is the build
              without the knob.
 controller   LuaEngine::Update's own k controller (sub_004022d0, replayed
              from the disassembly) over 3600 ticks: with the knob its
              sum of k is within 1% of the build without the knob, with
              the recipe's step clamp and without one (not identical: a
              carried step runs on a heap one render older).
 mutants      twelve one-line defects in the derivation, the unit or the
              carry rule, each of which must be caught by the checks above.
 cmake        the knob block refuses each missing prerequisite with its own
              message (cmake_knob_probe.py) and admits the full combination.
 log lines    every new line fits isaac_vita_log's 384-byte body at the
              largest value of every field.
 scheduler    (runtime/kage_vita_gc_idle_scheduler_oracle.c) the real
              kage_vita_fullspeed_scheduler.c: with the knob compiled in and
              unused its transcript is the scheduler's without the knob; the
              30-Hz floor holds under every policy; the carry decision is
              the unit's (host_vita_lua_gc_idle_rule.h) and never moves
              the atomic step; prints the MODEL lines the report quotes.

Usage: python test_vita_lua_gc_state_idle.py [--lua-source DIR] [--cc CLANG]
           [--work DIR]
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
RUNTIME = ROOT / "runtime"
ORACLE = RUNTIME / "vita_lua_gc_state_idle_oracle.c"
ORACLE_GUEST = RUNTIME / "vita_lua_gc_state_idle_oracle_guest"
MODEL = RUNTIME / "kage_vita_gc_idle_scheduler_oracle.c"
UNIT = RUNTIME / "host_vita_lua_gc.c"
RULE = RUNTIME / "host_vita_lua_gc_idle_rule.h"
sys.path.insert(0, str(ROOT / "vita"))
import lua53_gc_finalizer_pace as pace  # noqa: E402

# test_vita_lua_gc_finalizer_pace.py DERIVED_SHA256: the R5 default output
DERIVED_SHA256 = (
    "34dd3a2d16975d0f1e59b1dcc20bedefa905d742b8907a2c675c7f7b6cd16516")
ALLOC_LATCH = 65536                      # the recipe's budget (G0)
LOG_BODY = 384                           # recomp/vita/platform.c:208
SEED = "0x20261112u"

PROFILE = "-DISAAC_VITA_LUA_GC_PROFILE=1"
STATE = "-DISAAC_VITA_LUA_GC_STATE_PROFILE=1"
IDLE = "-DISAAC_VITA_LUA_GC_IDLE_STEP=1"
CLOCK = "-DVITA_LUA_GC_ORACLE_CLOCK=1"


def fail(message: str) -> None:
    raise SystemExit(f"FAIL: {message}")


def need(ok: bool, message: str) -> None:
    if not ok:
        fail(message)


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


def run_status(cmd: list[str], cwd: Path) -> tuple[int, str]:
    done = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True)
    return done.returncode, done.stdout + done.stderr


# --- derivation --------------------------------------------------------------
def derivation_tests(pristine: bytes) -> tuple[bytes, bytes]:
    for name, data in (("pristine", pristine),
                       ("derived", pace.derive(pristine))):
        need(data.splitlines()[51] == b"#define STEPMULADJ\t\t200",
             f"{name} lgc.c:52 is not STEPMULADJ 200 "
             f"(VITA_LUA_GC_IDLE_STEPMULADJ copies it)")
    need(UNIT.read_text(encoding="utf-8").count(
        "#define VITA_LUA_GC_IDLE_STEPMULADJ 200\n") == 1,
         "the unit's copy of STEPMULADJ")
    need(hashlib.sha256(pace.derive(pristine)).hexdigest() == DERIVED_SHA256,
         "derive() without state_profile is not the pinned R5 default")
    off = pace.derive(pristine, alloc_latch=ALLOC_LATCH)
    on = pace.derive(pristine, alloc_latch=ALLOC_LATCH, state_profile=True)
    off_lines = off.splitlines(keepends=True)
    on_lines = on.splitlines(keepends=True)
    need(len(off_lines) == len(on_lines) ==
         len(pristine.splitlines(keepends=True)),
         "--state-profile changed the line count")
    changed = tuple(i + 1 for i, (a, b) in enumerate(zip(off_lines, on_lines))
                    if a != b)
    need(changed == tuple(sorted(pace.STATE_PROFILE_LINES)),
         f"--state-profile changed lines {changed}, "
         f"expected {sorted(pace.STATE_PROFILE_LINES)}")
    hooks = (b"isaac_lua_gc_state_begin", b"isaac_lua_gc_state_mark",
             b"isaac_lua_gc_state_end")
    for line in changed:
        text = on_lines[line - 1]
        need(any(hook in text for hook in hooks),
             f"lgc.c:{line} changed without a state hook")
    need(not any(hook in off for hook in hooks) and
         b"isaac_gcsp_cls" not in off,
         "state hooks in the derivation without the flag")
    # every mark id the unit knows is emitted, and no other
    ids = sorted({int(m) for m in re.findall(
        rb"isaac_lua_gc_state_mark\((\d+),", on)})
    need(ids == [8, 9, 10, 11, 12], f"literal mark ids {ids}")
    need(on.count(b"isaac_lua_gc_state_mark(isaac_gcsp_cls, 0)") == 1,
         "the gcstate mark")
    print(f"derivation: default sha {DERIVED_SHA256[:12]} unchanged; "
          f"--state-profile rewrites lgc.c lines "
          f"{','.join(map(str, changed))} only "
          f"(sha256 {hashlib.sha256(on).hexdigest()[:12]}, alloc latch "
          f"{ALLOC_LATCH})")
    return off, on


# --- builds ------------------------------------------------------------------
class Builder:
    def __init__(self, cc: Path, lua_src: Path, sources: list[Path],
                 pristine_baselib: bytes, work: Path) -> None:
        self.cc, self.lua_src, self.work = cc, lua_src, work
        common = work / "common"
        common.mkdir(parents=True, exist_ok=True)
        lstate = (lua_src / "lstate.c").read_bytes()
        old = b"  return luaS_hash(buff, p, h);\n}\n"
        need(lstate.count(old) == 1, "lstate.c: makeseed's return")
        # as test_vita_lua_gc_finalizer_pace.py build_exact: the seed alone,
        # no addresses, so two builds can be compared step for step
        (common / "lstate.c").write_bytes(lstate.replace(
            old, b"  (void)buff; (void)p; return h;\n}\n"))
        self.objects = []
        flags = ["-m32", "-O2", "-w", f"-I{lua_src}"]
        for source in sources:
            if source.name in ("lgc.c", "lbaselib.c", "lua.c", "luac.c"):
                continue
            obj = common / (source.stem + ".o")
            if source.name == "lstate.c":
                run([str(cc), *flags, f"-Dluai_makeseed()={SEED}", "-c",
                     str(common / "lstate.c"), "-o", str(obj)], work)
            else:
                run([str(cc), *flags, "-c", str(source), "-o", str(obj)],
                    work)
            self.objects.append(str(obj))
        self.baselib = common / "lbaselib.o"
        (common / "lbaselib.c").write_bytes(
            pace.derive_baselib(pristine_baselib))
        run([str(cc), *flags, "-c", str(common / "lbaselib.c"), "-o",
             str(self.baselib)], work)
        self.lgc: dict[str, Path] = {}

    def lgc_object(self, tag: str, data: bytes) -> Path:
        if tag not in self.lgc:
            directory = self.work / f"lgc-{tag}"
            directory.mkdir(parents=True, exist_ok=True)
            (directory / "lgc.c").write_bytes(data)
            obj = directory / "lgc.o"
            run([str(self.cc), "-m32", "-O2", "-w", f"-I{self.lua_src}",
                 "-c", str(directory / "lgc.c"), "-o", str(obj)], self.work)
            self.lgc[tag] = obj
        return self.lgc[tag]

    def oracle(self, name: str, lgc: Path, defines: list[str],
               unit_edit: tuple[Path, str, str] | None = None,
               expect_fail: bool = False) -> Path | None:
        directory = self.work / f"oracle-{name}"
        directory.mkdir(parents=True, exist_ok=True)
        source = ORACLE
        if unit_edit is not None:
            # a quoted include resolves next to the including file first:
            # the oracle's copy picks up the mutated unit beside it
            # (the unit includes the rule header by a quoted name too)
            for original in (UNIT, RULE):
                text = original.read_text(encoding="utf-8")
                if original == unit_edit[0]:
                    need(text.count(unit_edit[1]) == 1,
                         f"mutant {name}: anchor not found once: "
                         f"{unit_edit[1]!r}")
                    text = text.replace(unit_edit[1], unit_edit[2])
                (directory / original.name).write_text(
                    text, encoding="utf-8", newline="\n")
            shutil.copyfile(ORACLE, directory / ORACLE.name)
            shutil.copytree(ORACLE_GUEST, directory / ORACLE_GUEST.name,
                            dirs_exist_ok=True)
            source = directory / ORACLE.name
        obj = directory / "oracle.o"
        code, out = run_status(
            [str(self.cc), "-m32", "-std=gnu11", "-O2", "-Wall", "-Wextra",
             f"-I{self.lua_src}", f"-I{RUNTIME}", *defines, "-c",
             str(source), "-o", str(obj)], self.work)
        if expect_fail:
            return None if code else obj
        need(code == 0, f"oracle {name} did not compile:\n{out}")
        need("warning:" not in out,
             f"oracle {name}: clang -Wall -Wextra warnings:\n{out}")
        exe = directory / "oracle.exe"
        link = [str(self.cc), "-m32", str(obj), *self.objects,
                str(self.baselib), str(lgc), "-o", str(exe)]
        if os.name == "nt":
            link.insert(2, "-Wl,/DYNAMICBASE:NO")
        else:
            link += ["-lm"]
        run(link, self.work)
        return exe


def oracle_run(exe: Path, *mode: str) -> tuple[int, list[str]]:
    code, out = run_status([str(exe), *mode], exe.parent)
    return code, out.replace("\r\n", "\n").splitlines()


# --- state lines -------------------------------------------------------------
def fields(line: str) -> dict[str, list[int]]:
    """`name(...)=1,2 other=3` -> {"name": [1, 2], "other": [3]} (integers)."""
    found = {}
    for key, value in re.findall(r"([a-z_]+)(?:\([a-z_,]+\))?=(-?\d+(?:,-?\d+)*)"
                                 r"(?![>\d])", line):
        found[key] = [int(v) for v in value.split(",")]
    return found


TIME_FIELDS = re.compile(r"gcstep\(n,sum_us,p95_us,max_us\)=(\d+),\d+,\d+,\d+")
CLASSES = ("pause", "prop", "atomic", "sweep", "swpend", "callfin", "tailfin")


def state_problems(lines: list[str]) -> list[str]:
    """Every accounting defect in one state-virtual transcript."""
    problems = []
    ref = next((l for l in lines if l.startswith("REF ")), None)
    if ref is None:
        return ["no REF line: " + " | ".join(lines[-3:])]
    reference = fields(ref)
    state = [fields(l) for l in lines if "] gc state: " in l]
    cls = [fields(l) for l in lines if "] gc cls: " in l]
    asub = [fields(l) for l in lines if "] gc asub: " in l]
    worst = [l for l in lines if "] gc worst: " in l]
    windows = sum("] gc window: " in l for l in lines)
    if not (len(state) == len(cls) == len(asub) == len(worst) == windows
            == 40):
        return [f"{windows} windows, {len(state)} state, {len(cls)} cls, "
                f"{len(asub)} asub, {len(worst)} worst lines"]
    for c, a in zip(cls, asub):
        c["asub_us"] = a["us"]
    atomic = [0, 0, 0]
    subs = [0] * 5
    steps = 0
    for index, (s, c) in enumerate(zip(state, cls), 1):
        origins = sum(s[o][1] for o in ("upd", "alloc", "other"))
        classes = sum(c[k][1] for k in CLASSES)
        if origins != classes:
            problems.append(f"win {index}: origins {origins} us != classes "
                            f"{classes} us")
        if sum(c["asub_us"]) != c["atomic"][1]:
            problems.append(f"win {index}: atomic {c['atomic'][1]} us != its "
                            f"sub-phases {sum(c['asub_us'])}")
        if s["win"] != [index] or c["win"] != [index]:
            problems.append(f"win numbering at {index}")
        if s["dropped"] != [0]:
            problems.append(f"win {index}: dropped {s['dropped']}")
        for i in range(3):
            atomic[i] += s["atomic"][i]
        for i in range(5):
            subs[i] += c["asub_us"][i]
        steps += sum(s[o][0] for o in ("upd", "alloc", "other"))
        if sum(s["atomic"][:3]) != c["atomic"][0]:
            problems.append(f"win {index}: atomic steps by origin "
                            f"{s['atomic'][:3]} != class n {c['atomic'][0]}")
    if atomic[0] != reference["atomic_upd"][0]:
        problems.append(f"atomic at the Update site: unit {atomic[0]}, "
                        f"observer {reference['atomic_upd'][0]}")
    if atomic[1] != reference["atomic_alloc"][0]:
        problems.append(f"atomic in allocation-driven steps: unit "
                        f"{atomic[1]}, observer {reference['atomic_alloc'][0]}")
    if atomic[2]:
        problems.append(f"atomic under another lua_gc: {atomic[2]}")
    if min(subs) <= 0:
        problems.append(f"an atomic sub-phase was never charged: {subs}")
    if min(atomic[:2]) <= 0 or not steps:
        problems.append(f"the transcript does not exercise both origins: "
                        f"{atomic}, {steps} steps")
    return problems


def state_tests(builder: Builder, off: bytes, on: bytes) -> dict[str, Path]:
    lgc_on = builder.lgc_object("state", on)
    lgc_off = builder.lgc_object("plain", off)
    s1 = builder.oracle("S1", lgc_on, [PROFILE, STATE, CLOCK])
    s0 = builder.oracle("S0", lgc_off, [PROFILE, CLOCK])
    code1, out1 = oracle_run(s1, "state-virtual")
    code0, out0 = oracle_run(s0, "state-virtual")
    need(code1 == 0 and code0 == 0, f"state-virtual: {out1[-2:]} {out0[-2:]}")
    need(out1[-1].startswith("DIGEST ") and out1[-1] == out0[-1],
         f"the marks changed the collector: {out1[-1]} vs {out0[-1]}")
    need(out1[-2] == out0[-2], f"REF differs: {out1[-2]} vs {out0[-2]}")

    def window(lines: list[str]) -> list[str]:
        return [TIME_FIELDS.sub(r"gcstep=\1", l) for l in lines
                if "] gc window: " in l]
    need(window(out1) == window(out0) and len(window(out1)) == 40,
         "a non-time field of the gc window lines differs with the marks")
    need(not any("gc state" in l or "gc cls" in l or "gc worst" in l or
                 "gc asub" in l for l in out0),
         "state lines without the knob")
    need(sum("] gc state profile: on=1 marks=13 classes=7 window=120 update "
             "steps" in l for l in out1) == 1, "state banner, once")
    problems = state_problems(out1)
    need(not problems, "state accounting:\n  " + "\n  ".join(problems))
    reference = fields(out1[-2])
    print(f"state (virtual clock, 40 windows): collector transcript and gc "
          f"window fields identical with and without the marks "
          f"({out1[-1]}); classes == origins in every window; atomic at "
          f"Update site {reference['atomic_upd'][0]} / in allocation-driven "
          f"steps {reference['atomic_alloc'][0]} == outside observer; 5/5 "
          f"atomic sub-phases charged")

    # clock reads the marks add: the virtual clock advances 3 us per read
    def step_us(lines: list[str]) -> int:
        return sum(int(TIME_FIELDS.search(l).group(0).split(",")[4])
                   for l in lines if "] gc window: " in l)
    luac_steps = sum(sum(fields(l)[o][0] for o in ("upd", "alloc", "other"))
                     for l in out1 if "] gc state: " in l)
    update_reads = (step_us(out1) - step_us(out0)) / 3
    print(f"state cost: {update_reads:.0f} extra clock reads inside "
          f"{40 * 120} Update-site lua_gc calls of this transcript "
          f"({luac_steps} luaC_steps in all; a step costs 2 reads + 1 per "
          f"class change)")

    # real clock: can the probe tell why an atomic is long?
    sums = {}
    for heap in ("weak", "control"):
        code, out = oracle_run(s1, "state-real", heap)
        need(code == 0, f"state-real {heap}: {out[-2:]}")
        cls = [fields(l) for l in out if "] gc cls: " in l]
        asub = [fields(l) for l in out if "] gc asub: " in l]
        need(len(cls) == len(asub) == 10,
             f"state-real {heap}: {len(cls)} windows")
        sums[heap] = [sum(a["us"][i] for a in asub) for i in range(5)]
        sums[heap + "-atomic"] = sum(c["atomic"][1] for c in cls)
        if heap == "weak":
            worst = max((fields(l) for l in out if "] gc worst: " in l),
                        key=lambda f: f["cls_us"][2])
            need(worst["cls_us"][2] >= 500 and
                 2 * worst["cls_us"][2] >= worst["us"][0],
                 f"weak heap: no window's worst step is mostly atomic "
                 f"(best: {worst['cls_us'][2]} of {worst['us'][0]} us)")
            sums["worst"] = (worst["us"][0], worst["cls_us"][2],
                             worst["asub_us"])
    need(sums["weak"][2] >= 500 and
         sums["weak"][2] >= 10 * sums["control"][2] + 100,
         f"weak-table time not isolated: weak {sums['weak']}, control "
         f"{sums['control']}")
    print(f"state (host clock): atomic sub-phases us "
          f"(roots,again,weak,fnz,tail) weak heap {sums['weak']} vs the same "
          f"objects held strongly {sums['control']}; the weak heap's "
          f"longest atomic step: {sums['worst'][0]} us, {sums['worst'][1]} "
          f"in atomic, sub-phases {sums['worst'][2]}")
    return {"S1": s1, "S0": s0, "lgc_on": lgc_on, "lgc_off": lgc_off}


# --- idle --------------------------------------------------------------------
# script: (calls, quiet, kind(unk,free,busy,late) of every window)
# quiet = nothing allocates between a carried step and its run, so the step
# the rule carried is the step that ran: it must never run atomic().
IDLE_SCRIPTS = {
    "unknown": (240, 0, [120, 0, 0, 0]),
    "idle-15ms": (240, 0, [0, 120, 0, 0]),
    "fight-6ms": (2400, 1, [0, 60, 60, 0]),
    "fight-shift": (2400, 1, [0, 60, 60, 0]),      # windows close on BUSY
    "fight-6ms-atomic13": (2400, 1, [0, 60, 60, 0]),
    "fight-render-allocs": (2400, 0, [0, 60, 60, 0]),
    "fight-big-heap": (1200, 1, [0, 60, 60, 0]),
    "zero-slack": (1200, 1, [0, 0, 60, 60]),
    "busy-busy": (1200, 0, [0, 0, 120, 0]),
    "no-count": (1200, 1, [0, 60, 60, 0]),         # no LUA_GCCOUNT call
    "bad-kind": (240, 0, [120, 0, 0, 0]),
}
IDLE_BANNER = ("] gc idle step: on=1 owed_max=1 window=120 site=004023c6 "
               "count_site=004022ec walk_max=512 carry=propb+sweep+callfin")
IDLE_SUMMARY = re.compile(
    r"IDLE ([a-z0-9-]+): calls=(\d+) ran=(\d+) owed_end=0 carried=(\d+) "
    r"carried\(prop,sweep,callfin\)=(\d+),(\d+),(\d+) "
    r"busy_inline\(noop,pause,prop,atomic\)=(\d+),(\d+),(\d+),(\d+) "
    r"flips\(own,carried,busy_inline\)=(\d+),(\d+),(\d+) quiet=([01]) PASS$")
# busy(noop,pause,prop,propb,atomic,sweep,callfin)
B_NOOP, B_PAUSE, B_PROP, B_PROPB, B_ATOMIC, B_SWEEP, B_CALLFIN = range(7)


def idle_sections(lines: list[str]) -> dict[str, list[str]]:
    sections: dict[str, list[str]] = {}
    current = None
    for line in lines:
        if line.startswith("IDLE-BEGIN "):
            current = sections.setdefault(line.split()[1], [])
        elif current is not None:
            current.append(line)
    return sections


def idle_windows(lines: list[str]) -> list[dict[str, list[int]]] | str:
    idle = [fields(l) for l in lines if "] gc idle: " in l]
    times = [fields(l) for l in lines if "] gc idle us: " in l]
    why = [fields(l) for l in lines if "] gc idle why: " in l]
    if not len(idle) == len(times) == len(why):
        return (f"{len(idle)} gc idle, {len(times)} gc idle us, {len(why)} "
                f"gc idle why lines")
    for row, more, cause in zip(idle, times, why):
        if not row["win"] == more["win"] == cause["win"]:
            return f"window numbers {row['win']} {more['win']} {cause['win']}"
        row.update(more)
        row.update(cause)
    return idle


def idle_problems(code: int, lines: list[str], profile: bool
                  ) -> tuple[list[str], dict[str, dict[str, int]]]:
    """Every defect of one `idle` transcript, and the per-script totals."""
    problems: list[str] = []
    totals: dict[str, dict[str, int]] = {}
    if code:
        failed = [l for l in lines if l.startswith("FAIL ")]
        last = (failed or lines or [""])[-1]
        return [f"exit {code}: " + last[last.find("oracle.c:") + 9:]], totals
    sections = idle_sections(lines)
    if list(sections) != list(IDLE_SCRIPTS) + ["state-change"]:
        return [f"scripts {list(sections)}"], totals
    if sum(IDLE_BANNER in l for l in lines) != 1:
        problems.append("idle banner, once")
    if "IDLE state-change: dropped=2 PASS" not in sections["state-change"]:
        problems.append("state-change script")
    if profile != any("] gc window: " in l for l in lines):
        problems.append("gc window lines vs ISAAC_VITA_LUA_GC_PROFILE")
    for script, (calls, quiet, kinds) in IDLE_SCRIPTS.items():
        body = sections[script]
        summary = next((m for m in map(IDLE_SUMMARY.match, body) if m), None)
        if summary is None:
            problems.append(f"script {script} did not pass")
            continue
        (s_calls, s_ran, carried, c_prop, c_sweep, c_callfin, i_noop, i_pause,
         i_prop, i_atomic, f_own, f_carried, f_inline, s_quiet) = map(
             int, summary.groups()[1:])
        if (summary.group(1), s_calls, s_ran, s_quiet) != (script, calls,
                                                           calls, quiet):
            problems.append(f"{script}: {summary.group(0)}")
        if carried != c_prop + c_sweep + c_callfin:
            problems.append(f"{script}: carried {carried} != its phases")
        windows = idle_windows(body)
        if isinstance(windows, str):
            problems.append(f"{script}: {windows}")
            continue
        if len(windows) != calls // 120:
            problems.append(f"{script}: {len(windows)} windows for {calls} "
                            f"calls")
            continue
        owed_kb = 0
        total = {"deferred": 0, "propb": 0, "sweep": 0, "callfin": 0,
                 "atomic": 0, "atomic_idle": 0, "catomic": 0, "settle_count": 0,
                 "settle_step": 0, "max_us": 0, "over_us": 0, "walks": 0,
                 "walk_objects": 0, "walk_max": 0, "owed_out_windows": 0,
                 "busy": 0, "inline_noop": 0, "inline_pause": 0,
                 "inline_prop": 0, "inline_atomic": 0}
        for row in windows:
            where = f"{script} win {row['win'][0]}"
            busy, carry, settle = row["busy"], row["carry"], row["settle"]
            if row["calls"] != [120] or row["kind"] != kinds:
                problems.append(f"{where}: calls {row['calls']} kind "
                                f"{row['kind']}, expected {kinds}")
            # review B3: the kb balance with a step owed across the close
            kb_in, kb_out, kb_drop = row["owed_kb"]
            if row["kb"][0] + kb_in != row["kb"][1] + kb_out + kb_drop:
                problems.append(f"{where}: kb req {row['kb'][0]} + in {kb_in} "
                                f"!= ran {row['kb'][1]} + out {kb_out} + drop "
                                f"{kb_drop}")
            if kb_in != owed_kb:
                problems.append(f"{where}: owed_kb in {kb_in}, the window "
                                f"before left {owed_kb}")
            owed_kb = kb_out
            if row["dropped"] != [0] or kb_drop:
                problems.append(f"{where}: dropped {row['dropped']}")
            if sum(row["ran"]) != 120 - row["deferred"][0] + sum(settle):
                problems.append(f"{where}: ran {row['ran']} != 120 - deferred "
                                f"{row['deferred'][0]} + settled {settle}")
            if row["deferred"][0] != sum(carry):
                problems.append(f"{where}: deferred {row['deferred']} != "
                                f"carry {carry}")
            if sum(busy) != row["kind"][2]:
                problems.append(f"{where}: busy phases {busy} != BUSY tails "
                                f"{row['kind'][2]}")
            if (carry[0] > busy[B_PROPB] or carry[1] > busy[B_SWEEP] or
                    carry[2] > busy[B_CALLFIN]):
                problems.append(f"{where}: carry {carry} above busy {busy}")
            if script != "busy-busy" and carry != [
                    busy[B_PROPB], busy[B_SWEEP], busy[B_CALLFIN]]:
                # only two BUSY tails in a row can find the ledger full
                problems.append(f"{where}: a carriable BUSY step ran inline: "
                                f"carry {carry}, busy {busy}")
            if settle[script != "no-count"]:
                problems.append(f"{where}: settle(count,step) {settle}")
            if row["walk"][0] != busy[B_PROP] + busy[B_PROPB] or \
                    row["walk"][2] > 512:
                problems.append(f"{where}: walk {row['walk']} vs busy {busy}")
            if quiet and row["catomic"] != [0]:
                problems.append(f"{where}: a carried step ran atomic() in a "
                                f"quiet script: catomic {row['catomic']}")
            total["deferred"] += row["deferred"][0]
            total["propb"] += carry[0]
            total["sweep"] += carry[1]
            total["callfin"] += carry[2]
            total["atomic"] += sum(row["atomic"])
            total["atomic_idle"] += row["atomic"][0]
            total["catomic"] += row["catomic"][0]
            total["settle_count"] += settle[0]
            total["settle_step"] += settle[1]
            total["max_us"] = max(total["max_us"], row["max_us"][0])
            total["over_us"] += row["over_us"][0]
            total["walks"] += row["walk"][0]
            total["walk_objects"] += row["walk"][1]
            total["walk_max"] = max(total["walk_max"], row["walk"][2])
            total["owed_out_windows"] += kb_out != 0
            total["busy"] += row["kind"][2]
            total["inline_noop"] += busy[B_NOOP]
            total["inline_pause"] += busy[B_PAUSE]
            total["inline_prop"] += busy[B_PROP]
            total["inline_atomic"] += busy[B_ATOMIC]
        total["inline_flips"] = f_inline
        totals[script] = total
        # the unit's lines against the oracle's own recorder
        if (total["deferred"], total["propb"], total["sweep"],
                total["callfin"]) != (carried, c_prop, c_sweep, c_callfin):
            problems.append(f"{script}: unit carried {total['deferred']} "
                            f"({total['propb']},{total['sweep']},"
                            f"{total['callfin']}), recorder {carried} "
                            f"({c_prop},{c_sweep},{c_callfin})")
        if script != "busy-busy" and (
                total["inline_noop"], total["inline_pause"],
                total["inline_prop"], total["inline_atomic"]) != (
                    i_noop, i_pause, i_prop, i_atomic):
            problems.append(f"{script}: unit BUSY inline phases != recorder "
                            f"({i_noop},{i_pause},{i_prop},{i_atomic})")
        if total["atomic"] != f_own + f_carried:
            problems.append(f"{script}: atomic by place {total['atomic']} != "
                            f"recorder's white flips {f_own + f_carried}")
        if total["catomic"] != f_carried:
            problems.append(f"{script}: catomic {total['catomic']} != "
                            f"recorder's carried flips {f_carried}")
        if quiet and f_carried:
            problems.append(f"{script}: {f_carried} carried atomic steps")
        if kinds[2] and not kinds[0]:
            if not carried:
                problems.append(f"{script}: BUSY tails and nothing carried")
        elif carried or total["busy"]:
            problems.append(f"{script}: carried {carried} without BUSY")
    if problems:
        return problems, totals
    # review B3: a window that closes on a BUSY call carries kb across it
    for script in ("fight-shift", "busy-busy"):
        if not totals[script]["owed_out_windows"]:
            problems.append(f"{script}: no window closed with a step owed")
    if totals["fight-6ms"]["owed_out_windows"]:
        problems.append("fight-6ms: a window closed with a step owed")
    # review B1, positive side: steps that reach the atomic at a BUSY tail
    # exist in these transcripts and were kept inline (none was carried:
    # catomic == the recorder's carried flips == 0 in the quiet scripts)
    if not sum(t["inline_flips"] for t in totals.values()):
        problems.append("no script runs an atomic step at a BUSY tail")
    # every carriable phase is carried somewhere
    for phase in ("propb", "sweep", "callfin"):
        if not sum(t[phase] for t in totals.values()):
            problems.append(f"no script carries a {phase} step")
    # review B1: 13 ms atomic steps against 6 ms of slack -- none of them was
    # carried (catomic above), none is reported as fitting the slack
    atomic13 = totals["fight-6ms-atomic13"]
    if (atomic13["atomic_idle"] or not 13000 <= atomic13["max_us"] <= 13100
            or not atomic13["over_us"]):
        problems.append(f"atomic13: {atomic13}")
    if totals["no-count"]["settle_step"] != totals["no-count"]["deferred"] or \
            totals["fight-6ms"]["settle_count"] != \
            totals["fight-6ms"]["deferred"]:
        problems.append("settle place")
    return problems, totals


CTL_LINE = re.compile(r"CTL clamp=(\d+) ticks=(\d+) sum_k=(\d+) final_k=(\d+) "
                      r"min_k=(\d+) max_k=(\d+) grew=(\d+) decayed=(\d+) "
                      r"mean_count_kb=(\d+) cycles=(\d+) steps=(\d+)$")


def ctl_run(exe: Path, clamp: int) -> tuple[str, dict[str, int]]:
    code, out = oracle_run(exe, "ctl", str(clamp))
    need(code == 0 and out, f"ctl {clamp}: exit {code}: {out[-1:]}")
    match = CTL_LINE.match(out[-1])
    need(match is not None, f"ctl {clamp}: {out[-1]}")
    names = ("clamp", "ticks", "sum_k", "final_k", "min_k", "max_k", "grew",
             "decayed", "mean_count_kb", "cycles", "steps")
    return out[-1], dict(zip(names, map(int, match.groups())))


def ctl_tests(parts: dict[str, Path]) -> None:
    """Review B2: LuaEngine::Update's own controller (sub_004022d0, replayed
    by the oracle from the disassembly) reads lua_gc(GCCOUNT) and adapts k.
    With the carried step run inside the GCCOUNT import it must see what it
    sees in the build without the knob, up to the heap drift named below."""
    report = []
    for clamp in (64, 0):
        off_line, off = ctl_run(parts["S0"], clamp)
        on_line, on = ctl_run(parts["I1"], clamp)
        need(off["ticks"] == on["ticks"] == off["steps"] == on["steps"],
             f"ctl {clamp}: {off_line} | {on_line}")
        need(off["grew"] > 100 and off["sum_k"] > 0,
             f"ctl {clamp}: the controller never adapted: {off_line}")
        drift = (on["sum_k"] - off["sum_k"]) / off["sum_k"]
        # Not identical, and it cannot be: the carried step runs after the
        # allocations of one render instead of before them, so the collector
        # walks a slightly different heap.  What the fix pass guarantees is
        # that every count is read with the same number of steps behind it.
        need(abs(drift) <= 0.01,
             f"ctl clamp {clamp}: sum_k drifts {drift:+.2%}: off "
             f"{off_line} | on {on_line}")
        need(abs(on["final_k"] - off["final_k"]) <= off["final_k"] // 10 + 10,
             f"ctl clamp {clamp}: final k {on['final_k']} vs "
             f"{off['final_k']}")
        report.append(f"clamp {clamp}: off sum_k={off['sum_k']} final_k="
                      f"{off['final_k']} grew={off['grew']} decayed="
                      f"{off['decayed']} / on sum_k={on['sum_k']} final_k="
                      f"{on['final_k']} grew={on['grew']} decayed="
                      f"{on['decayed']} ({drift:+.2%})")
    print("controller replay (sub_004022d0, 3600 ticks, fight script): "
          + "; ".join(report))


def idle_tests(builder: Builder, parts: dict[str, Path]) -> None:
    lgc = parts["lgc_off"]
    totals = {}
    for name, defines in (("I1", [PROFILE, IDLE, CLOCK]),
                          ("I1n", [IDLE, CLOCK])):
        exe = builder.oracle(name, lgc, defines)
        code, out = oracle_run(exe, "idle")
        problems, totals = idle_problems(code, out, PROFILE in defines)
        need(not problems, f"idle {name}:\n  " + "\n  ".join(problems[:12]))
        parts[name] = exe
    code, out = oracle_run(parts["S0"], "idle-off")
    off = [l for l in out if l.startswith("IDLE off: ")]
    need(code == 0 and len(off) == 1 and
         re.match(r"IDLE off: calls=240 ran=240 DIGEST [0-9a-f]{16} PASS$",
                  off[0]) is not None, f"idle-off: {out[-1:]}")
    both = builder.oracle("SI", parts["lgc_on"], [PROFILE, STATE, IDLE, CLOCK])
    code, out = oracle_run(both, "idle")
    problems, _ = idle_problems(code, out, True)
    need(not problems, "idle with the state profile:\n  " +
         "\n  ".join(problems[:12]))
    calls = sum(v[0] for v in IDLE_SCRIPTS.values())
    print(f"idle: {len(IDLE_SCRIPTS)} scheduler scripts, {calls} calls, + "
          f"lua_State change, with and without ISAAC_VITA_LUA_GC_PROFILE and "
          f"with the state profile: every step ran once, in order, with its "
          f"own k, in its own call or the next one's GCCOUNT import (before "
          f"the count); no step was carried from a non-BUSY tail, from pause, "
          f"from an atomic or a no-op; in the quiet scripts no carried step "
          f"ran atomic(); kb req + owed.in == ran + owed.out + drop in every "
          f"window, also in the ones that close on a BUSY call")
    for script in ("fight-6ms", "fight-shift", "fight-render-allocs",
                   "fight-big-heap", "busy-busy"):
        t = totals[script]
        print(f"  {script}: BUSY tails {t['busy']}, carried {t['deferred']} "
              f"(propb {t['propb']}, sweep {t['sweep']}, callfin "
              f"{t['callfin']}), kept inline (noop {t['inline_noop']}, pause "
              f"{t['inline_pause']}, prop unproven {t['inline_prop']}, atomic "
              f"{t['inline_atomic']}), catomic {t['catomic']}, gray walks "
              f"{t['walks']} x {t['walk_objects'] // max(t['walks'], 1)} "
              f"objects (max {t['walk_max']})")
    ctl_tests(parts)


# --- mutants -----------------------------------------------------------------
MUTANTS = 12


def mutant_tests(builder: Builder, on: bytes, parts: dict[str, Path]) -> None:
    killed = []

    def lgc_mutant(tag: str, line: int, replacement: bytes | None) -> None:
        lines = on.splitlines(keepends=True)
        off_line = pace.derive(pristine_lgc, alloc_latch=ALLOC_LATCH
                               ).splitlines(keepends=True)[line - 1]
        lines[line - 1] = off_line if replacement is None else replacement
        need(lines[line - 1] != on.splitlines(keepends=True)[line - 1],
             f"mutant {tag} is a no-op")
        exe = builder.oracle(tag, builder.lgc_object(tag, b"".join(lines)),
                             [PROFILE, STATE, CLOCK])
        code, out = oracle_run(exe, "state-virtual")
        problems = [f"exit {code}"] if code else state_problems(out)
        need(bool(problems), f"mutant {tag} survived")
        killed.append(f"{tag} ({problems[0]})")

    def unit_mutant(tag: str, old: str, new: str, mode: str,
                    path: Path = UNIT) -> Path:
        defines = ([PROFILE, STATE, CLOCK] if mode == "state-virtual"
                   else [PROFILE, IDLE, CLOCK])
        lgc = parts["lgc_on"] if mode == "state-virtual" else parts["lgc_off"]
        exe = builder.oracle(tag, lgc, defines, unit_edit=(path, old, new))
        code, out = oracle_run(exe, mode)
        problems = (state_problems(out) if mode == "state-virtual" and not code
                    else idle_problems(code, out, True)[0])
        need(bool(problems), f"mutant {tag} survived")
        killed.append(f"{tag} ({problems[0][:90]})")
        return exe

    pristine_lgc = (builder.lua_src / "lgc.c").read_bytes()
    lgc_mutant("no-gcstate-mark", 1134, None)
    lgc_mutant("no-grayagain-cut", 997, None)
    unit_mutant("origin-always-upd",
                "s->step.origin = !s_vita_lua_gc_profile.depth ? "
                "VITA_GCO_ALLOC :",
                "s->step.origin = 0 ? VITA_GCO_ALLOC :", "state-virtual")
    unit_mutant("no-end-charge",
                "    vita_lua_gc_state_charge(s, now);\n    s->open = 0U;",
                "    s->open = 0U;", "state-virtual")
    unit_mutant("carried-step-wrong-k",
                "(void)vita_lua_gc_idle_run(state, owed_kb, kind, &slack_us, "
                "1);",
                "(void)vita_lua_gc_idle_run(state, owed_kb + 1, kind, "
                "&slack_us, 1);", "idle")
    unit_mutant("owed-max-2", "#define VITA_LUA_GC_IDLE_OWED_MAX 1U",
                "#define VITA_LUA_GC_IDLE_OWED_MAX 2U", "idle")
    unit_mutant("never-carries",
                "    if (vita_lua_gc_idle_carry(busy, q->owed, "
                "VITA_LUA_GC_IDLE_OWED_MAX,\n",
                "    if (0 && vita_lua_gc_idle_carry(busy, q->owed, "
                "VITA_LUA_GC_IDLE_OWED_MAX,\n", "idle")
    # review B1: the rule before the fix pass (every BUSY step is carried)
    unit_mutant("carries-unproven-prop",
                "    return phase == VITA_GCI_PHASE_SWEEP || "
                "phase == VITA_GCI_PHASE_CALLFIN ||\n"
                "        phase == VITA_GCI_PHASE_PROP_BOUND;",
                "    return phase != VITA_GCI_PHASE_NOOP;", "idle",
                path=RULE)
    # ... and a gray-list proof that always succeeds
    unit_mutant("prop-always-proven",
                "        return bound ? VITA_GCI_PHASE_PROP_BOUND : "
                "VITA_GCI_PHASE_PROP;",
                "        (void)bound; return VITA_GCI_PHASE_PROP_BOUND;",
                "idle")
    # ... and one that forgets the step's own overshoot (GCSTEPSIZE)
    unit_mutant("proof-without-stepsize",
                "    lu_mem need = (lu_mem)budget + (lu_mem)GCSTEPSIZE;",
                "    lu_mem need = (lu_mem)budget / 4U;", "idle")
    # review B2: the carried step runs at the GCSTEP import, after the count
    exe = unit_mutant("settle-after-count",
                      "            vita_lua_gc_idle_settle(state, 1);\n",
                      "            (void)0;\n", "idle")
    # measured, not asserted: what the controller replay sees of it
    for clamp in (64, 0):
        off = ctl_run(parts["S0"], clamp)[1]["sum_k"]
        fixed = ctl_run(parts["I1"], clamp)[1]["sum_k"]
        late = ctl_run(exe, clamp)[1]["sum_k"]
        killed[-1] += (f"; replay clamp {clamp} sum_k drift: settle at the "
                       f"count {(fixed - off) / off:+.2%}, after it "
                       f"{(late - off) / off:+.2%}")
    # review B3: the kb balance without the step owed at the window's close
    unit_mutant("owed-out-not-printed",
                "                   (unsigned)q->owed_in_kb, "
                "(unsigned)owed_out_kb,\n",
                "                   (unsigned)q->owed_in_kb, 0U,\n", "idle")
    print(f"mutants: {len(killed)}/{MUTANTS} caught -- " + "; ".join(killed))
    need(len(killed) == MUTANTS, "mutant count")


# --- refusals and log lines --------------------------------------------------
def refusal_tests(builder: Builder) -> None:
    for name, defines, needle in (
            ("state-without-profile", [STATE, CLOCK],
             "ISAAC_VITA_LUA_GC_STATE_PROFILE requires "
             "ISAAC_VITA_LUA_GC_PROFILE"),
            ("no-feature", [CLOCK],
             "host_vita_lua_gc.c is compiled only for its opt-in features")):
        code, out = run_status(
            [str(builder.cc), "-m32", "-std=gnu11", "-fsyntax-only",
             f"-I{builder.lua_src}", f"-I{RUNTIME}", *defines, str(ORACLE)],
            builder.work)
        need(code != 0 and needle in out, f"{name} not refused: {out[-300:]}")
    scheduler = (RUNTIME / "kage_vita_fullspeed_scheduler.c").read_text(
        encoding="utf-8")
    need(scheduler.count(
        '#  error "ISAAC_VITA_LUA_GC_IDLE_STEP is unavailable with '
        'ISAAC_VITA_FULLSPEED_HEAD_ADVANCE') == 1,
        "scheduler refusal of HEAD_ADVANCE")
    print("refusals: STATE without PROFILE, the unit without a feature, "
          "IDLE with HEAD_ADVANCE (#error)")


def log_line_tests() -> None:
    text = UNIT.read_text(encoding="utf-8")
    checked = []
    for match in re.finditer(r'isaac_vita_log\(((?:\s*"(?:[^"\\]|\\.)*")+)',
                             text):
        fmt = "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', match.group(1)))
        tag = re.match(r"\[isaac-lua\] (gc (?:state|cls|asub|worst|idle)[a-z ]*):",
                       fmt)
        if not tag:
            continue
        longest = (fmt.replace("%08x", "f" * 8).replace("%u", "4294967295")
                   .replace("%d", "-2147483648").replace("%s", "alloc"))
        need("%" not in longest, f"unhandled conversion in {fmt!r}")
        need(len(longest) < LOG_BODY,
             f"{tag.group(1)}: {len(longest)} characters at the largest "
             f"values, the log body holds {LOG_BODY - 1}")
        checked.append(f"{tag.group(1)} {len(longest)}")
    need(len(checked) == 9, f"expected 9 new log lines, found {checked}")
    print(f"log lines: worst-case length under {LOG_BODY} -- "
          + ", ".join(checked))


# --- CMake refusals ----------------------------------------------------------
def cmake_tests(work: Path) -> None:
    """The knob block's refusals, through cmake_knob_probe.py (no SDK)."""
    directory = work / "cmake"
    directory.mkdir(parents=True, exist_ok=True)
    init = directory / "init.cmake"
    init.write_text("", encoding="utf-8")
    lua = ["ISAAC_VITA_LUA=ON"]
    state = lua + ["ISAAC_VITA_LUA_GC_STATE_PROFILE=ON"]
    idle = lua + ["ISAAC_VITA_LUA_GC_IDLE_STEP=ON"]
    scheduler = ["ISAAC_VITA_KAGE=ON", "ISAAC_VITA_FULLSPEED_SCHEDULER=ON",
                 "ISAAC_VITA_FULLSPEED_SERVICE_RESERVE=ON"]
    cases = (
        (["ISAAC_VITA_LUA_GC_STATE_PROFILE=ON"],
         "ISAAC_VITA_LUA_GC_IDLE_STEP require ISAAC_VITA_LUA=ON"),
        (["ISAAC_VITA_LUA_GC_IDLE_STEP=ON"],
         "ISAAC_VITA_LUA_GC_IDLE_STEP require ISAAC_VITA_LUA=ON"),
        (state, "ISAAC_VITA_LUA_GC_STATE_PROFILE requires "
                "ISAAC_VITA_LUA_GC_PROFILE=ON"),
        (state + ["ISAAC_VITA_LUA_GC_PROFILE=ON"],
         "ISAAC_VITA_LUA_GC_STATE_PROFILE requires "
         "ISAAC_VITA_LUA_GC_FINALIZER_PACE=ON"),
        (idle, "ISAAC_VITA_LUA_GC_IDLE_STEP requires "
               "ISAAC_VITA_FULLSPEED_SCHEDULER=ON"),
        (idle + scheduler + ["ISAAC_VITA_FULLSPEED_HEAD_ADVANCE=ON"],
         "ISAAC_VITA_LUA_GC_IDLE_STEP and ISAAC_VITA_FULLSPEED_HEAD_ADVANCE "
         "are exclusive"),
        (state + idle + scheduler + ["ISAAC_VITA_LUA_GC_PROFILE=ON",
                                     "ISAAC_VITA_LUA_GC_FINALIZER_PACE=ON"],
         None),
    )
    for number, (keys, needle) in enumerate(cases):
        args = [sys.executable, str(ROOT / "vita" / "cmake_knob_probe.py"),
                "-C", str(init), "-S", str(ROOT / "vita"), "-B",
                str(directory / f"b{number}")]
        for key in keys:
            args += ["-D", key]
        code, out = run_status(args, directory)
        text = " ".join(out.split())           # cmake wraps long messages
        if needle is None:
            need(code == 0, f"cmake refused {keys}: {text[-400:]}")
        else:
            need(code != 0 and needle in text,
                 f"cmake: {keys} not refused with {needle!r}: {text[-400:]}")
    text = (ROOT / "vita" / "CMakeLists.txt").read_text(encoding="utf-8")
    need(text.count("list(APPEND ISAAC_VITA_LUA_GC_FINALIZER_PACE_ARGS "
                    "--state-profile)") == 1, "--state-profile wiring")
    need(text.count("APPEND PROPERTY COMPILE_DEFINITIONS "
                    "ISAAC_VITA_LUA_GC_IDLE_STEP=1)") == 1 and
         text.count("ISAAC_VITA_LUA_GC_STATE_PROFILE=1)") == 1,
         "compile definitions wiring")
    print(f"cmake (knob probe): {len(cases) - 1} refusals with their own "
          f"message, the full combination admitted; --state-profile and the "
          f"definitions are appended only under their knobs")


# --- log reader ---------------------------------------------------------------
def reader_tests(work: Path) -> None:
    """vita/lua_gc_state_report.py reads what the unit prints."""
    sys.path.insert(0, str(ROOT / "vita"))
    import lua_gc_state_report as reader
    exe = work / "oracle-SI" / "oracle.exe"
    for mode, kinds in (("state-virtual", ("state", "cls", "asub", "worst",
                                           "idle", "idle us", "idle why")),
                        ("idle", ("state", "cls", "idle", "idle us",
                                  "idle why"))):
        code, out = oracle_run(exe, mode)
        need(code == 0, f"reader: oracle {mode} failed")
        runs = reader.parse(out)
        need(len(runs) == 1 and runs[0], f"reader: {mode}: {len(runs)} runs")
        for number, window in runs[0].items():
            for kind in kinds:
                need(kind in window, f"reader: {mode} win {number}: no {kind}")
            by_origin = sum(window["state"][o][1] for o in reader.ORIGINS)
            by_class = sum(window["cls"][c][1] for c in reader.CLASSES)
            need(by_origin == by_class,
                 f"reader: {mode} win {number}: origins {by_origin} us, "
                 f"classes {by_class} us")
        text = reader.report_run(1, runs[0], 3, 5000)
        need(any("origin alloc" in line for line in text) and
             any("idle step: calls=" in line for line in text) and
             any("BUSY tails by the step issued there:" in line
                 for line in text) and
             not any("BROKEN" in line for line in text) and
             sum("kb balance" in line for line in text) == 1,
             f"reader: {mode}: summary incomplete")
    # --bid: launches of a cumulative log, split where the prefix time falls
    idle = next(l for l in out if "] gc idle: " in l)
    body = idle[idle.index("[isaac-lua]"):]
    log = ["[9.500 thr 0x1] boot bid=perf:a x=1",
           "[35.000 thr 0x1] " + body,
           "[35.001 thr 0x2] " + body.replace("win=", "win=9", 1),
           "[34.999 thr 0x1] late line of the same launch",
           "[9.400 thr 0x1] boot bid=perf:b",
           "[36.000 thr 0x1] " + body]
    found = reader.launches(log)
    need([(bid, len(lines)) for bid, lines in found] ==
         [("perf:a", 4), ("perf:b", 2)], f"reader: launches {found}")
    runs = reader.parse(found[1][1])
    need(len(runs) == 1 and len(runs[0]) == 1 and
         next(iter(runs[0].values()))["t"] == 36.0,
         "reader: the --bid launch and its prefix time")
    need(reader.in_phase(36.0, [(35_500_000, 36_000_000)]) and
         not reader.in_phase(36.0, [(36_000_000, 37_000_000)]),
         "reader: a record belongs to the window that ends at its time")
    need(reader.parse(["[isaac-lua] gc window: n=120"]) == [],
         "reader: a log without the records must give no runs")
    print("reader: lua_gc_state_report.py parses every record kind the unit "
          "prints; per window its class time equals its origin time")


# --- scheduler model ---------------------------------------------------------
MODEL_FLAGS = ["-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
               "-DISAAC_VITA_RUNTIME_SETTINGS=1",
               "-DISAAC_VITA_FULLSPEED_SCHEDULER=1",
               "-DISAAC_VITA_FULLSPEED_SERVICE_RESERVE=1",
               "-DISAAC_VITA_FULLSPEED_EXIT_ZERO_FRAME_SITES=1"]
MODEL_SCENES = 9
MODEL_POLICIES = ("off", "on", "sweep", "legacy", "nogc")
MODEL_STEP_US = 2200          # the model's largest non-atomic step (callfin)


def model_tests(cc: Path, work: Path) -> None:
    directory = work / "model"
    directory.mkdir(parents=True, exist_ok=True)
    sources = [str(MODEL), str(RUNTIME / "kage_vita_fullspeed_scheduler.c")]
    include = [f"-I{RUNTIME}", f"-I{ROOT / 'vita'}"]
    with_knob = directory / "with.exe"
    without = directory / "without.exe"
    run([str(cc), *MODEL_FLAGS, IDLE, *include, *sources, "-o",
         str(with_knob)], work)
    run([str(cc), *MODEL_FLAGS, *include, *sources, "-o", str(without)], work)
    code, out = oracle_run(with_knob)
    need(code == 0 and out[-1] == "MODEL PASS", f"model: {out[-2:]}")
    code, plain = oracle_run(without, "off-only")
    need(code == 0, f"model without the knob: {plain[-2:]}")
    digests = [l for l in out if l.startswith("DIGEST ")]
    need(digests == plain and len(digests) == MODEL_SCENES,
         "the scheduler compiled with the knob and not asked differs from "
         "the scheduler without it")
    need((RUNTIME / "host_vita_lua_gc_idle_rule.h").read_text(
        encoding="utf-8").count("static inline int vita_lua_gc_idle_carry(")
        == 1 and MODEL.read_text(encoding="utf-8").count(
            "vita_lua_gc_idle_carry(kind == KAGE_VITA_GC_IDLE_BUSY, owed,")
        == 1 and UNIT.read_text(encoding="utf-8").count(
            "vita_lua_gc_idle_carry(busy, q->owed, VITA_LUA_GC_IDLE_OWED_MAX,")
        == 1, "the model and the unit must call the one carry rule")
    scenes: dict[str, dict[str, dict[str, float]]] = {}
    for line in out:
        match = re.match(r"MODEL (\S+) (\w+): pps=([\d.]+) ups=([\d.]+) "
                         r"max_gap_us=(\d+) late_ms=([\d.]+)", line)
        if match:
            row = {"pps": float(match.group(3)), "ups": float(match.group(4)),
                   "gap": float(match.group(5)),
                   "late": float(match.group(6))}
            big = re.search(r"carried=(\d+) .* big_carried=(\d+)$", line)
            if big:
                row["carried"] = float(big.group(1))
                row["big_carried"] = float(big.group(2))
            scenes.setdefault(match.group(1), {})[match.group(2)] = row
    need(len(scenes) == MODEL_SCENES and
         all(tuple(v) == MODEL_POLICIES for v in scenes.values()),
         f"model scenes {sorted(scenes)}")
    legacy_big = 0.0
    for name, by in scenes.items():
        for policy, row in by.items():
            need(row["ups"] <= 30.005,
                 f"{name}/{policy}: {row['ups']} updates/s above the floor")
        for policy in ("on", "sweep"):
            need(by[policy]["big_carried"] == 0,
                 f"{name}/{policy}: an atomic step was carried")
            # review B1: what the rule may move is one non-atomic step, so an
            # update can start later than without the knob by that and no more
            need(by[policy]["gap"] <= by["off"]["gap"] + MODEL_STEP_US,
                 f"{name}/{policy}: longest update gap {by[policy]['gap']} us "
                 f"vs off {by['off']['gap']}")
            need(by[policy]["ups"] >= by["off"]["ups"] - 0.12,
                 f"{name}/{policy}: loses updates/s: {by}")
            need(by[policy]["pps"] >= by["off"]["pps"] - 0.05,
                 f"{name}/{policy}: presents fewer frames: {by}")
            need(by["nogc"]["pps"] >= by[policy]["pps"] - 0.05,
                 f"{name}/{policy}: beats its own ceiling: {by}")
        need(by["on"]["carried"] >= by["sweep"]["carried"],
             f"{name}: the proof carried fewer steps than no proof")
        legacy_big += by["legacy"]["big_carried"]
    need(legacy_big > 0, "positive control: the rule before the fix pass "
         "carries no atomic step in any scene")
    summaries = [l for l in out if l.startswith("SWEEP-SUMMARY ")]
    need(len(summaries) == 2 * (len(MODEL_POLICIES) - 1), "sweep summaries")
    sweep: dict[tuple[str, str], dict[str, float]] = {}
    for line in summaries:
        match = re.match(
            r"SWEEP-SUMMARY jitter_us=(\d+) gc_us=\d+ points=41 (\w+)-off "
            r"pps: mean=([+-][\d.]+) best=([+-][\d.]+)@\d+ "
            r"worst=([+-][\d.]+)@\d+ ups worst=([+-][\d.]+) "
            r"late_ms worst=([+-][\d.]+) big_carried=(\d+)$", line)
        need(match is not None, f"sweep summary: {line}")
        sweep[(match.group(1), match.group(2))] = dict(zip(
            ("mean", "best", "worst", "ups", "late", "big"),
            map(float, match.groups()[2:])))
    for (width, policy), row in sweep.items():
        if policy in ("on", "sweep"):
            need(row["big"] == 0, f"sweep {width}/{policy}: atomic carried")
            need(row["ups"] >= -0.12,
                 f"sweep {width}/{policy}: loses updates/s: {row}")
            need(sweep[(width, "nogc")]["mean"] >= row["mean"] - 0.01,
                 f"sweep {width}/{policy}: above the no-GC ceiling")
    need(sum(row["big"] for (_, p), row in sweep.items() if p == "legacy")
         > 0, "sweep: legacy carries no atomic step")
    print(f"scheduler: knob compiled in and not asked == scheduler without "
          f"it ({MODEL_SCENES} scene digests); no policy starts Game::Update "
          f"early (>= 33,333 us between starts, asserted in the oracle); "
          f"`on` and `sweep` (the unit's carry rule, shared header) carry no "
          f"atomic step and lengthen no update gap by more than "
          f"{MODEL_STEP_US} us; the rule before the fix pass (`legacy`) "
          f"carries {legacy_big:.0f} atomic steps in the fixed scenes")
    for line in out:
        if line.startswith(("MODEL ", "SWEEP-SUMMARY ")) and \
                line != "MODEL PASS":
            print("  " + line)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--lua-source", type=Path)
    parser.add_argument("--cc", type=Path)
    parser.add_argument("--work", type=Path)
    args = parser.parse_args()
    lua_root = R.discover_source(args.lua_source)
    sources = R.validate_source(lua_root, R.records())
    lua_src = lua_root / "src"
    pristine = (lua_src / "lgc.c").read_bytes()
    pristine_baselib = (lua_src / "lbaselib.c").read_bytes()
    cc = find_cc(args.cc)
    holder = None
    if args.work is None:
        holder = tempfile.TemporaryDirectory(prefix="isaac-lua-gc-state-")
        work = Path(holder.name)
    else:
        work = args.work.resolve()
        work.mkdir(parents=True, exist_ok=True)
    try:
        off, on = derivation_tests(pristine)
        log_line_tests()
        cmake_tests(work)
        builder = Builder(cc, lua_src, sources, pristine_baselib, work)
        refusal_tests(builder)
        parts = state_tests(builder, off, on)
        idle_tests(builder, parts)
        reader_tests(work)
        mutant_tests(builder, on, parts)
        model_tests(cc, work)
    finally:
        if holder is not None:
            holder.cleanup()
    print("test_vita_lua_gc_state_idle: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
