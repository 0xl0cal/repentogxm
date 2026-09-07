#!/usr/bin/env python3
"""Prove the stock-reference patch chain cannot change texture-name allocation."""

from __future__ import annotations

import hashlib
import re
from pathlib import Path


SOURCE_COMMIT = "73dd57a8857f89f2353881c6de5891959c5c1983"
# Reviewed through 0033. The recipe changed, not the stock name allocator.
BUILD_SCRIPT_SHA256 = "4f5c15dc5453f7fed16f7f1225009cd0bd2f50200ea59bd3144342fa8ebf0434"
EXPECTED_PATHS = {
    "0001-deterministic-build-and-init-oob.patch": (
        "Makefile",
        "source/vgl.c",
    ),
    "0002-exact-gpu-draw-optimizations.patch": (
        "source/custom_shaders.c",
        "source/draw.c",
        "source/ffp.c",
        "source/framebuffers.c",
        "source/gxm.c",
        "source/isaac_gxm_state_shadow.c",
        "source/isaac_gxm_state_shadow.h",
        "source/misc.c",
        "source/shared.h",
        "source/vgl.c",
        "source/vitaGL.h",
    ),
    "0003-exact-coloroffset-gpu-optimizations.patch": (
        "source/custom_shaders.c",
        "source/vitaGL.h",
    ),
    "0004-hardened-custom-shader-cache.patch": (
        "source/custom_shaders.c",
        "source/vitaGL.h",
    ),
    # gxm.c-only diagnostics/knobs (GL_TIME_PROFILE: 0005 + 0006; FBO render
    # target scenes: 0007) and the FBO valid region (0009: gxm.c plus the
    # glViewport record in misc.c and the tile-clipper clamp in tests.c);
    # none of them may reach textures.c.
    "0005-isaac-scene-timer.patch": ("source/gxm.c",),
    "0006-isaac-scene-split.patch": ("source/gxm.c",),
    "0007-isaac-fbo-rt-scenes.patch": ("source/gxm.c",),
    # Diagnostic ColorOffset fragment-shader probe: custom_shaders.c only
    # (vitaGL.h stays byte-identical; the statistics struct is consumer-declared).
    "0008-isaac-coloroffset-fs-probe.patch": ("source/custom_shaders.c",),
    "0012-isaac-coloroffset-staging-proof.patch": ("source/custom_shaders.c",),
    "0013-isaac-coloroffset-plain-fastpath.patch": ("source/custom_shaders.c",),
    # Optional storage upload/promotion hooks, not texture-name allocation.
    "0010-isaac-p8-safe-upload.patch": ("source/textures.c",),
    "0009-isaac-fbo-valid-region.patch": (
        "source/gxm.c",
        "source/misc.c",
        "source/tests.c",
    ),
    "0011-isaac-exact-laser-p8.patch": (
        "source/framebuffers.c", "source/textures.c", "source/utils/gpu_utils.h"),
    "0014-isaac-single-final-fragment-bind.patch": ("source/custom_shaders.c",),
    "0015-isaac-coloroffset-plain-vertex-pair.patch": ("source/custom_shaders.c",),
    "0016-isaac-laser-light-halo-clip.patch": (
        "source/custom_shaders.c", "source/draw.c", "source/shared.h"),
    "0017-isaac-laser-light-nearest.patch": (
        "source/draw.c", "source/shared.h", "source/isaac_gxm_state_shadow.c",
        "source/isaac_gxm_state_shadow.h"),
    "0018-isaac-transform-uniform-noop.patch": ("source/custom_shaders.c",),
    "0019-isaac-laser-atlas-nearest.patch": (
        "source/custom_shaders.c", "source/draw.c", "source/shared.h"),
    "0020-isaac-native-resource-profile.patch": (
        "source/framebuffers.c", "source/gxm.c", "source/shared.h",
        "source/textures.c", "source/utils/gpu_utils.c", "source/utils/mem_utils.h"),
    # 0021 adds only an RT invalidation hook to glDeleteTextures; the whole
    # function changes, but texture-name allocation/recycling does not.
    "0021-isaac-fbo-rt-reuse.patch": (
        "source/framebuffers.c", "source/gxm.c", "source/shared.h", "source/textures.c"),
    "0022-isaac-coloroffset-link-proof.patch": ("source/custom_shaders.c",),
    "0023-isaac-skip-zero-fragment-uniform.patch": ("source/custom_shaders.c",),
    "0024-isaac-fbo-rt-bounded-lease.patch": (
        "source/shared.h", "source/gxm.c", "source/utils/gpu_utils.c"),
    "0025-isaac-sdk-sparse-profile.patch": (
        "source/draw.c", "source/misc.c", "source/gxm.c"),
    "0026-isaac-coloroffset-metadata-once.patch": ("source/custom_shaders.c",),
    "0027-isaac-laser-halo-profile.patch": ("source/custom_shaders.c",),
    "0028-sync-fbo-float-selector.patch": ("source/gxm.c",),
    "0029-invalidate-resized-fbo-scissor.patch": ("source/shared.h",),
    "0030-correct-flipped-scissor-replay.patch": ("source/gxm.c",),
    "0031-correct-flipped-scissor-replay-valid-region.patch": ("source/gxm.c",),
    "0032-isaac-white-plain-census.patch": ("source/custom_shaders.c",),
    "0033-isaac-fbo-rt-interlude-reuse.patch": ("source/shared.h",),
}
# Keep declarations and application order independent: SDK sparse is declared
# beside the scene profiles but applied after metadata-once. Both remain exact.
EXPECTED_PATCH_VARIABLES = (
    ("contract_patch", "0001-deterministic-build-and-init-oob.patch"),
    ("gpu_draw_patch", "0002-exact-gpu-draw-optimizations.patch"),
    ("coloroffset_patch", "0003-exact-coloroffset-gpu-optimizations.patch"),
    ("coloroffset_fs_probe_patch", "0008-isaac-coloroffset-fs-probe.patch"),
    ("shader_cache_patch", "0004-hardened-custom-shader-cache.patch"),
    ("coloroffset_staging_patch", "0012-isaac-coloroffset-staging-proof.patch"),
    ("coloroffset_plain_patch", "0013-isaac-coloroffset-plain-fastpath.patch"),
    ("coloroffset_single_final_bind_patch", "0014-isaac-single-final-fragment-bind.patch"),
    ("coloroffset_plain_vertex_pair_patch", "0015-isaac-coloroffset-plain-vertex-pair.patch"),
    ("scene_timer_patch", "0005-isaac-scene-timer.patch"),
    ("scene_split_patch", "0006-isaac-scene-split.patch"),
    ("sdk_sparse_patch", "0025-isaac-sdk-sparse-profile.patch"),
    ("fbo_rt_scenes_patch", "0007-isaac-fbo-rt-scenes.patch"),
    ("p8_safe_patch", "0010-isaac-p8-safe-upload.patch"),
    ("fbo_valid_region_patch", "0009-isaac-fbo-valid-region.patch"),
    ("laser_p8_patch", "0011-isaac-exact-laser-p8.patch"),
    ("laser_light_halo_patch", "0016-isaac-laser-light-halo-clip.patch"),
    ("laser_light_nearest_patch", "0017-isaac-laser-light-nearest.patch"),
    ("coloroffset_transform_uniform_patch", "0018-isaac-transform-uniform-noop.patch"),
    ("laser_atlas_nearest_patch", "0019-isaac-laser-atlas-nearest.patch"),
    ("native_resource_patch", "0020-isaac-native-resource-profile.patch"),
    ("fbo_rt_reuse_patch", "0021-isaac-fbo-rt-reuse.patch"),
    ("fbo_rt_reuse_lease_patch", "0024-isaac-fbo-rt-bounded-lease.patch"),
    ("fbo_rt_interlude_patch", "0033-isaac-fbo-rt-interlude-reuse.patch"),
    ("coloroffset_link_proof_patch", "0022-isaac-coloroffset-link-proof.patch"),
    ("skip_zero_fragment_uniform_patch", "0023-isaac-skip-zero-fragment-uniform.patch"),
    ("coloroffset_metadata_once_patch", "0026-isaac-coloroffset-metadata-once.patch"),
    ("laser_halo_profile_patch", "0027-isaac-laser-halo-profile.patch"),
    ("white_census_patch", "0032-isaac-white-plain-census.patch"),
    ("fbo_float_sync_patch", "0028-sync-fbo-float-selector.patch"),
    ("fbo_scissor_resize_patch", "0029-invalidate-resized-fbo-scissor.patch"),
    ("fbo_scissor_replay_patch", "0030-correct-flipped-scissor-replay.patch"),
    ("fbo_scissor_replay_region_patch", "0031-correct-flipped-scissor-replay-valid-region.patch"),
)
EXPECTED_PATCH_APPLICATIONS = (
    "contract_patch", "gpu_draw_patch", "coloroffset_patch",
    "coloroffset_fs_probe_patch", "shader_cache_patch", "coloroffset_staging_patch",
    "coloroffset_plain_patch", "coloroffset_single_final_bind_patch",
    "coloroffset_plain_vertex_pair_patch", "scene_timer_patch", "scene_split_patch",
    "fbo_rt_scenes_patch", "p8_safe_patch", "fbo_valid_region_patch", "laser_p8_patch",
    "laser_light_halo_patch", "laser_light_nearest_patch",
    "coloroffset_transform_uniform_patch", "laser_atlas_nearest_patch",
    "native_resource_patch", "fbo_rt_reuse_patch", "fbo_rt_reuse_lease_patch",
    "fbo_rt_interlude_patch", "coloroffset_link_proof_patch",
    "skip_zero_fragment_uniform_patch", "coloroffset_metadata_once_patch",
    "sdk_sparse_patch", "laser_halo_profile_patch", "fbo_float_sync_patch",
    "fbo_scissor_resize_patch", "fbo_scissor_replay_region_patch",
    "fbo_scissor_replay_patch", "white_census_patch",
)
FORBIDDEN_ALLOCATOR_TOKENS = (
    "glGenTextures",
    "glDeleteTextures",
    "texture_slots",
    "TEXTURES_NUM",
    "TEX_UNUSED",
    "TEX_UNINITIALIZED",
    "gpu_free_texture",
)


