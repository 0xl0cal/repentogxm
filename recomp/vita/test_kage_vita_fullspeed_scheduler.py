#!/usr/bin/env python3
"""Compile/run parity-aware cadence oracles and freeze its build surface."""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import tempfile
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
    if backend.count("KAGE_VITA_FULLSPEED_RESET();") != 1 or \
            backend.count("KAGE_VITA_FULLSPEED_DEACTIVATE();") != 1 or \
            backend.count("kage_vita_backend_activate_session();") != 2:
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

    scheduler_expected = "Vita cadence30 scheduler oracle: PASS"
    phase_expected = "Vita cadence30/phase integration oracle: PASS"
    if scheduler_expected not in scheduler_stdout:
        raise AssertionError(f"unexpected scheduler oracle: {scheduler_stdout!r}")
    if scheduler_expected not in scheduler_exit_stdout:
        raise AssertionError(
            f"unexpected exit-site scheduler oracle: {scheduler_exit_stdout!r}")
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
    print(scheduler_stdout.strip())
    print(head_stdout[0].splitlines()[0])
    print("Full-head OFF emitted-code identity and refused heavy oversleep model: PASS")
    print(phase_cache_off_stdout.strip())
    print(phase_cache_on_stdout.strip())
    print(lifecycle_off_stdout.strip())
    print(lifecycle_on_stdout.strip())
    print(
        "Vita cadence30 scheduler build contract: PASS "
        "(default OFF; exact three generated owners; production lifecycle; "
        "profiler-aligned)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
