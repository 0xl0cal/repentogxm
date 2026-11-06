#!/usr/bin/env python3
"""Compile/run parity-aware cadence oracles and freeze its build surface."""

from __future__ import annotations

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
    print(
        "Vita cadence30 scheduler build contract: PASS "
        "(default OFF; exact three generated owners; production lifecycle; "
        "profiler-aligned)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