def patch_changed_paths_and_lines(path: Path) -> tuple[tuple[str, ...], list[str]]:
    changed_paths: list[str] = []
    changed_lines: list[str] = []
    for line in path.read_text(encoding="utf-8").splitlines():
        if line.startswith("diff --git "):
            match = re.fullmatch(r"diff --git a/(\S+) b/(\S+)", line)
            if match is None or match.group(1) != match.group(2):
                raise AssertionError(f"unmodelled patch path header: {line!r}")
            changed_paths.append(match.group(1))
        elif line.startswith(("+++ ", "--- ")):
            continue
        elif line.startswith(("+", "-")):
            changed_lines.append(line[1:])
    if len(changed_paths) != len(set(changed_paths)):
        raise AssertionError(f"duplicate patch path in {path.name}")
    return tuple(changed_paths), changed_lines


def main() -> int:
    stock = Path(__file__).resolve().parent / "vitagl-stock-reference"
    build_script_path = stock / "build.sh"
    build_script_bytes = build_script_path.read_bytes()
    if hashlib.sha256(build_script_bytes).hexdigest() != BUILD_SCRIPT_SHA256:
        raise AssertionError("stock vitaGL build script drifted")
    build_script = build_script_bytes.decode("utf-8")
    match = re.search(r"(?m)^source_commit=([0-9a-f]{40})$", build_script)
    if match is None or match.group(1) != SOURCE_COMMIT:
        raise AssertionError("stock vitaGL source commit drifted")

    actual_patch_names = {path.name for path in stock.glob("*.patch")}
    if actual_patch_names != set(EXPECTED_PATHS):
        raise AssertionError(
            "stock vitaGL patch-file census drifted: "
            f"{sorted(actual_patch_names)!r} != {sorted(EXPECTED_PATHS)!r}"
        )
    patch_variables = tuple(re.findall(
        r'(?m)^([a-z][a-z0-9_]*_patch)="\$script_dir/([^"/]+\.patch)"$',
        build_script,
    ))
    if patch_variables != EXPECTED_PATCH_VARIABLES:
        raise AssertionError(
            f"stock vitaGL patch-variable census drifted: {patch_variables!r}"
        )
    patch_applications = tuple(re.findall(
        r'(?m)^\s+apply_source_patch "\$([a-z][a-z0-9_]*_patch)"$',
        build_script,
    ))
    if patch_applications != EXPECTED_PATCH_APPLICATIONS:
        raise AssertionError(
            "stock vitaGL patch-application census drifted: "
            f"{patch_applications!r} != {EXPECTED_PATCH_APPLICATIONS!r}"
        )
    if "textures.c" in build_script:
        raise AssertionError("stock vitaGL build script acquired a textures.c mutation")
    if 'p8_safe_upload=${ISAAC_P8_SAFE_UPLOAD:-0}' not in build_script or not re.search(
        r'if \[ "\$p8_safe_upload" = 1 \]; then\s+'
        r'apply_source_patch "\$p8_safe_patch"\s+'
        r'cp -- "\$p8_safe_policy" "\$source_dir/source/isaac_p8_texture_safety.h"\s+'
        r'fi', build_script,
    ):
        raise AssertionError("P8 texture storage hooks escaped their default-OFF switch")

    # 0010 calls a helper in the existing storage owner. Reading an existing
    # texture's UNINITIALIZED state and retiring its old pixel/palette storage
    # are allowed; allocation/recycling of texture names is still forbidden.
    p8_policy = (stock / "isaac_p8_texture_safety.h").read_text(encoding="utf-8")
    for token in FORBIDDEN_ALLOCATOR_TOKENS:
        if token not in ("TEX_UNINITIALIZED", "gpu_free_texture") and token in p8_policy:
            raise AssertionError(f"P8 storage helper reached name allocator: {token}")
    if re.search(r"\bgpu_free_texture\s*\(|\btex->status\s*=\s*TEX_UNINITIALIZED\b", p8_policy):
        raise AssertionError("P8 storage helper recycles a texture name")
    laser_policy = (stock / "isaac_laser_p8.h").read_text(encoding="utf-8")
    if re.search(r"\b(?:glGenTextures|glDeleteTextures|gpu_free_texture)\s*\(|"
                 r"\b(?:tex->|staged\.)status\s*=\s*TEX_(?:UNUSED|UNINITIALIZED)\b",
                 laser_policy):
        raise AssertionError("exact laser P8 helper changes texture-name ownership")
    if 'laser_atlas_p8=${ISAAC_LASER_ATLAS_P8:-0}' not in build_script or not re.search(
        r'if \[ "\$laser_atlas_p8" = 1 \]; then\s+'
        r'apply_source_patch "\$laser_p8_patch"\s+'
        r'cp -- "\$laser_p8_policy" "\$source_dir/source/isaac_laser_p8.h"\s+'
        r'cp -- "\$laser_p8_reference" "\$source_dir/source/isaac_laser_p8_reference.h"\s+'
        r'fi', build_script,
    ):
        raise AssertionError("exact laser P8 hooks escaped their default-OFF switch")
    if 'fbo_rt_reuse=${ISAAC_FBO_RT_REUSE:-0}' not in build_script or not re.search(
        r'if \[ "\$fbo_rt_reuse" = 1 \]; then\s+'
        r'apply_source_patch "\$fbo_rt_reuse_patch"\s+'
        r'cp -- "\$fbo_rt_reuse_header" "\$source_dir/source/isaac_fbo_rt_reuse.h"\s+'
        r'cp -- "\$fbo_rt_reuse_source" "\$source_dir/source/isaac_fbo_rt_reuse.c"\s+'
        r'fi', build_script,
    ):
        raise AssertionError("RT retirement hook escaped its default-OFF switch")
    # The one permitted glDeleteTextures edit invalidates an RT lease, not a
    # texture name. Pin the complete textures.c delta, not just its call token.
    rt_patch = (stock / "0021-isaac-fbo-rt-reuse.patch").read_text(encoding="utf-8")
    rt_textures = rt_patch.split(
        "diff --git a/source/textures.c b/source/textures.c\n", 1
    )[1]
    rt_changes = [line for line in rt_textures.splitlines()
                  if line.startswith(("+", "-")) and not line.startswith(("+++", "---"))]
    if rt_changes != [
        "+#ifdef HAVE_ISAAC_FBO_RT_REUSE",
        "+\t\t\t\t\t\tisaacFboRtInvalidate((uintptr_t)fb);",
        "+#endif",
    ]:
        raise AssertionError("RT reuse changed more than its exact retirement hook")
    for helper in ("isaac_fbo_rt_reuse.h", "isaac_fbo_rt_reuse.c"):
        text = (stock / helper).read_text(encoding="utf-8")
        for token in FORBIDDEN_ALLOCATOR_TOKENS:
            if token in text:
                raise AssertionError(f"RT helper reached texture-name allocator: {helper}: {token}")
    # The new lifecycle hook reads existing slots, never allocates names.
    # Keep the exception exact; native glGen/glDelete bodies are compared by
    # the freshly-materialized patch-chain check too.
    laser_slot_reads = {
        "if (tex_id && tex_id < TEXTURES_NUM)",
        "isaacLaserP8Restore(&texture_slots[tex_id]);",
        "if (texture < TEXTURES_NUM &&",
        "isaacLaserP8Restore(&texture_slots[texture]);",
        "texture_units[unit].tex_id[0] < TEXTURES_NUM)",
        "isaacLaserP8Restore(&texture_slots[texture_units[unit].tex_id[0]]);",
    }

    checked_lines = 0
    for name, expected_paths in EXPECTED_PATHS.items():
        paths, changed_lines = patch_changed_paths_and_lines(stock / name)
        if paths != expected_paths:
            raise AssertionError(
                f"{name} path census drifted: {paths!r} != {expected_paths!r}"
            )
        if "source/textures.c" in paths and name not in (
                "0010-isaac-p8-safe-upload.patch", "0011-isaac-exact-laser-p8.patch",
                "0020-isaac-native-resource-profile.patch", "0021-isaac-fbo-rt-reuse.patch"):
            raise AssertionError(f"{name} acquired the texture allocator owner")
        for line in changed_lines:
            for token in FORBIDDEN_ALLOCATOR_TOKENS:
                if token in line:
                    if name == "0011-isaac-exact-laser-p8.patch" and line.strip() in laser_slot_reads:
                        continue
                    raise AssertionError(
                        f"{name} changed allocator token {token!r}: {line!r}"
                    )
        checked_lines += len(changed_lines)

    print(
        "Stock vitaGL texture allocator patch isolation: PASS; "
        f"commit={SOURCE_COMMIT[:7]} patches={len(EXPECTED_PATHS)} "
        f"changed_lines={checked_lines}"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
