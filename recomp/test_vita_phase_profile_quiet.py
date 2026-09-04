#!/usr/bin/env python3
"""Source/codegen oracle for production features with phase receipts OFF."""

from __future__ import annotations

from pathlib import Path
import re
import sys


HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import gen_all as G  # noqa: E402


def _active_lines(lines: tuple[str, ...], defines: set[str]) -> tuple[str, ...]:
    active = True
    stack: list[tuple[bool, bool]] = []
    result: list[str] = []
    for line in lines:
        stripped = line.strip()
        if stripped.startswith("#if "):
            names = re.findall(r"defined\(([^)]+)\)", stripped)
            condition = bool(names) and all(name in defines for name in names)
            stack.append((active, condition))
            active = active and condition
        elif stripped == "#else":
            parent, condition = stack[-1]
            active = parent and not condition
        elif stripped == "#endif":
            active, unused_condition = stack.pop()
        elif active:
            result.append(line)
    assert not stack
    return tuple(result)


def _joined_active(lines: tuple[str, ...], defines: set[str]) -> str:
    return "\n".join(_active_lines(lines, defines))


def _verify_codegen_off_on() -> None:
    vita = {"__vita__"}
    phase = {"ISAAC_VITA_PHASE_RECEIPTS"}

    pill_feature = {"ISAAC_VITA_PILL_BLOOM_BYPASS"}
    pill = (
        G.render_vita_pill_bloom_bypass_local()
        + G.render_vita_pill_bloom_bypass_site(
            G.VITA_PILL_BLOOM_BYPASS_CAPTURE_SITE_RVA
        )
        + G.render_vita_pill_bloom_bypass_site(
            G.VITA_PILL_BLOOM_BYPASS_COMPOSITE_SITE_RVA
        )
    )
    pill_off = _joined_active(pill, vita | pill_feature)
    pill_on = _joined_active(pill, vita | pill_feature | phase)
    for token in (
        "_guest_vita_pill_bloom_bypassed = 1U;",
        "goto L_002ceaad;",
        "goto L_002cf5a1;",
    ):
        assert token in pill_off and token in pill_on
    assert "kage_vita_phase_profile" not in pill_off
    assert "note_pill_bloom_capture_skip" in pill_on
    assert "note_pill_bloom_composite_skip" in pill_on

    half_feature = {"ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES"}
    half_create = G.render_vita_transient_bloom_half_res_create()
    half_off = _joined_active(half_create, vita | half_feature)
    half_on = _joined_active(half_create, vita | half_feature | phase)
    for token in (
        "_guest_vita_bloom_width / 2U",
        "_guest_vita_bloom_height / 2U",
    ):
        assert token in half_off and token in half_on
    assert "kage_vita_phase_profile" not in half_off
    assert half_on.count("note_bloom_half_create") == 4
    for site in G.VITA_TRANSIENT_BLOOM_HALF_RES_RENDER_SITES:
        proof = G.render_vita_transient_bloom_half_res_render(site)
        assert not _active_lines(proof, vita | half_feature)
        assert "kage_vita_phase_profile" in _joined_active(
            proof, vita | half_feature | phase
        )

    poop_feature = {
        "ISAAC_VITA_POOP_FX_PROFILE",
        "ISAAC_VITA_POOP_FX_SINGLE_CLOUD",
    }
    poop_add = G.render_vita_poop_fx_site(
        "add", G.VITA_POOP_FX_ADD_CAPTURE_SITE_RVA
    )
    poop_active = G.render_vita_poop_fx_site(
        "render", G.VITA_POOP_FX_RENDER_ACTIVE_SITE_RVA
    )
    poop_cap = G.render_vita_poop_fx_site(
        "render", G.VITA_POOP_FX_RENDER_CAP_SITE_RVA
    )
    assert not _active_lines(poop_add, vita | poop_feature)
    assert not _active_lines(poop_active, vita | poop_feature)
    cap_off = _joined_active(poop_cap, vita | poop_feature)
    cap_on = _joined_active(poop_cap, vita | poop_feature | phase)
    assert "goto L_004ec78e;" in cap_off and "goto L_004ec78e;" in cap_on
    assert "kage_vita_phase_profile" not in cap_off
    assert "note_poop_fx_cap" in cap_on


