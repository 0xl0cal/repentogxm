#!/usr/bin/env python3
"""Compile/run parity-aware cadence oracles and freeze its build surface."""

from __future__ import annotations

import hashlib
import os
import re
import shutil
import subprocess
import tempfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path


def compiler() -> str:
    for name in ("cc", "gcc", "clang"):
        found = shutil.which(name)
        if found:
            return found
    candidates = (
        Path(os.environ.get("ProgramFiles", r"C:\Program Files")) /
        "LLVM" / "bin" / "clang.exe",
        Path(os.environ.get("ProgramFiles", r"C:\Program Files")) /
        "Microsoft Visual Studio" / "2022" / "Community" / "VC" /
        "Tools" / "Llvm" / "x64" / "bin" / "clang.exe",
    )
    for candidate in candidates:
        if candidate.is_file():
            return str(candidate)
    raise RuntimeError("no host C compiler found")


def run(command: list[str]) -> str:
    completed = subprocess.run(
        command, check=False, text=True, capture_output=True
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"command failed ({completed.returncode}): {command!r}\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    return completed.stdout


def run_status(command: list[str]) -> subprocess.CompletedProcess:
    return subprocess.run(command, check=False, text=True, capture_output=True)


# ISAAC_VITA_FULLSPEED_FLOOR_DEBT (S5 floor debt): the recipe
# parameters every OBSERVE/ON leg runs with, and the oracle combinations the
# knob-OFF legs above already run (each OBSERVE/ON leg is compared with its
# own OFF twin).
FLOOR_DEBT_RECIPE = (
    "-DISAAC_VITA_FULLSPEED_FLOOR_DEBT_CAP_US=8000",
    "-DISAAC_VITA_FULLSPEED_ADMIT_SLACK_US=2000",
)
RESERVE_DUTY_FLAGS = (
    "-DISAAC_VITA_RUNTIME_SETTINGS=1",
    "-DISAAC_VITA_FULLSPEED_SCHEDULER=1",
    "-DISAAC_VITA_FULLSPEED_SERVICE_RESERVE=1",
    "-DISAAC_VITA_FULLSPEED_EXIT_ZERO_FRAME_SITES=1",
)
INT2J_FLAGS = ("-DISAAC_VITA_THEORA_RENDER_PRIORITY=1",) + RESERVE_DUTY_FLAGS
RENDER_COST_FLAG = "-DISAAC_VITA_FULLSPEED_RENDER_COST_EXCLUDES_PRESENT=1"
FLOOR_DEBT_SETS = (
    ("plain", ()),
    ("duty", ("-DISAAC_VITA_RUNTIME_SETTINGS=1",
              "-DISAAC_VITA_FULLSPEED_SCHEDULER=1")),
    ("reserve-duty", RESERVE_DUTY_FLAGS),
    ("gl-admission",
     ("-DISAAC_VITA_GL_THREAD_RENDER_ADMISSION=1",) + RESERVE_DUTY_FLAGS),
    ("video-priority", INT2J_FLAGS),
    ("render-cost-plain", (RENDER_COST_FLAG,)),
    ("render-cost-int2j", INT2J_FLAGS + (RENDER_COST_FLAG,)),
)
FLOOR_DEBT_SCENARIO = re.compile(
    r"^Vita floor-debt scenario (\S+): (updates=\d+ presents=\d+ ticks=\d+ "
    r"wall_us=\d+ ups=\d+\.\d{3} drop=\d+ digest=[0-9a-f]{16})"
    r"( p=\d+ late=\S+ kept=\d+ over=\S+ clamp=\S+ stale=\S+ gf=\S+ "
    r"slack=\S+)?$",
    re.MULTILINE,
)
FLOOR_DEBT_GAP_TEXT = (
    "start[j]-start[i] >= (j-i)*33.333 ms - cap, <=31 starts/s")


def floor_debt_scenarios(stdout: str, runtime_settings: bool
                         ) -> dict[str, tuple[dict[str, str], str, str | None]]:
    """name -> (behaviour fields, behaviour text, receipt text or None)."""
    found: dict[str, tuple[dict[str, str], str, str | None]] = {}
    for match in FLOOR_DEBT_SCENARIO.finditer(stdout):
        if match.group(1) in found:
            raise AssertionError(f"floor-debt scenario twice: {match.group(1)}")
        fields = dict(item.split("=", 1) for item in match.group(2).split())
        found[match.group(1)] = (fields, match.group(2), match.group(3))
    # jitter, jitter-wide, edge, 4 stalls x (window, edge, render, update),
    # 16 heavy-edge, 16 heavy-medium-edge, heavy, pause-resume, slack,
    # refuse; then wake-skew-edge, wake-skew-short-edge, 4
    # wake-extra, head-clock-reset, head-wait-backward, rule-a-one-tick,
    # rule-a-below-tick; 3 wake-extra-in-window; the three
    # port.ini fps_lock rooms with runtime settings.
    expected = 68 + (3 if runtime_settings else 0)
    if len(found) != expected or stdout.count(
            "Vita floor-debt scenario ") != expected:
        raise AssertionError(
            f"floor-debt scenario count {len(found)} != {expected}:\n{stdout}")
    return found


def without_floor_debt_lines(stdout: str) -> list[str]:
    return [line for line in stdout.splitlines()
            if not line.startswith("Vita floor-debt scenario")]


def verify_floor_debt(common: list[str], runtime: Path, vita: Path,
                      output: Path, off_stdout: dict[str, str],
                      phase_off_stdout: str) -> list[str]:
    """OBSERVE/ON oracle legs, negative controls and OFF identities."""
    sources = [str(runtime / "kage_vita_fullspeed_scheduler.c"),
               str(runtime / "kage_vita_fullspeed_scheduler_oracle.c")]
    report: list[str] = []

    def build_and_run(name: str, flags: tuple[str, ...]
                      ) -> subprocess.CompletedProcess:
        exe = output / f"floor-debt-{name}.exe"
        run(common + list(flags) + sources + ["-o", str(exe)])
        return run_status([str(exe)])

    jobs: dict[str, tuple[str, ...]] = {}
    for name, flags in FLOOR_DEBT_SETS:
        jobs[f"observe-{name}"] = flags + FLOOR_DEBT_RECIPE + (
            "-DISAAC_VITA_FULLSPEED_FLOOR_DEBT=1",)
        jobs[f"on-{name}"] = flags + FLOOR_DEBT_RECIPE + (
            "-DISAAC_VITA_FULLSPEED_FLOOR_DEBT=2",)
    # Other cap/slack: the restated contract is checked against the cap the
    # build carries, not against the recipe's 8000/2000.
    jobs["on-cap4000-slack1000"] = (
        "-DISAAC_VITA_FULLSPEED_FLOOR_DEBT=2",
        "-DISAAC_VITA_FULLSPEED_FLOOR_DEBT_CAP_US=4000",
        "-DISAAC_VITA_FULLSPEED_ADMIT_SLACK_US=1000")
    jobs["on-no-slack"] = ("-DISAAC_VITA_FULLSPEED_FLOOR_DEBT=2",
                           "-DISAAC_VITA_FULLSPEED_FLOOR_DEBT_CAP_US=8000")
    # Negative controls: the oracle must catch the design draft without the
    # stale cut, the first version without the grid clamp, rule A
    # (one Game tick) without the head-stale rule, and rule A alone without
    # the clamp either.
    jobs["neg-no-stale-cut"] = FLOOR_DEBT_RECIPE + (
        "-DISAAC_VITA_FULLSPEED_FLOOR_DEBT=2",
        "-DISAAC_KAGE_VITA_FULLSPEED_ORACLE_NO_STALE_CUT=1")
    jobs["neg-no-grid-clamp"] = FLOOR_DEBT_RECIPE + (
        "-DISAAC_VITA_FULLSPEED_FLOOR_DEBT=2",
        "-DISAAC_KAGE_VITA_FULLSPEED_ORACLE_NO_GRID_CLAMP=1")
    jobs["neg-no-head-stale-cut"] = FLOOR_DEBT_RECIPE + (
        "-DISAAC_VITA_FULLSPEED_FLOOR_DEBT=2",
        "-DISAAC_KAGE_VITA_FULLSPEED_ORACLE_NO_HEAD_STALE_CUT=1")
    jobs["neg-rule-a-only"] = FLOOR_DEBT_RECIPE + (
        "-DISAAC_VITA_FULLSPEED_FLOOR_DEBT=2",
        "-DISAAC_KAGE_VITA_FULLSPEED_ORACLE_NO_HEAD_STALE_CUT=1",
        "-DISAAC_KAGE_VITA_FULLSPEED_ORACLE_NO_GRID_CLAMP=1")
    # At most 4 compile+oracle workers (one child process each): the host is
    # shared with other jobs.
    with ThreadPoolExecutor(max_workers=min(4, os.cpu_count() or 1)) as pool:
        futures = {name: pool.submit(build_and_run, name, flags)
                   for name, flags in jobs.items()}
        done = {name: future.result() for name, future in futures.items()}

    # Each control must fail at its own first symptom, not anywhere.
    controls = {
        # the design draft: the 150-ms stall leaves the floor ahead for good
        "neg-no-stale-cut": "floor-debt update-stall-150ms-edge: presents 120",
        # the first version (no grid clamp): one pace-wake overshoot 250 us above the
        # head's with no service pins gf at +100 us in every sample
        "neg-no-grid-clamp": "floor-debt wake-skew-short-edge: presents 240 "
                             "updates 120 gf(n,pos,max) 120,120,100, fresh 240 120",
        # with the clamp, gf stays <= 0 without the grid rule; the heavy
        # room's drops are then filed as late, not stale_grid
        "neg-no-head-stale-cut": "run.debt.stale_grid > 0u",
        # rule A with neither the grid rule nor the clamp: heavy room gf > 0
        "neg-rule-a-only": "run.debt.gf_pos == 0u && run.debt.gf_max_us <= 0",
    }
    for name, symptom in controls.items():
        if done[name].returncode == 0:
            raise AssertionError(f"floor-debt negative control {name} passed")
        if symptom not in done[name].stderr:
            raise AssertionError(
                f"{name} control failed elsewhere: {done[name].stderr!r}")
    report.append(
        "Floor-debt negative controls: PASS (no stale cut -> "
        "update-stall-150ms-edge 120 presents vs 240 fresh; no grid clamp -> "
        "wake-skew-short-edge gf +100 us in 120 of 120 samples; grid rule "
        "off -> heavy room drops not stale_grid; rule A alone -> heavy room "
        "gf > 0)")

    for name in ("on-cap4000-slack1000", "on-no-slack"):
        result = done[name]
        if result.returncode != 0 or \
                "Vita cadence30 scheduler oracle: PASS" not in result.stdout:
            raise AssertionError(
                f"floor-debt {name} oracle failed: {result.stdout[-2000:]!r} "
                f"{result.stderr!r}")
    if "Vita floor-debt scenarios: PASS (mode=on cap=4000us;" not in \
            done["on-cap4000-slack1000"].stdout:
        raise AssertionError("cap-4000 floor-debt PASS line missing")

    for name, flags in FLOOR_DEBT_SETS:
        runtime_settings = "-DISAAC_VITA_RUNTIME_SETTINGS=1" in flags
        service_reserve = "-DISAAC_VITA_FULLSPEED_SERVICE_RESERVE=1" in flags
        off = off_stdout[name]
        observe = done[f"observe-{name}"]
        on = done[f"on-{name}"]
        for mode, result in (("observe", observe), ("on", on)):
            if result.returncode != 0 or \
                    "Vita cadence30 scheduler oracle: PASS" not in result.stdout:
                raise AssertionError(
                    f"floor-debt {mode}-{name} oracle failed: "
                    f"{result.stdout[-2000:]!r} {result.stderr!r}")
        if "Vita floor-debt scenarios: PASS (mode=off;" not in off or \
                "Vita floor-debt scenarios: PASS (mode=observe cap=8000us;" \
                not in observe.stdout or \
                "Vita floor-debt scenarios: PASS (mode=on cap=8000us;" \
                not in on.stdout:
            raise AssertionError(f"floor-debt mode PASS lines ({name})")
        off_runs = floor_debt_scenarios(off, runtime_settings)
        observe_runs = floor_debt_scenarios(observe.stdout, runtime_settings)
        on_runs = floor_debt_scenarios(on.stdout, runtime_settings)
        if set(off_runs) != set(observe_runs) or set(off_runs) != set(on_runs):
            raise AssertionError(f"floor-debt scenario sets differ ({name})")
        # OBSERVE is behaviour-neutral: every scenario's updates, presents,
        # ticks, wall time, drops and start/present digest equal OFF, and
        # every other oracle line is byte-identical.
        for scenario, (_, text, receipt) in off_runs.items():
            if receipt is not None:
                raise AssertionError(f"OFF printed a receipt: {scenario}")
            if observe_runs[scenario][1] != text:
                raise AssertionError(
                    f"OBSERVE changed {name}/{scenario}: "
                    f"{observe_runs[scenario][1]} != {text}")
            if observe_runs[scenario][2] is None or \
                    on_runs[scenario][2] is None:
                raise AssertionError(f"receipt missing: {name}/{scenario}")
        off_rest = [line for line in without_floor_debt_lines(off)
                    if not line.startswith("Vita floor-debt scenarios: PASS")]
        observe_rest = [
            line for line in without_floor_debt_lines(observe.stdout)
            if not line.startswith("Vita floor-debt scenarios: PASS")]
        if off_rest != observe_rest:
            raise AssertionError(f"OBSERVE changed another oracle line ({name})")
        # ON: Game updates per second never below OFF in any scenario; the
        # jitter rooms gain, the slack room admits frames OFF refuses, the
        # refused room and the render-bound heavy room are unchanged.
        for scenario, (fields, _, _) in off_runs.items():
            on_fields = on_runs[scenario][0]
            if float(on_fields["ups"]) < float(fields["ups"]) - 0.001:
                raise AssertionError(
                    f"ON lowered UPS in {name}/{scenario}: "
                    f"{on_fields['ups']} < {fields['ups']}")
        jitter_rooms = ["jitter", "jitter-wide"] + (
            ["jitter-fps30", "jitter-fps40", "jitter-fps45"]
            if runtime_settings else [])
        for scenario in jitter_rooms:
            if float(on_runs[scenario][0]["ups"]) < \
                    float(off_runs[scenario][0]["ups"]) + 0.3:
                raise AssertionError(
                    f"ON gained under 0.3 UPS in {name}/{scenario}")
        if int(on_runs["slack"][0]["presents"]) <= \
                int(off_runs["slack"][0]["presents"]):
            raise AssertionError(f"ON+slack admitted nothing more ({name})")
        for scenario in ("refuse", "heavy"):
            for field in ("updates", "presents", "ticks", "drop"):
                if on_runs[scenario][0][field] != off_runs[scenario][0][field]:
                    raise AssertionError(
                        f"ON changed {name}/{scenario} {field}")
        if FLOOR_DEBT_GAP_TEXT not in on.stdout or \
                FLOOR_DEBT_GAP_TEXT in off or \
                "<=31 Game update starts per sliding second (floor debt on)" \
                not in on.stdout:
            raise AssertionError(f"ON start-contract PASS text ({name})")
        pinned_on = ("Vita cadence30 Present-wait costs (service reserve, "
                     "floor-debt on cap 8000us admit-slack 2000us): "
                     "pinned 39 cases")
        if service_reserve != (pinned_on in on.stdout):
            raise AssertionError(f"ON pinned Present-wait table ({name})")
        report.append(
            f"[floor-debt {name}] OFF==OBSERVE in {len(off_runs)} scenarios; "
            f"ON jitter {off_runs['jitter'][0]['ups']} -> "
            f"{on_runs['jitter'][0]['ups']} UPS, jitter-wide "
            f"{off_runs['jitter-wide'][0]['ups']} -> "
            f"{on_runs['jitter-wide'][0]['ups']} UPS, slack presents "
            f"{off_runs['slack'][0]['presents']} -> "
            f"{on_runs['slack'][0]['presents']}"
            + ("".join(
                f", fps{fps} {off_runs[f'jitter-fps{fps}'][0]['ups']} -> "
                f"{on_runs[f'jitter-fps{fps}'][0]['ups']}"
                for fps in (30, 40, 45)) if runtime_settings else ""))
    for line in done["on-reserve-duty"].stdout.splitlines():
        if "Present-wait costs" in line:
            report.append(line)

    # Configuration errors are compile errors in the unit itself.
    negative_compiles = (
        (("-DISAAC_VITA_FULLSPEED_ADMIT_SLACK_US=2000",),
         "admit slack without floor debt lowers UPS"),
        (("-DISAAC_VITA_FULLSPEED_FLOOR_DEBT=2",
          "-DISAAC_VITA_FULLSPEED_FLOOR_DEBT_CAP_US=8000",
          "-DISAAC_VITA_FULLSPEED_ADMIT_SLACK_US=8000"),
         "admit slack must be smaller than the floor debt cap"),
        (("-DISAAC_VITA_FULLSPEED_FLOOR_DEBT=2",
          "-DISAAC_VITA_FULLSPEED_FLOOR_DEBT_CAP_US=16667"),
         "floor debt cap must be 1..16666 us"),
        (("-DISAAC_VITA_FULLSPEED_FLOOR_DEBT=1",),
         "floor debt cap must be 1..16666 us"),
        (("-DISAAC_VITA_FULLSPEED_FLOOR_DEBT=3",
          "-DISAAC_VITA_FULLSPEED_FLOOR_DEBT_CAP_US=8000"),
         "ISAAC_VITA_FULLSPEED_FLOOR_DEBT must be 1 (OBSERVE) or 2 (ON)"),
        (("-DISAAC_VITA_FULLSPEED_SCHEDULER=1",
          "-DISAAC_VITA_FULLSPEED_SERVICE_RESERVE=1",
          "-DISAAC_VITA_FULLSPEED_HEAD_ADVANCE=1",
          "-DISAAC_VITA_FULLSPEED_FLOOR_DEBT=1",
          "-DISAAC_VITA_FULLSPEED_FLOOR_DEBT_CAP_US=8000"),
         "floor debt and full-head advance are not proven together"),
    )
    for flags, message in negative_compiles:
        result = run_status(common + list(flags) + [
            "-fsyntax-only", str(runtime / "kage_vita_fullspeed_scheduler.c")])
        if result.returncode == 0 or message not in result.stderr:
            raise AssertionError(
                f"floor-debt config {flags} not refused with {message!r}: "
                f"{result.stderr!r}")
    report.append(f"Floor-debt configuration errors: PASS "
                  f"({len(negative_compiles)} refused at compile time)")

    # An explicit FLOOR_DEBT=0 (with the CMake cap/slack defaults) emits the
    # same code as no define, in both units the knob reaches.
    for unit, flags in (
            ("kage_vita_fullspeed_scheduler.c",
             ("-DISAAC_VITA_FULLSPEED_SCHEDULER=1",
              "-DISAAC_VITA_FULLSPEED_SERVICE_RESERVE=1")),
            ("kage_vita_phase_profile.c",
             # The host can compile the profiler only in its oracle shape.
             ("-DISAAC_KAGE_VITA_PHASE_PROFILE_ORACLE=1",
              "-DISAAC_VITA_PHASE_PROFILE=1",
              "-DISAAC_VITA_FULLSPEED_SCHEDULER=1"))):
        assembly = []
        for zero in (False, True):
            target = output / f"floor-debt-zero-{unit}-{int(zero)}.s"
            extra = ["-DISAAC_VITA_FULLSPEED_FLOOR_DEBT=0",
                     "-DISAAC_VITA_FULLSPEED_FLOOR_DEBT_CAP_US=8000",
                     "-DISAAC_VITA_FULLSPEED_ADMIT_SLACK_US=0"] if zero else []
            run(common + list(flags) + extra + [
                "-S", str(runtime / unit), "-o", str(target)])
            assembly.append(target.read_bytes())
        if assembly[0] != assembly[1]:
            raise AssertionError(f"FLOOR_DEBT=0 changes emitted code: {unit}")
    report.append("Floor-debt OFF emitted-code identity (=0 vs absent, "
                  "scheduler + phase profile): PASS")

    # The oversleep model (constant Render, no lateness below one tick that
    # OBSERVE could act on) is identical OFF/OBSERVE.
    models = []
    for mode in ("off", "observe"):
        exe = output / f"overshoot-floor-debt-{mode}.exe"
        flags = ["-DISAAC_VITA_FULLSPEED_SCHEDULER=1"] + (
            ["-DISAAC_VITA_FULLSPEED_FLOOR_DEBT=1"] + list(FLOOR_DEBT_RECIPE)
            if mode == "observe" else [])
        run(common + flags + [
            str(runtime / "kage_vita_fullspeed_scheduler.c"),
            str(vita / "host_tests" / "kage_vita_fullspeed_overshoot_model.c"),
            "-o", str(exe)])
        models.append(exe)
    for render in (7400, 12000, 20000, 34000, 55000):
        for overshoot in (180, 900):
            for offset in (0, 15000):
                args = ["200000", str(overshoot), str(render), str(offset)]
                if run([str(models[0])] + args) != run([str(models[1])] + args):
                    raise AssertionError(
                        "floor-debt OBSERVE altered the oversleep model")
    report.append("Floor-debt OBSERVE oversleep model identical to OFF "
                  "(20 cases): PASS")

    # Phase integration: ph120.fd/ph120.fs after ph120.w, the attribution
    # identity exact in OBSERVE and within the cap under ON.
    phase_rates = {}
    phase_off_rate = re.search(r"window-2 game-rate=(\d+\.\d{3}) Hz",
                               phase_off_stdout)
    if phase_off_rate is None:
        raise AssertionError("phase oracle lost its game-rate field")
    for mode, cache in ((1, False), (2, False), (1, True)):
        exe = output / f"phase-floor-debt-{mode}-{int(cache)}.exe"
        command = common + [
            "-DISAAC_KAGE_VITA_PHASE_PROFILE_ORACLE=1",
            "-DISAAC_VITA_PHASE_PROFILE=1",
            "-DISAAC_VITA_FULLSPEED_SCHEDULER=1",
            f"-DISAAC_VITA_FULLSPEED_FLOOR_DEBT={mode}",
            *FLOOR_DEBT_RECIPE,
            '-DISAAC_VITA_PHASE_PROFILE_BUILD_ID="fullspeed:phase-oracle"',
            '-DISAAC_VITA_FULLSPEED_SCHEDULER_BUILD_ID="fullspeed:phase-oracle"',
        ] + (["-DISAAC_VITA_GUEST_LOOKUP_CACHE=1"] if cache else []) + [
            str(runtime / "kage_vita_fullspeed_scheduler.c"),
            str(runtime / "kage_vita_phase_profile.c"),
            str(runtime / "kage_vita_fullspeed_phase_oracle.c"),
            "-o", str(exe),
        ]
        run(command)
        stdout = run([str(exe)])
        records = ("records=9; ordered/bounded t,c,a,g,s,r,w,fd,fs "
                   "(fd attribution exact; fs floor stamp exact)" if cache else
                   "records=8; ordered/bounded t,c,a,s,r,w,fd,fs "
                   "(fd attribution exact; fs floor stamp exact)")
        rate = re.search(r"window-2 game-rate=(\d+\.\d{3}) Hz", stdout)
        if "Vita cadence30/phase integration oracle: PASS" not in stdout or \
                records not in stdout or rate is None:
            raise AssertionError(f"floor-debt phase oracle: {stdout!r}")
        phase_rates[(mode, cache)] = rate.group(1)
    if phase_rates[(1, False)] != phase_off_rate.group(1) or \
            phase_rates[(1, True)] != phase_off_rate.group(1) or \
            float(phase_rates[(2, False)]) <= float(phase_off_rate.group(1)):
        raise AssertionError(f"floor-debt phase game rates: {phase_rates!r}")
    report.append(
        "Floor-debt phase integration: PASS (records=8/9 with fd,fs after w; "
        f"window-2 game-rate OFF {phase_off_rate.group(1)} = OBSERVE "
        f"{phase_rates[(1, False)]}, ON {phase_rates[(2, False)]} Hz)")
    return report


def cmake_program() -> str | None:
    found = shutil.which("cmake")
    if found:
        return found
    candidate = Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / \
        "CMake" / "bin" / "cmake.exe"
    return str(candidate) if candidate.is_file() else None


def verify_floor_debt_cmake(root: Path, output: Path) -> str:
    """Run the CMake option block for S5 itself (cmake -P on the extracted
    lines, cache sets turned into defaults) against valid and refused
    values: the mode it derives and the FATAL it raises."""
    cmake = cmake_program()
    if cmake is None:
        return "Floor-debt CMake option block: skipped (no cmake)"
    text = (root / "recomp" / "vita" / "CMakeLists.txt").read_text(
        encoding="utf-8")
    start = text.index("# S5 floor debt (ISAAC_VITA_FULLSPEED_FLOOR_DEBT):")
    end = text.index("option(ISAAC_VITA_STABLE_30_PRESENTATION", start)
    # The GL admit wait block (R60-DRYADMIT-SPEC-20261028.md 2.1) follows S5
    # because it reads the floor-debt mode: it has its own leg below.
    admit_wait = text.find(ADMIT_WAIT_CMAKE_MARK, start, end)
    if admit_wait >= 0:
        end = admit_wait
    block, count = re.subn(
        r'set\((ISAAC_VITA_\w+) (\S+) CACHE STRING\s+"[^"]*"\)',
        r"if(NOT DEFINED \1)\n  set(\1 \2)\nendif()", text[start:end])
    if count != 3:
        raise AssertionError(f"S5 CMake block has {count} cache strings, not 3")
    script = output / "floor-debt-options.cmake"
    script.write_text(
        block + '\nmessage(STATUS "floor-debt mode=${ISAAC_VITA_FULLSPEED_FLOOR_DEBT_MODE}")\n',
        encoding="utf-8")
    base = {"ISAAC_VITA_FULLSPEED_SCHEDULER": "ON",
            "ISAAC_VITA_FULLSPEED_HEAD_ADVANCE": "OFF"}
    cases = (
        ({}, "mode=0"),
        ({"ISAAC_VITA_FULLSPEED_FLOOR_DEBT": "OBSERVE"}, "mode=1"),
        ({"ISAAC_VITA_FULLSPEED_FLOOR_DEBT": "on"}, "mode=2"),
        ({"ISAAC_VITA_FULLSPEED_FLOOR_DEBT": "ON",
          "ISAAC_VITA_FULLSPEED_FLOOR_DEBT_CAP_US": "16666",
          "ISAAC_VITA_FULLSPEED_ADMIT_SLACK_US": "2000"}, "mode=2"),
        ({"ISAAC_VITA_FULLSPEED_FLOOR_DEBT": "1"}, "must be OFF, OBSERVE or ON"),
        ({"ISAAC_VITA_FULLSPEED_FLOOR_DEBT_CAP_US": "0"}, "must be an integer 1..16666"),
        ({"ISAAC_VITA_FULLSPEED_FLOOR_DEBT_CAP_US": "16667"}, "must be an integer 1..16666"),
        ({"ISAAC_VITA_FULLSPEED_ADMIT_SLACK_US": "-1"}, "must be a non-negative integer"),
        ({"ISAAC_VITA_FULLSPEED_ADMIT_SLACK_US": "2000"},
         "requires ISAAC_VITA_FULLSPEED_FLOOR_DEBT=OBSERVE or ON"),
        ({"ISAAC_VITA_FULLSPEED_FLOOR_DEBT": "ON",
          "ISAAC_VITA_FULLSPEED_ADMIT_SLACK_US": "8000"},
         "must be below ISAAC_VITA_FULLSPEED_FLOOR_DEBT_CAP_US"),
        ({"ISAAC_VITA_FULLSPEED_FLOOR_DEBT": "OBSERVE",
          "ISAAC_VITA_FULLSPEED_SCHEDULER": "OFF"},
         "requires ISAAC_VITA_FULLSPEED_SCHEDULER=ON"),
        ({"ISAAC_VITA_FULLSPEED_FLOOR_DEBT": "ON",
          "ISAAC_VITA_FULLSPEED_HEAD_ADVANCE": "ON"}, "are exclusive"),
    )
    for values, expected in cases:
        merged = dict(base, **values)
        result = run_status([cmake] + [f"-D{k}={v}" for k, v in merged.items()]
                            + ["-P", str(script)])
        # CMake wraps a long message: compare with whitespace collapsed.
        text_out = " ".join((result.stdout + result.stderr).split())
        fatal = expected.startswith("mode=") is False
        if expected not in text_out or (result.returncode != 0) != fatal:
            raise AssertionError(
                f"S5 CMake block with {values}: rc={result.returncode}, "
                f"expected {expected!r}:\n{text_out}")
    # The cap/slack refusals above run with the knob at its default OFF: both
    # values are validated in every mode (a bad value is FATAL even with
    # FLOOR_DEBT=OFF), which the next count pins.
    off_refusals = sum(1 for values, expected in cases
                       if "ISAAC_VITA_FULLSPEED_FLOOR_DEBT" not in values
                       and not expected.startswith("mode="))
    if off_refusals != 4:
        raise AssertionError(f"S5 CMake block: {off_refusals} knob-OFF refusals, not 4")
    return (f"Floor-debt CMake option block: PASS (cmake -P, {len(cases)} cases: "
            "default OFF, OBSERVE/ON/on accepted, 8 refusals, 4 of them with the "
            "knob OFF: bad cap 0/16667, slack -1, slack 2000)")


ADMIT_BEHIND_CMAKE_MARK = "# R60-FABLE-PLAN.md lane B: with the GL render thread a frame behind"


# The revision the knob sits on: each production unit with every knob block
# removed must be its text. The knob was written on 22327fa (r60-tonight-
# 20261106 HEAD) and merged onto the 0.2.0 base 66ce1c8 (020-gc-20261112)
# at 7d23e50; that base changed kage_vita_backend.c, the scheduler and the
# phase profile outside the knob (FBO deferred attach, Lua GC idle query,
# GXM RT census), so the OFF reading is the merge's first parent, not 22327fa.
ADMIT_BEHIND_BASE = "66ce1c896fa52cd1d9d62b8047e5f3ab5d78f775"
ADMIT_BEHIND_GUARD = re.compile(
    r"^\s*#\s*if\s+defined\((ISAAC_VITA_GL_THREAD_ADMIT_BEHIND|KAGE_VITA_GL_ADMIT_BEHIND)\)"
    r"(\s*&&\s*(ISAAC_VITA_GL_THREAD_ADMIT_BEHIND|KAGE_VITA_GL_ADMIT_BEHIND)(\s*==\s*2)?)?\s*$")
ADMIT_BEHIND_STOCK_GATE = " && !defined(KAGE_VITA_GL_ADMIT_BEHIND)"
ADMIT_BEHIND_UNITS = (
    "gl_record_backend.c", "gl_record_backend.h", "kage_vita_backend.c",
    "kage_vita_backend.h", "kage_vita_fullspeed_scheduler.c",
    "kage_vita_fullspeed_scheduler.h", "kage_vita_phase_profile.c")


# ISAAC_VITA_UPDATE_HITCH_PROBE (020-fba-20261112 via the IMG/hitch merge,
# int-020 c0dea61) sits in two of these units in whole blocks of its own, no
# #else arm; its own OFF proof is vita/test_kage_vita_update_hitch.py and the
# box whole-tree OFF identity.  Taken out first, the count pinned per unit.
HITCH_PROBE_GUARD = re.compile(r"^\s*#\s*if\s+defined\(ISAAC_VITA_UPDATE_HITCH_PROBE\)\s*$")
HITCH_PROBE_BLOCKS = {
    "kage_vita_fullspeed_scheduler.c": 3,
    "kage_vita_phase_profile.c": 1,
}


def strip_hitch_probe_blocks(text: str) -> tuple[str, int]:
    """`text` with every outermost ISAAC_VITA_UPDATE_HITCH_PROBE block
    removed; an #else/#elif arm on one is refused, never guessed.  A block
    inserted between two blank lines brought its own separator (the one in
    kage_vita_phase_profile.c did): the blank line after it goes too."""
    out, depth, blocks, eat_blank = [], 0, 0, False
    for line in text.splitlines(keepends=True):
        if not depth:
            if eat_blank and not line.strip():
                eat_blank = False
                continue
            eat_blank = False
            if HITCH_PROBE_GUARD.match(line):
                depth, blocks = 1, blocks + 1
                continue
            out.append(line)
            continue
        if re.match(r"\s*#\s*if", line):
            depth += 1
        elif re.match(r"\s*#\s*endif", line):
            depth -= 1
            if not depth:
                eat_blank = bool(out) and not out[-1].strip()
        elif depth == 1 and re.match(r"\s*#\s*(else|elif)", line):
            raise AssertionError("#else on an update hitch probe block is not modelled")
    if depth:
        raise AssertionError("unterminated update hitch probe block")
    return "".join(out), blocks


# ISAAC_VITA_GL_DRAW_PACKET (ROAD60-SYNTHESIS.md lane C, 020-drawpkt-
# 20261112 on int-020 d874a40) sits in three of these units, every edit a
# whole `#if defined(ISAAC_VITA_GL_DRAW_PACKET)` block, some with an #else arm
# that carries the base lines verbatim; its own OFF proofs are
# vita/test_gl_draw_packet.py and the box whole-tree OFF identity.  Read OFF
# first (#else arms kept), the count pinned per unit.
DRAW_PACKET_GUARD = re.compile(r"^\s*#\s*if\s+defined\(ISAAC_VITA_GL_DRAW_PACKET\)\s*$")
DRAW_PACKET_BLOCKS = {
    "gl_record_backend.c": 14,
    "gl_record_backend.h": 2,
    "kage_vita_phase_profile.c": 4,
}


def strip_admit_behind_blocks(text: str, guard: re.Pattern = ADMIT_BEHIND_GUARD,
                              what: str = "GL admit behind") -> tuple[str, int]:
    """`text` as the preprocessor reads it with the knob undefined: every
    outermost `#if defined(<knob>)[ && <knob>[ == 2]]` block removed, its
    #else arm kept; (text, blocks).  `guard`/`what`: another knob read the
    same way (the draw packet)."""
    out: list[str] = []
    stack: list[str] = []
    blocks = 0
    for line in text.splitlines(keepends=True):
        emit = "knob-if" not in stack
        directive = PP_DIRECTIVE.match(line)
        if directive:
            kind = directive.group(1)
            if kind in ("if", "ifdef", "ifndef"):
                if emit and guard.match(line):
                    stack.append("knob-if")
                    blocks += 1
                    continue
                stack.append("other")
            elif kind == "elif" and stack and stack[-1].startswith("knob"):
                raise AssertionError(f"#elif on a {what} block is not modelled")
            elif kind == "else" and stack and stack[-1] == "knob-if":
                stack[-1] = "knob-else"
                continue
            elif kind == "endif":
                if not stack:
                    raise AssertionError("unbalanced #endif")
                if stack.pop().startswith("knob"):
                    continue
        if emit:
            out.append(line)
    if stack:
        raise AssertionError(f"unterminated conditional: {stack}")
    return "".join(out), blocks


def verify_admit_behind_off_reading(root: Path) -> str:
    """Knob OFF defines nothing, so every production unit must read as the
    base revision: with the knob blocks removed the file is the base file
    byte for byte, and no other line of it names the knob."""
    runtime = root / "recomp" / "runtime"
    counts = []
    for unit in ADMIT_BEHIND_UNITS:
        text = (runtime / unit).read_text(encoding="utf-8").replace("\r\n", "\n")
        base = git_show(root, ADMIT_BEHIND_BASE, f"recomp/runtime/{unit}")
        if base is None:
            return ("GL admit behind OFF reading: skipped (base revision "
                    f"{ADMIT_BEHIND_BASE[:7]} not in this repository)")
        text, draw_packet = strip_admit_behind_blocks(text, DRAW_PACKET_GUARD, "GL draw packet")
        if draw_packet != DRAW_PACKET_BLOCKS.get(unit, 0):
            raise AssertionError(f"{unit}: {draw_packet} GL draw packet blocks, not "
                                 f"{DRAW_PACKET_BLOCKS.get(unit, 0)}")
        if "DRAW_PACKET" in text or "draw_packet" in text:
            raise AssertionError(f"{unit}: a GL draw packet name outside its blocks")
        text, hitch = strip_hitch_probe_blocks(text)
        if hitch != HITCH_PROBE_BLOCKS.get(unit, 0):
            raise AssertionError(f"{unit}: {hitch} update hitch probe blocks, not "
                                 f"{HITCH_PROBE_BLOCKS.get(unit, 0)}")
        stripped, blocks = strip_admit_behind_blocks(text)
        if not blocks:
            raise AssertionError(f"{unit}: no GL admit behind block")
        # The one edit that is not a block: the stock behind-refusal's own
        # guard gained "&& !defined(<knob>)", true with the knob undefined
        # (the knob block below the reserve takes the refusal over).
        excluded = stripped.count(ADMIT_BEHIND_STOCK_GATE)
        if excluded != (1 if unit == "kage_vita_fullspeed_scheduler.c" else 0):
            raise AssertionError(f"{unit}: {excluded} stock-gate exclusions")
        stripped = stripped.replace(ADMIT_BEHIND_STOCK_GATE, "")
        if stripped != base:
            mine, theirs = stripped.splitlines(), base.splitlines()
            first = next((i for i, (a, b) in enumerate(zip(mine, theirs)) if a != b),
                         min(len(mine), len(theirs)))
            raise AssertionError(
                f"{unit}: OFF reading differs from {ADMIT_BEHIND_BASE[:7]} at line "
                f"{first + 1}: {mine[first:first + 1]} vs {theirs[first:first + 1]}")
        for needle in ("ADMIT_BEHIND", "admit_behind", "worker_outstanding"):
            if needle in stripped:
                raise AssertionError(f"{unit}: {needle!r} outside a knob block")
        counts.append(f"{unit} {blocks}")
    # The self-test: a line edited outside a knob block must be caught.
    probe = (runtime / "kage_vita_fullspeed_scheduler.c").read_text(
        encoding="utf-8").replace("\r\n", "\n")
    planted = probe.replace("\n#include ", "\n#include  ", 1)
    if planted == probe or strip_admit_behind_blocks(
            strip_hitch_probe_blocks(planted)[0])[0] == git_show(
            root, ADMIT_BEHIND_BASE, "recomp/runtime/kage_vita_fullspeed_scheduler.c"):
        raise AssertionError("GL admit behind OFF reading: the planted edit went unseen")
    untouched = [name for name in ("gl_record_ring.c", "gl_record_ring.h")
                 if (runtime / name).read_text(encoding="utf-8").replace("\r\n", "\n")
                 != git_show(root, ADMIT_BEHIND_BASE, f"recomp/runtime/{name}")]
    if untouched:
        raise AssertionError(f"GL admit behind touched {untouched}")
    return ("GL admit behind OFF reading: PASS (GL draw packet blocks "
            f"({sum(DRAW_PACKET_BLOCKS.values())}, #else arms kept), update hitch probe blocks "
            f"({sum(HITCH_PROBE_BLOCKS.values())}) and knob blocks removed, #else arms kept == "
            f"{ADMIT_BEHIND_BASE[:7]} byte for byte but the stock gate guard, which gained "
            "\"&& !defined(KAGE_VITA_GL_ADMIT_BEHIND)\"; no other knob name outside a block; blocks: "
            + ", ".join(counts) + "; gl_record_ring.c/.h == base; a planted edit "
            "outside a block is caught)")


def verify_admit_behind_cmake(root: Path, output: Path) -> str:
    """Run the ISAAC_VITA_GL_THREAD_ADMIT_BEHIND option block itself (cmake
    -P on the extracted lines, the two cache sets turned into defaults)
    against valid and refused values, as the admit-wait leg does."""
    cmake = cmake_program()
    if cmake is None:
        return "GL admit behind CMake option block: skipped (no cmake)"
    text = (root / "recomp" / "vita" / "CMakeLists.txt").read_text(
        encoding="utf-8")
    start = text.index(ADMIT_BEHIND_CMAKE_MARK)
    end = text.index("option(ISAAC_VITA_STABLE_30_PRESENTATION", start)
    block, count = re.subn(
        r'set\((ISAAC_VITA_\w+) (\S+) CACHE STRING\s+"[^"]*"\)',
        r"if(NOT DEFINED \1)\n  set(\1 \2)\nendif()", text[start:end])
    block = block.replace(
        "set_property(CACHE ISAAC_VITA_GL_THREAD_ADMIT_BEHIND PROPERTY STRINGS "
        "OFF OBSERVE ON)\n", "")
    if count != 2 or "set_property(CACHE" in block:
        raise AssertionError(f"GL admit behind CMake block: {count} cache strings")
    script = output / "admit-behind-options.cmake"
    script.write_text(
        block + '\nmessage(STATUS "admit-behind mode=${ISAAC_VITA_GL_THREAD_ADMIT_BEHIND_MODE} '
        'bytes=${ISAAC_VITA_GL_THREAD_ADMIT_BEHIND_BYTES}.")\n',
        encoding="utf-8")
    base = {"ISAAC_VITA_GL_THREAD_RENDER_ADMISSION": "ON",
            "ISAAC_VITA_PHASE_PROFILE": "ON",
            "ISAAC_VITA_GL_THREAD_ADMIT_WAIT_MODE": "0",
            "ISAAC_VITA_FULLSPEED_HEAD_ADVANCE": "OFF",
            "ISAAC_VITA_FULLSPEED_RENDER_COST_EXCLUDES_PRESENT": "OFF",
            "ISAAC_VITA_FULLSPEED_FLOOR_DEBT_MODE": "0"}
    knob = "ISAAC_VITA_GL_THREAD_ADMIT_BEHIND"
    proven = "is proven only with stock render cost"
    decimal = "must be a decimal 1..524288"
    cases = (
        ({}, "mode=0 bytes=65536."),
        ({"ISAAC_VITA_GL_THREAD_RENDER_ADMISSION": "OFF"}, "mode=0 bytes=65536."),
        ({"ISAAC_VITA_PHASE_PROFILE": "OFF"}, "mode=0 bytes=65536."),
        # OFF reads nothing else: not the bound, not the other knobs.
        ({knob + "_BYTES": "abc", "ISAAC_VITA_GL_THREAD_ADMIT_WAIT_MODE": "2"},
         "mode=0 bytes=abc."),
        ({knob: "OBSERVE"}, "mode=1 bytes=65536."),
        ({knob: "on", knob + "_BYTES": "1"}, "mode=2 bytes=1."),
        ({knob: "ON", knob + "_BYTES": "524288",
          "ISAAC_VITA_FULLSPEED_FLOOR_DEBT_MODE": "1"}, "mode=2 bytes=524288."),
        ({knob: "2"}, "must be OFF, OBSERVE or ON"),
        ({knob: "ON", knob + "_BYTES": "0"}, decimal),
        ({knob: "ON", knob + "_BYTES": "524289"}, decimal),
        ({knob: "OBSERVE", knob + "_BYTES": "1e3"}, decimal),
        ({knob: "ON", knob + "_BYTES": "0x100"}, decimal),
        ({knob: "ON", knob + "_BYTES": "-5"}, decimal),
        ({knob: "ON", knob + "_BYTES": "065536"}, decimal),
        ({knob: "ON", knob + "_BYTES": "1000000000000"}, decimal),
        ({knob: "ON", knob + "_BYTES": ""}, decimal),
        ({knob: "OBSERVE", "ISAAC_VITA_GL_THREAD_RENDER_ADMISSION": "OFF"},
         "requires ISAAC_VITA_GL_THREAD_RENDER_ADMISSION=ON"),
        ({knob: "OBSERVE", "ISAAC_VITA_PHASE_PROFILE": "OFF"},
         "=OBSERVE requires ISAAC_VITA_PHASE_PROFILE=ON"),
        # ON is a release lever: without the profile it loses only the records.
        ({knob: "ON", "ISAAC_VITA_PHASE_PROFILE": "OFF"}, "mode=2 bytes=65536."),
        ({knob: "ON", "ISAAC_VITA_GL_THREAD_ADMIT_WAIT_MODE": "1"},
         "each own the behind arm of the gate"),
        ({knob: "ON", "ISAAC_VITA_FULLSPEED_HEAD_ADVANCE": "ON"}, proven),
        ({knob: "ON", "ISAAC_VITA_FULLSPEED_RENDER_COST_EXCLUDES_PRESENT": "ON"},
         proven),
        ({knob: "OBSERVE", "ISAAC_VITA_FULLSPEED_FLOOR_DEBT_MODE": "2"}, proven),
    )
    refused = 0
    for values, expected in cases:
        merged = dict(base, **values)
        result = run_status([cmake] + [f"-D{k}={v}" for k, v in merged.items()]
                            + ["-P", str(script)])
        text_out = " ".join((result.stdout + result.stderr).split())
        fatal = not expected.startswith("mode=")
        refused += fatal
        if expected not in text_out or (result.returncode != 0) != fatal:
            raise AssertionError(
                f"GL admit behind CMake block with {values}: rc={result.returncode}, "
                f"expected {expected!r}:\n{text_out}")
    # The definitions statement: both values, the four units, inside the mode test.
    statement = (
        '    if(ISAAC_VITA_GL_THREAD_ADMIT_BEHIND_MODE)\n')
    defs = text.index(statement)
    defs_end = text.index("    endif()\n", defs)
    body = text[defs:defs_end]
    for unit in ("kage_vita_fullspeed_scheduler.c", "kage_vita_backend.c",
                 "gl_record_backend.c", "kage_vita_phase_profile.c"):
        if f'"${{ISAAC_RUNTIME}}/{unit}"' not in body:
            raise AssertionError(f"GL admit behind definitions miss {unit}")
    if "/gl_record_ring.c" in body or \
            "ISAAC_VITA_GL_THREAD_ADMIT_BEHIND=${ISAAC_VITA_GL_THREAD_ADMIT_BEHIND_MODE}\n" not in body or \
            "ISAAC_VITA_GL_THREAD_ADMIT_BEHIND_BYTES=${ISAAC_VITA_GL_THREAD_ADMIT_BEHIND_BYTES})\n" not in body or \
            "bound ${ISAAC_VITA_GL_THREAD_ADMIT_BEHIND_BYTES} outstanding ring bytes" not in body:
        raise AssertionError("GL admit behind definitions statement changed")
    if text.count("ISAAC_VITA_GL_THREAD_ADMIT_BEHIND_MODE)\n      #") != 1:
        raise AssertionError("GL admit behind definitions statement is not unique")
    return (f"GL admit behind CMake option block: PASS (cmake -P, {len(cases)} cases: "
            "default OFF, OFF reads neither the bound nor the other knobs (no "
            "admission, no phase profile, ADMIT_WAIT=ON), "
            f"OBSERVE / on+1 / ON+524288+FLOOR_DEBT=OBSERVE accepted, {refused} "
            "refusals: value 2, bytes 0 / 524289 / 1e3 / 0x100 / -5 / 065536 / "
            "1000000000000 / empty, no admission, OBSERVE without the "
            "phase profile (ON without it accepted), ADMIT_WAIT, head advance, C2, "
            "FLOOR_DEBT=ON; one definitions statement: mode + bytes on the "
            "scheduler, backend, record backend and phase profile, not the ring)")


ADMIT_WAIT_CMAKE_MARK = "# R60-DRYADMIT-SPEC-20261028.md: with the GL render thread"


def verify_admit_wait_cmake(root: Path, output: Path) -> str:
    """Run the ISAAC_VITA_GL_THREAD_ADMIT_WAIT option block itself (cmake -P
    on the extracted lines, the cache set turned into a default) against
    valid and refused values, as the floor-debt leg above does."""
    cmake = cmake_program()
    if cmake is None:
        return "GL admit wait CMake option block: skipped (no cmake)"
    text = (root / "recomp" / "vita" / "CMakeLists.txt").read_text(
        encoding="utf-8")
    start = text.index(ADMIT_WAIT_CMAKE_MARK)
    end = text.index("option(ISAAC_VITA_STABLE_30_PRESENTATION", start)
    # The GL admit behind block (R60-FABLE-PLAN.md lane B) follows this one
    # because it reads this knob's mode: it has its own leg below.
    admit_behind = text.find(ADMIT_BEHIND_CMAKE_MARK, start, end)
    if admit_behind >= 0:
        end = admit_behind
    block, count = re.subn(
        r'set\((ISAAC_VITA_\w+) (\S+) CACHE STRING\s+"[^"]*"\)',
        r"if(NOT DEFINED \1)\n  set(\1 \2)\nendif()", text[start:end])
    block = block.replace(
        "set_property(CACHE ISAAC_VITA_GL_THREAD_ADMIT_WAIT PROPERTY STRINGS "
        "OFF OBSERVE ON)\n", "")
    if count != 1 or "set_property(CACHE" in block:
        raise AssertionError(f"GL admit wait CMake block: {count} cache strings")
    script = output / "admit-wait-options.cmake"
    script.write_text(
        block + '\nmessage(STATUS "admit-wait mode=${ISAAC_VITA_GL_THREAD_ADMIT_WAIT_MODE}")\n',
        encoding="utf-8")
    base = {"ISAAC_VITA_GL_THREAD_RENDER_ADMISSION": "ON",
            "ISAAC_VITA_FULLSPEED_HEAD_ADVANCE": "OFF",
            "ISAAC_VITA_FULLSPEED_RENDER_COST_EXCLUDES_PRESENT": "OFF",
            "ISAAC_VITA_FULLSPEED_FLOOR_DEBT_MODE": "0"}
    proven = "is proven only with stock render cost"
    cases = (
        ({}, "mode=0"),
        ({"ISAAC_VITA_GL_THREAD_RENDER_ADMISSION": "OFF"}, "mode=0"),
        ({"ISAAC_VITA_GL_THREAD_ADMIT_WAIT": "OBSERVE"}, "mode=1"),
        ({"ISAAC_VITA_GL_THREAD_ADMIT_WAIT": "on"}, "mode=2"),
        ({"ISAAC_VITA_GL_THREAD_ADMIT_WAIT": "ON",
          "ISAAC_VITA_FULLSPEED_FLOOR_DEBT_MODE": "1"}, "mode=2"),
        ({"ISAAC_VITA_GL_THREAD_ADMIT_WAIT": "1"}, "must be OFF, OBSERVE or ON"),
        ({"ISAAC_VITA_GL_THREAD_ADMIT_WAIT": "OBSERVE",
          "ISAAC_VITA_GL_THREAD_RENDER_ADMISSION": "OFF"},
         "requires ISAAC_VITA_GL_THREAD_RENDER_ADMISSION=ON"),
        ({"ISAAC_VITA_GL_THREAD_ADMIT_WAIT": "ON",
          "ISAAC_VITA_FULLSPEED_HEAD_ADVANCE": "ON"}, proven),
        ({"ISAAC_VITA_GL_THREAD_ADMIT_WAIT": "ON",
          "ISAAC_VITA_FULLSPEED_RENDER_COST_EXCLUDES_PRESENT": "ON"}, proven),
        ({"ISAAC_VITA_GL_THREAD_ADMIT_WAIT": "OBSERVE",
          "ISAAC_VITA_FULLSPEED_FLOOR_DEBT_MODE": "2"}, proven),
    )
    for values, expected in cases:
        merged = dict(base, **values)
        result = run_status([cmake] + [f"-D{k}={v}" for k, v in merged.items()]
                            + ["-P", str(script)])
        text_out = " ".join((result.stdout + result.stderr).split())
        fatal = not expected.startswith("mode=")
        if expected not in text_out or (result.returncode != 0) != fatal:
            raise AssertionError(
                f"GL admit wait CMake block with {values}: rc={result.returncode}, "
                f"expected {expected!r}:\n{text_out}")
    return (f"GL admit wait CMake option block: PASS (cmake -P, {len(cases)} cases: "
            "default OFF, OFF without admission, OBSERVE/on/ON+FLOOR_DEBT=OBSERVE "
            "accepted, 5 refusals: value 1, no admission, head advance, C2, "
            "FLOOR_DEBT=ON)")


# ISAAC_VITA_GL_RECORD_CONSUMER_TIMEOUT_US (R60-EXT-SYNTH T2,
# R60-EXT-SYNTH.md): the GL render
# thread's (record ring consumer's) idle-wait timeout.  Unset = no definition
# anywhere; set = one definition on gl_record_ring.c, which also prints the
# receipt.  The producer (game thread) waits keep 2000 us.
CONSUMER_TIMEOUT_KNOB = "ISAAC_VITA_GL_RECORD_CONSUMER_TIMEOUT_US"
CONSUMER_TIMEOUT_CMAKE_MARK = (
    "# R60-EXT-SYNTH T2 (R60-EXT-SYNTH.md): the")
CONSUMER_TIMEOUT_GUARD = f"#if defined({CONSUMER_TIMEOUT_KNOB})"
CONSUMER_TIMEOUT_RECEIPT = ("KAGE VITA GL RING: consumer_timeout_us=%u "
                            "producer_timeout_us=2000 record=ph120.rw")
# The revision the knob was written on (r60-tonight-20261106 HEAD before
# it): gl_record_ring.c with every knob block removed must be its text.
CONSUMER_TIMEOUT_BASE = "e74dade8cdaebb413c8600167caba5904bac5239"
GL_RECORD_CMAKE_SECTION = (
    '  if(ISAAC_VITA_GL_THREAD_RECORD)\n    list(APPEND ISAAC_VITA_RUNTIME_SOURCES\n'
    '      "${ISAAC_RUNTIME}/gl_record_backend.c"')
GL_RECORD_CMAKE_SECTION_END = "  if(ISAAC_VITA_GL_OWNER_CHECK)\n"
PP_DIRECTIVE = re.compile(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b")


def strip_knob_blocks(text: str, guard: str) -> tuple[str, int]:
    """`text` with every `guard` block removed and its #else arm kept, i.e.
    what the preprocessor reads with the knob undefined; (text, blocks)."""
    out: list[str] = []
    stack: list[str] = []
    blocks = 0
    for line in text.splitlines(keepends=True):
        emit = "knob-if" not in stack
        if line.strip() == guard:
            stack.append("knob-if")
            blocks += 1
            continue
        directive = PP_DIRECTIVE.match(line)
        if directive:
            kind = directive.group(1)
            if kind in ("if", "ifdef", "ifndef"):
                stack.append("other")
            elif kind == "elif" and stack and stack[-1].startswith("knob"):
                raise AssertionError(f"#elif inside a {guard} block is not modelled")
            elif kind == "else" and stack and stack[-1] == "knob-if":
                stack[-1] = "knob-else"
                continue
            elif kind == "endif":
                if not stack:
                    raise AssertionError("unbalanced #endif")
                if stack.pop().startswith("knob"):
                    continue
        if emit:
            out.append(line)
    if stack:
        raise AssertionError(f"unterminated conditional: {stack}")
    return "".join(out), blocks


def git_show(root: Path, revision: str, path: str) -> str | None:
    result = subprocess.run(["git", "-C", str(root), "show", f"{revision}:{path}"],
                            check=False, capture_output=True)
    if result.returncode != 0:
        return None
    return result.stdout.decode("utf-8").replace("\r\n", "\n")


def preprocess_ring(cc: str, source: Path, runtime: Path, stub: Path,
                    defines: list[str], vita: bool = True) -> subprocess.CompletedProcess:
    """`cc -E -P` of a gl_record_ring.c for the Vita branch (or, vita=False,
    for _WIN32): psp2/windows/string headers stubbed empty, the same stubs on
    both sides of a comparison."""
    flags = ["-E", "-P", "-D__vita__" if vita else "-D_WIN32=1",
             f"-I{stub}", f"-I{runtime}"]
    version = run_status([cc, "--version"])
    if "clang" in (version.stdout + version.stderr).lower():
        # A freestanding ARM target: no predefined _WIN32, no host libc.
        flags = ["--target=armv7a-none-eabi", "-ffreestanding"] + flags
    return run_status([cc] + flags + [f"-D{d}" for d in defines] + [str(source)])


def verify_consumer_timeout_source(root: Path, cc: str, output: Path) -> str:
    """The C side: only gl_record_ring.c reads the knob, every edit sits in a
    knob block (the file without them is the base revision byte for byte), the
    producer keeps its 2000-us wait, the consumer alone takes the knob's
    wait, the receipt exists only with the knob, and the preprocessed unit
    with the knob unset equals the base's for every worker define set."""
    runtime = root / "recomp" / "runtime"
    # Recursive (host_tests/, oracle includes, vendor/, third_party/ too), so
    # "only gl_record_ring.c reads the knob" holds for every C/C++ source
    # under recomp/runtime and recomp/vita, not just their top levels.
    sources = sorted(
        p for folder in (runtime, root / "recomp" / "vita")
        for p in folder.rglob("*")
        if p.is_file() and p.suffix in (".c", ".h", ".inc", ".cpp", ".hpp", ".S"))
    texts = {p.relative_to(root).as_posix(): p.read_text(encoding="utf-8", errors="replace")
             for p in sources}
    readers = sorted(path for path, body in texts.items() if CONSUMER_TIMEOUT_KNOB in body)
    if readers != ["recomp/runtime/gl_record_ring.c"]:
        raise AssertionError(f"{CONSUMER_TIMEOUT_KNOB} read by {readers}")
    if not any(path.startswith("recomp/vita/host_tests/") for path in texts):
        raise AssertionError("reader scan saw no file under recomp/vita/host_tests/: not recursive")
    ring_path = runtime / "gl_record_ring.c"
    ring = ring_path.read_text(encoding="utf-8").replace("\r\n", "\n")
    stripped, blocks = strip_knob_blocks(ring, CONSUMER_TIMEOUT_GUARD)
    if blocks != 4:
        raise AssertionError(f"gl_record_ring.c: {blocks} knob blocks, not 4")
    for needle in (CONSUMER_TIMEOUT_KNOB, "spsc_sleep_os_consumer",
                   "KAGE VITA GL RING", "isaac_vita_log"):
        if needle in stripped:
            raise AssertionError(f"gl_record_ring.c: {needle!r} outside a knob block")
    win32 = ring[ring.index("#if defined(_WIN32)\n#include <windows.h>"):
                 ring.index("#elif defined(__vita__)")]
    vita = ring[ring.index("#elif defined(__vita__)"):
                ring.index('#error "GL record worker: no platform primitives')]
    if CONSUMER_TIMEOUT_KNOB in win32 or "spsc_sleep_os_consumer" in win32:
        raise AssertionError("gl_record_ring.c: the knob reaches the _WIN32 branch")
    stock_sleep = ("static int spsc_sleep_os(intptr_t sema)\n{\n"
                   "    SceUInt timeout = 2000u;\n"
                   "    return sceKernelWaitSema((int)sema, 1, &timeout) >= 0;\n}\n")
    if vita.count(stock_sleep) != 1:
        raise AssertionError("gl_record_ring.c: the __vita__ spsc_sleep_os no longer waits 2000u")
    # Producer waits: append and wait_done on the stock 2000-us sleep, the
    # admit wait's bounded slice (ADMIT_WAIT=ON) on its own; none on the knob.
    producer = sorted(re.findall(r"\bspsc_sleep_os\w*\(ring->producer_sema[,)]", ring))
    if producer != ["spsc_sleep_os(ring->producer_sema)"] * 2 + \
            ["spsc_sleep_os_for(ring->producer_sema,"]:
        raise AssertionError(f"gl_record_ring.c: producer waits {producer}")
    consumer = sorted(re.findall(r"\bspsc_sleep_os\w*\(ring->consumer_sema\)", ring))
    consumer_off = re.findall(r"\bspsc_sleep_os\w*\(ring->consumer_sema\)", stripped)
    if consumer != ["spsc_sleep_os(ring->consumer_sema)",
                    "spsc_sleep_os_consumer(ring->consumer_sema)"] or \
            consumer_off != ["spsc_sleep_os(ring->consumer_sema)"]:
        raise AssertionError(f"gl_record_ring.c: consumer waits {consumer} / unset {consumer_off}")
    if ring.count(CONSUMER_TIMEOUT_RECEIPT) != 1:
        raise AssertionError("gl_record_ring.c: receipt literal count != 1")
    receipt_sites = sorted(path for path, body in texts.items() if "KAGE VITA GL RING:" in body)
    if receipt_sites != ["recomp/runtime/gl_record_ring.c"]:
        raise AssertionError(f"receipt prefix in {receipt_sites}")
    base = git_show(root, CONSUMER_TIMEOUT_BASE, "recomp/runtime/gl_record_ring.c")
    if base is None:
        # No git object (a snapshot, or WSL git on a Windows worktree): the
        # -E legs below compare against the knob-stripped text instead.
        base_name = "the knob-stripped text (git base unavailable: SKIPPED base identity)"
        base = stripped
    else:
        base_name = f"{CONSUMER_TIMEOUT_BASE[:7]}"
        if stripped != base:
            raise AssertionError(
                f"gl_record_ring.c without its knob blocks differs from {base_name}")
    # Preprocessed identity (knob unset) against the base, per worker define
    # set; positive controls: the knob changes the text, and the C guards
    # refuse a bad value, a missing worker and a non-Vita target.
    stub = output / "consumer-timeout-stub"
    (stub / "psp2" / "kernel").mkdir(parents=True, exist_ok=True)
    for header in ("psp2/kernel/threadmgr.h", "string.h", "windows.h"):
        (stub / header).write_text("", encoding="utf-8")
    base_path = output / "consumer-timeout-base" / "gl_record_ring.c"
    base_path.parent.mkdir(parents=True, exist_ok=True)
    base_path.write_text(base, encoding="utf-8", newline="\n")
    worker = "ISAAC_VITA_GL_THREAD_WORKER=1"
    define_sets = ([], [worker], [worker, "ISAAC_VITA_GL_THREAD_ADMIT_WAIT=1"],
                   [worker, "ISAAC_VITA_GL_THREAD_ADMIT_WAIT=2"])
    for defines in define_sets:
        now = preprocess_ring(cc, ring_path, runtime, stub, defines)
        then = preprocess_ring(cc, base_path, runtime, stub, defines)
        if now.returncode != 0 or then.returncode != 0:
            raise AssertionError(f"cc -E {defines}: rc {now.returncode}/{then.returncode}\n"
                                 f"{now.stderr}\n{then.stderr}")
        if now.stdout != then.stdout:
            raise AssertionError(f"cc -E {defines}: knob-unset text differs from the base")
    unset = preprocess_ring(cc, ring_path, runtime, stub, [worker]).stdout
    knob = preprocess_ring(cc, ring_path, runtime, stub, [worker, f"{CONSUMER_TIMEOUT_KNOB}=500"])
    if knob.returncode != 0 or knob.stdout == unset or \
            "SceUInt timeout = (SceUInt)500;" not in knob.stdout or \
            knob.stdout.count("SceUInt timeout = 2000u;") != 1 or \
            '"KAGE VITA GL RING: consumer_timeout_us=%u producer_timeout_us=2000 ' \
            'record=ph120.rw"' not in knob.stdout:
        raise AssertionError(f"cc -E with the knob at 500: rc {knob.returncode}\n{knob.stderr}")
    refusals = (
        ([worker, f"{CONSUMER_TIMEOUT_KNOB}=99"], True, "must be 100..2000"),
        ([worker, f"{CONSUMER_TIMEOUT_KNOB}=2001"], True, "must be 100..2000"),
        ([f"{CONSUMER_TIMEOUT_KNOB}=500"], True, "needs the Vita GL render thread"),
        ([worker, f"{CONSUMER_TIMEOUT_KNOB}=500"], False, "needs the Vita GL render thread"),
    )
    for defines, vita_target, expected in refusals:
        result = preprocess_ring(cc, ring_path, runtime, stub, defines, vita_target)
        if result.returncode == 0 or expected not in result.stderr:
            raise AssertionError(f"cc -E {defines} vita={vita_target}: not refused "
                                 f"({expected!r}):\n{result.stderr}")
    # The _WIN32 (host oracle) reading is untouched: equal to the base's.
    win_now = preprocess_ring(cc, ring_path, runtime, stub, [worker], False)
    win_then = preprocess_ring(cc, base_path, runtime, stub, [worker], False)
    if win_now.returncode != 0 or win_now.stdout != win_then.stdout or \
            "WaitForSingleObject((HANDLE)sema, 1000u)" not in win_now.stdout:
        raise AssertionError(f"cc -E _WIN32: differs from the base or failed:\n{win_now.stderr}")
    return (f"GL record consumer timeout source: PASS (only gl_record_ring.c reads "
            f"{CONSUMER_TIMEOUT_KNOB} or prints the receipt, of {len(texts)} C/C++ "
            "sources under recomp/runtime and recomp/vita, recursive; 4 knob blocks, the file without them == "
            f"{base_name} byte for byte; __vita__ spsc_sleep_os keeps 2000u; "
            "producer waits spsc_sleep_os x2 + spsc_sleep_os_for x1, none on the knob; "
            "consumer wait spsc_sleep_os_consumer only with the knob; receipt only in "
            f"the knob; cc -E knob-unset == base for {len(define_sets)} __vita__ define "
            "sets and the _WIN32 worker reading; 500 -> (SceUInt)500 + receipt; #error on "
            "99, 2001, no worker, _WIN32 target)")


def verify_consumer_timeout_cmake(root: Path, output: Path) -> str:
    """The option block and the GL recorder/worker source-definition section
    run by `cmake -P` (real set_property(SOURCE), read back per unit): unset
    defines nothing on any unit, a value adds the one definition to
    gl_record_ring.c and leaves every other unit's list unchanged; bad values
    and a value without the worker are FATAL.  Plus a whole-file scope check:
    no other CMake statement names the knob."""
    cmake = cmake_program()
    if cmake is None:
        return "GL record consumer timeout CMake: skipped (no cmake)"
    text = (root / "recomp" / "vita" / "CMakeLists.txt").read_text(encoding="utf-8")
    start = text.index(CONSUMER_TIMEOUT_CMAKE_MARK)
    option_end = text.index("# Sites that call vitaGL or read its state", start)
    if text.index("option(ISAAC_VITA_GL_THREAD_WORKER\n") > start:
        raise AssertionError("consumer timeout option block precedes option(GL_THREAD_WORKER)")
    options, count = re.subn(
        r'set\((ISAAC_VITA_\w+) ("[^"]*"|\S+) CACHE STRING\s+"[^"]*"\)',
        r"if(NOT DEFINED \1)\n  set(\1 \2)\nendif()", text[start:option_end])
    if count != 1 or "CACHE" in options:
        raise AssertionError(f"consumer timeout option block: {count} cache strings")
    section_start = text.index(GL_RECORD_CMAKE_SECTION)
    section_end = text.index(GL_RECORD_CMAKE_SECTION_END, section_start)
    section = text[section_start:section_end]
    worker_at = section.index("  if(ISAAC_VITA_GL_THREAD_WORKER)\n")
    define_at = section.index(f'    if(NOT "${{{CONSUMER_TIMEOUT_KNOB}}}" STREQUAL "")\n')
    define_end = section.index("\n    endif()\n", define_at) + len("\n    endif()\n")
    define_block = section[define_at:define_end]
    if not worker_at < define_at < section.index('message(STATUS "Isaac Vita GL render thread: ON'):
        raise AssertionError("consumer timeout define block is not inside if(GL_THREAD_WORKER)")
    # Whole-file scope: the knob appears only in the option block and in
    # the define block, which has one set_property naming one unit.
    outside = (text[:start] + text[option_end:section_start + define_at] +
               text[section_start + define_end:])
    if CONSUMER_TIMEOUT_KNOB in outside:
        raise AssertionError("CMakeLists.txt names the consumer timeout knob outside its blocks")
    sets = re.findall(r"set_property\(SOURCE[^)]*\)", define_block)
    if len(sets) != 1 or re.findall(r'"\$\{ISAAC_RUNTIME\}/([^"]+)"', sets[0]) != \
            ["gl_record_ring.c"]:
        raise AssertionError(f"consumer timeout define block: {sets}")
    units = sorted(set(re.findall(r'"\$\{ISAAC_RUNTIME\}/([A-Za-z0-9_]+\.c)"', section)))
    tail = "\n".join(
        f'get_property(d SOURCE "${{ISAAC_RUNTIME}}/{unit}" PROPERTY COMPILE_DEFINITIONS)\n'
        f'message(STATUS "SIM defs {unit} = ${{d}}")' for unit in units)
    script = output / "consumer-timeout-options.cmake"
    script.write_text(
        'set(ISAAC_RUNTIME "/rt")\nset(ISAAC_VITA_RUNTIME_SOURCES "")\n' +
        options + "\n" + section + "\n" + tail + "\n", encoding="utf-8")
    base = {"ISAAC_VITA_GL_THREAD_RECORD": "ON", "ISAAC_VITA_GL_THREAD_WORKER": "ON",
            "ISAAC_VITA_GL_THREAD_WORKER_PROOF": "OFF",
            "ISAAC_VITA_GL_THREAD_RENDER_ADMISSION": "ON", "ISAAC_VITA_IMAGE_RETAIN": "ON"}
    bad_value = "must be empty or a decimal integer 100..2000"
    no_worker = "requires ISAAC_VITA_GL_THREAD_WORKER=ON"
    cases = (
        ("unset", {}, None),
        ("empty", {CONSUMER_TIMEOUT_KNOB: ""}, None),
        ("500", {CONSUMER_TIMEOUT_KNOB: "500"}, "500"),
        ("100", {CONSUMER_TIMEOUT_KNOB: "100"}, "100"),
        ("2000", {CONSUMER_TIMEOUT_KNOB: "2000"}, "2000"),
        ("250+admit-wait-on", {CONSUMER_TIMEOUT_KNOB: "250",
                               "ISAAC_VITA_GL_THREAD_ADMIT_WAIT_MODE": "2"}, "250"),
        ("unset+admit-wait-on", {"ISAAC_VITA_GL_THREAD_ADMIT_WAIT_MODE": "2"}, None),
        ("no-worker-unset", {"ISAAC_VITA_GL_THREAD_WORKER": "OFF",
                             "ISAAC_VITA_GL_THREAD_RENDER_ADMISSION": "OFF"}, None),
        ("99", {CONSUMER_TIMEOUT_KNOB: "99"}, bad_value),
        ("2001", {CONSUMER_TIMEOUT_KNOB: "2001"}, bad_value),
        ("abc", {CONSUMER_TIMEOUT_KNOB: "abc"}, bad_value),
        ("0500", {CONSUMER_TIMEOUT_KNOB: "0500"}, bad_value),
        ("-1", {CONSUMER_TIMEOUT_KNOB: "-1"}, bad_value),
        ("1e3", {CONSUMER_TIMEOUT_KNOB: "1e3"}, bad_value),
        ("1e20-digits", {CONSUMER_TIMEOUT_KNOB: "100000000000000000000"}, bad_value),
        ("no-worker-500", {CONSUMER_TIMEOUT_KNOB: "500", "ISAAC_VITA_GL_THREAD_WORKER": "OFF",
                           "ISAAC_VITA_GL_THREAD_RENDER_ADMISSION": "OFF"}, no_worker),
    )
    results: dict[str, dict[str, list[str]]] = {}
    for name, values, expected in cases:
        merged = dict(base, **values)
        result = run_status([cmake] + [f"-D{k}={v}" for k, v in merged.items()]
                            + ["-P", str(script)])
        text_out = " ".join((result.stdout + result.stderr).split())
        refused = expected is not None and not expected.isdigit()
        if (result.returncode != 0) != refused or (refused and expected not in text_out):
            raise AssertionError(
                f"consumer timeout CMake {name}: rc={result.returncode}, "
                f"expected {expected!r}:\n{text_out}")
        if refused:
            continue
        defs = {}
        for line in result.stdout.splitlines():
            m = re.match(r"-- SIM defs (\S+) = (.*)$", line)
            if m:
                defs[m.group(1)] = [v for v in m.group(2).split(";") if v]
        if sorted(defs) != units:
            raise AssertionError(f"consumer timeout CMake {name}: units {sorted(defs)}")
        carriers = sorted(u for u, d in defs.items()
                          if any(v.startswith(CONSUMER_TIMEOUT_KNOB) for v in d))
        status = "Isaac Vita GL record consumer timeout:" in text_out
        if expected is None:
            if carriers or status:
                raise AssertionError(f"consumer timeout CMake {name}: defined on {carriers}")
        else:
            want = f"{CONSUMER_TIMEOUT_KNOB}={expected}"
            if carriers != ["gl_record_ring.c"] or \
                    defs["gl_record_ring.c"].count(want) != 1 or not status or \
                    f"Isaac Vita GL record consumer timeout: {expected} us" not in text_out:
                raise AssertionError(f"consumer timeout CMake {name}: {carriers} {defs}")
        results[name] = defs
    # A value changes no other unit and appends only its own definition.
    for on, off in (("500", "unset"), ("100", "unset"), ("2000", "unset"),
                    ("250+admit-wait-on", "unset+admit-wait-on"),
                    ("empty", "unset")):
        a, b = results[off], results[on]
        for unit in units:
            want = a[unit] + ([f"{CONSUMER_TIMEOUT_KNOB}={on.split('+')[0]}"]
                              if unit == "gl_record_ring.c" and on != "empty" else [])
            if b[unit] != want:
                raise AssertionError(f"consumer timeout CMake {on} vs {off}: {unit} "
                                     f"{b[unit]} (want {want})")
    if not any(v.startswith("ISAAC_VITA_GL_THREAD_ADMIT_WAIT=2")
               for v in results["250+admit-wait-on"]["gl_record_ring.c"]):
        raise AssertionError("consumer timeout CMake: admit-wait shape lost its own define")
    refusals = sum(1 for _, _, e in cases if e is not None and not e.isdigit())
    return (f"GL record consumer timeout CMake: PASS (cmake -P, {len(cases)} cases over "
            f"{len(units)} recorder/worker units: unset/empty/no-worker define nothing; "
            "100/500/2000 and 250+ADMIT_WAIT=ON add exactly "
            f"{CONSUMER_TIMEOUT_KNOB}=<n> to gl_record_ring.c, every other unit unchanged; "
            f"{refusals} refusals: 99, 2001, abc, 0500, -1, 1e3, 1e20, WORKER OFF + 500; "
            "no other CMake statement names the knob)")


def verify_floor_debt_contract(cmake: str, scheduler_source: str) -> None:
    """S5 CMake defaults, refusals and wiring scope."""
    for needle in (
            'set(ISAAC_VITA_FULLSPEED_FLOOR_DEBT OFF CACHE STRING',
            'set(ISAAC_VITA_FULLSPEED_FLOOR_DEBT_CAP_US 8000 CACHE STRING',
            'set(ISAAC_VITA_FULLSPEED_ADMIT_SLACK_US 0 CACHE STRING',
            '"ISAAC_VITA_FULLSPEED_FLOOR_DEBT must be OFF, OBSERVE or ON "',
            '"ISAAC_VITA_FULLSPEED_FLOOR_DEBT_CAP_US must be an integer 1..16666 "',
            '"ISAAC_VITA_FULLSPEED_FLOOR_DEBT requires ISAAC_VITA_FULLSPEED_SCHEDULER=ON"',
            '"ISAAC_VITA_FULLSPEED_FLOOR_DEBT and ISAAC_VITA_FULLSPEED_HEAD_ADVANCE are exclusive',
            '"ISAAC_VITA_FULLSPEED_ADMIT_SLACK_US requires ISAAC_VITA_FULLSPEED_FLOOR_DEBT=OBSERVE or ON',
            '"ISAAC_VITA_FULLSPEED_ADMIT_SLACK_US must be below ISAAC_VITA_FULLSPEED_FLOOR_DEBT_CAP_US"'):
        if cmake.count(needle) != 1:
            raise AssertionError(f"CMake floor-debt contract lost: {needle}")
    start = cmake.index("      if(ISAAC_VITA_FULLSPEED_FLOOR_DEBT_MODE)")
    block = cmake[start:cmake.index("\n      endif()", start)]
    units = re.findall(r'"\$\{ISAAC_RUNTIME\}/([a-z0-9_]+\.c)"', block)
    if sorted(set(units)) != ["kage_vita_fullspeed_scheduler.c",
                              "kage_vita_phase_profile.c"] or \
            "list(APPEND" in block or "GENERATED_OWNERS" in block:
        raise AssertionError("floor-debt wiring must touch only the scheduler "
                             "and phase-profile units")
    if "ORACLE_NO_" in cmake:
        raise AssertionError("CMake must never set an oracle negative control")
    if scheduler_source.count("kage_vita_fullspeed_floor_debt_anchor(") != 2 \
            or scheduler_source.count(
                "kage_vita_fullspeed_floor_debt_note_grid();") != 1:
        raise AssertionError("floor-debt pace hook duplicated or lost")


def verify_source_contract(root: Path) -> None:
    cmake = (root / "recomp" / "vita" / "CMakeLists.txt").read_text(
        encoding="utf-8"
    )
    wrapper = (root / "tools" / "build_vita.py").read_text(encoding="utf-8")
    hooks = (
        root / "recomp" / "runtime" / "kage_vita_generated_hooks.c"
    ).read_text(encoding="utf-8")
    backend = (
        root / "recomp" / "runtime" / "kage_vita_backend.c"
    ).read_text(encoding="utf-8")
    manual = (
        root / "recomp" / "runtime" / "manual_kage_vita.c"
    ).read_text(encoding="utf-8")
    generator = (root / "recomp" / "gen_all.py").read_text(encoding="utf-8")
    phase_profile = (
        root / "recomp" / "runtime" / "kage_vita_phase_profile.c"
    ).read_text(encoding="utf-8")
    scheduler_source = (
        root / "recomp" / "runtime" / "kage_vita_fullspeed_scheduler.c"
    ).read_text(encoding="utf-8")

    required_cmake = (
        'option(ISAAC_VITA_FULLSPEED_SCHEDULER',
        '"${ISAAC_RUNTIME}/kage_vita_fullspeed_scheduler.c"',
        'REGEX "^void sub_0048bc50\\\\(CPU \\\\*__restrict c\\\\)$"',
        'REGEX "^void sub_004b0010\\\\(CPU \\\\*__restrict c\\\\)$"',
        'REGEX "^void sub_002cdcf0\\\\(CPU \\\\*__restrict c\\\\)$"',
        'REGEX "^void sub_004b0600\\\\(CPU \\\\*__restrict c\\\\)$"',
        'REGEX "^void sub_004b3fd0\\\\(CPU \\\\*__restrict c\\\\)$"',
        'set(ISAAC_GENERATED_CANONICAL_C ${ISAAC_GENERATED_C})',
        'ISAAC_VITA_FULLSPEED_GENERATED_OWNER_COUNT EQUAL 3',
        'ISAAC_VITA_FULLSPEED_SCHEDULER_BUILD_ID',
    )
    for needle in required_cmake:
        if needle not in cmake:
            raise AssertionError(f"CMake scheduler contract lost: {needle}")
    if not re.search(r'option\(ISAAC_VITA_FULLSPEED_HEAD_ADVANCE\s+"[^"]+" OFF\)', cmake):
        raise AssertionError("full-head advance must remain default OFF")
    if not re.search(
            r'option\(ISAAC_VITA_FULLSPEED_RENDER_COST_EXCLUDES_PRESENT\s+"[^"]+" OFF\)',
            cmake):
        raise AssertionError("render-cost admission knob must remain default OFF")
    if ('ISAAC_VITA_FULLSPEED_RENDER_COST_EXCLUDES_PRESENT requires '
            'ISAAC_VITA_FULLSPEED_SCHEDULER=ON') not in cmake:
        raise AssertionError("render-cost admission knob dependency lost")
    head_build = cmake[cmake.index('      if(ISAAC_VITA_FULLSPEED_HEAD_ADVANCE)'):]
    head_build = head_build[:head_build.index('      endif()')]
    if head_build.count('"${ISAAC_RUNTIME}/kage_vita_fullspeed_scheduler.c"') != 1 or \
            'ISAAC_VITA_FULLSPEED_HEAD_ADVANCE=1' not in head_build or \
            'ISAAC_VITA_FULLSPEED_GENERATED_OWNERS' in head_build:
        raise AssertionError("full-head advance must alter only scheduler runtime flags")
    if 'ISAAC_VITA_FULLSPEED_HEAD_ADVANCE requires FULLSPEED_SCHEDULER=ON and FULLSPEED_SERVICE_RESERVE=ON' not in cmake:
        raise AssertionError("full-head advance dependencies lost")
    for owner in (
            "APP", "MANAGER", "GAME_UPDATE", "RENDER", "GAME_PUBLISH"):
        if cmake.count(f"ISAAC_FULLSPEED_{owner}_OWNER_MATCH") != 2:
            raise AssertionError(
                f"CMake {owner.lower()} owner discovery is absent or duplicated"
            )
    fullspeed_start = cmake.index(
        "    if(ISAAC_VITA_FULLSPEED_SCHEDULER)"
    )
    fullspeed_end = cmake.index(
        "    if(ISAAC_VITA_STABLE_30_PRESENTATION)", fullspeed_start
    )
    fullspeed_block = cmake[fullspeed_start:fullspeed_end]
    if fullspeed_block.count(
            "foreach(ISAAC_GENERATED_SOURCE IN LISTS "
            "ISAAC_GENERATED_CANONICAL_C)") != 1:
        raise AssertionError(
            "fullspeed owner discovery no longer scans the canonical corpus"
        )
    if "foreach(ISAAC_GENERATED_SOURCE IN LISTS ISAAC_GENERATED_C)" in \
            fullspeed_block:
        raise AssertionError(
            "fullspeed owner discovery regressed to the derived source list"
        )
    direct_default_counts = {
        '"${ISAAC_VITA_DIRECT_DEFAULT_CANONICAL_SOURCE}")': 1,
        '"${ISAAC_VITA_DIRECT_DEFAULT_OUTPUT}")': 2,
    }
    for needle, expected in direct_default_counts.items():
        if fullspeed_block.count(needle) != expected:
            raise AssertionError(
                f"fullspeed direct-default owner remap drifted: {needle}"
            )
    if re.search(
            r"set\(ISAAC_VITA_FULLSPEED_GAME_UPDATE_OWNER\s+"
            r"\"\$\{ISAAC_VITA_DIRECT_DEFAULT_OUTPUT\}\"\)",
            fullspeed_block):
        raise AssertionError(
            "separate Game::Update owner was incorrectly direct-default remapped"
        )
    if fullspeed_block.count(
            '"${ISAAC_RUNTIME}/kage_vita_backend.c"') != 1:
        raise AssertionError(
            "production Vita backend lost its scheduler compile definition"
        )
    for option in (
            "ISAAC_VITA_FULLSPEED_SCHEDULER",
            "ISAAC_VITA_GUEST_LOOKUP_CACHE"):
        if f'"-D{option}=OFF"' not in wrapper or \
                f'"{option}": False' not in wrapper:
            raise AssertionError(
                f"canonical wrapper no longer pins {option} OFF"
            )

    profile = hooks.index("KAGE_VITA_PHASE_PROFILE_LOOP_HEAD(s_loop_count);")
    scheduler = hooks.index("KAGE_VITA_FULLSPEED_LOOP_HEAD();")
    input_sample = hooks.index("(void)kage_vita_input_sample();")
    if not profile < scheduler < input_sample:
        raise AssertionError("loop-head profile/scheduler/input order changed")
    if "KAGE_VITA_FULLSPEED_RESET();" in hooks:
        raise AssertionError("dead PC wrapper still owns cadence reset")
    # GL render thread (ISAAC_VITA_GL_THREAD_WORKER): the worker's initialize
    # owes the session activation to after its init closure, one extra call.
    worker_activate = backend.count(
        "if (s_owed_activate) kage_vita_backend_activate_session();")
    if backend.count("KAGE_VITA_FULLSPEED_RESET();") != 1 or \
            backend.count("KAGE_VITA_FULLSPEED_DEACTIVATE();") != 1 or \
            worker_activate > 1 or \
            backend.count("kage_vita_backend_activate_session();") - \
            worker_activate != 2:
        raise AssertionError(
            "Vita boundary reset/deactivate/activation ownership drifted"
        )
    if manual.count("kage_vita_backend_initialize(width, height)") != 1 or \
            "kage_pc_backend_initialize" in manual:
        raise AssertionError("manual KAGE no longer proves the direct Vita route")
    if hooks.count("KAGE_VITA_PHASE_PROFILE_RENDER_SKIPPED();") != 1:
        raise AssertionError("skipped Render no longer closes one Update sample")
    if "sub_002cdcf0" in scheduler_source or "sub_002cdcf0" in hooks:
        raise AssertionError("native cadence code must never call Game::Update")
    if "sceKernelDelayThread" not in scheduler_source:
        raise AssertionError("60-Hz wrapper lost its absolute-deadline wait")
    if "s_interpolation_enabled = interpolation_enabled != 0u;" not in \
            scheduler_source or \
            "KAGE_VITA_TICK_GAME_END) == 0u" not in scheduler_source:
        raise AssertionError(
            "post-Update interpolation transition handling is absent"
        )
    plan_start = scheduler_source.index(
        "int kage_vita_fullspeed_scheduler_plan_render("
    )
    plan_end = scheduler_source.index(
        "void kage_vita_fullspeed_scheduler_note_render_entry(void)",
        plan_start,
    )
    if scheduler_source[plan_start:plan_end].count(
            "interpolation_enabled != s_interpolation_enabled") != 1:
        raise AssertionError(
            "cadence interpolation transition gate drifted"
        )
    if "kage_vita_fullspeed_scheduler_deactivate(void)" not in \
            scheduler_source:
        raise AssertionError("cadence teardown no longer disarms the scheduler")
    for needle in (
            "kage_vita_fullspeed_scheduler_note_manager_counter_rebase(",
            "old_manager_counter != s_manager_counter_before",
            "new_manager_counter != old_manager_counter + 1u",
            "s_manager_counter_before = new_manager_counter;",
            "kage_vita_fullspeed_scheduler_note_game_pointer_publish(",
            "s_game_pointer != 0u || old_game_pointer != 0u ||",
            "new_game_pointer == 0u",
            "KAGE_VITA_TICK_GAME_POINTER_PUBLISH)) != 0u",
            "s_game_pointer = new_game_pointer;"):
        if needle not in scheduler_source:
            raise AssertionError(
                f"exact Game publication policy drifted: {needle}"
            )
    if hooks.count(
            "kage_pc_backend_fullspeed_note_manager_counter_rebase(") != 1:
        raise AssertionError("generated-hook Manager rebase route drifted")
    if hooks.count(
            "kage_pc_backend_fullspeed_note_game_pointer_publish(") != 1:
        raise AssertionError("generated-hook Game publication route drifted")
    if hooks.count(
            "kage_pc_backend_fullspeed_note_game_update_early_return(") != 1:
        raise AssertionError("generated-hook early Game return route drifted")

    if generator.count("kage_pc_backend_fullspeed_plan_render(") != 2 or \
            generator.count("kage_pc_backend_fullspeed_bypass_limiter(void)") != 1:
        raise AssertionError("generated scheduler gates are absent or duplicated")
    if not re.search(
        r"KAGE_PC_RENDER_ENTRY_RVA.*?fullspeed_plan_render.*?"
        r"KAGE_PC_LIMITER_ENTRY_RVA.*?fullspeed_bypass_limiter",
        generator,
        re.DOTALL,
    ):
        raise AssertionError("Render and limiter gates changed machine order")
    for marker in (
        "kage_pc_backend_fullspeed_note_manager_entry",
        "kage_pc_backend_fullspeed_note_manager_counter_rebase",
        "kage_pc_backend_fullspeed_note_game_pointer_publish",
        "kage_pc_backend_fullspeed_note_game_update_begin",
        "kage_pc_backend_fullspeed_note_game_update_early_return",
        "kage_pc_backend_fullspeed_note_game_update_end",
        "kage_pc_backend_fullspeed_note_render_body",
    ):
        if generator.count(marker) != 2:
            raise AssertionError(f"generated cadence observation drifted: {marker}")

    if '"ctr(p,a,v)=%u,%u,%u gptr(p,a,v)=%u,%u,%u "' not in phase_profile or \
            '"frame(last:s,b,e)=%08x,%u,%u "' not in phase_profile or \
            '"frame(fail:s,b,e)=%08x,%u,%u\\n"' not in phase_profile or \
            '"[kage-vita] ph120.r ' not in phase_profile:
        raise AssertionError("scheduler lifecycle/frame reason record is absent")

    timestamp_contract = (
        '"at=%08x%08x cad(w,s,d,m,g,r,b,p)="',
        "uint32_t last_outer_loop, uint64_t report_at_us,",
        "s_last_outer_loop, now, &guest,",
        "(unsigned)(uint32_t)(report_at_us >> 32)",
        "(unsigned)(uint32_t)report_at_us",
    )
    for needle in timestamp_contract:
        if needle not in phase_profile:
            raise AssertionError(
                f"phase report-head timestamp contract lost: {needle}"
            )
    if "%llu" in phase_profile:
        raise AssertionError("Vita phase profiler must not print uint64_t directly")
    verify_floor_debt_contract(cmake, scheduler_source)


def main() -> int:
    vita = Path(__file__).resolve().parent
    root = vita.parent.parent
    runtime = root / "recomp" / "runtime"
    verify_source_contract(root)
    cc = compiler()
    common = [
        cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
        f"-I{runtime}", f"-I{vita}",
    ]
    with tempfile.TemporaryDirectory(prefix="isaac-fullspeed-") as value:
        output = Path(value)
        scheduler_exe = output / (
            "scheduler-oracle.exe" if os.name == "nt" else "scheduler-oracle"
        )
        scheduler_stdout = run(common + [
            str(runtime / "kage_vita_fullspeed_scheduler.c"),
            str(runtime / "kage_vita_fullspeed_scheduler_oracle.c"),
            "-o", str(scheduler_exe),
        ])
        scheduler_stdout = run([str(scheduler_exe)])
        # The Exit-to-menu zero-frame sites are an opt-in policy; the oracle
        # must pass with the two sites accepted as well as failed open.
        scheduler_exit_exe = output / (
            "scheduler-oracle-exit.exe" if os.name == "nt" else
            "scheduler-oracle-exit"
        )
        run(common + [
            "-DISAAC_VITA_FULLSPEED_EXIT_ZERO_FRAME_SITES=1",
            str(runtime / "kage_vita_fullspeed_scheduler.c"),
            str(runtime / "kage_vita_fullspeed_scheduler_oracle.c"),
            "-o", str(scheduler_exit_exe),
        ])
        scheduler_exit_stdout = run([str(scheduler_exit_exe)])
        # port.ini fps_lock duty (ISAAC_VITA_RUNTIME_SETTINGS): the same
        # oracle with the knob and the scheduler definition it receives in
        # the production build; every baseline scenario must still pass at
        # the default duty and the duty scenarios join them.
        scheduler_duty_exe = output / (
            "scheduler-oracle-duty.exe" if os.name == "nt" else
            "scheduler-oracle-duty"
        )
        run(common + [
            "-DISAAC_VITA_RUNTIME_SETTINGS=1",
            "-DISAAC_VITA_FULLSPEED_SCHEDULER=1",
            str(runtime / "kage_vita_fullspeed_scheduler.c"),
            str(runtime / "kage_vita_fullspeed_scheduler_oracle.c"),
            "-o", str(scheduler_duty_exe),
        ])
        scheduler_duty_stdout = run([str(scheduler_duty_exe)])

        # The video candidate combines service reserve with runtime locks.
        # Execute the transition and lock fixtures in that combination too,
        # rather than inferring it from separate duty/head-advance tests.
        scheduler_reserve_exe = output / "scheduler-oracle-reserve-duty.exe"
        run(common + [
            "-DISAAC_VITA_RUNTIME_SETTINGS=1",
            "-DISAAC_VITA_FULLSPEED_SCHEDULER=1",
            "-DISAAC_VITA_FULLSPEED_SERVICE_RESERVE=1",
            "-DISAAC_VITA_FULLSPEED_EXIT_ZERO_FRAME_SITES=1",
            str(runtime / "kage_vita_fullspeed_scheduler.c"),
            str(runtime / "kage_vita_fullspeed_scheduler_oracle.c"),
            "-o", str(scheduler_reserve_exe),
        ])
        scheduler_reserve_stdout = run([str(scheduler_reserve_exe)])

        # ISAAC_VITA_GL_THREAD_RENDER_ADMISSION in the production combination:
        # every baseline scenario passes with the render thread never behind,
        # and the admission scenario runs both answers.
        admission_exe = output / "scheduler-oracle-gl-admission.exe"
        run(common + [
            "-DISAAC_VITA_GL_THREAD_RENDER_ADMISSION=1",
            "-DISAAC_VITA_RUNTIME_SETTINGS=1",
            "-DISAAC_VITA_FULLSPEED_SCHEDULER=1",
            "-DISAAC_VITA_FULLSPEED_SERVICE_RESERVE=1",
            "-DISAAC_VITA_FULLSPEED_EXIT_ZERO_FRAME_SITES=1",
            str(runtime / "kage_vita_fullspeed_scheduler.c"),
            str(runtime / "kage_vita_fullspeed_scheduler_oracle.c"),
            "-o", str(admission_exe),
        ])
        admission_stdout = run([str(admission_exe)])
        if "Vita GL render admission: PASS" not in admission_stdout or \
                "Vita cadence30 scheduler oracle: PASS" not in admission_stdout:
            raise AssertionError(f"GL render admission oracle:\n{admission_stdout}")
        if "render admission" in scheduler_reserve_stdout:
            raise AssertionError("render admission leaks into the knob-OFF oracle")

        # ISAAC_VITA_GL_THREAD_ADMIT_WAIT (R60-DRYADMIT-SPEC-20261028.md 6):
        # the admission combination with the knob at OBSERVE and ON, and ON
        # with FLOOR_DEBT=OBSERVE, the one floor-debt mode it accepts.  The
        # 40 worker cases pin every printed field in each mode; OBSERVE must
        # behave as OFF (its worker lines equal OFF's once the ph120.aw
        # suffix is dropped) and no scenario outside the worker cases may
        # move.  The PASS totals are the oracle's own output (MEASURED,
        # clang 2026-10-28, review fix pass: 26 -> 40 cases).
        admit_wait_pass = {
            "off": "Vita GL admit wait (off): PASS (40 worker cases, every "
                   "printed field pinned; presents=40356 refused=6443 over "
                   "10x120 loops per case, 600 updates each)",
            "observe": "Vita GL admit wait (observe): PASS (40 worker cases, "
                       "every printed field pinned; presents=40356 "
                       "refused=6443 over 10x120 loops per case, 600 updates "
                       "each)",
            "on": "Vita GL admit wait (on): PASS (40 worker cases, every "
                  "printed field pinned; presents=45249 refused=1344 over "
                  "10x120 loops per case, 600 updates each)",
        }
        admission_flags = [
            "-DISAAC_VITA_GL_THREAD_RENDER_ADMISSION=1", *RESERVE_DUTY_FLAGS]
        admit_wait_stdout = {"off": admission_stdout}
        for name, flags in (
                ("observe", ["-DISAAC_VITA_GL_THREAD_ADMIT_WAIT=1"]),
                ("on", ["-DISAAC_VITA_GL_THREAD_ADMIT_WAIT=2"]),
                ("off-fd-observe", ["-DISAAC_VITA_FULLSPEED_FLOOR_DEBT=1",
                                    *FLOOR_DEBT_RECIPE]),
                ("on-fd-observe", ["-DISAAC_VITA_GL_THREAD_ADMIT_WAIT=2",
                                   "-DISAAC_VITA_FULLSPEED_FLOOR_DEBT=1",
                                   *FLOOR_DEBT_RECIPE])):
            exe = output / f"scheduler-oracle-gl-admit-wait-{name}.exe"
            run(common + admission_flags + flags + [
                str(runtime / "kage_vita_fullspeed_scheduler.c"),
                str(runtime / "kage_vita_fullspeed_scheduler_oracle.c"),
                "-o", str(exe),
            ])
            admit_wait_stdout[name] = run([str(exe)])
        for name, text in admit_wait_stdout.items():
            want = admit_wait_pass[name.split("-")[0]]
            if want not in text.splitlines() or \
                    "Vita cadence30 scheduler oracle: PASS" not in text:
                raise AssertionError(f"GL admit wait {name} oracle:\n{text}")

        def worker_split(text: str) -> tuple[list[str], list[str]]:
            worker, rest = [], []
            for line in text.splitlines():
                if line.startswith("Vita GL admit wait ("):
                    continue  # the PASS line, pinned above per mode
                if " worker " in line:
                    worker.append(line.split(" | ", 1)[0])
                else:
                    rest.append(line)
            return worker, rest

        off_worker, off_rest = worker_split(admit_wait_stdout["off"])
        observe_worker, observe_rest = worker_split(
            admit_wait_stdout["observe"])
        if len(off_worker) != 40 or observe_worker != off_worker:
            raise AssertionError("GL admit wait OBSERVE moved a worker case")
        # Each mode against its own OFF twin: FLOOR_DEBT=OBSERVE prints its
        # own scenario lines, so ON + FLOOR_DEBT=OBSERVE is compared with
        # FLOOR_DEBT=OBSERVE alone.  The worker cases do not read floor
        # debt: their lines are the same with and without it, in OFF and ON.
        fd_off_worker, fd_off_rest = worker_split(
            admit_wait_stdout["off-fd-observe"])
        for name, twin in (("observe", off_rest), ("on", off_rest),
                           ("on-fd-observe", fd_off_rest)):
            if worker_split(admit_wait_stdout[name])[1] != twin:
                raise AssertionError(
                    f"GL admit wait {name} moved a non-worker scenario")
        if fd_off_worker != off_worker or \
                worker_split(admit_wait_stdout["on-fd-observe"])[0] != \
                worker_split(admit_wait_stdout["on"])[0]:
            raise AssertionError(
                "GL admit wait worker cases moved under FLOOR_DEBT=OBSERVE")
        if "admit wait" in scheduler_reserve_stdout:
            raise AssertionError("GL admit wait leaks into the knob-OFF oracle")
        # The CMake/#error matrix: the conflicts and an out-of-range value
        # must not compile; FLOOR_DEBT=OBSERVE must.
        for name, flags, message in (
                ("value-3", admission_flags +
                 ["-DISAAC_VITA_GL_THREAD_ADMIT_WAIT=3"],
                 "ISAAC_VITA_GL_THREAD_ADMIT_WAIT must be 1 (OBSERVE) or 2 (ON)"),
                ("no-admission", list(RESERVE_DUTY_FLAGS) +
                 ["-DISAAC_VITA_GL_THREAD_ADMIT_WAIT=2"],
                 "GL admit wait refines GL render admission"),
                ("render-cost", admission_flags +
                 ["-DISAAC_VITA_GL_THREAD_ADMIT_WAIT=2", RENDER_COST_FLAG],
                 "GL admit wait is proven with stock render cost"),
                ("floor-debt-on", admission_flags +
                 ["-DISAAC_VITA_GL_THREAD_ADMIT_WAIT=2",
                  "-DISAAC_VITA_FULLSPEED_FLOOR_DEBT=2", *FLOOR_DEBT_RECIPE],
                 "GL admit wait is proven with stock render cost"),
                ("head-advance", admission_flags +
                 ["-DISAAC_VITA_GL_THREAD_ADMIT_WAIT=2",
                  "-DISAAC_VITA_FULLSPEED_HEAD_ADVANCE=1"],
                 "GL admit wait is proven with stock render cost")):
            refused = run_status(common + flags + [
                "-c", str(runtime / "kage_vita_fullspeed_scheduler.c"),
                "-o", str(output / f"admit-wait-{name}.o")])
            if refused.returncode == 0 or message not in refused.stderr:
                raise AssertionError(
                    f"GL admit wait {name} compiled:\n{refused.stderr}")
        admit_wait_report = [
            admit_wait_pass["off"], admit_wait_pass["observe"],
            admit_wait_pass["on"],
            "GL admit wait oracle legs: PASS (OBSERVE worker lines == OFF "
            "without the ph120.aw suffix; no non-worker line moved in "
            "OBSERVE, ON or ON+FLOOR_DEBT=OBSERVE against its OFF twin; "
            "worker lines unchanged by FLOOR_DEBT=OBSERVE; 5 #error builds "
            "refused: value 3, no admission, C2, FLOOR_DEBT=ON, head advance)"]

        # ISAAC_VITA_GL_THREAD_ADMIT_BEHIND (R60-FABLE-PLAN.md lane B): the
        # admission combination with the knob at OBSERVE and ON (bound
        # 65536), and with FLOOR_DEBT=OBSERVE.  Each build runs the 40
        # worker cases three times: "worker" with every behind gate over
        # the bound, "within" with every gate within it, "mixed" with bytes
        # proportional to the closure's remaining time.  The twin is the
        # knob-OFF oracle with its stock gate never told "behind".
        ab = ["-DISAAC_VITA_GL_THREAD_ADMIT_BEHIND_BYTES=65536"]
        admit_behind_stdout = {}
        for name, flags in (
                ("twin", ["-DORACLE_GL_ADMIT_BEHIND_TWIN"]),
                ("observe", ["-DISAAC_VITA_GL_THREAD_ADMIT_BEHIND=1", *ab]),
                ("on", ["-DISAAC_VITA_GL_THREAD_ADMIT_BEHIND=2", *ab]),
                ("observe-fd-observe", ["-DISAAC_VITA_GL_THREAD_ADMIT_BEHIND=1", *ab,
                                        "-DISAAC_VITA_FULLSPEED_FLOOR_DEBT=1",
                                        *FLOOR_DEBT_RECIPE]),
                ("on-fd-observe", ["-DISAAC_VITA_GL_THREAD_ADMIT_BEHIND=2", *ab,
                                   "-DISAAC_VITA_FULLSPEED_FLOOR_DEBT=1",
                                   *FLOOR_DEBT_RECIPE])):
            exe = output / f"scheduler-oracle-gl-admit-behind-{name}.exe"
            run(common + admission_flags + flags + [
                str(runtime / "kage_vita_fullspeed_scheduler.c"),
                str(runtime / "kage_vita_fullspeed_scheduler_oracle.c"),
                "-o", str(exe),
            ])
            admit_behind_stdout[name] = run([str(exe)])
        admit_behind_pass = {
            "observe": "Vita GL admit behind (observe): PASS (3 x 40 worker cases pinned to the stock numbers: over the bound, within it, mixed; presents=40356,40356 refused=6443,6443)",
            "on": "Vita GL admit behind (on): PASS (over the bound: 40 worker cases pinned to the stock numbers; within: presents=46234 refused=519; mixed: presents=45156 refused=1643; bound 65536 inclusive, bound+1 refused; hold without late head and late head without hold arm nothing; hold + late head: 30 stock refusals over exactly 30 non-full phases)",
        }

        # The directed time-test runs (st p / f / x at a behind gate, tt, ov
        # before tt) and the count of refusals whose predicted Present /
        # Render-return instants the oracle held against its own measurement.
        admit_behind_time_test = {
            "observe": "Vita GL admit behind (time test): PASS (gate forced behind, n st(p,f,x) ov wa ok bo tt rs: passing 50 50,0,0 0 50 0 0 0 50; failing 50 0,50,0 0 0 0 0 0 50; failing over the bound 50 0,50,0 50 0 0 0 0 50; menu 50 0,0,50 0 50 0 0 0 50; 29678 refusals handed gate + the measured last Render body / cost, 213 of them with a Present wait between the two)",
            "on": "Vita GL admit behind (time test): PASS (gate forced behind, n st(p,f,x) ov wa ok bo tt rs: passing 50 50,0,0 0 50 50 0 0 0; failing 50 0,50,0 0 0 0 0 50 50; failing over the bound 50 0,50,0 50 0 0 0 0 50; menu 50 0,0,50 0 50 50 0 0 0; 13434 refusals handed gate + the measured last Render body / cost, 542 of them with a Present wait between the two)",
        }
        # sha256 of the 120 ph120.ab / ph120.abp suffixes (n st ov wa ok bo tt
        # obmax hl pm wp phl hd) of the worker, within and mixed passes, in
        # that order, and the sums of pm c / pm ok / wp c / wp ok / phl over
        # them: a pin, so a change to the closure words is seen.
        admit_behind_suffix_pin = {
            "observe": ("604f3fde92cfd387776d15f1dc446aa03b03732943d2ec8b6c62ad4c4297d5d2", (19335, 13605, 11753, 9007, 75)),
            "on": ("a8515560c7463e3b13cab1a51c18ba86fc92925ed930cb3a5a7cfa837e7c6d25", (8609, 4953, 895, 368, 487)),
        }

        def behind_suffix_pin(text: str) -> tuple[str, tuple[int, ...]]:
            suffixes = []
            for label in ("worker", "within", "mixed"):
                suffixes += [line.partition(" | ")[2] for line in text.splitlines()
                             if line.startswith("span=") and f" {label} " in line]
            sums = [0, 0, 0, 0, 0]
            for suffix in suffixes:
                fields = dict(item.split("=", 1) for item in suffix.split())
                pm, wp = fields["pm"].split(","), fields["wp"].split(",")
                for i, value in enumerate((pm[0], pm[1], wp[0], wp[1], fields["phl"])):
                    sums[i] += int(value)
            if len(suffixes) != 120:
                raise AssertionError(f"GL admit behind: {len(suffixes)} suffixes")
            return (hashlib.sha256("\n".join(suffixes).encode("ascii")).hexdigest(),
                    tuple(sums))

        def behind_split(text: str) -> tuple[dict[str, list[str]], list[str], list[str]]:
            """label -> worker-case lines without the ph120.ab suffix; the
            suffixes of the "within" lines; every other line."""
            passes = {"worker": [], "within": [], "mixed": []}
            within_suffix, rest = [], []
            for line in text.splitlines():
                if line.startswith(("Vita GL admit wait (", "Vita GL admit behind (")):
                    continue  # pinned per mode
                for label in passes:
                    if f" {label} " in line and line.startswith("span="):
                        head, _, suffix = line.partition(" | ")
                        passes[label].append(head.replace(f" {label} ", " worker ", 1))
                        if label == "within":
                            within_suffix.append(suffix)
                        break
                else:
                    rest.append(line)
            return passes, within_suffix, rest

        for name, text in admit_behind_stdout.items():
            mode = name.split("-")[0]
            # Over the bound = the stock numbers: the 40 pinned worker lines.
            if admit_wait_pass["off"] not in text.splitlines() or \
                    "Vita cadence30 scheduler oracle: PASS" not in text:
                raise AssertionError(f"GL admit behind {name} oracle:\n{text}")
            if mode != "twin" and admit_behind_pass[mode] not in text.splitlines():
                raise AssertionError(f"GL admit behind {name} PASS line:\n{text}")
            if mode != "twin" and admit_behind_time_test[mode] not in text.splitlines():
                raise AssertionError(
                    f"GL admit behind {name} time-test line:\n" + "\n".join(
                        line for line in text.splitlines() if "(time test)" in line))
            if mode != "twin" and behind_suffix_pin(text) != admit_behind_suffix_pin[mode]:
                raise AssertionError(
                    f"GL admit behind {name}: ph120.ab / abp suffixes moved: "
                    f"{behind_suffix_pin(text)}")
            passes, _, rest = behind_split(text)
            twin_rest = fd_off_rest if name.endswith("fd-observe") else off_rest
            if passes["worker"] != off_worker:
                raise AssertionError(
                    f"GL admit behind {name}: an over-the-bound worker case moved")
            if rest != twin_rest:
                raise AssertionError(
                    f"GL admit behind {name} moved a non-worker scenario")
            if mode == "observe" and (passes["within"] != off_worker or
                                      passes["mixed"] != off_worker):
                raise AssertionError(
                    "GL admit behind OBSERVE moved a worker case")
            if mode != "observe" and len(passes["within"]) != 40:
                raise AssertionError(f"GL admit behind {name}: within pass size")
        # ON within the bound == stock admitting the frame, wherever the ON
        # back-off never armed (hl=0): every printed field but rs (stock
        # counts no refusal at a gate it is never told is behind; ON counts
        # the frames its time test refuses) equals the twin's.
        twin_within = behind_split(admit_behind_stdout["twin"])[0]["within"]
        admit_behind_equal = {}
        for name in ("on", "on-fd-observe"):
            passes, suffix, _ = behind_split(admit_behind_stdout[name])
            equal = admitted = armed = 0
            for mine, twin, tail in zip(passes["within"], twin_within, suffix):
                fields = dict(item.split("=", 1) for item in tail.split())
                if fields["hl"].split(",")[0] != "0":
                    armed += 1
                    continue
                strip = lambda line: re.sub(r" rs=\d+ ", " ", line)
                if strip(mine) != strip(twin):
                    raise AssertionError(
                        f"GL admit behind {name}: within-the-bound case differs "
                        f"from the stock-admitted twin:\n{mine}\n{twin}")
                equal += 1
                admitted += fields["ok"] != "0"
            admit_behind_equal[name] = (equal, admitted, armed)
        if admit_behind_equal["on"] != (35, 27, 5) or \
                admit_behind_equal["on-fd-observe"] != admit_behind_equal["on"]:
            raise AssertionError(
                f"GL admit behind twin comparison: {admit_behind_equal}")
        if behind_split(admit_behind_stdout["on-fd-observe"])[0] != \
                behind_split(admit_behind_stdout["on"])[0]:
            raise AssertionError(
                "GL admit behind worker cases moved under FLOOR_DEBT=OBSERVE")
        for text in (scheduler_reserve_stdout, admission_stdout):
            if "admit behind" in text or " within " in text:
                raise AssertionError("GL admit behind leaks into a knob-OFF oracle")
        # Every mutant of the scheduler's knob code must fail the oracle:
        # the ON ones in the ON build, the ones in the code both modes share
        # (the gate's copy of the time test, the predicted Present instant)
        # in the OBSERVE build too, where they corrupt only the receipts.
        admit_behind_mutants = (
            "ANY_HOLD", "NO_BACKOFF", "BACKOFF_SHORT", "NO_BOUND",
            "BOUND_EXCLUSIVE", "TIME_TEST_SKIP", "TEST_FAIL_ZERO",
            "COST_DOUBLE", "CLOSURE_AT_RETURN")
        admit_behind_observe_mutants = (
            "TEST_FAIL_ZERO", "COST_DOUBLE", "CLOSURE_AT_RETURN")
        admit_behind_killed = []
        for value, mutants in ((2, admit_behind_mutants),
                               (1, admit_behind_observe_mutants)):
            for mutant in mutants:
                exe = output / f"scheduler-oracle-gl-admit-behind-mut-{value}-{mutant}.exe"
                run(common + admission_flags + [
                    f"-DISAAC_VITA_GL_THREAD_ADMIT_BEHIND={value}", *ab,
                    f"-DKAGE_VITA_GL_ADMIT_BEHIND_ORACLE_MUTATE_{mutant}",
                    str(runtime / "kage_vita_fullspeed_scheduler.c"),
                    str(runtime / "kage_vita_fullspeed_scheduler_oracle.c"),
                    "-o", str(exe),
                ])
                killed = run_status([str(exe)])
                if killed.returncode == 0 or \
                        "cadence30 oracle failed at line" not in killed.stderr:
                    raise AssertionError(
                        f"GL admit behind mutant {mutant} (mode {value}) survived:\n"
                        f"{killed.stdout[-400:]}\n{killed.stderr}")
                admit_behind_killed.append((value, mutant))
        if len(admit_behind_killed) != 12:
            raise AssertionError(f"GL admit behind mutants: {admit_behind_killed}")
        scheduler_source = (runtime / "kage_vita_fullspeed_scheduler.c").read_text(
            encoding="utf-8")
        if sorted(set(re.findall(
                r"KAGE_VITA_GL_ADMIT_BEHIND_ORACLE_MUTATE_(\w+)",
                scheduler_source))) != sorted(admit_behind_mutants):
            raise AssertionError("GL admit behind: a mutant has no leg")
        # The #error matrix.
        for name, flags, message in (
                ("value-3", admission_flags +
                 ["-DISAAC_VITA_GL_THREAD_ADMIT_BEHIND=3", *ab],
                 "ISAAC_VITA_GL_THREAD_ADMIT_BEHIND must be 1 (OBSERVE) or 2 (ON)"),
                ("no-admission", list(RESERVE_DUTY_FLAGS) +
                 ["-DISAAC_VITA_GL_THREAD_ADMIT_BEHIND=2", *ab],
                 "GL admit behind refines GL render admission"),
                ("admit-wait", admission_flags +
                 ["-DISAAC_VITA_GL_THREAD_ADMIT_BEHIND=2", *ab,
                  "-DISAAC_VITA_GL_THREAD_ADMIT_WAIT=1"],
                 "GL admit behind and GL admit wait each own the behind arm"),
                ("render-cost", admission_flags +
                 ["-DISAAC_VITA_GL_THREAD_ADMIT_BEHIND=2", *ab, RENDER_COST_FLAG],
                 "GL admit behind is proven with stock render cost"),
                ("floor-debt-on", admission_flags +
                 ["-DISAAC_VITA_GL_THREAD_ADMIT_BEHIND=1", *ab,
                  "-DISAAC_VITA_FULLSPEED_FLOOR_DEBT=2", *FLOOR_DEBT_RECIPE],
                 "GL admit behind is proven with stock render cost"),
                ("head-advance", admission_flags +
                 ["-DISAAC_VITA_GL_THREAD_ADMIT_BEHIND=2", *ab,
                  "-DISAAC_VITA_FULLSPEED_HEAD_ADVANCE=1"],
                 "GL admit behind is proven with stock render cost"),
                ("no-bytes", admission_flags +
                 ["-DISAAC_VITA_GL_THREAD_ADMIT_BEHIND=2"],
                 "ISAAC_VITA_GL_THREAD_ADMIT_BEHIND_BYTES must be 1..524288"),
                ("bytes-0", admission_flags +
                 ["-DISAAC_VITA_GL_THREAD_ADMIT_BEHIND=1",
                  "-DISAAC_VITA_GL_THREAD_ADMIT_BEHIND_BYTES=0"],
                 "ISAAC_VITA_GL_THREAD_ADMIT_BEHIND_BYTES must be 1..524288"),
                ("bytes-524289", admission_flags +
                 ["-DISAAC_VITA_GL_THREAD_ADMIT_BEHIND=2",
                  "-DISAAC_VITA_GL_THREAD_ADMIT_BEHIND_BYTES=524289"],
                 "ISAAC_VITA_GL_THREAD_ADMIT_BEHIND_BYTES must be 1..524288")):
            refused = run_status(common + flags + [
                "-c", str(runtime / "kage_vita_fullspeed_scheduler.c"),
                "-o", str(output / f"admit-behind-{name}.o")])
            if refused.returncode == 0 or message not in refused.stderr:
                raise AssertionError(
                    f"GL admit behind {name} compiled:\n{refused.stderr}")
        admit_behind_report = [
            admit_behind_pass["observe"], admit_behind_pass["on"],
            "(observe) " + admit_behind_time_test["observe"],
            "(on) " + admit_behind_time_test["on"],
            "GL admit behind oracle legs: PASS (OBSERVE: 3 x 40 worker lines "
            "== OFF byte for byte without the ph120.ab suffix; ON over the "
            "bound: 40 worker lines == OFF; ON within the bound: 35 of 40 "
            "cases never arm the back-off and equal the stock-admitted twin "
            "in every field but rs, 27 of them with ok > 0, 5 arm it; no "
            "non-worker line moved in OBSERVE, ON or either + "
            "FLOOR_DEBT=OBSERVE against its OFF twin; worker cases unchanged "
            "by FLOOR_DEBT=OBSERVE; the 120 ph120.ab / abp suffixes of each "
            "mode pinned by digest (OBSERVE pm c/ok, wp c/ok, phl = "
            + "/".join(map(str, admit_behind_suffix_pin["observe"][1]))
            + "; ON = " + "/".join(map(str, admit_behind_suffix_pin["on"][1]))
            + "); 12 mutant builds killed: ON x ANY_HOLD, NO_BACKOFF, "
            "BACKOFF_SHORT, NO_BOUND, BOUND_EXCLUSIVE, TIME_TEST_SKIP, "
            "TEST_FAIL_ZERO, COST_DOUBLE, CLOSURE_AT_RETURN; OBSERVE x "
            "TEST_FAIL_ZERO, COST_DOUBLE, CLOSURE_AT_RETURN; 9 #error builds "
            "refused: value 3, no admission, ADMIT_WAIT, C2, FLOOR_DEBT=ON, "
            "head advance, no bytes, bytes 0, bytes 524289)"]

        priority_exe = output / "scheduler-oracle-video-priority.exe"
        priority_exe = output / "scheduler-oracle-video-priority.exe"
        run(common + [
            "-DISAAC_VITA_THEORA_RENDER_PRIORITY=1",
            "-DISAAC_VITA_RUNTIME_SETTINGS=1",
            "-DISAAC_VITA_FULLSPEED_SCHEDULER=1",
            "-DISAAC_VITA_FULLSPEED_SERVICE_RESERVE=1",
            "-DISAAC_VITA_FULLSPEED_EXIT_ZERO_FRAME_SITES=1",
            str(runtime / "kage_vita_fullspeed_scheduler.c"),
            str(runtime / "kage_vita_fullspeed_scheduler_oracle.c"),
            "-o", str(priority_exe),
        ])
        priority_stdout = run([str(priority_exe)])

        # ISAAC_VITA_FULLSPEED_RENDER_COST_EXCLUDES_PRESENT: the same oracle
        # with the knob, alone and in the int2j combination.  Every baseline
        # scenario must still pass; the Present-wait cases switch to their
        # knob-ON expectations.
        render_cost_stdout = []
        for name, flags in (
                ("plain", []),
                ("int2j", ["-DISAAC_VITA_THEORA_RENDER_PRIORITY=1",
                           "-DISAAC_VITA_RUNTIME_SETTINGS=1",
                           "-DISAAC_VITA_FULLSPEED_SCHEDULER=1",
                           "-DISAAC_VITA_FULLSPEED_SERVICE_RESERVE=1",
                           "-DISAAC_VITA_FULLSPEED_EXIT_ZERO_FRAME_SITES=1"])):
            exe = output / f"scheduler-render-cost-{name}.exe"
            run(common + flags + [
                "-DISAAC_VITA_FULLSPEED_RENDER_COST_EXCLUDES_PRESENT=1",
                str(runtime / "kage_vita_fullspeed_scheduler.c"),
                str(runtime / "kage_vita_fullspeed_scheduler_oracle.c"),
                "-o", str(exe),
            ])
            render_cost_stdout.append(run([str(exe)]))
        # The oversleep model's stall sits in the Render body and its Present
        # takes a constant 110 us, so the knob must not change it at all.
        render_cost_models = []
        for enabled in (False, True):
            exe = output / f"overshoot-render-cost-{int(enabled)}.exe"
            flags = ["-DISAAC_VITA_FULLSPEED_SCHEDULER=1"] + (
                ["-DISAAC_VITA_FULLSPEED_RENDER_COST_EXCLUDES_PRESENT=1"]
                if enabled else [])
            run(common + flags + [
                str(runtime / "kage_vita_fullspeed_scheduler.c"),
                str(vita / "host_tests" / "kage_vita_fullspeed_overshoot_model.c"),
                "-o", str(exe)])
            render_cost_models.append(exe)
        for render in (7400, 12000, 20000, 34000, 55000):
            for overshoot in (180, 900):
                for offset in (0, 15000):
                    args = ["200000", str(overshoot), str(render), str(offset)]
                    if (run([str(render_cost_models[0])] + args) !=
                            run([str(render_cost_models[1])] + args)):
                        raise AssertionError(
                            "render-cost knob altered the oversleep model")

        head_flags = ["-DISAAC_VITA_FULLSPEED_SCHEDULER=1",
                      "-DISAAC_VITA_FULLSPEED_SERVICE_RESERVE=1"]
        head_stdout = []
        for exit_sites in (False, True):
            head_exe = output / f"scheduler-head-{int(exit_sites)}.exe"
            flags = head_flags + ["-DISAAC_VITA_FULLSPEED_HEAD_ADVANCE=1"]
            if exit_sites:
                flags += ["-DISAAC_VITA_FULLSPEED_EXIT_ZERO_FRAME_SITES=1"]
            run(common + flags + [
                str(runtime / "kage_vita_fullspeed_scheduler.c"),
                str(runtime / "kage_vita_fullspeed_scheduler_oracle.c"),
                "-o", str(head_exe),
            ])
            head_stdout.append(run([str(head_exe)]))
        # An explicit zero is as OFF as absence, including emitted host code.
        head_off_assembly = []
        for defined_zero in (False, True):
            assembly = output / f"head-off-{int(defined_zero)}.s"
            flags = head_flags + (["-DISAAC_VITA_FULLSPEED_HEAD_ADVANCE=0"]
                                  if defined_zero else [])
            run(common + flags + ["-S",
                str(runtime / "kage_vita_fullspeed_scheduler.c"),
                "-o", str(assembly)])
            head_off_assembly.append(assembly.read_bytes())
        if head_off_assembly[0] != head_off_assembly[1]:
            raise AssertionError("HEAD_ADVANCE=0 changes emitted OFF code")

        # The existing oversleep model must stay identical when the heavy
        # render cost refuses head advance; no replacement timing framework.
        heavy_exes = []
        for enabled in (False, True):
            exe = output / f"overshoot-head-{int(enabled)}.exe"
            flags = head_flags + (["-DISAAC_VITA_FULLSPEED_HEAD_ADVANCE=1"]
                                  if enabled else [])
            run(common + flags + [str(runtime / "kage_vita_fullspeed_scheduler.c"),
                str(vita / "host_tests" / "kage_vita_fullspeed_overshoot_model.c"),
                "-o", str(exe)])
            heavy_exes.append(exe)
        for render in (34000, 55000):
            for overshoot in (180, 900):
                args = ["200000", str(overshoot), str(render), "17000"]
                if run([str(heavy_exes[0])] + args) != run([str(heavy_exes[1])] + args):
                    raise AssertionError("head advance altered refused heavy-render model")

        def compile_phase(cache_enabled: bool) -> str:
            suffix = "cache-on" if cache_enabled else "cache-off"
            phase_exe = output / (
                f"phase-oracle-{suffix}.exe" if os.name == "nt" else
                f"phase-oracle-{suffix}"
            )
            command = common + [
                "-DISAAC_KAGE_VITA_PHASE_PROFILE_ORACLE=1",
                "-DISAAC_VITA_PHASE_PROFILE=1",
                "-DISAAC_VITA_FULLSPEED_SCHEDULER=1",
                '-DISAAC_VITA_PHASE_PROFILE_BUILD_ID="fullspeed:phase-oracle"',
                '-DISAAC_VITA_FULLSPEED_SCHEDULER_BUILD_ID="fullspeed:phase-oracle"',
                str(runtime / "kage_vita_fullspeed_scheduler.c"),
                str(runtime / "kage_vita_phase_profile.c"),
                str(runtime / "kage_vita_fullspeed_phase_oracle.c"),
                "-o", str(phase_exe),
            ]
            if cache_enabled:
                command.insert(
                    len(common), "-DISAAC_VITA_GUEST_LOOKUP_CACHE=1"
                )
            run(command)
            return run([str(phase_exe)])

        phase_cache_off_stdout = compile_phase(False)
        phase_cache_on_stdout = compile_phase(True)

        def compile_lifecycle(enabled: bool) -> str:
            suffix = "on" if enabled else "off"
            lifecycle_exe = output / (
                f"lifecycle-oracle-{suffix}.exe" if os.name == "nt" else
                f"lifecycle-oracle-{suffix}"
            )
            command = common + [
                "-DISAAC_KAGE_VITA_BACKEND_ORACLE=1",
                "-DISAAC_VITA_VITAGL_STOCK_REFERENCE=1",
            ]
            if enabled:
                command += [
                    "-DISAAC_VITA_FULLSPEED_SCHEDULER=1",
                    '-DISAAC_VITA_FULLSPEED_SCHEDULER_BUILD_ID="lifecycle"',
                ]
            command += [str(runtime / "kage_vita_backend.c")]
            if enabled:
                command += [str(
                    runtime / "kage_vita_fullspeed_scheduler.c"
                )]
            command += [
                str(runtime / "kage_vita_fullspeed_lifecycle_oracle.c"),
                "-o", str(lifecycle_exe),
            ]
            run(command)
            return run([str(lifecycle_exe)])

        lifecycle_off_stdout = compile_lifecycle(False)
        lifecycle_on_stdout = compile_lifecycle(True)

        floor_debt_report = verify_floor_debt(common, runtime, vita, output, {
            "plain": scheduler_stdout,
            "duty": scheduler_duty_stdout,
            "reserve-duty": scheduler_reserve_stdout,
            "gl-admission": admission_stdout,
            "video-priority": priority_stdout,
            "render-cost-plain": render_cost_stdout[0],
            "render-cost-int2j": render_cost_stdout[1],
        }, phase_cache_off_stdout)
        floor_debt_report.append(verify_floor_debt_cmake(root, output))
        admit_wait_report.append(verify_admit_wait_cmake(root, output))
        admit_behind_report.append(verify_admit_behind_cmake(root, output))
        admit_behind_report.append(verify_admit_behind_off_reading(root))
        consumer_timeout_report = [
            verify_consumer_timeout_source(root, cc, output),
            verify_consumer_timeout_cmake(root, output),
        ]

    scheduler_expected = "Vita cadence30 scheduler oracle: PASS"
    phase_expected = "Vita cadence30/phase integration oracle: PASS"
    if scheduler_expected not in scheduler_stdout:
        raise AssertionError(f"unexpected scheduler oracle: {scheduler_stdout!r}")
    if scheduler_expected not in scheduler_exit_stdout:
        raise AssertionError(
            f"unexpected exit-site scheduler oracle: {scheduler_exit_stdout!r}")
    duty_expected = "Vita cadence30 fps-lock duty: PASS"
    transition_expected = "Vita Continue->cutscene->gameplay: PASS"
    for stdout in (scheduler_stdout, scheduler_exit_stdout,
                   scheduler_duty_stdout, scheduler_reserve_stdout, priority_stdout, *head_stdout):
        if transition_expected not in stdout or scheduler_expected not in stdout:
            raise AssertionError(f"missing Continue transition oracle: {stdout!r}")
    if duty_expected not in scheduler_reserve_stdout:
        raise AssertionError(f"missing reserve/duty oracle: {scheduler_reserve_stdout!r}")
    if scheduler_expected not in scheduler_duty_stdout or \
            duty_expected not in scheduler_duty_stdout:
        raise AssertionError(
            f"unexpected fps-lock duty oracle: {scheduler_duty_stdout!r}")
    for stdout in head_stdout:
        if scheduler_expected not in stdout or "Vita full-head advance: PASS" not in stdout:
            raise AssertionError(f"unexpected full-head oracle: {stdout!r}")
    if phase_expected not in phase_cache_off_stdout or \
            "records=6; ordered/bounded t,c,a,s,r,w" not in \
            phase_cache_off_stdout or \
            "report-at exact/monotonic" not in phase_cache_off_stdout:
        raise AssertionError(
            f"unexpected cache-OFF phase oracle: {phase_cache_off_stdout!r}"
        )
    if phase_expected not in phase_cache_on_stdout or \
            "records=7; ordered/bounded t,c,a,g,s,r,w" not in \
            phase_cache_on_stdout or \
            "report-at exact/monotonic" not in phase_cache_on_stdout:
        raise AssertionError(
            f"unexpected cache-ON phase oracle: {phase_cache_on_stdout!r}"
        )
    if "Vita cadence scheduler-OFF boundary oracle: PASS" not in \
            lifecycle_off_stdout:
        raise AssertionError(
            f"unexpected scheduler-OFF lifecycle oracle: {lifecycle_off_stdout!r}"
        )
    if "Vita cadence production-boundary lifecycle oracle: PASS" not in \
            lifecycle_on_stdout:
        raise AssertionError(
            f"unexpected scheduler-ON lifecycle oracle: {lifecycle_on_stdout!r}"
        )
    wait_off = "Vita cadence30 Present-wait admission: PASS (inclusive render cost;"
    wait_on = ("Vita cadence30 Present-wait admission: PASS "
               "(render-cost=excludes-present;")
    wait_pinned = "Vita cadence30 Present-wait costs (service reserve): pinned 39 cases"
    for stdout in (scheduler_stdout, scheduler_exit_stdout, scheduler_duty_stdout,
                   scheduler_reserve_stdout, priority_stdout, *head_stdout):
        if wait_off not in stdout or wait_on in stdout:
            raise AssertionError(f"missing knob-OFF Present-wait oracle: {stdout!r}")
    for stdout in render_cost_stdout:
        if (wait_on not in stdout or wait_off in stdout or
                scheduler_expected not in stdout or
                transition_expected not in stdout):
            raise AssertionError(f"unexpected knob-ON scheduler oracle: {stdout!r}")
    for stdout in (scheduler_reserve_stdout, priority_stdout, render_cost_stdout[1]):
        if wait_pinned not in stdout:
            raise AssertionError(f"missing pinned Present-wait costs: {stdout!r}")
    print(scheduler_stdout.strip())
    print(next(line for line in scheduler_duty_stdout.splitlines()
               if duty_expected in line))
    print("Continue transition with service-reserve/runtime-duty/Exit-sites combined: PASS")
    if "Vita video Render priority: PASS" not in priority_stdout:
        raise AssertionError(f"missing video priority oracle: {priority_stdout!r}")
    print(next(line for line in priority_stdout.splitlines()
               if "Vita video Render priority: PASS" in line))
    print(next(line for line in head_stdout[0].splitlines()
               if "Vita full-head advance: PASS" in line))
    print("Full-head OFF emitted-code identity and refused heavy oversleep model: PASS")
    for stdout in (priority_stdout, render_cost_stdout[1]):
        for line in stdout.splitlines():
            if "Present-wait" in line:
                print(line)
    print("Render-cost knob: all baseline scenarios PASS with the knob ON "
          "(plain, int2j); oversleep model identical OFF/ON: PASS")
    print(phase_cache_off_stdout.strip())
    print(phase_cache_on_stdout.strip())
    print(lifecycle_off_stdout.strip())
    print(lifecycle_on_stdout.strip())
    for line in floor_debt_report:
        print(line)
    for line in admit_wait_report:
        print(line)
    for line in admit_behind_report:
        print(line)
    for line in consumer_timeout_report:
        print(line)
    print(
        "Vita cadence30 scheduler build contract: PASS "
        "(default OFF; exact three generated owners; production lifecycle; "
        "profiler-aligned)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
