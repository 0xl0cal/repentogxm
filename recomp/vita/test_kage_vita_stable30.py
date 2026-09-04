#!/usr/bin/env python3
"""Compile/run the stable30 oracle and freeze its opt-in build surface."""

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
    wrapper = (root / "tools" / "build_vita.py").read_text(
        encoding="utf-8"
    )
    backend = (
        root / "recomp" / "runtime" / "kage_vita_backend.c"
    ).read_text(encoding="utf-8")
    hooks = (
        root / "recomp" / "runtime" / "kage_vita_generated_hooks.c"
    ).read_text(encoding="utf-8")
    policy = (
        root / "recomp" / "runtime" / "kage_vita_stable30.c"
    ).read_text(encoding="utf-8")
    header = (
        root / "recomp" / "runtime" / "kage_vita_stable30.h"
    ).read_text(encoding="utf-8")
    generator = (root / "recomp" / "gen_all.py").read_text(
        encoding="utf-8"
    )
    gate = (
        root / "recomp" / "vita" / "vita_raw_allocator_gate.py"
    ).read_text(encoding="utf-8")

    for needle in (
        "option(ISAAC_VITA_STABLE_30_PRESENTATION",
        '"${ISAAC_RUNTIME}/kage_vita_stable30.c"',
        r'REGEX "^void sub_0048bc50\\(CPU \\*__restrict c\\)$"',
        r'REGEX "^void sub_004b0010\\(CPU \\*__restrict c\\)$"',
        r'REGEX "^void sub_004b0600\\(CPU \\*__restrict c\\)$"',
        "ISAAC_VITA_STABLE30_GENERATED_OWNER_COUNT EQUAL 2",
        "ISAAC_VITA_STABLE30_BUILD_ID=\\\"${ISAAC_VITA_GUEST_LINK_ID}\\\"",
        "stable 30-Hz presentation and the fullspeed scheduler are exclusive",
    ):
        if needle not in cmake:
            raise AssertionError(f"CMake stable30 contract lost: {needle}")
    start = cmake.index("    if(ISAAC_VITA_STABLE_30_PRESENTATION)")
    end = cmake.index("    if(ISAAC_VITA_GUEST_LOOKUP_CACHE)", start)
    block = cmake[start:end]
    if block.count(
            "foreach(ISAAC_GENERATED_SOURCE IN LISTS "
            "ISAAC_GENERATED_CANONICAL_C)") != 1:
        raise AssertionError("stable30 owner discovery left canonical corpus")
    if "foreach(ISAAC_GENERATED_SOURCE IN LISTS ISAAC_GENERATED_C)" in block:
        raise AssertionError("stable30 owner discovery uses a derived list")
    for owner in ("APP", "MANAGER", "RENDER"):
        if block.count(f"ISAAC_STABLE30_{owner}_OWNER_MATCH") != 2:
            raise AssertionError(f"stable30 {owner.lower()} owner drifted")
    if block.count('"${ISAAC_VITA_DIRECT_DEFAULT_OUTPUT}"') != 1:
        raise AssertionError("stable30 direct-default owner remap drifted")
    if block.count("ISAAC_VITA_STABLE30_BUILD_ID=") != 1 or \
            block.count('"${ISAAC_RUNTIME}/kage_vita_stable30.c"') != 3:
        raise AssertionError("stable30 build-ID owner scope drifted")

    if '"-DISAAC_VITA_STABLE_30_PRESENTATION="' not in wrapper or \
            '"ISAAC_VITA_STABLE_30_PRESENTATION": stable_30_presentation' \
            not in wrapper or \
            "stable_30_presentation: bool = False" not in wrapper or \
            "assert not defaults.stable_30_presentation" not in wrapper:
        raise AssertionError("build wrapper no longer makes stable30 opt-in")
    if "--stable-30-presentation requires --kage" not in wrapper:
        raise AssertionError("build wrapper lost the stable30 KAGE guard")

    if backend.count("KAGE_VITA_STABLE30_RESET();") != 1 or \
            backend.count("KAGE_VITA_STABLE30_DEACTIVATE();") != 1:
        raise AssertionError("stable30 lifecycle boundary drifted")
    fullspeed = hooks.index("KAGE_VITA_FULLSPEED_LOOP_HEAD();")
    stable = hooks.index("KAGE_VITA_STABLE30_LOOP_HEAD();")
    stall = hooks.index("KAGE_VITA_STALL_NOTE_LOOP();")
    if not fullspeed < stable < stall:
        raise AssertionError("stable30 loop-head ownership/order drifted")
    present_note = hooks.index("KAGE_VITA_FULLSPEED_NOTE_PRESENT();")
    present_before = hooks.index("KAGE_VITA_STABLE30_BEFORE_PRESENT();")
    platform_present = hooks.index("result = kage_vita_backend_present();")
    present_after = hooks.index("KAGE_VITA_STABLE30_AFTER_PRESENT(result);")
    present_return = hooks.index("return result;", present_after)
    if not present_note < present_before < platform_present < \
            present_after < present_return:
        raise AssertionError("stable30 pre/post-Present wrapper order drifted")
    for marker in (
        "kage_pc_backend_stable30_manager_entry",
        "kage_pc_backend_stable30_plan_render",
        "kage_pc_backend_stable30_bypass_limiter",
    ):
        if hooks.count(marker) != 1 or generator.count(marker) != 2:
            raise AssertionError(f"stable30 generated route drifted: {marker}")

    if "KAGE_VITA_STABLE30_PERIOD_UNITS 100000u" not in header:
        raise AssertionError("stable30 exact 1/30 period changed")
    for receipt in (
        "KAGE VITA STABLE30: bid=%.32s period=100000/3us",
        "manager=004b0027>004b0043 render=004b0614>004b1089",
        "limiter=0048be3e present=wrapper-pre/post full-only=1",
        "ISAAC_VITA_STABLE30_BUILD_ID",
    ):
        if receipt not in policy:
            raise AssertionError(f"stable30 first-use receipt drifted: {receipt}")
    if policy.count("s_logged = 1u;") != 1:
        raise AssertionError("stable30 receipt is no longer first-use only")
    if "s_pacing_enabled = 0u;" not in policy or \
            "if (!s_enabled" not in policy or \
            "s_phase != KAGE_VITA_STABLE30_PHASE_NONFULL" not in policy:
        raise AssertionError("stable30 failure/deactivation split drifted")
    clear_start = policy.index("static void kage_vita_stable30_clear(")
    reset_start = policy.index("void kage_vita_stable30_reset(void)")
    loop_start = policy.index("void kage_vita_stable30_note_loop_head(void)")
    before_start = policy.index(
        "void kage_vita_stable30_before_present(void)"
    )
    if "s_render_present_armed = 0u;" not in \
            policy[clear_start:reset_start] or \
            "s_render_present_armed = 0u;" not in \
            policy[loop_start:before_start]:
        raise AssertionError("stable30 one-shot lifecycle clearing drifted")
    after_start = policy.index(
        "void kage_vita_stable30_after_present(int succeeded)"
    )
    manager_start = policy.index(
        "int kage_vita_stable30_manager_entry", after_start
    )
    before_block = policy[before_start:after_start]
    after_block = policy[after_start:manager_start]
    if "!s_render_present_armed" not in before_block or \
            "kage_vita_stable30_wait_until(s_present_deadline_units)" \
            not in before_block:
        raise AssertionError("stable30 pre-Present one-shot floor drifted")
    consume = after_block.index("s_render_present_armed = 0u;")
    success = after_block.index("if (!succeeded)")
    rebase = after_block.index(
        "now_units + KAGE_VITA_STABLE30_PERIOD_UNITS"
    )
    if "!s_render_present_armed" not in after_block or \
            "s_phase != KAGE_VITA_STABLE30_PHASE_FULL" not in after_block or \
            "now = isaac_vita_get_process_time();" not in after_block or \
            not consume < success < rebase:
        raise AssertionError("stable30 post-Present one-shot rebase drifted")
    plan_start = policy.index("int kage_vita_stable30_plan_render(")
    plan_end = policy.index("int kage_vita_stable30_finish_tick(", plan_start)
    plan_block = policy[plan_start:plan_end]
    for needle in (
        "s_render_present_armed = 0u;",
        "s_phase == KAGE_VITA_STABLE30_PHASE_FULL",
        "(manager_counter & 1u) != 0u",
        "s_render_present_armed = 1u;",
        "s_phase == KAGE_VITA_STABLE30_PHASE_NONFULL",
        "(manager_counter & 1u) == 0u",
    ):
        if needle not in plan_block:
            raise AssertionError(f"stable30 render ownership drifted: {needle}")
    for needle in (
        "VITA_STABLE30_MANAGER_GUARD_CHAIN_RVA = 0x004B0043",
        '"80bf6ca2040000"',
        '"740d"',
        '"80bf6da2040000"',
        '"0f8488050000"',
    ):
        if needle not in generator:
            raise AssertionError(f"stable30 Manager guard pin drifted: {needle}")
    for forbidden in (
        "sub_002cdcf0", "kage_pc_backend_present", "st8(", "st16(",
        "st32(", "st64(",
    ):
        if forbidden in policy:
            raise AssertionError(f"stable30 policy acquired guest work: {forbidden}")

    for needle in (
        'STABLE30_PRESENTATION_CACHE_KEY = "ISAAC_VITA_STABLE_30_PRESENTATION"',
        'STABLE30_BUILD_ID_MACRO = "ISAAC_VITA_STABLE30_BUILD_ID"',
        "def verify_stable30_compile_scope(",
        'expected_build_id_owners = {"kage_vita_stable30.c"}',
        "verify_stable30_compile_scope(records, cache)",
    ):
        if needle not in gate:
            raise AssertionError(f"raw gate stable30 contract lost: {needle}")
    if not re.search(
            r"stable30 generated owner/seam census changed:.*?"
            r"stable30 compile-definition scope changed:.*?"
            r"stable30 build-ID scope changed:", gate, re.DOTALL):
        raise AssertionError("raw gate stable30 fail-closed order drifted")


def main() -> int:
    vita = Path(__file__).resolve().parent
    root = vita.parent.parent
    runtime = root / "recomp" / "runtime"
    verify_source_contract(root)
    cc = compiler()
    with tempfile.TemporaryDirectory(prefix="isaac-stable30-") as value:
        output = Path(value) / (
            "stable30-oracle.exe" if os.name == "nt" else "stable30-oracle"
        )
        run([
            cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
            '-DISAAC_VITA_STABLE30_BUILD_ID="host-oracle"',
            f"-I{runtime}", f"-I{vita}",
            str(runtime / "kage_vita_stable30.c"),
            str(runtime / "kage_vita_stable30_oracle.c"),
            "-o", str(output),
        ])
        stdout = run([str(output)])
    expected = "Vita stable30 presentation oracle: PASS"
    if expected not in stdout:
        raise AssertionError(f"unexpected stable30 oracle: {stdout!r}")
    print(stdout.strip())
    print(
        "Vita stable30 build contract: PASS "
        "(default OFF; two exact generated owners; authenticated first-use "
        "receipt; render-owned one-shot pre/post-Present floor; "
        "failure parity fallback; "
        "no guest writes)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