def _verify_build_contract() -> None:
    cmake = (HERE / "vita/CMakeLists.txt").read_text(encoding="utf-8")
    hooks = (HERE / "runtime/kage_vita_generated_hooks.c").read_text(
        encoding="utf-8"
    )
    gl_backend = (HERE / "runtime/gl_vita_backend.c").read_text(
        encoding="utf-8"
    )
    build_script = (
        HERE / "vita/vitagl-stock-reference/build.sh"
    ).read_text(encoding="utf-8")
    build_tool = (HERE.parent / "tools/build_vita.py").read_text(
        encoding="utf-8"
    )

    for feature in (
        "ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS",
        "ISAAC_VITA_PILL_BLOOM_BYPASS",
        "ISAAC_VITA_TRANSIENT_BLOOM_HALF_RES",
        "ISAAC_VITA_POOP_FX_PROFILE",
    ):
        assert f"{feature} requires ISAAC_VITA_PHASE_PROFILE" not in cmake
    assert "ISAAC_VITA_FULLSPEED_SCHEDULER requires ISAAC_VITA_KAGE=ON" \
        in cmake
    assert cmake.count("ISAAC_VITA_PHASE_RECEIPTS=1") == 3
    assert "ISAAC_VITA_PHASE_PROFILE_MODE_FILE" in cmake
    assert '"ISAAC_PHASE_PROFILE=${ISAAC_VITA_PHASE_PROFILE_MODE}"' in cmake
    assert 'option(ISAAC_VITA_STAGE_HEARTBEAT' in cmake
    assert "ISAAC_VITA_STAGE_HEARTBEAT=1" in cmake
    assert hooks.count("defined(ISAAC_VITA_STAGE_HEARTBEAT)") >= 8
    assert "#if defined(ISAAC_VITA_STAGE_HEARTBEAT)\n    if " \
        "(kage_vita_log_stage_heartbeat" in hooks
    shader_source = gl_backend[
        gl_backend.index("static void vita_glShaderSource"):
        gl_backend.index("static void vita_glTexImage2D")
    ]
    assert "ISAAC_VITA_COLOROFFSET_GPU_OPTIMIZATIONS" in shader_source
    assert "ISAAC_VITA_PHASE_PROFILE" not in shader_source

    assert "phase_profile=${ISAAC_PHASE_PROFILE:-0}" in build_script
    assert "HAVE_ISAAC_PHASE_PROFILE=1" in build_script
    assert "printf 'phase_profile=%s\\n'" in build_script
    assert '"ISAAC_VITA_PHASE_PROFILE",\n         "phase_profile"' in build_tool
    assert build_tool.count('"-DISAAC_VITA_STAGE_HEARTBEAT=OFF"') == 2


def _verify_vitagl_counter_fences() -> None:
    gpu = (
        HERE / "vita/vitagl-stock-reference/0002-exact-gpu-draw-optimizations.patch"
    ).read_text(encoding="utf-8")
    color = (
        HERE
        / "vita/vitagl-stock-reference/0003-exact-coloroffset-gpu-optimizations.patch"
    ).read_text(encoding="utf-8")
    gxm = (
        HERE / "vita/vitagl-stock-reference/isaac_gxm_state_policy.h"
    ).read_text(encoding="utf-8")
    assert gpu.count("#ifdef HAVE_ISAAC_PHASE_PROFILE") == 1
    assert "#define ISAAC_GPU_DRAW_COUNT(field) ((void)0)" in gpu
    assert gpu.count("ISAAC_GPU_DRAW_COUNT(") >= 16
    assert gpu.count("ISAAC_GPU_DRAW_ADD(") >= 6
    assert color.count("#ifdef HAVE_ISAAC_PHASE_PROFILE") == 1
    assert "#define ISAAC_COLOROFFSET_COUNT(field) ((void)0)" in color
    assert color.count("ISAAC_COLOROFFSET_COUNT(") >= 18
    assert color.count("ISAAC_COLOROFFSET_ADD(") >= 3

    gpu_writes = re.findall(
        r"^\+.*isaac_gpu_draw_stats\.[a-z_]+(?:\+\+|\s*\+=|\s*=)",
        gpu,
        flags=re.M,
    )
    color_writes = re.findall(
        r"^\+.*isaac_coloroffset_stats\.[a-z_]+(?:\+\+|\s*\+=|\s*=)",
        color,
        flags=re.M,
    )
    assert len(gpu_writes) == 3
    assert len(color_writes) == 2
    assert gxm.count("#ifdef HAVE_ISAAC_PHASE_PROFILE") == 1
    assert "#define ISAAC_GXM_STATE_COUNT(policy, field) ((void)0)" in gxm
    assert gxm.count("ISAAC_GXM_STATE_COUNT(policy,") == 8
    assert gxm.count("policy->counters.") == 0


def main() -> int:
    _verify_codegen_off_on()
    _verify_build_contract()
    _verify_vitagl_counter_fences()
    print(
        "Vita production quiet-profile oracle: PASS; "
        "feature behavior stable; guest/GL/GXM receipts OFF"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
