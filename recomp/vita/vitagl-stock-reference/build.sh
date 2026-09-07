#!/usr/bin/env bash
set -eu

source_commit=73dd57a8857f89f2353881c6de5891959c5c1983
source_short=73dd57a
source_epoch=1786822413
source_sha=f484dd9d2aec707ac5f91352c6e0631239f2330fe6769e8476cfe425331c400a
oob_fix_commit=f24ad3e66f7f34bebe70598d1302c34f4eff4a54

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
contract_patch="$script_dir/0001-deterministic-build-and-init-oob.patch"
gpu_draw_patch="$script_dir/0002-exact-gpu-draw-optimizations.patch"
gpu_draw_policy="$script_dir/isaac_gpu_draw_policy.h"
gxm_state_policy="$script_dir/isaac_gxm_state_policy.h"
coloroffset_patch="$script_dir/0003-exact-coloroffset-gpu-optimizations.patch"
coloroffset_policy="$script_dir/isaac_coloroffset_gpu_policy.h"
coloroffset_fs_probe_patch="$script_dir/0008-isaac-coloroffset-fs-probe.patch"
shader_cache_patch="$script_dir/0004-hardened-custom-shader-cache.patch"
coloroffset_staging_patch="$script_dir/0012-isaac-coloroffset-staging-proof.patch"
coloroffset_staging_policy="$script_dir/isaac_coloroffset_staging.h"
coloroffset_plain_patch="$script_dir/0013-isaac-coloroffset-plain-fastpath.patch"
coloroffset_plain_policy="$script_dir/isaac_coloroffset_plain_policy.h"
coloroffset_plain_integration="$script_dir/isaac_coloroffset_plain_vitagl.h"
coloroffset_plain_fp16_header="$script_dir/isaac_coloroffset_plain_fp16.h"
coloroffset_single_final_bind_patch="$script_dir/0014-isaac-single-final-fragment-bind.patch"
coloroffset_plain_vertex_pair_patch="$script_dir/0015-isaac-coloroffset-plain-vertex-pair.patch"
coloroffset_plain_vertex_pair_integration="$script_dir/isaac_coloroffset_plain_vertex_pair.h"
shader_cache_policy="$script_dir/isaac_shader_cache_policy.h"
shader_cache_integration="$script_dir/isaac_shader_cache_vitagl.h"
shader_cache_block_list="$script_dir/isaac_shader_cache_block_list.h"
scene_timer_patch="$script_dir/0005-isaac-scene-timer.patch"
scene_split_patch="$script_dir/0006-isaac-scene-split.patch"
sdk_sparse_patch="$script_dir/0025-isaac-sdk-sparse-profile.patch"
sdk_sparse_api="$script_dir/isaac_sdk_sparse_profile.h"
sdk_sparse_internal="$script_dir/isaac_sdk_sparse_profile_internal.h"
sdk_sparse_source="$script_dir/isaac_sdk_sparse_profile.c"
fbo_rt_scenes_patch="$script_dir/0007-isaac-fbo-rt-scenes.patch"
p8_safe_patch="$script_dir/0010-isaac-p8-safe-upload.patch"
p8_safe_policy="$script_dir/isaac_p8_texture_safety.h"
fbo_valid_region_patch="$script_dir/0009-isaac-fbo-valid-region.patch"
laser_p8_patch="$script_dir/0011-isaac-exact-laser-p8.patch"
laser_p8_policy="$script_dir/isaac_laser_p8.h"
laser_p8_reference="$script_dir/isaac_laser_p8_reference.h"
laser_light_halo_patch="$script_dir/0016-isaac-laser-light-halo-clip.patch"
laser_light_halo_policy="$script_dir/isaac_laser_light_halo_policy.h"
laser_light_halo_integration="$script_dir/isaac_laser_light_halo_vitagl.h"
laser_light_nearest_patch="$script_dir/0017-isaac-laser-light-nearest.patch"
laser_light_nearest_policy="$script_dir/isaac_laser_light_nearest.h"
coloroffset_transform_uniform_patch="$script_dir/0018-isaac-transform-uniform-noop.patch"
coloroffset_transform_uniform_header="$script_dir/isaac_coloroffset_transform_uniform.h"
laser_atlas_nearest_patch="$script_dir/0019-isaac-laser-atlas-nearest.patch"
laser_atlas_nearest_policy="$script_dir/isaac_laser_atlas_nearest.h"
native_resource_patch="$script_dir/0020-isaac-native-resource-profile.patch"
native_resource_api="$script_dir/isaac_native_resource_profile.h"
native_resource_internal="$script_dir/isaac_native_resource_profile_internal.h"
native_resource_source="$script_dir/isaac_native_resource_profile.c"
fbo_rt_reuse_patch="$script_dir/0021-isaac-fbo-rt-reuse.patch"
fbo_rt_reuse_header="$script_dir/isaac_fbo_rt_reuse.h"
fbo_rt_reuse_source="$script_dir/isaac_fbo_rt_reuse.c"
fbo_rt_reuse_lease_patch="$script_dir/0024-isaac-fbo-rt-bounded-lease.patch"
fbo_rt_interlude_patch="$script_dir/0033-isaac-fbo-rt-interlude-reuse.patch"
coloroffset_link_proof_patch="$script_dir/0022-isaac-coloroffset-link-proof.patch"
skip_zero_fragment_uniform_patch="$script_dir/0023-isaac-skip-zero-fragment-uniform.patch"
coloroffset_metadata_once_patch="$script_dir/0026-isaac-coloroffset-metadata-once.patch"
laser_halo_profile_patch="$script_dir/0027-isaac-laser-halo-profile.patch"
laser_halo_profile_api="$script_dir/isaac_laser_halo_profile.h"
white_census_patch="$script_dir/0032-isaac-white-plain-census.patch"
fbo_float_sync_patch="$script_dir/0028-sync-fbo-float-selector.patch"
fbo_scissor_resize_patch="$script_dir/0029-invalidate-resized-fbo-scissor.patch"
fbo_scissor_replay_patch="$script_dir/0030-correct-flipped-scissor-replay.patch"
fbo_scissor_replay_region_patch="$script_dir/0031-correct-flipped-scissor-replay-valid-region.patch"
gpu_draw_optimizations=${ISAAC_GPU_DRAW_OPTIMIZATIONS:-0}
shader_cache=${ISAAC_SHADER_CACHE:-0}
shader_cache_observer_compat=${ISAAC_SHADER_CACHE_OBSERVER_COMPAT:-0}
canonical_quad_zero_copy=${ISAAC_CANONICAL_QUAD_ZERO_COPY:-0}
gxm_state_shadow=${ISAAC_GXM_STATE_SHADOW:-0}
coloroffset_optimizations=${ISAAC_COLOROFFSET_GPU_OPTIMIZATIONS:-0}
phase_profile=${ISAAC_PHASE_PROFILE:-0}
gl_time_profile=${ISAAC_GL_TIME_PROFILE:-0}
gl_time_sdk_sparse=${ISAAC_GL_TIME_SDK_SPARSE:-0}
native_resource_profile=${ISAAC_NATIVE_RESOURCE_PROFILE:-0}
fbo_rt_reuse=${ISAAC_FBO_RT_REUSE:-0}
fbo_rt_reuse_lease=${ISAAC_FBO_RT_REUSE_LEASE:-0}
fbo_rt_interlude_reuse=${ISAAC_FBO_RT_INTERLUDE_REUSE:-0}
fbo_rt_scenes=${ISAAC_FBO_RT_SCENES:-0}
# 0 = no 0008 patch, 1/2 = diagnostic TRIVIAL/NODISCARD, 3 = gated neutral
# production specialization. Diagnostic modes intentionally change the picture.
coloroffset_fs_probe=${ISAAC_COLOROFFSET_FS_PROBE:-0}
coloroffset_staging_proof=${ISAAC_COLOROFFSET_STAGING_PROOF:-0}
coloroffset_single_final_bind=${ISAAC_COLOROFFSET_SINGLE_FINAL_BIND:-0}
coloroffset_plain_fastpath=${ISAAC_COLOROFFSET_PLAIN_FASTPATH:-0}
coloroffset_staging_plain_fusion=${ISAAC_COLOROFFSET_STAGING_PLAIN_FUSION:-0}
coloroffset_staging_finite_neon=${ISAAC_COLOROFFSET_STAGING_FINITE_NEON:-0}
coloroffset_staging_limits_neon=${ISAAC_COLOROFFSET_STAGING_LIMITS_NEON:-0}
coloroffset_staging_outline=${ISAAC_COLOROFFSET_STAGING_OUTLINE:-0}
coloroffset_plain_fp16=${ISAAC_COLOROFFSET_PLAIN_FP16:-0}
coloroffset_plain_vertex_pair=${ISAAC_COLOROFFSET_PLAIN_VERTEX_PAIR:-0}
coloroffset_transform_uniform=${ISAAC_COLOROFFSET_TRANSFORM_UNIFORM:-0}
coloroffset_link_proof=${ISAAC_COLOROFFSET_LINK_PROOF:-0}
skip_zero_fragment_uniform=${ISAAC_SKIP_ZERO_FRAGMENT_UNIFORM:-0}
coloroffset_metadata_once=${ISAAC_COLOROFFSET_METADATA_ONCE:-0}
p8_safe_upload=${ISAAC_P8_SAFE_UPLOAD:-0}
fbo_valid_region=${ISAAC_FBO_VALID_REGION:-0}
laser_atlas_p8=${ISAAC_LASER_ATLAS_P8:-0}
laser_p8_swizzle=${ISAAC_LASER_P8_SWIZZLE:-0}
laser_light_halo_clip=${ISAAC_LASER_LIGHT_HALO_CLIP:-0}
laser_light_halo_depth_approx=${ISAAC_LASER_LIGHT_HALO_DEPTH_APPROX:-0}
laser_red_cap_side_clip=${ISAAC_LASER_RED_CAP_SIDE_CLIP:-0}
laser_light_producer_uv=${ISAAC_LASER_LIGHT_PRODUCER_UV:-0}
laser_halo_proven_emit=${ISAAC_LASER_HALO_PROVEN_EMIT:-0}
laser_halo_profile=${ISAAC_LASER_HALO_PROFILE:-0}
laser_light_nearest=${ISAAC_LASER_LIGHT_NEAREST:-0}
laser_atlas_nearest=${ISAAC_LASER_ATLAS_NEAREST:-0}

for value in \
    "gpu_draw_optimizations:$gpu_draw_optimizations" \
    "shader_cache:$shader_cache" \
    "shader_cache_observer_compat:$shader_cache_observer_compat" \
    "canonical_quad_zero_copy:$canonical_quad_zero_copy" \
    "gxm_state_shadow:$gxm_state_shadow" \
    "coloroffset_optimizations:$coloroffset_optimizations" \
    "phase_profile:$phase_profile" \
    "gl_time_profile:$gl_time_profile" \
    "gl_time_sdk_sparse:$gl_time_sdk_sparse" \
    "native_resource_profile:$native_resource_profile" \
    "fbo_rt_reuse:$fbo_rt_reuse" \
    "fbo_rt_reuse_lease:$fbo_rt_reuse_lease" \
    "fbo_rt_interlude_reuse:$fbo_rt_interlude_reuse" \
    "fbo_rt_scenes:$fbo_rt_scenes" \
    "coloroffset_staging_proof:$coloroffset_staging_proof" \
    "coloroffset_single_final_bind:$coloroffset_single_final_bind" \
    "coloroffset_plain_fastpath:$coloroffset_plain_fastpath" \
    "coloroffset_staging_plain_fusion:$coloroffset_staging_plain_fusion" \
    "coloroffset_staging_finite_neon:$coloroffset_staging_finite_neon" \
    "coloroffset_staging_limits_neon:$coloroffset_staging_limits_neon" \
    "coloroffset_staging_outline:$coloroffset_staging_outline" \
    "coloroffset_plain_fp16:$coloroffset_plain_fp16" \
    "coloroffset_plain_vertex_pair:$coloroffset_plain_vertex_pair" \
    "coloroffset_transform_uniform:$coloroffset_transform_uniform" \
    "coloroffset_link_proof:$coloroffset_link_proof" \
    "skip_zero_fragment_uniform:$skip_zero_fragment_uniform" \
    "coloroffset_metadata_once:$coloroffset_metadata_once" \
    "p8_safe_upload:$p8_safe_upload" \
    "fbo_valid_region:$fbo_valid_region" \
    "laser_atlas_p8:$laser_atlas_p8" \
    "laser_p8_swizzle:$laser_p8_swizzle" \
    "laser_light_halo_clip:$laser_light_halo_clip" \
    "laser_light_halo_depth_approx:$laser_light_halo_depth_approx" \
    "laser_red_cap_side_clip:$laser_red_cap_side_clip" \
    "laser_light_producer_uv:$laser_light_producer_uv" \
    "laser_halo_proven_emit:$laser_halo_proven_emit" \
    "laser_halo_profile:$laser_halo_profile" \
    "laser_light_nearest:$laser_light_nearest" \
    "laser_atlas_nearest:$laser_atlas_nearest"; do
    name=${value%%:*}
    setting=${value#*:}
    case "$setting" in
        0|1) ;;
        *)
            echo "$name must be exactly 0 or 1" >&2
            exit 2
            ;;
    esac
done
if [ "$shader_cache_observer_compat" = 1 ] && [ "$shader_cache" != 1 ]; then
    echo "shader-cache observer compatibility requires ISAAC_SHADER_CACHE=1" >&2
    exit 2
fi
if [ "$laser_atlas_nearest" = 1 ] &&
   { [ "$shader_cache" != 1 ] || [ "$laser_atlas_p8" != 1 ] ||
     [ "$coloroffset_staging_proof" != 1 ] ||
     [ "$coloroffset_fs_probe" != 3 ] ||
     [ "$coloroffset_plain_vertex_pair" != 1 ]; }; then
    echo "laser atlas nearest requires SHADER_CACHE=1, LASER_ATLAS_P8=1, STAGING_PROOF=1, neutral mode 3 and PLAIN_VERTEX_PAIR=1" >&2
    exit 2
fi
if [ "$laser_light_nearest" = 1 ] && [ "$laser_light_halo_clip" != 1 ]; then
    echo "light laser nearest requires ISAAC_LASER_LIGHT_HALO_CLIP=1" >&2
    exit 2
fi
if [ "$laser_light_halo_depth_approx" = 1 ] && [ "$laser_light_halo_clip" != 1 ]; then
    echo "Light halo depth approximation requires ISAAC_LASER_LIGHT_HALO_CLIP=1" >&2
    exit 2
fi
if [ "$laser_red_cap_side_clip" = 1 ] && [ "$laser_light_halo_clip" != 1 ]; then
    echo "Red cap side clip requires ISAAC_LASER_LIGHT_HALO_CLIP=1" >&2
    exit 2
fi
if [ "$laser_light_producer_uv" = 1 ] && [ "$laser_light_halo_clip" != 1 ]; then
    echo "Light producer UV requires ISAAC_LASER_LIGHT_HALO_CLIP=1" >&2
    exit 2
fi
if [ "$laser_halo_proven_emit" = 1 ] && [ "$laser_light_producer_uv" != 1 ]; then
    echo "Proven halo emission requires ISAAC_LASER_LIGHT_PRODUCER_UV=1" >&2
    exit 2
fi
if [ "$laser_halo_profile" = 1 ] &&
   { [ "$phase_profile" != 1 ] || [ "$laser_light_halo_clip" != 1 ] ||
     [ "$coloroffset_metadata_once" != 1 ]; }; then
    echo "Halo profile requires PHASE_PROFILE, HALO_CLIP and METADATA_ONCE=1 (current authenticated recipes; no GL_TIME dependency)" >&2
    exit 2
fi
if [ "$laser_light_halo_clip" = 1 ] &&
   { [ "$laser_atlas_p8" != 1 ] || [ "$coloroffset_plain_fastpath" != 1 ] ||
     [ "$coloroffset_single_final_bind" != 1 ] || [ "$shader_cache" != 1 ]; }; then
    echo "Light halo clip requires exact P8, plain fastpath, single final bind and shader cache" >&2
    exit 2
fi
case "$coloroffset_fs_probe" in
    0|1|2|3) ;;
    *)
        echo "coloroffset_fs_probe must be exactly 0, 1 (TRIVIAL), 2 (NODISCARD) or 3 (NEUTRAL)" >&2
        exit 2
        ;;
esac
if [ "$coloroffset_fs_probe" != 0 ] && [ "$coloroffset_optimizations" != 1 ]; then
    echo "ColorOffset FS probe requires ISAAC_COLOROFFSET_GPU_OPTIMIZATIONS=1" >&2
    exit 2
fi
if [ "$coloroffset_staging_proof" = 1 ] && [ "$coloroffset_fs_probe" != 3 ]; then
    echo "ColorOffset staging proof requires ISAAC_COLOROFFSET_FS_PROBE=3 (NEUTRAL)" >&2
    exit 2
fi
if [ "$coloroffset_link_proof" = 1 ] && [ "$coloroffset_optimizations" != 1 ]; then
    echo "ColorOffset link proof requires ISAAC_COLOROFFSET_GPU_OPTIMIZATIONS=1" >&2
    exit 2
fi
if [ "$coloroffset_metadata_once" = 1 ] &&
   { [ "$laser_light_halo_clip" != 1 ] || [ "$coloroffset_plain_vertex_pair" != 1 ] ||
     [ "$coloroffset_transform_uniform" != 1 ] || [ "$coloroffset_link_proof" != 1 ] ||
     [ "$skip_zero_fragment_uniform" != 1 ]; }; then
    echo "ColorOffset metadata-once requires HALO_CLIP, PLAIN_VERTEX_PAIR, TRANSFORM_UNIFORM, LINK_PROOF and SKIP_ZERO_FRAGMENT_UNIFORM=1 (two authenticated full-source recipes)" >&2
    exit 2
fi
if [ "$coloroffset_single_final_bind" = 1 ] &&
   { [ "$coloroffset_staging_proof" != 1 ] || [ "$coloroffset_fs_probe" != 3 ]; }; then
    echo "ColorOffset single final bind requires staging proof and NEUTRAL mode" >&2
    exit 2
fi
if [ "$coloroffset_plain_fastpath" = 1 ] && [ "$coloroffset_staging_proof" != 1 ]; then
    echo "ColorOffset plain fastpath requires ISAAC_COLOROFFSET_STAGING_PROOF=1" >&2
    exit 2
fi
if [ "$coloroffset_staging_plain_fusion" = 1 ] &&
   { [ "$coloroffset_staging_proof" != 1 ] || [ "$coloroffset_plain_fastpath" != 1 ]; }; then
    echo "ColorOffset staging PLAIN fusion requires STAGING_PROOF=1 and PLAIN_FASTPATH=1" >&2
    exit 2
fi
if [ "$coloroffset_staging_finite_neon" = 1 ] &&
   { [ "$coloroffset_staging_proof" != 1 ] || [ "$coloroffset_staging_plain_fusion" != 1 ]; }; then
    echo "ColorOffset staging finite NEON requires STAGING_PROOF=1 and STAGING_PLAIN_FUSION=1" >&2
    exit 2
fi
if [ "$coloroffset_staging_limits_neon" = 1 ] && [ "$coloroffset_staging_finite_neon" != 1 ]; then
    echo "ColorOffset staging limits NEON requires STAGING_FINITE_NEON=1" >&2
    exit 2
fi
if [ "$coloroffset_staging_outline" = 1 ] && [ "$coloroffset_staging_limits_neon" != 1 ]; then
    echo "ColorOffset staging outline requires STAGING_LIMITS_NEON=1" >&2
    exit 2
fi
if [ "$coloroffset_plain_fp16" = 1 ] && [ "$coloroffset_plain_fastpath" != 1 ]; then
    echo "ColorOffset plain FP16 requires ISAAC_COLOROFFSET_PLAIN_FASTPATH=1" >&2
    exit 2
fi
if [ "$coloroffset_plain_fp16" = 1 ] && [ "$coloroffset_plain_vertex_pair" = 1 ]; then
    echo "ColorOffset plain FP16 is incompatible with ISAAC_COLOROFFSET_PLAIN_VERTEX_PAIR=1" >&2
    exit 2
fi
if [ "$coloroffset_plain_vertex_pair" = 1 ] &&
   { [ "$coloroffset_plain_fastpath" != 1 ] || [ "$coloroffset_single_final_bind" != 1 ]; }; then
    echo "ColorOffset plain vertex pair requires PLAIN_FASTPATH=1 and SINGLE_FINAL_BIND=1" >&2
    exit 2
fi
if [ "$coloroffset_transform_uniform" = 1 ] && [ "$coloroffset_plain_vertex_pair" != 1 ]; then
    echo "ColorOffset Transform uniform requires ISAAC_COLOROFFSET_PLAIN_VERTEX_PAIR=1" >&2
    exit 2
fi
if { [ "$canonical_quad_zero_copy" = 1 ] || [ "$gxm_state_shadow" = 1 ]; } &&
   [ "$gpu_draw_optimizations" != 1 ]; then
    echo "draw-submission switches require ISAAC_GPU_DRAW_OPTIMIZATIONS=1" >&2
    exit 2
fi
if [ "$coloroffset_optimizations" = 1 ] &&
   [ "$gpu_draw_optimizations" != 1 ]; then
    echo "ColorOffset GPU optimizations require ISAAC_GPU_DRAW_OPTIMIZATIONS=1" >&2
    exit 2
fi
# The scene-timer patch (0005) and its EndScene split (0006) are written
# against gxm.c as left by the 0002 draw patch and only have a consumer
# through the phase profiler.
if [ "$gl_time_profile" = 1 ] &&
   { [ "$gpu_draw_optimizations" != 1 ] || [ "$phase_profile" != 1 ]; }; then
    echo "GL time profile requires ISAAC_GPU_DRAW_OPTIMIZATIONS=1 and ISAAC_PHASE_PROFILE=1" >&2
    exit 2
fi
# The FBO render-target scenes patch (0007) edits gxm.c hunks disjoint from
# 0005/0006, so every flag combination applies, but like them it is supported
# only on the 0002 tree; its consumer is kage_vita_backend.c.
if [ "$native_resource_profile" = 1 ] && [ "$gl_time_profile" != 1 ]; then
    echo "Native resource profile requires ISAAC_GL_TIME_PROFILE=1" >&2
    exit 2
fi
if [ "$gl_time_sdk_sparse" = 1 ] &&
   { [ "$gl_time_profile" != 1 ] || [ "$canonical_quad_zero_copy" != 1 ]; }; then
    echo "SDK-sparse profile requires ISAAC_GL_TIME_PROFILE=1 and ISAAC_CANONICAL_QUAD_ZERO_COPY=1" >&2
    exit 2
fi
if [ "$fbo_rt_scenes" = 1 ] && [ "$gpu_draw_optimizations" != 1 ]; then
    echo "FBO render-target scenes require ISAAC_GPU_DRAW_OPTIMIZATIONS=1" >&2
    exit 2
fi
if [ "$fbo_rt_reuse" = 1 ] && [ "$fbo_rt_scenes" != 1 ]; then
    echo "FBO RT reuse requires ISAAC_FBO_RT_SCENES=1 (eight-scene runtime policy)" >&2
    exit 2
fi
if [ "$fbo_rt_reuse_lease" = 1 ] && [ "$fbo_rt_reuse" != 1 ]; then
    echo "Bounded FBO RT lease requires ISAAC_FBO_RT_REUSE=1" >&2
    exit 2
fi
if [ "$fbo_rt_interlude_reuse" = 1 ] && [ "$fbo_rt_reuse" != 1 ]; then
    echo "FBO RT interlude reuse requires ISAAC_FBO_RT_REUSE=1" >&2
    exit 2
fi
# The FBO valid-region patch (0009) is written against gxm.c as left by
# 0005/0006 (its BeginScene hunk sits between the scene-timer brackets) and
# its counters are read through the phase profiler (ph120.gx/ph120.vz), so it
# requires the GL time profile, which already implies 0002 and the phase
# profile.  It commutes with 0007 (test_vitagl_stock_patch_chain.py proves
# both orders); its mode hook is called by kage_vita_backend.c.
if [ "$fbo_valid_region" = 1 ] && [ "$gl_time_profile" != 1 ]; then
    echo "FBO valid region requires ISAAC_GL_TIME_PROFILE=1" >&2
    exit 2
fi

if [ "$laser_atlas_p8" = 1 ] && [ "$p8_safe_upload" != 1 ]; then
    echo "exact laser P8 requires ISAAC_P8_SAFE_UPLOAD=1" >&2
    exit 2
fi
if [ "$laser_p8_swizzle" = 1 ] && [ "$laser_atlas_p8" != 1 ]; then
    echo "laser P8 swizzle requires ISAAC_LASER_ATLAS_P8=1" >&2
    exit 2
fi

if [ "$#" -ne 1 ]; then
    echo "usage: $0 OUTPUT_ROOT" >&2
    exit 2
fi
if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi
if [ ! -x "$VITASDK/bin/arm-vita-eabi-gcc" ] ||
   [ ! -x "$VITASDK/bin/arm-vita-eabi-gcc-ar" ] ||
   [ ! -x "$VITASDK/bin/arm-vita-eabi-nm" ]; then
    echo "VITASDK lacks the ARM compiler/archive tools: $VITASDK" >&2
    exit 2
fi
for tool in curl env git make sha256sum tar; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "$tool is required for the stock vitaGL reference build" >&2
        exit 2
    fi
done
PATH="$VITASDK/bin:$PATH"
export PATH

case "$1" in
    /*) requested_root=$1 ;;
    *) requested_root=$(pwd)/$1 ;;
esac
mkdir -p "$requested_root"
output_root=$(CDPATH= cd -- "$requested_root" && pwd)
sdk_root=$(CDPATH= cd -- "$VITASDK" && pwd)
case "$output_root" in
    /|/home|/home/*/..)
        echo "refusing unsafe stock vitaGL output root: $output_root" >&2
        exit 2
        ;;
    "$sdk_root"|"$sdk_root"/*)
        echo "refusing to place stock vitaGL inside VitaSDK: $output_root" >&2
        exit 2
        ;;
esac

source_url="https://codeload.github.com/Rinnegatamante/vitaGL/tar.gz/$source_commit"
downloads="$output_root/downloads"
work="$output_root/work"
prefix="$output_root/prefix"
source_archive="$downloads/vitaGL-$source_commit.tar.gz"
mkdir -p "$downloads" "$work" "$prefix/lib" "$prefix/include"

verify_sha()
{
    actual=$(sha256sum "$1" | awk '{print $1}')
    if [ "$actual" != "$2" ]; then
        echo "SHA-256 mismatch for $1: $actual != $2" >&2
        exit 3
    fi
}

download()
{
    url=$1
    destination=$2
    expected=$3
    if [ -f "$destination" ]; then
        verify_sha "$destination" "$expected"
        return
    fi

    partial="$destination.part.$$"
    trap 'rm -f -- "$partial"' EXIT HUP INT TERM
    curl --fail --location --retry 3 --output "$partial" "$url"
    verify_sha "$partial" "$expected"
    mv -- "$partial" "$destination"
    trap - EXIT HUP INT TERM
}

apply_source_patch()
{
    patch=$1
    # 0016 anchors its signature hunk on the preceding function tail: optional
    # 0015 adds declarations immediately after that signature. Keep both native
    # variants supported without changing 0015; all other patches retain the
    # normal context requirement, and the final source is still SHA-checked.
    patch_context=
    if [ "$patch" = "$laser_light_halo_patch" ] ||
       [ "$patch" = "$laser_light_nearest_patch" ] ||
       [ "$patch" = "$laser_atlas_nearest_patch" ] ||
       [ "$patch" = "$sdk_sparse_patch" ]; then
        patch_context=--unidiff-zero
    fi
    # The stock tree declares text=auto.  Without this pin, an ambient Windows
    # core.autocrlf=true (or a hostile core.eol) rewrites every touched source
    # to CRLF and changes the whole-file evidence hashes even though the patch
    # content is identical.
    (cd "$source_dir" &&
        git -c core.autocrlf=false -c core.eol=lf \
            apply $patch_context --check "$patch")
    (cd "$source_dir" &&
        git -c core.autocrlf=false -c core.eol=lf \
            apply $patch_context "$patch")
}

download "$source_url" "$source_archive" "$source_sha"
patch_sha=$(sha256sum "$contract_patch" | awk '{print $1}')
gpu_draw_patch_sha=$(sha256sum "$gpu_draw_patch" | awk '{print $1}')
gpu_draw_policy_sha=$(sha256sum "$gpu_draw_policy" | awk '{print $1}')
gxm_state_policy_sha=$(sha256sum "$gxm_state_policy" | awk '{print $1}')
coloroffset_patch_sha=$(sha256sum "$coloroffset_patch" | awk '{print $1}')
coloroffset_policy_sha=$(sha256sum "$coloroffset_policy" | awk '{print $1}')
coloroffset_fs_probe_patch_sha=$(sha256sum "$coloroffset_fs_probe_patch" | awk '{print $1}')
coloroffset_staging_patch_sha=$(sha256sum "$coloroffset_staging_patch" | awk '{print $1}')
coloroffset_staging_policy_sha=$(sha256sum "$coloroffset_staging_policy" | awk '{print $1}')
coloroffset_single_final_bind_patch_sha=$(sha256sum "$coloroffset_single_final_bind_patch" | awk '{print $1}')
coloroffset_plain_patch_sha=$(sha256sum "$coloroffset_plain_patch" | awk '{print $1}')
coloroffset_plain_policy_sha=$(sha256sum "$coloroffset_plain_policy" | awk '{print $1}')
coloroffset_plain_integration_sha=$(sha256sum "$coloroffset_plain_integration" | awk '{print $1}')
coloroffset_plain_fp16_header_sha=$(sha256sum "$coloroffset_plain_fp16_header" | awk '{print $1}')
coloroffset_plain_vertex_pair_patch_sha=$(sha256sum "$coloroffset_plain_vertex_pair_patch" | awk '{print $1}')
coloroffset_plain_vertex_pair_integration_sha=$(sha256sum "$coloroffset_plain_vertex_pair_integration" | awk '{print $1}')
coloroffset_transform_uniform_patch_sha=$(sha256sum "$coloroffset_transform_uniform_patch" | awk '{print $1}')
coloroffset_transform_uniform_header_sha=$(sha256sum "$coloroffset_transform_uniform_header" | awk '{print $1}')
coloroffset_link_proof_patch_sha=$(sha256sum "$coloroffset_link_proof_patch" | awk '{print $1}')
skip_zero_fragment_uniform_patch_sha=$(sha256sum "$skip_zero_fragment_uniform_patch" | awk '{print $1}')
coloroffset_metadata_once_patch_sha=$(sha256sum "$coloroffset_metadata_once_patch" | awk '{print $1}')
shader_cache_patch_sha=$(sha256sum "$shader_cache_patch" | awk '{print $1}')
shader_cache_policy_sha=$(sha256sum "$shader_cache_policy" | awk '{print $1}')
shader_cache_integration_sha=$(sha256sum "$shader_cache_integration" | awk '{print $1}')
shader_cache_block_list_sha=$(sha256sum "$shader_cache_block_list" | awk '{print $1}')
scene_timer_patch_sha=$(sha256sum "$scene_timer_patch" | awk '{print $1}')
scene_split_patch_sha=$(sha256sum "$scene_split_patch" | awk '{print $1}')
sdk_sparse_patch_sha=$(sha256sum "$sdk_sparse_patch" | awk '{print $1}')
sdk_sparse_api_sha=$(sha256sum "$sdk_sparse_api" | awk '{print $1}')
sdk_sparse_internal_sha=$(sha256sum "$sdk_sparse_internal" | awk '{print $1}')
sdk_sparse_source_sha=$(sha256sum "$sdk_sparse_source" | awk '{print $1}')
fbo_rt_scenes_patch_sha=$(sha256sum "$fbo_rt_scenes_patch" | awk '{print $1}')
p8_safe_patch_sha=$(sha256sum "$p8_safe_patch" | awk '{print $1}')
p8_safe_policy_sha=$(sha256sum "$p8_safe_policy" | awk '{print $1}')
fbo_valid_region_patch_sha=$(sha256sum "$fbo_valid_region_patch" | awk '{print $1}')
laser_p8_patch_sha=$(sha256sum "$laser_p8_patch" | awk '{print $1}')
laser_p8_policy_sha=$(sha256sum "$laser_p8_policy" | awk '{print $1}')
laser_light_halo_patch_sha=$(sha256sum "$laser_light_halo_patch" | awk '{print $1}')
laser_light_halo_policy_sha=$(sha256sum "$laser_light_halo_policy" | awk '{print $1}')
laser_light_halo_integration_sha=$(sha256sum "$laser_light_halo_integration" | awk '{print $1}')
laser_halo_profile_patch_sha=$(sha256sum "$laser_halo_profile_patch" | awk '{print $1}')
fbo_float_sync_patch_sha=$(sha256sum "$fbo_float_sync_patch" | awk '{print $1}')
laser_halo_profile_api_sha=$(sha256sum "$laser_halo_profile_api" | awk '{print $1}')
white_census_patch_sha=$(sha256sum "$white_census_patch" | awk '{print $1}')
fbo_scissor_resize_patch_sha=$(sha256sum "$fbo_scissor_resize_patch" | awk '{print $1}')
fbo_scissor_replay_patch_sha=$(sha256sum "$fbo_scissor_replay_patch" | awk '{print $1}')
fbo_scissor_replay_region_patch_sha=$(sha256sum "$fbo_scissor_replay_region_patch" | awk '{print $1}')
laser_light_nearest_patch_sha=$(sha256sum "$laser_light_nearest_patch" | awk '{print $1}')
laser_light_nearest_policy_sha=$(sha256sum "$laser_light_nearest_policy" | awk '{print $1}')
laser_atlas_nearest_patch_sha=$(sha256sum "$laser_atlas_nearest_patch" | awk '{print $1}')
laser_atlas_nearest_policy_sha=$(sha256sum "$laser_atlas_nearest_policy" | awk '{print $1}')
native_resource_patch_sha=$(sha256sum "$native_resource_patch" | awk '{print $1}')
native_resource_api_sha=$(sha256sum "$native_resource_api" | awk '{print $1}')
native_resource_internal_sha=$(sha256sum "$native_resource_internal" | awk '{print $1}')
native_resource_source_sha=$(sha256sum "$native_resource_source" | awk '{print $1}')
fbo_rt_reuse_patch_sha=$(sha256sum "$fbo_rt_reuse_patch" | awk '{print $1}')
fbo_rt_reuse_header_sha=$(sha256sum "$fbo_rt_reuse_header" | awk '{print $1}')
fbo_rt_reuse_source_sha=$(sha256sum "$fbo_rt_reuse_source" | awk '{print $1}')
fbo_rt_reuse_lease_patch_sha=$(sha256sum "$fbo_rt_reuse_lease_patch" | awk '{print $1}')
fbo_rt_interlude_patch_sha=$(sha256sum "$fbo_rt_interlude_patch" | awk '{print $1}')
laser_p8_reference_sha=$(sha256sum "$laser_p8_reference" | awk '{print $1}')
script_sha=$(sha256sum "$script_dir/build.sh" | awk '{print $1}')
build_flags="SOFTFP_ABI=1 NO_DEBUG=1 NO_SPLASHSCREEN=1 SINGLE_THREADED_GC=1"
gpu_draw_policy_name=disabled
coloroffset_policy_name=disabled
if [ "$gpu_draw_optimizations" = 1 ]; then
    build_flags="$build_flags HAVE_ISAAC_GPU_DRAW_OPTIMIZATIONS=1"
    gpu_draw_policy_name=exact-single-stream-layout-cache-draw-stats-v2
fi
if [ "$shader_cache" = 1 ]; then
    build_flags="$build_flags HAVE_ISAAC_SHADER_CACHE=1"
fi
if [ "$canonical_quad_zero_copy" = 1 ]; then
    build_flags="$build_flags HAVE_ISAAC_CANONICAL_QUAD_ZERO_COPY=1"
fi
if [ "$gxm_state_shadow" = 1 ]; then
    build_flags="$build_flags HAVE_ISAAC_GXM_STATE_SHADOW=1"
fi
if [ "$coloroffset_optimizations" = 1 ]; then
    build_flags="$build_flags HAVE_ISAAC_COLOROFFSET_GPU_OPTIMIZATIONS=1"
    coloroffset_policy_name=exact-stock-source-plus-opaque-noblend-v3
fi
coloroffset_fs_probe_name=off
if [ "$coloroffset_staging_proof" = 1 ]; then
    build_flags="$build_flags HAVE_ISAAC_COLOROFFSET_STAGING_PROOF=1"
fi
if [ "$coloroffset_single_final_bind" = 1 ]; then
    build_flags="$build_flags HAVE_ISAAC_COLOROFFSET_SINGLE_FINAL_BIND=1"
fi
if [ "$coloroffset_plain_fastpath" = 1 ]; then
    build_flags="$build_flags HAVE_ISAAC_COLOROFFSET_PLAIN_FASTPATH=1"
fi
if [ "$coloroffset_staging_plain_fusion" = 1 ]; then
    build_flags="$build_flags HAVE_ISAAC_COLOROFFSET_STAGING_PLAIN_FUSION=1"
fi
if [ "$coloroffset_staging_finite_neon" = 1 ]; then
    build_flags="$build_flags HAVE_ISAAC_COLOROFFSET_STAGING_FINITE_NEON=1"
fi
if [ "$coloroffset_staging_limits_neon" = 1 ]; then
    build_flags="$build_flags HAVE_ISAAC_COLOROFFSET_STAGING_LIMITS_NEON=1"
fi
if [ "$coloroffset_staging_outline" = 1 ]; then
    build_flags="$build_flags HAVE_ISAAC_COLOROFFSET_STAGING_OUTLINE=1"
fi
if [ "$coloroffset_plain_fp16" = 1 ]; then
    build_flags="$build_flags HAVE_ISAAC_COLOROFFSET_PLAIN_FP16=1"
fi
if [ "$coloroffset_plain_vertex_pair" = 1 ]; then
    build_flags="$build_flags HAVE_ISAAC_COLOROFFSET_PLAIN_VERTEX_PAIR=1"
fi
if [ "$coloroffset_transform_uniform" = 1 ]; then
    build_flags="$build_flags HAVE_ISAAC_COLOROFFSET_TRANSFORM_UNIFORM=1"
fi
if [ "$coloroffset_link_proof" = 1 ]; then
    build_flags="$build_flags HAVE_ISAAC_COLOROFFSET_LINK_PROOF=1"
fi
if [ "$skip_zero_fragment_uniform" = 1 ]; then
    build_flags="$build_flags HAVE_ISAAC_SKIP_ZERO_FRAGMENT_UNIFORM=1"
fi
if [ "$coloroffset_metadata_once" = 1 ]; then
    build_flags="$build_flags HAVE_ISAAC_COLOROFFSET_METADATA_ONCE=1"
fi
if [ "$coloroffset_fs_probe" != 0 ]; then
    build_flags="$build_flags HAVE_ISAAC_COLOROFFSET_FS_PROBE=$coloroffset_fs_probe"
    if [ "$coloroffset_fs_probe" = 1 ]; then
        coloroffset_fs_probe_name=trivial
    elif [ "$coloroffset_fs_probe" = 3 ]; then
        coloroffset_fs_probe_name=neutral
    else
        coloroffset_fs_probe_name=nodiscard
    fi
fi
if [ "$phase_profile" = 1 ]; then
    build_flags="$build_flags HAVE_ISAAC_PHASE_PROFILE=1"
fi
gxm_source_patch_name=none
if [ "$gl_time_profile" = 1 ]; then
    build_flags="$build_flags HAVE_ISAAC_GL_TIME_PROFILE=1"
    gxm_source_patch_name=0005-isaac-scene-timer+0006-isaac-scene-split
fi
if [ "$gl_time_sdk_sparse" = 1 ]; then
    build_flags="$build_flags HAVE_ISAAC_GL_TIME_SDK_SPARSE=1"
    gxm_source_patch_name="$gxm_source_patch_name+0025-isaac-sdk-sparse-profile"
fi
if [ "$native_resource_profile" = 1 ]; then
    build_flags="$build_flags HAVE_ISAAC_NATIVE_RESOURCE_PROFILE=1"
    gxm_source_patch_name="$gxm_source_patch_name+0020-isaac-native-resource-profile"
fi
if [ "$fbo_rt_reuse" = 1 ]; then
    build_flags="$build_flags HAVE_ISAAC_FBO_RT_REUSE=1"
    gxm_source_patch_name="$gxm_source_patch_name+0021-isaac-fbo-rt-reuse"
fi
if [ "$fbo_rt_reuse_lease" = 1 ]; then
    build_flags="$build_flags HAVE_ISAAC_FBO_RT_REUSE_LEASE=1"
    gxm_source_patch_name="$gxm_source_patch_name+0024-isaac-fbo-rt-bounded-lease"
fi
if [ "$fbo_rt_interlude_reuse" = 1 ]; then
    build_flags="$build_flags HAVE_ISAAC_FBO_RT_INTERLUDE_REUSE=1"
    gxm_source_patch_name="$gxm_source_patch_name+0033-isaac-fbo-rt-interlude-reuse"
fi
if [ "$fbo_rt_scenes" = 1 ]; then
    build_flags="$build_flags HAVE_ISAAC_FBO_RT_SCENES=1"
    if [ "$gxm_source_patch_name" = none ]; then
        gxm_source_patch_name=0007-isaac-fbo-rt-scenes
    else
        gxm_source_patch_name="$gxm_source_patch_name+0007-isaac-fbo-rt-scenes"
    fi
fi
if [ "$p8_safe_upload" = 1 ]; then
    build_flags="$build_flags HAVE_ISAAC_P8_SAFE_UPLOAD=1"
fi
if [ "$fbo_valid_region" = 1 ]; then
    # 0009 also edits misc.c (glViewport) and tests.c (update_scissor_test);
    # the contract key keeps its name, the patch name records the scope.
    build_flags="$build_flags HAVE_ISAAC_FBO_VALID_REGION=1"
    gxm_source_patch_name="$gxm_source_patch_name+0009-isaac-fbo-valid-region"
fi
if [ "$laser_atlas_p8" = 1 ]; then
    build_flags="$build_flags HAVE_ISAAC_LASER_ATLAS_P8=1"
fi
if [ "$laser_p8_swizzle" = 1 ]; then
    build_flags="$build_flags HAVE_ISAAC_LASER_P8_SWIZZLE=1 SUPPORT_SMALL_FMT=1"
fi
if [ "$laser_light_halo_clip" = 1 ]; then
    build_flags="$build_flags HAVE_ISAAC_LASER_LIGHT_HALO_CLIP=1"
fi
if [ "$laser_light_halo_depth_approx" = 1 ]; then
    build_flags="$build_flags HAVE_ISAAC_LASER_LIGHT_HALO_DEPTH_APPROX=1"
fi
if [ "$laser_red_cap_side_clip" = 1 ]; then
    build_flags="$build_flags HAVE_ISAAC_LASER_RED_CAP_SIDE_CLIP=1"
fi
if [ "$laser_light_producer_uv" = 1 ]; then
    build_flags="$build_flags HAVE_ISAAC_LASER_LIGHT_PRODUCER_UV=1"
fi
if [ "$laser_halo_proven_emit" = 1 ]; then
    build_flags="$build_flags HAVE_ISAAC_LASER_HALO_PROVEN_EMIT=1"
fi
if [ "$laser_halo_profile" = 1 ]; then
    build_flags="$build_flags HAVE_ISAAC_LASER_HALO_PROFILE=1"
fi
if [ "$laser_light_nearest" = 1 ]; then
    build_flags="$build_flags HAVE_ISAAC_LASER_LIGHT_NEAREST=1"
fi
if [ "$laser_atlas_nearest" = 1 ]; then
    build_flags="$build_flags HAVE_ISAAC_LASER_ATLAS_NEAREST=1"
fi
# Keep the historical input prefix shared by both identities. Shader-only
# compatibility normalizes GL_TIME/NATIVE_RESOURCE values and all four exact
# observer defines (including SDK_SPARSE and LASER_HALO_PROFILE); only the
# sparse and halo diagnostic suffixes below are excluded. Every functional hash, including script_sha and disabled
# patches, PHASE and all other options remain exact. The metadata-once option
# and patch hash affect BOTH identities. Editing this script also changes both
# keys relative to the previous revision, even when the new option is OFF.
emit_recipe_inputs()
{
    local recipe_gl_time=$1 recipe_native_resource=$2 recipe_build_flags=$3
    printf '%s\n' \
    "$source_commit" "$source_sha" "$patch_sha" "$script_sha" \
    "$recipe_build_flags" "$oob_fix_commit" "$gpu_draw_optimizations" \
    "$gpu_draw_patch_sha" "$gpu_draw_policy_sha" \
    "$canonical_quad_zero_copy" "$gxm_state_shadow" \
    "$gxm_state_policy_sha" \
    "$coloroffset_optimizations" "$coloroffset_patch_sha" \
    "$coloroffset_policy_sha" "$phase_profile" "$shader_cache" \
    "$shader_cache_patch_sha" "$shader_cache_policy_sha" \
    "$shader_cache_integration_sha" "$shader_cache_block_list_sha" \
    "$recipe_gl_time" "$scene_timer_patch_sha" \
    "$recipe_native_resource" "$native_resource_patch_sha" \
    "$native_resource_api_sha" "$native_resource_internal_sha" "$native_resource_source_sha" \
    "$fbo_rt_reuse" "$fbo_rt_reuse_patch_sha" "$fbo_rt_reuse_header_sha" "$fbo_rt_reuse_source_sha" \
    "$fbo_rt_reuse_lease" "$fbo_rt_reuse_lease_patch_sha" \
    "$fbo_rt_interlude_reuse" "$fbo_rt_interlude_patch_sha" \
    "$scene_split_patch_sha" "$fbo_rt_scenes" "$fbo_rt_scenes_patch_sha" \
    "$coloroffset_fs_probe" "$coloroffset_fs_probe_patch_sha" \
    "$coloroffset_staging_proof" "$coloroffset_staging_patch_sha" \
    "$coloroffset_staging_policy_sha" \
    "$coloroffset_single_final_bind" "$coloroffset_single_final_bind_patch_sha" \
    "$coloroffset_plain_fastpath" "$coloroffset_plain_patch_sha" \
    "$coloroffset_plain_policy_sha" "$coloroffset_plain_integration_sha" \
    "$coloroffset_staging_plain_fusion" \
    "$coloroffset_staging_finite_neon" \
    "$coloroffset_staging_limits_neon" \
    "$coloroffset_staging_outline" \
    "$coloroffset_plain_fp16" "$coloroffset_plain_fp16_header_sha" \
    "$coloroffset_plain_vertex_pair" "$coloroffset_plain_vertex_pair_patch_sha" \
    "$coloroffset_plain_vertex_pair_integration_sha" \
    "$coloroffset_transform_uniform" "$coloroffset_transform_uniform_patch_sha" \
    "$coloroffset_transform_uniform_header_sha" \
    "$coloroffset_link_proof" "$coloroffset_link_proof_patch_sha" \
    "$skip_zero_fragment_uniform" "$skip_zero_fragment_uniform_patch_sha" \
    "$coloroffset_metadata_once" "$coloroffset_metadata_once_patch_sha" \
    "$p8_safe_upload" "$p8_safe_patch_sha" "$p8_safe_policy_sha" \
    "$fbo_valid_region" "$fbo_valid_region_patch_sha" \
    "$laser_atlas_p8" "$laser_p8_patch_sha" "$laser_p8_policy_sha" \
    "$laser_p8_reference_sha" "$laser_p8_swizzle" "$laser_light_halo_clip" \
    "$laser_light_halo_patch_sha" "$laser_light_halo_policy_sha" \
    "$laser_light_halo_integration_sha" "$laser_light_nearest" \
    "$laser_light_halo_depth_approx" \
    "$laser_red_cap_side_clip" \
    "$laser_light_producer_uv" \
    "$laser_halo_proven_emit" \
    "$laser_light_nearest_patch_sha" "$laser_light_nearest_policy_sha" \
    "$laser_atlas_nearest" "$laser_atlas_nearest_patch_sha" \
    "$laser_atlas_nearest_policy_sha" "$fbo_float_sync_patch_sha" "$fbo_scissor_resize_patch_sha" \
    "$fbo_scissor_replay_patch_sha" "$fbo_scissor_replay_region_patch_sha"
    # ON changes the embedded cache key and must have a distinct native source
    # directory/marker too. Do not append anything to the legacy OFF recipe.
    if [ "$shader_cache_observer_compat" = 1 ]; then
        printf '%s\n' 'shader-cache-observer-compat-v1=1'
    fi
}
# Diagnostic source bytes remain in the native recipe even when disabled.
# Only the opt-in observer-compatible shader identity omits this suffix; a
# changed patch/helper must never re-use a stale native source directory.
emit_sdk_sparse_recipe_inputs()
{
    printf '%s\n' "$gl_time_sdk_sparse" "$sdk_sparse_patch_sha" \
        "$sdk_sparse_api_sha" "$sdk_sparse_internal_sha" "$sdk_sparse_source_sha"
}
emit_halo_profile_recipe_inputs()
{
    # Counter-only owner seam; functional halo policy/integration hashes stay
    # in the shared prefix, including their observer-guarded source changes.
    printf '%s\n' "$laser_halo_profile" "$laser_halo_profile_patch_sha" \
        "$laser_halo_profile_api_sha" "$white_census_patch_sha"
}
recipe=$({ emit_recipe_inputs "$gl_time_profile" "$native_resource_profile" \
    "$build_flags"; emit_sdk_sparse_recipe_inputs; emit_halo_profile_recipe_inputs; } | sha256sum | awk '{print $1}')
shader_cache_recipe=$recipe
shader_cache_identity=full-native-recipe
if [ "$shader_cache_observer_compat" = 1 ]; then
    # These exact tokens are appended with one leading space above. Removing
    # them preserves the order and bytes of every remaining compiler define.
    shader_cache_flags=" $build_flags "
    shader_cache_flags=${shader_cache_flags// HAVE_ISAAC_GL_TIME_PROFILE=1 / }
    shader_cache_flags=${shader_cache_flags// HAVE_ISAAC_NATIVE_RESOURCE_PROFILE=1 / }
    shader_cache_flags=${shader_cache_flags// HAVE_ISAAC_GL_TIME_SDK_SPARSE=1 / }
    shader_cache_flags=${shader_cache_flags// HAVE_ISAAC_LASER_HALO_PROFILE=1 / }
    shader_cache_flags=${shader_cache_flags# }
    shader_cache_flags=${shader_cache_flags% }
    shader_cache_recipe=$(emit_recipe_inputs 0 0 "$shader_cache_flags" | \
        sha256sum | awk '{print $1}')
    shader_cache_identity=observer-gl-time-native-resource-v1
fi
source_parent="$work/source-$recipe"
source_dir="$source_parent/vitaGL-$source_commit"
marker="$source_parent/.isaac-stock-vitagl-recipe"

zero_fragment_source_identity() {
    # Preserve the existing full-file gate after optional 0023. Derived from
    # fresh pinned sources, including both 0022 states and cache-OFF Transform.
    case "$shader_source_sha" in
    07657638528d6acfba87abee60f0bf7fa1c23a65aad7fbf92f12784287008b1c) shader_source_sha=7b466a3b33c35ef463dca2d03d979cb3653b88d30aba6e58716b3e5c81b26989 ;;
    0a13fbb359dd78603d45e07abd8ddcc49559c9251238d89897de41e98e29caa6) shader_source_sha=ff90be1aa80ca20418102646288e19595cd337d492eebc57054f80b7bfda4d91 ;;
    113d62e360c0722175c0124bc754537ad8abb03a5db15fbb7dfa94077b27c3a8) shader_source_sha=9c39d9534406e4051efecdd491eab716adb5d283c8284998e45557494a052231 ;;
    1703e6adde1b06fd97a00c0f0792f3b36e65c0e01210a9f6658587c64aac092b) shader_source_sha=bf3697fb66aad06fbb6adf6b5cc4dd70151ade429a83087e25c6028ff8f98bd4 ;;
    1a8285ee3377855359268306d65a879f9697e2235e337aa9be4f496108da14d5) shader_source_sha=d2b6514e7884e9292b90b66efe74081fe3c012fd9587b6423f1c74ccf459361d ;;
    358f3419a559d7d00d463afeaa788e87b08fe13fd40fbc082e4d0ff18bd9ae7d) shader_source_sha=3ef55913d7243e69c807bd40b48d21679efa71fde05bd39943621beee59cdfd9 ;;
    51377c7a9f75a27a093c86ba3106314fb3a08dff824fe6228ebbd796a106c9ed) shader_source_sha=05cf465e6cecf617858ee0e7ccd744d78898cb3d5753abeeb43b9449eb5db77f ;;
    56a8b62caf27f5da78bd7154397370986dc799a69acb9311adfad3e305b4946a) shader_source_sha=50cede9265e809bbc543232043c26eda72ed8036026091c944c9f2ff18252a0f ;;
    5ac2bedbaf323475dd586ed85b7b392ce53436b9fddb16a5a64e89690a52e613) shader_source_sha=8a77920f8816d3a17d7a5a136269c11d20ea0ab12fc2126791b1ca00e624c905 ;;
    5d8df29f49e2994f38181d061b27829273f92841262597cae832a30279d4dea8) shader_source_sha=d493f0bfb12692a91410cdaf59b46e42ded49d4274cf13fb1d9a6be0627871df ;;
    5e050036e861a698e93ab8934d96c1921cac4f6d7877fea87f67d5c9b8ca8c68) shader_source_sha=3c82134f74d80122b353949cc99599027c94097b7aa6cb4600c099095dbe0bf3 ;;
    64c3d77ec8f6ea565616bd5c3e5012ec7efd104a714faeff08747600be1f8737) shader_source_sha=d66f7fe6293404c5921d95dd6b80d1bfb5719c4bc880390c186eb63b2cf7d46f ;;
    6a9ca09632ab40e86af8d6752789bb75c6c0a10ebd03174339264e306c78f70e) shader_source_sha=cb707b23c71a4fc06fce72be0ec0fa8be25e2c461ba7adeff4a5493ac59f99fb ;;
    71ac71f8a637b22c4de50cba110590f4348b5cfdd3869710d71f4c8d3ecd64e5) shader_source_sha=e4bc783bf87bf273e30601461b6a532b68a6660ce628d5451bf6f289c0d5cb6c ;;
    74854ceb523d1549c989455e44d6330f478566486269300d54086b508ad1221f) shader_source_sha=e0cd5d23ad52a4a3f7c1e45ff78f2d21e8a3a9a9ed463458b9013c78a32b2906 ;;
    7602026e7a49a904c507a0395fa0367fc7504e711f9e97917201cddabb35c0d2) shader_source_sha=f93b8ffe1cacb8f1656b4d3451f42e05202ce336d3e0a85d38e1b3fcd7ee3d50 ;;
    871c42274ae38417fae87c65c0de5cab137a741def6b446ab52956f0d4610489) shader_source_sha=7afd718fab909dc25e0798b5c91604ad9f3aaf5c8048384f2e1b59f883b48c3e ;;
    8888d87edeeb488d46a2def21a3f4e1dcd3ba986677c3ec5fd5495761ff78219) shader_source_sha=2f4ecef7aa887de1939f4d45ec3a8e887958a9bcb89bff4a9e0923570f94d061 ;;
    8e92cdb4dbb3683070be26ff00e59b0112b37da129bcd88b20bcc17c11501c75) shader_source_sha=190a33f9bfc45bb28a97f8f7ffbfa98991aecf851242a6b85fd28831ffae8484 ;;
    978752ece599cf284b620991eba2ee20b0cb8c969ec15db9b00539d5d2e1a529) shader_source_sha=9ee2b66d6b26c5e82bd9d9bddb215a1bf59e6e320bcb0a0f4d50f3e3c875644e ;;
    a4ff11a5911646d6ea70278a2243b737b50693876105d181569767a452e3c0ea) shader_source_sha=29fed0b2a751e2640730d5a0e88f44ac9481f4cc9eb79f6378ff833a98ba0906 ;;
    a88131764bd930f7b7118336290ece627ed153540480d03dbea90349ae078294) shader_source_sha=8b41481f78d659e33e7b7bdbc3c93228a46f1a31861e3e73a81e651ba07bea60 ;;
    b39ff1c95d948e2294d94f701f25666ee216cd87efefb9df4195e224960db1fc) shader_source_sha=d6a01e531242d19339d6cbf0993afcdfe86e72d77755e397cbfe446f71b5b37f ;;
    c15c96580aaf3a391f8d0a27714d0858037f6230cc931b7e4b103d3a9e4d1e76) shader_source_sha=ed2fd9efbaf765e47df71ab59c30013134b19add06173a70fe2c520b8ea0a583 ;;
    c27605f306f1160a27f30fc5ade7c15492fcd5da2f34a37643edd9844b20a9e2) shader_source_sha=088bc50625c975479c6e7031ab109ae1cba584a3a040fd8bd9add6be211fb0c6 ;;
    c4b77d487337a853a87abf6b0d3be265fdf69218c88d9666c64a868fc3b67ee2) shader_source_sha=41cbe75f9ae2d0fe79ae7c6611ffe7f0c3e5a08c571b13714b773721c2e36801 ;;
    d304c32ea1b88f8511531cb2681e20ca4144236dae06d42c1507fd6da6ed4718) shader_source_sha=c6ad323e15e05ccbb0abf17cb80085f9988e5aca3668872b0ab47dad10c69107 ;;
    d6ea8bf8e98276d9fd247d4bf5b72196edbbf260fd79457c60ba6e553407f359) shader_source_sha=365f61a75e9c747217e30d6fb246fb9570a07166b028992f98edafc351332cf5 ;;
    e662b4454e467cf4a0210a85c9d423d7dd676769d67e0f75c0421fe572fab415) shader_source_sha=3424c38ee164e400714346f274efcaa17fb0ee8a4875a36c63cd5063e963615e ;;
    e887f6bcc48d6984e617028c0c17aeec89ace87df8315c55821cf89bd39aa894) shader_source_sha=22c534e74cac92678e74f1fd49b6d7591f7e5a92361dd8682de0a53a8c7481c2 ;;
    *) echo "unsupported zero-fragment-uniform source combination" >&2; exit 5 ;;
    esac
}

if [ ! -f "$marker" ]; then
    if [ -e "$source_parent" ]; then
        echo "unmarked stock vitaGL source directory already exists: $source_parent" >&2
        exit 4
    fi
    mkdir -p "$source_parent"
    tar -xf "$source_archive" -C "$source_parent"
    apply_source_patch "$contract_patch"
    if [ "$gpu_draw_optimizations" = 1 ]; then
        apply_source_patch "$gpu_draw_patch"
        cp -- "$gpu_draw_policy" \
            "$source_dir/source/isaac_gpu_draw_policy.h"
        cp -- "$gxm_state_policy" \
            "$source_dir/source/isaac_gxm_state_policy.h"
    fi
    if [ "$coloroffset_optimizations" = 1 ]; then
        apply_source_patch "$coloroffset_patch"
        cp -- "$coloroffset_policy" \
            "$source_dir/source/isaac_coloroffset_gpu_policy.h"
    fi
    # 0008 hooks the 0003 ColorOffset machinery in custom_shaders.c and is
    # written against that tree; its hunks are disjoint from 0004's, so the
    # shader-cache patch still applies on top (test_vitagl_stock_patch_chain.py
    # proves the chain with and without 0005/0006/0007).
    if [ "$coloroffset_fs_probe" != 0 ]; then
        apply_source_patch "$coloroffset_fs_probe_patch"
    fi
    if [ "$shader_cache" = 1 ]; then
        apply_source_patch "$shader_cache_patch"
        cp -- "$shader_cache_policy" \
            "$source_dir/source/isaac_shader_cache_policy.h"
        cp -- "$shader_cache_integration" \
            "$source_dir/source/isaac_shader_cache_vitagl.h"
        cp -- "$shader_cache_block_list" \
            "$source_dir/source/isaac_shader_cache_block_list.h"
    fi
    # 0012 changes only the mode-3 packed copy / per-draw eligibility path.
    # It follows 0008 and commutes with the optional shader-cache patch.
    if [ "$coloroffset_staging_proof" = 1 ]; then
        apply_source_patch "$coloroffset_staging_patch"
        cp -- "$coloroffset_staging_policy" "$source_dir/source/isaac_coloroffset_staging.h"
    fi
    if [ "$coloroffset_plain_fastpath" = 1 ]; then
        apply_source_patch "$coloroffset_plain_patch"
        cp -- "$coloroffset_plain_policy" "$source_dir/source/isaac_coloroffset_plain_policy.h"
        cp -- "$coloroffset_plain_integration" "$source_dir/source/isaac_coloroffset_plain_vitagl.h"
        if [ "$coloroffset_plain_fp16" = 1 ]; then
            cp -- "$coloroffset_plain_fp16_header" "$source_dir/source/isaac_coloroffset_plain_fp16.h"
        fi
    fi
    if [ "$coloroffset_single_final_bind" = 1 ]; then
        apply_source_patch "$coloroffset_single_final_bind_patch"
    fi
    if [ "$coloroffset_plain_vertex_pair" = 1 ]; then
        apply_source_patch "$coloroffset_plain_vertex_pair_patch"
        cp -- "$coloroffset_plain_vertex_pair_integration" "$source_dir/source/isaac_coloroffset_plain_vertex_pair.h"
    fi
    if [ "$gl_time_profile" = 1 ]; then
        apply_source_patch "$scene_timer_patch"
        apply_source_patch "$scene_split_patch"
    fi
    # 0007 follows 0005/0006 in this order; its hunks are disjoint from
    # theirs, so it also applies alone (test_vitagl_stock_patch_chain.py
    # proves all four combinations).
    if [ "$fbo_rt_scenes" = 1 ]; then
        apply_source_patch "$fbo_rt_scenes_patch"
    fi
    if [ "$p8_safe_upload" = 1 ]; then
        apply_source_patch "$p8_safe_patch"
        cp -- "$p8_safe_policy" "$source_dir/source/isaac_p8_texture_safety.h"
    fi
    # 0009 follows 0007; it needs the 0005/0006 BeginScene brackets as
    # context (refused on a tree without them) and commutes with 0007.
    if [ "$fbo_valid_region" = 1 ]; then
        apply_source_patch "$fbo_valid_region_patch"
    fi
    if [ "$laser_atlas_p8" = 1 ]; then
        apply_source_patch "$laser_p8_patch"
        cp -- "$laser_p8_policy" "$source_dir/source/isaac_laser_p8.h"
        cp -- "$laser_p8_reference" "$source_dir/source/isaac_laser_p8_reference.h"
    fi
    if [ "$laser_light_halo_clip" = 1 ] || [ "$laser_atlas_nearest" = 1 ]; then
        apply_source_patch "$laser_light_halo_patch"
        cp -- "$laser_light_halo_policy" "$source_dir/source/isaac_laser_light_halo_policy.h"
        cp -- "$laser_light_halo_integration" "$source_dir/source/isaac_laser_light_halo_vitagl.h"
    fi
    if [ "$laser_light_nearest" = 1 ] || [ "$laser_atlas_nearest" = 1 ]; then
        apply_source_patch "$laser_light_nearest_patch"
        cp -- "$laser_light_nearest_policy" "$source_dir/source/isaac_laser_light_nearest.h"
    fi
    # 0018 requires 0015 and leaves its draw/uniform-restore path unchanged.
    # Apply after halo/nearest transport and before optional 0019.
    if [ "$coloroffset_transform_uniform" = 1 ]; then
        apply_source_patch "$coloroffset_transform_uniform_patch"
        cp -- "$coloroffset_transform_uniform_header" "$source_dir/source/isaac_coloroffset_transform_uniform.h"
    fi
    # 0019 extends the reused 0016/0017 transport without enabling their
    # geometry/sampler options.  Only explicit flags enter EXTRA_CFLAGS.
    if [ "$laser_atlas_nearest" = 1 ]; then
        apply_source_patch "$laser_atlas_nearest_patch"
        cp -- "$laser_atlas_nearest_policy" "$source_dir/source/isaac_laser_atlas_nearest.h"
        cp -- "$laser_light_nearest_policy" "$source_dir/source/isaac_laser_light_nearest.h"
    fi
    if [ "$native_resource_profile" = 1 ]; then
        apply_source_patch "$native_resource_patch"
        cp -- "$native_resource_api" "$source_dir/source/isaac_native_resource_profile.h"
        cp -- "$native_resource_internal" "$source_dir/source/isaac_native_resource_profile_internal.h"
        cp -- "$native_resource_source" "$source_dir/source/isaac_native_resource_profile.c"
    fi
    if [ "$fbo_rt_reuse" = 1 ]; then
        apply_source_patch "$fbo_rt_reuse_patch"
        cp -- "$fbo_rt_reuse_header" "$source_dir/source/isaac_fbo_rt_reuse.h"
        cp -- "$fbo_rt_reuse_source" "$source_dir/source/isaac_fbo_rt_reuse.c"
    fi
    if [ "$fbo_rt_reuse_lease" = 1 ]; then
        apply_source_patch "$fbo_rt_reuse_lease_patch"
    fi
    if [ "$fbo_rt_interlude_reuse" = 1 ]; then
        apply_source_patch "$fbo_rt_interlude_patch"
    fi
    if [ "$coloroffset_link_proof" = 1 ]; then
        apply_source_patch "$coloroffset_link_proof_patch"
    fi
    if [ "$skip_zero_fragment_uniform" = 1 ]; then
        apply_source_patch "$skip_zero_fragment_uniform_patch"
    fi
    if [ "$coloroffset_metadata_once" = 1 ]; then
        apply_source_patch "$coloroffset_metadata_once_patch"
    fi
    if [ "$gl_time_sdk_sparse" = 1 ]; then
        apply_source_patch "$sdk_sparse_patch"
        cp -- "$sdk_sparse_api" "$source_dir/source/isaac_sdk_sparse_profile.h"
        cp -- "$sdk_sparse_internal" "$source_dir/source/isaac_sdk_sparse_profile_internal.h"
        cp -- "$sdk_sparse_source" "$source_dir/source/isaac_sdk_sparse_profile.c"
    fi
    if [ "$laser_halo_profile" = 1 ]; then
        apply_source_patch "$laser_halo_profile_patch"
        cp -- "$laser_halo_profile_api" "$source_dir/source/isaac_laser_halo_profile.h"
    fi
    # Mandatory stock correctness repairs, after all optional source patches.
    # All hashes stay in functional identities, never observer-normalized.
    apply_source_patch "$fbo_float_sync_patch"
    apply_source_patch "$fbo_scissor_resize_patch"
    # Same correction in the two existing source shapes; preserve 0009's clamp.
    if [ "$fbo_valid_region" = 1 ]; then
        apply_source_patch "$fbo_scissor_replay_region_patch"
    else
        apply_source_patch "$fbo_scissor_replay_patch"
    fi
    if [ "$laser_halo_profile" = 1 ]; then
        apply_source_patch "$white_census_patch"
    fi
    printf '%s\n' "$recipe" > "$marker"
fi
if [ "$skip_zero_fragment_uniform" = 1 ]; then
    if [ "$(grep -Fc 'if (ISAAC_FRAGMENT_DEFAULT_UNIFORM_NEEDED(p)) {' "$source_dir/source/custom_shaders.c")" != 2 ]; then
        echo "stock vitaGL zero-fragment-uniform patch is incomplete" >&2
        exit 5
    fi
else
    if grep -Fq 'HAVE_ISAAC_SKIP_ZERO_FRAGMENT_UNIFORM' "$source_dir/source/custom_shaders.c"; then
        echo "stock vitaGL source unexpectedly contains zero-fragment-uniform suppression" >&2
        exit 5
    fi
fi
if [ "$shader_cache" = 1 ]; then
    for evidence in \
        '#include "isaac_shader_cache_vitagl.h"' \
        'isaac_shader_cache_check_program' \
        'vglGetIsaacShaderCacheStats'; do
        if ! grep -Fq "$evidence" "$source_dir/source/custom_shaders.c" \
           && ! grep -Fq "$evidence" "$source_dir/source/vitaGL.h" \
           && ! grep -Fq "$evidence" \
                "$source_dir/source/isaac_shader_cache_vitagl.h"; then
            echo "stock vitaGL shader-cache patch is incomplete: $evidence" >&2
            exit 5
        fi
    done
    verify_sha "$source_dir/source/isaac_shader_cache_policy.h" \
        "$shader_cache_policy_sha"
    verify_sha "$source_dir/source/isaac_shader_cache_vitagl.h" \
        "$shader_cache_integration_sha"
    verify_sha "$source_dir/source/isaac_shader_cache_block_list.h" \
        "$shader_cache_block_list_sha"
    # Third key: the 0008 ColorOffset FS probe applied (any non-zero mode;
    # the mode itself is a compile definition, not a source difference).
    coloroffset_fs_probe_applied=0
    if [ "$coloroffset_fs_probe" != 0 ]; then
        coloroffset_fs_probe_applied=1
    fi
    case "$gpu_draw_optimizations:$coloroffset_optimizations:$coloroffset_fs_probe_applied" in
    0:0:0)
        shader_source_sha=51377c7a9f75a27a093c86ba3106314fb3a08dff824fe6228ebbd796a106c9ed
        shader_header_sha=a0c2141c568048797a9431c3f3947d1975a013448d4327edb53316894d990a4e
        ;;
    1:0:0)
        shader_source_sha=1a8285ee3377855359268306d65a879f9697e2235e337aa9be4f496108da14d5
        shader_header_sha=4ed165659b6909b4a329e974b78ff48cab1d777dcd638a3dc066128dc1260bcc
        ;;
    1:1:0)
        shader_source_sha=6a9ca09632ab40e86af8d6752789bb75c6c0a10ebd03174339264e306c78f70e
        shader_header_sha=59e81f903cf6e6a0f5fd8f5b8785bea3b3d5ee65d5f1724f3e54d0808e6f0224
        ;;
    1:1:1)
        shader_source_sha=978752ece599cf284b620991eba2ee20b0cb8c969ec15db9b00539d5d2e1a529
        shader_header_sha=59e81f903cf6e6a0f5fd8f5b8785bea3b3d5ee65d5f1724f3e54d0808e6f0224
        ;;
    *)
        echo "unsupported shader-cache source combination" >&2
        exit 5
        ;;
    esac
    if [ "$coloroffset_staging_proof" = 1 ]; then
        shader_source_sha=c27605f306f1160a27f30fc5ade7c15492fcd5da2f34a37643edd9844b20a9e2
    fi
    if [ "$coloroffset_plain_fastpath" = 1 ]; then
        shader_source_sha=d304c32ea1b88f8511531cb2681e20ca4144236dae06d42c1507fd6da6ed4718
    fi
    if [ "$coloroffset_single_final_bind" = 1 ]; then
        if [ "$coloroffset_plain_fastpath" = 1 ]; then
            shader_source_sha=8888d87edeeb488d46a2def21a3f4e1dcd3ba986677c3ec5fd5495761ff78219
        else
            shader_source_sha=358f3419a559d7d00d463afeaa788e87b08fe13fd40fbc082e4d0ff18bd9ae7d
        fi
    fi
    if [ "$coloroffset_plain_vertex_pair" = 1 ]; then
        shader_source_sha=a4ff11a5911646d6ea70278a2243b737b50693876105d181569767a452e3c0ea
    fi
    if [ "$laser_light_halo_clip" = 1 ] || [ "$laser_atlas_nearest" = 1 ]; then
        shader_source_sha=64c3d77ec8f6ea565616bd5c3e5012ec7efd104a714faeff08747600be1f8737
        # Forward-compatible with the independently reviewed optional 0015.
        # Keep this override AFTER its source-hash selection when merging.
        if [ "${coloroffset_plain_vertex_pair:-0}" = 1 ]; then
            shader_source_sha=e887f6bcc48d6984e617028c0c17aeec89ace87df8315c55821cf89bd39aa894
        fi
        verify_sha "$source_dir/source/isaac_laser_light_halo_policy.h" "$laser_light_halo_policy_sha"
        verify_sha "$source_dir/source/isaac_laser_light_halo_vitagl.h" "$laser_light_halo_integration_sha"
    fi
    if [ "$laser_light_nearest" = 1 ] || [ "$laser_atlas_nearest" = 1 ]; then
        verify_sha "$source_dir/source/isaac_laser_light_nearest.h" "$laser_light_nearest_policy_sha"
    fi
    if [ "$coloroffset_transform_uniform" = 1 ]; then
        shader_source_sha=71ac71f8a637b22c4de50cba110590f4348b5cfdd3869710d71f4c8d3ecd64e5
        if [ "$laser_light_halo_clip" = 1 ] || [ "$laser_atlas_nearest" = 1 ]; then
            shader_source_sha=5d8df29f49e2994f38181d061b27829273f92841262597cae832a30279d4dea8
        fi
    fi
    # Contextual application plus these whole-file gates make placement and
    # resulting source identity fail closed for every supported combination.
    if [ "$laser_atlas_nearest" = 1 ]; then
        # Fresh pinned-source matrix, with transport materialized in both modes.
        case "$coloroffset_transform_uniform" in
        0) shader_source_sha=c4b77d487337a853a87abf6b0d3be265fdf69218c88d9666c64a868fc3b67ee2 ;;
        1) shader_source_sha=5ac2bedbaf323475dd586ed85b7b392ce53436b9fddb16a5a64e89690a52e613 ;;
        esac
    fi
    if [ "$coloroffset_link_proof" = 1 ]; then
        # Same whole-file identity gate, after the independently optional 0022.
        # Derived by regenerating every supported ColorOffset shader chain.
        case "$shader_source_sha" in
        6a9ca09632ab40e86af8d6752789bb75c6c0a10ebd03174339264e306c78f70e) shader_source_sha=a88131764bd930f7b7118336290ece627ed153540480d03dbea90349ae078294 ;;
        978752ece599cf284b620991eba2ee20b0cb8c969ec15db9b00539d5d2e1a529) shader_source_sha=1703e6adde1b06fd97a00c0f0792f3b36e65c0e01210a9f6658587c64aac092b ;;
        c27605f306f1160a27f30fc5ade7c15492fcd5da2f34a37643edd9844b20a9e2) shader_source_sha=56a8b62caf27f5da78bd7154397370986dc799a69acb9311adfad3e305b4946a ;;
        d304c32ea1b88f8511531cb2681e20ca4144236dae06d42c1507fd6da6ed4718) shader_source_sha=0a13fbb359dd78603d45e07abd8ddcc49559c9251238d89897de41e98e29caa6 ;;
        358f3419a559d7d00d463afeaa788e87b08fe13fd40fbc082e4d0ff18bd9ae7d) shader_source_sha=07657638528d6acfba87abee60f0bf7fa1c23a65aad7fbf92f12784287008b1c ;;
        8888d87edeeb488d46a2def21a3f4e1dcd3ba986677c3ec5fd5495761ff78219) shader_source_sha=8e92cdb4dbb3683070be26ff00e59b0112b37da129bcd88b20bcc17c11501c75 ;;
        a4ff11a5911646d6ea70278a2243b737b50693876105d181569767a452e3c0ea) shader_source_sha=5e050036e861a698e93ab8934d96c1921cac4f6d7877fea87f67d5c9b8ca8c68 ;;
        64c3d77ec8f6ea565616bd5c3e5012ec7efd104a714faeff08747600be1f8737) shader_source_sha=b39ff1c95d948e2294d94f701f25666ee216cd87efefb9df4195e224960db1fc ;;
        e887f6bcc48d6984e617028c0c17aeec89ace87df8315c55821cf89bd39aa894) shader_source_sha=e662b4454e467cf4a0210a85c9d423d7dd676769d67e0f75c0421fe572fab415 ;;
        71ac71f8a637b22c4de50cba110590f4348b5cfdd3869710d71f4c8d3ecd64e5) shader_source_sha=d6ea8bf8e98276d9fd247d4bf5b72196edbbf260fd79457c60ba6e553407f359 ;;
        5d8df29f49e2994f38181d061b27829273f92841262597cae832a30279d4dea8) shader_source_sha=7602026e7a49a904c507a0395fa0367fc7504e711f9e97917201cddabb35c0d2 ;;
        c4b77d487337a853a87abf6b0d3be265fdf69218c88d9666c64a868fc3b67ee2) shader_source_sha=74854ceb523d1549c989455e44d6330f478566486269300d54086b508ad1221f ;;
        5ac2bedbaf323475dd586ed85b7b392ce53436b9fddb16a5a64e89690a52e613) shader_source_sha=871c42274ae38417fae87c65c0de5cab137a741def6b446ab52956f0d4610489 ;;
        *) echo "unsupported ColorOffset link-proof source combination" >&2; exit 5 ;;
        esac
    fi
    if [ "$skip_zero_fragment_uniform" = 1 ]; then
        zero_fragment_source_identity
    fi
    if [ "$coloroffset_metadata_once" = 1 ]; then
        # Fresh pinned before/after identities; never bypass the full-file gate.
        case "$shader_source_sha" in
        f93b8ffe1cacb8f1656b4d3451f42e05202ce336d3e0a85d38e1b3fcd7ee3d50) shader_source_sha=1a432f2a70ae62acf2b50d00cf61170c29daaa0004543c285c310224d1cc0989 ;;
        7afd718fab909dc25e0798b5c91604ad9f3aaf5c8048384f2e1b59f883b48c3e) shader_source_sha=e4eb34d6f5b97d7f7b8c06abc5c15dd08448b6185f313149769b6eec67d3f864 ;;
        *) echo "unsupported ColorOffset metadata-once source combination" >&2; exit 5 ;;
        esac
    fi
    if [ "$laser_halo_profile" = 1 ]; then
        # Derived from the two current whole-file identities after 0026.
        # This observer never weakens the functional shader-source gate.
        case "$shader_source_sha" in
        1a432f2a70ae62acf2b50d00cf61170c29daaa0004543c285c310224d1cc0989) shader_source_sha=c0f50806292f24155744be90a4feb867f4c20664c2254d65577f9e9fdbc25881 ;;
        e4eb34d6f5b97d7f7b8c06abc5c15dd08448b6185f313149769b6eec67d3f864) shader_source_sha=fbe8199e47c83023cb70814f75081a780e22253904f54f9fedb0045480da250b ;;
        *) echo "unsupported halo-profile source combination" >&2; exit 5 ;;
        esac
        verify_sha "$source_dir/source/isaac_laser_halo_profile.h" "$laser_halo_profile_api_sha"
        case "$shader_source_sha" in
        c0f50806292f24155744be90a4feb867f4c20664c2254d65577f9e9fdbc25881) shader_source_sha=c070cf76a2a2270c1278a7cd29051955caf5118bd0a9e805bfd9b6143944130f ;;
        fbe8199e47c83023cb70814f75081a780e22253904f54f9fedb0045480da250b) shader_source_sha=0815d25a7e3ab3c66a9b9075975a47b93270df7cade3c57a744b2c454865608e ;;
        *) echo "unsupported white-census source combination" >&2; exit 5 ;;
        esac
    fi
    verify_sha "$source_dir/source/custom_shaders.c" "$shader_source_sha"
    verify_sha "$source_dir/source/vitaGL.h" "$shader_header_sha"
else
    if grep -Fq 'HAVE_ISAAC_SHADER_CACHE' \
            "$source_dir/source/custom_shaders.c" ||
       [ -e "$source_dir/source/isaac_shader_cache_policy.h" ]; then
        echo "stock vitaGL source unexpectedly contains the shader cache" >&2
        exit 5
    fi
    if [ -e "$source_dir/source/isaac_shader_cache_vitagl.h" ]; then
        echo "stock vitaGL source unexpectedly contains shader-cache integration" >&2
        exit 5
    fi
    if [ -e "$source_dir/source/isaac_shader_cache_block_list.h" ]; then
        echo "stock vitaGL source unexpectedly contains shader-cache block adapter" >&2
        exit 5
    fi
fi
if [ "$(sed -n '1p' "$marker")" != "$recipe" ]; then
    echo "stock vitaGL source recipe marker mismatch: $marker" >&2
    exit 4
fi
if [ "$coloroffset_transform_uniform" = 1 ]; then
    for evidence in \
        '#include "isaac_coloroffset_transform_uniform.h"' \
        'isaac_coloroffset_transform_unchanged(u, value)'; do
        if ! grep -Fq "$evidence" "$source_dir/source/custom_shaders.c"; then
            echo "stock vitaGL Transform uniform patch is incomplete: $evidence" >&2
            exit 5
        fi
    done
    verify_sha "$source_dir/source/isaac_coloroffset_transform_uniform.h" \
        "$coloroffset_transform_uniform_header_sha"
    if [ "$shader_cache" != 1 ]; then
        if [ "$coloroffset_link_proof" = 1 ]; then
            shader_source_sha=113d62e360c0722175c0124bc754537ad8abb03a5db15fbb7dfa94077b27c3a8
        else
            shader_source_sha=c15c96580aaf3a391f8d0a27714d0858037f6230cc931b7e4b103d3a9e4d1e76
        fi
        if [ "$skip_zero_fragment_uniform" = 1 ]; then
            zero_fragment_source_identity
        fi
        verify_sha "$source_dir/source/custom_shaders.c" "$shader_source_sha"
    fi
else
    if grep -Fq 'isaac_coloroffset_transform_unchanged' "$source_dir/source/custom_shaders.c" ||
       [ -e "$source_dir/source/isaac_coloroffset_transform_uniform.h" ]; then
        echo "stock vitaGL source unexpectedly contains Transform uniform suppression" >&2
        exit 5
    fi
fi
if [ "$coloroffset_link_proof" = 1 ]; then
    for evidence in \
        'isaac_coloroffset_fragment_sampler_is_exact' \
        'p->isaac_coloroffset_link_sampler_program != p->fshader->prog' \
        'progs[i].isaac_coloroffset_link_sampler_program = NULL;'; do
        if ! grep -Fq "$evidence" "$source_dir/source/custom_shaders.c"; then
            echo "stock vitaGL ColorOffset link-proof patch is incomplete: $evidence" >&2
            exit 5
        fi
    done
else
    if grep -Fq 'HAVE_ISAAC_COLOROFFSET_LINK_PROOF' "$source_dir/source/custom_shaders.c"; then
        echo "stock vitaGL source unexpectedly contains ColorOffset link proof" >&2
        exit 5
    fi
fi
if [ "$laser_atlas_nearest" = 1 ]; then
    verify_sha "$source_dir/source/isaac_laser_atlas_nearest.h" "$laser_atlas_nearest_policy_sha"
    verify_sha "$source_dir/source/isaac_laser_light_nearest.h" "$laser_light_nearest_policy_sha"
else
    if [ -e "$source_dir/source/isaac_laser_atlas_nearest.h" ]; then
        echo "stock vitaGL source unexpectedly contains laser atlas nearest" >&2
        exit 5
    fi
fi
if [ "$coloroffset_optimizations" = 1 ]; then
    for evidence in \
        '#include "isaac_coloroffset_gpu_policy.h"' \
        'vglIsaacMarkColorOffsetShader' \
        'isaac_coloroffset_select_opaque_fragment' \
        'vglGetIsaacColorOffsetStats'; do
        if ! grep -Fq "$evidence" "$source_dir/source/custom_shaders.c" \
           && ! grep -Fq "$evidence" "$source_dir/source/vitaGL.h"; then
            echo "stock vitaGL ColorOffset patch is incomplete: $evidence" >&2
            exit 5
        fi
    done
    verify_sha "$source_dir/source/isaac_coloroffset_gpu_policy.h" \
        "$coloroffset_policy_sha"
    if [ "$coloroffset_staging_proof" = 1 ]; then
        verify_sha "$source_dir/source/isaac_coloroffset_staging.h" \
            "$coloroffset_staging_policy_sha"
    fi
    if [ "$coloroffset_plain_fastpath" = 1 ]; then
        verify_sha "$source_dir/source/isaac_coloroffset_plain_policy.h" \
            "$coloroffset_plain_policy_sha"
        verify_sha "$source_dir/source/isaac_coloroffset_plain_vitagl.h" \
            "$coloroffset_plain_integration_sha"
    fi
    if [ "$coloroffset_plain_vertex_pair" = 1 ]; then
        verify_sha "$source_dir/source/isaac_coloroffset_plain_vertex_pair.h" \
            "$coloroffset_plain_vertex_pair_integration_sha"
    fi
    if [ "$coloroffset_plain_fp16" = 1 ]; then
        verify_sha "$source_dir/source/isaac_coloroffset_plain_fp16.h" \
            "$coloroffset_plain_fp16_header_sha"
    fi
else
    if grep -Fq 'HAVE_ISAAC_COLOROFFSET_GPU_OPTIMIZATIONS' \
            "$source_dir/source/custom_shaders.c" ||
       [ -e "$source_dir/source/isaac_coloroffset_gpu_policy.h" ]; then
        echo "stock vitaGL source unexpectedly contains ColorOffset optimizations" >&2
        exit 5
    fi
fi
# The probe lives in custom_shaders.c only; vitaGL.h stays byte-identical
# (consumer-declared statistics struct and endpoint, like the gxm.c hooks).
if [ "$coloroffset_fs_probe" != 0 ]; then
    for evidence in \
        'HAVE_ISAAC_COLOROFFSET_FS_PROBE' \
        'isaac_coloroffset_fs_probe_link' \
        'isaac_coloroffset_fs_probe_select' \
        'vglGetIsaacColorOffsetFsProbeStats'; do
        if ! grep -Fq "$evidence" "$source_dir/source/custom_shaders.c"; then
            echo "stock vitaGL ColorOffset FS probe patch is incomplete: $evidence" >&2
            exit 5
        fi
        if grep -Fq "$evidence" "$source_dir/source/vitaGL.h"; then
            echo "ColorOffset FS probe leaked into vitaGL.h: $evidence" >&2
            exit 5
        fi
    done
else
    if grep -Fq 'HAVE_ISAAC_COLOROFFSET_FS_PROBE' \
            "$source_dir/source/custom_shaders.c"; then
        echo "stock vitaGL source unexpectedly contains the ColorOffset FS probe" >&2
        exit 5
    fi
fi
if [ "$gl_time_profile" = 1 ]; then
    for evidence in \
        'HAVE_ISAAC_GL_TIME_PROFILE' \
        'vglIsaacSceneTimes' \
        'ISAAC_SCENE_TIME_END_BEGIN_SCENE' \
        'ISAAC_SCENE_TIME_END_END_SCENE' \
        'vglIsaacSceneWaits' \
        'ISAAC_SCENE_TIME_RT_CREATED' \
        'ISAAC_SCENE_TIME_DEPTH_CREATED' \
        'ISAAC_SCENE_TIME_SWAP' \
        'isaac_scene_end_ordinal_us'; do
        if ! grep -Fq "$evidence" "$source_dir/source/gxm.c"; then
            echo "stock vitaGL scene-timer/scene-split patches are incomplete: $evidence" >&2
            exit 5
        fi
    done
    # vitaGL.h must stay byte-identical: both hooks are declared by the consumer.
    if grep -Eq 'vglIsaacSceneTimes|vglIsaacSceneWaits' "$source_dir/source/vitaGL.h"; then
        echo "scene-timer hook leaked into vitaGL.h" >&2
        exit 5
    fi
else
    if grep -Eq 'HAVE_ISAAC_GL_TIME_PROFILE|vglIsaacSceneWaits' "$source_dir/source/gxm.c"; then
        echo "stock vitaGL source unexpectedly contains the scene timer" >&2
        exit 5
    fi
fi
if [ "$fbo_rt_scenes" = 1 ]; then
    for evidence in \
        'HAVE_ISAAC_FBO_RT_SCENES' \
        'gxm_fbo_rt_size' \
        'active_write_fb->height, ISAAC_FBO_RT_SCENES());' \
        'vglIsaacSetupFboRenderTargetScenes'; do
        if ! grep -Fq "$evidence" "$source_dir/source/gxm.c"; then
            echo "stock vitaGL FBO render-target scenes patch is incomplete: $evidence" >&2
            exit 5
        fi
    done
    if grep -Fq 'vglIsaacSetupFboRenderTargetScenes' "$source_dir/source/vitaGL.h"; then
        echo "FBO render-target scenes hook leaked into vitaGL.h" >&2
        exit 5
    fi
else
    # 0007 is not applied at all when the flag is 0, so even its guarded text
    # must be absent (0006 deliberately never spells these names).
    if grep -Eq 'HAVE_ISAAC_FBO_RT_SCENES|gxm_fbo_rt_size|vglIsaacSetupFboRenderTargetScenes' \
            "$source_dir/source/gxm.c"; then
        echo "stock vitaGL source unexpectedly contains the FBO render-target scenes patch" >&2
        exit 5
    fi
fi
if [ "$fbo_valid_region" = 1 ]; then
    for evidence in \
        'HAVE_ISAAC_FBO_VALID_REGION' \
        'isaac_fbo_region_for_scene' \
        'isaac_fbo_region_known' \
        'isaac_fbo_region_scene_reset_pending' \
        'ISAAC_FBO_REGION_RETRY_NULL' \
        'isaacFboRegionNoteViewport' \
        'isaacFboRegionClampClip' \
        'vglIsaacSetupFboValidRegion' \
        'vglIsaacFboValidRegionStats'; do
        if ! grep -Fq "$evidence" "$source_dir/source/gxm.c"; then
            echo "stock vitaGL FBO valid-region patch is incomplete: $evidence" >&2
            exit 5
        fi
    done
    # The two call sites outside gxm.c: the guest viewport record and the
    # tile-clipper clamp.
    if ! grep -Fq 'isaacFboRegionNoteViewport(x, y, width, height);' \
            "$source_dir/source/misc.c" ||
       ! grep -Fq 'ISAAC_FBO_REGION_CLAMP_CLIP(clip_x_min, clip_y_min, clip_x_max, clip_y_max);' \
            "$source_dir/source/tests.c"; then
        echo "stock vitaGL FBO valid-region patch is incomplete: misc.c/tests.c call sites" >&2
        exit 5
    fi
    # vitaGL.h and shared.h must stay byte-identical: both hooks and the
    # cross-file helpers are declared by their consumers.
    if grep -Fq 'isaacFboRegion' "$source_dir/source/vitaGL.h" ||
       grep -Fq 'vglIsaacSetupFboValidRegion' "$source_dir/source/vitaGL.h" ||
       grep -Fq 'vglIsaacFboValidRegionStats' "$source_dir/source/vitaGL.h" ||
       grep -Fq 'isaacFboRegion' "$source_dir/source/shared.h"; then
        echo "FBO valid-region hook leaked into vitaGL.h/shared.h" >&2
        exit 5
    fi
else
    # 0009 is not applied at all when the flag is 0, so even its guarded text
    # must be absent from all three files it edits.
    if grep -Eq 'HAVE_ISAAC_FBO_VALID_REGION|isaac_fbo_region|vglIsaacSetupFboValidRegion' \
            "$source_dir/source/gxm.c" ||
       grep -Fq 'isaacFboRegion' "$source_dir/source/misc.c" ||
       grep -Fq 'isaacFboRegion' "$source_dir/source/tests.c"; then
        echo "stock vitaGL source unexpectedly contains the FBO valid-region patch" >&2
        exit 5
    fi
fi
for evidence in \
    'VGL_GIT_HASH ?= $(shell git rev-parse --short HEAD 2>/dev/null)' \
    'CFLAGS += $(EXTRA_CFLAGS)' \
    '$(AR) -rcD $@ $^'; do
    if ! grep -Fq "$evidence" "$source_dir/Makefile"; then
        echo "stock vitaGL deterministic Makefile patch is incomplete: $evidence" >&2
        exit 5
    fi
done
if ! grep -Fq \
        'blit_streams[0].indexSource = SCE_GXM_INDEX_SOURCE_INDEX_16BIT;' \
        "$source_dir/source/vgl.c" ||
   grep -Fq 'blit_streams[1].indexSource' "$source_dir/source/vgl.c"; then
    echo "stock vitaGL lost the exact f24ad3 init OOB fix" >&2
    exit 5
fi
if [ "$gpu_draw_optimizations" = 1 ]; then
    for evidence in \
        '#include "isaac_gpu_draw_policy.h"' \
        'isaac_try_coalesce_single_stream' \
        'isaac_patch_vertex_program_cached' \
        'vglGetIsaacGpuDrawStats'; do
        if ! grep -Fq "$evidence" "$source_dir/source/custom_shaders.c" \
           && ! grep -Fq "$evidence" "$source_dir/source/vitaGL.h"; then
            echo "stock vitaGL GPU draw patch is incomplete: $evidence" >&2
            exit 5
        fi
    done
    verify_sha "$source_dir/source/isaac_gpu_draw_policy.h" \
        "$gpu_draw_policy_sha"
    verify_sha "$source_dir/source/isaac_gxm_state_policy.h" \
        "$gxm_state_policy_sha"
else
    if grep -Fq 'HAVE_ISAAC_GPU_DRAW_OPTIMIZATIONS' \
            "$source_dir/source/custom_shaders.c" ||
       [ -e "$source_dir/source/isaac_gpu_draw_policy.h" ]; then
        echo "default stock vitaGL source unexpectedly contains GPU draw optimizations" >&2
        exit 5
    fi
fi

jobs=${ISAAC_VITAGL_JOBS:-}
if [ -z "$jobs" ]; then
    jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1')
fi
debug_prefix="-fdebug-prefix-map=$source_dir=/usr/src/vitaGL-$source_commit"
file_prefix="-ffile-prefix-map=$source_dir=/usr/src/vitaGL-$source_commit"
extra_cflags="$debug_prefix $file_prefix"
if [ "$coloroffset_staging_proof" = 1 ]; then
    extra_cflags="$extra_cflags -DHAVE_ISAAC_COLOROFFSET_STAGING_PROOF=1"
fi
if [ "$coloroffset_single_final_bind" = 1 ]; then
    extra_cflags="$extra_cflags -DHAVE_ISAAC_COLOROFFSET_SINGLE_FINAL_BIND=1"
fi
if [ "$coloroffset_plain_fastpath" = 1 ]; then
    extra_cflags="$extra_cflags -DHAVE_ISAAC_COLOROFFSET_PLAIN_FASTPATH=1"
fi
if [ "$coloroffset_staging_plain_fusion" = 1 ]; then
    extra_cflags="$extra_cflags -DHAVE_ISAAC_COLOROFFSET_STAGING_PLAIN_FUSION=1"
fi
if [ "$coloroffset_staging_finite_neon" = 1 ]; then
    extra_cflags="$extra_cflags -DHAVE_ISAAC_COLOROFFSET_STAGING_FINITE_NEON=1"
fi
if [ "$coloroffset_staging_limits_neon" = 1 ]; then
    extra_cflags="$extra_cflags -DHAVE_ISAAC_COLOROFFSET_STAGING_LIMITS_NEON=1"
fi
if [ "$coloroffset_staging_outline" = 1 ]; then
    extra_cflags="$extra_cflags -DHAVE_ISAAC_COLOROFFSET_STAGING_OUTLINE=1"
fi
if [ "$coloroffset_plain_fp16" = 1 ]; then
    extra_cflags="$extra_cflags -DHAVE_ISAAC_COLOROFFSET_PLAIN_FP16=1"
fi
if [ "$coloroffset_plain_vertex_pair" = 1 ]; then
    extra_cflags="$extra_cflags -DHAVE_ISAAC_COLOROFFSET_PLAIN_VERTEX_PAIR=1"
fi
if [ "$coloroffset_transform_uniform" = 1 ]; then
    extra_cflags="$extra_cflags -DHAVE_ISAAC_COLOROFFSET_TRANSFORM_UNIFORM=1"
fi
if [ "$coloroffset_link_proof" = 1 ]; then
    extra_cflags="$extra_cflags -DHAVE_ISAAC_COLOROFFSET_LINK_PROOF=1"
fi
if [ "$skip_zero_fragment_uniform" = 1 ]; then
    extra_cflags="$extra_cflags -DHAVE_ISAAC_SKIP_ZERO_FRAGMENT_UNIFORM=1"
fi
if [ "$coloroffset_metadata_once" = 1 ]; then
    extra_cflags="$extra_cflags -DHAVE_ISAAC_COLOROFFSET_METADATA_ONCE=1"
fi
if [ "$gpu_draw_optimizations" = 1 ]; then
    extra_cflags="$extra_cflags -DHAVE_ISAAC_GPU_DRAW_OPTIMIZATIONS=1"
fi
if [ "$canonical_quad_zero_copy" = 1 ]; then
    extra_cflags="$extra_cflags -DHAVE_ISAAC_CANONICAL_QUAD_ZERO_COPY=1"
fi
if [ "$gxm_state_shadow" = 1 ]; then
    extra_cflags="$extra_cflags -DHAVE_ISAAC_GXM_STATE_SHADOW=1"
fi
if [ "$coloroffset_optimizations" = 1 ]; then
    extra_cflags="$extra_cflags -DHAVE_ISAAC_COLOROFFSET_GPU_OPTIMIZATIONS=1"
fi
if [ "$coloroffset_fs_probe" != 0 ]; then
    extra_cflags="$extra_cflags -DHAVE_ISAAC_COLOROFFSET_FS_PROBE=$coloroffset_fs_probe"
fi
if [ "$phase_profile" = 1 ]; then
    extra_cflags="$extra_cflags -DHAVE_ISAAC_PHASE_PROFILE=1"
fi
if [ "$gl_time_profile" = 1 ]; then
    extra_cflags="$extra_cflags -DHAVE_ISAAC_GL_TIME_PROFILE=1"
fi
if [ "$gl_time_sdk_sparse" = 1 ]; then
    extra_cflags="$extra_cflags -DHAVE_ISAAC_GL_TIME_SDK_SPARSE=1"
fi
if [ "$native_resource_profile" = 1 ]; then
    extra_cflags="$extra_cflags -DHAVE_ISAAC_NATIVE_RESOURCE_PROFILE=1"
fi
if [ "$fbo_rt_reuse" = 1 ]; then
    extra_cflags="$extra_cflags -DHAVE_ISAAC_FBO_RT_REUSE=1"
fi
if [ "$fbo_rt_reuse_lease" = 1 ]; then
    extra_cflags="$extra_cflags -DHAVE_ISAAC_FBO_RT_REUSE_LEASE=1"
fi
if [ "$fbo_rt_interlude_reuse" = 1 ]; then
    extra_cflags="$extra_cflags -DHAVE_ISAAC_FBO_RT_INTERLUDE_REUSE=1"
fi
if [ "$fbo_rt_scenes" = 1 ]; then
    extra_cflags="$extra_cflags -DHAVE_ISAAC_FBO_RT_SCENES=1"
fi
if [ "$p8_safe_upload" = 1 ]; then
    extra_cflags="$extra_cflags -DHAVE_ISAAC_P8_SAFE_UPLOAD=1"
fi
if [ "$fbo_valid_region" = 1 ]; then
    extra_cflags="$extra_cflags -DHAVE_ISAAC_FBO_VALID_REGION=1"
fi
if [ "$laser_atlas_p8" = 1 ]; then
    extra_cflags="$extra_cflags -DHAVE_ISAAC_LASER_ATLAS_P8=1"
fi
if [ "$laser_p8_swizzle" = 1 ]; then
    extra_cflags="$extra_cflags -DHAVE_ISAAC_LASER_P8_SWIZZLE=1 -DSUPPORT_SMALL_FMT=1"
fi
if [ "$laser_light_halo_clip" = 1 ]; then
    extra_cflags="$extra_cflags -DHAVE_ISAAC_LASER_LIGHT_HALO_CLIP=1"
fi
if [ "$laser_light_halo_depth_approx" = 1 ]; then
    extra_cflags="$extra_cflags -DHAVE_ISAAC_LASER_LIGHT_HALO_DEPTH_APPROX=1"
fi
if [ "$laser_red_cap_side_clip" = 1 ]; then
    extra_cflags="$extra_cflags -DHAVE_ISAAC_LASER_RED_CAP_SIDE_CLIP=1"
fi
if [ "$laser_light_producer_uv" = 1 ]; then
    extra_cflags="$extra_cflags -DHAVE_ISAAC_LASER_LIGHT_PRODUCER_UV=1"
fi
if [ "$laser_halo_proven_emit" = 1 ]; then
    extra_cflags="$extra_cflags -DHAVE_ISAAC_LASER_HALO_PROVEN_EMIT=1"
fi
if [ "$laser_halo_profile" = 1 ]; then
    extra_cflags="$extra_cflags -DHAVE_ISAAC_LASER_HALO_PROFILE=1"
fi
if [ "$laser_light_nearest" = 1 ]; then
    extra_cflags="$extra_cflags -DHAVE_ISAAC_LASER_LIGHT_NEAREST=1"
fi
if [ "$laser_atlas_nearest" = 1 ]; then
    extra_cflags="$extra_cflags -DHAVE_ISAAC_LASER_ATLAS_NEAREST=1"
fi
if [ "$shader_cache" = 1 ]; then
    extra_cflags="$extra_cflags -DHAVE_ISAAC_SHADER_CACHE=1 -DISAAC_SHADER_CACHE_BUILD_HEX=\\\"$shader_cache_recipe\\\""
fi

# Use an empty environment so no ambient make option can turn this control into
# another vitaGL profile. The stock flags, exact behavior switches and
# independent proof-counter switch define the whole behavioral set.
env -i PATH="$PATH" VITASDK="$VITASDK" SOURCE_DATE_EPOCH="$source_epoch" \
    make --directory="$source_dir" \
    --jobs="$jobs" \
    VGL_GIT_HASH="$source_short" \
    EXTRA_CFLAGS="$extra_cflags" \
    SOFTFP_ABI=1 \
    NO_DEBUG=1 \
    NO_SPLASHSCREEN=1 \
    SINGLE_THREADED_GC=1 \
    libvitaGL.a

built_library="$source_dir/libvitaGL.a"
built_header="$source_dir/source/vitaGL.h"
if [ ! -s "$built_library" ] || [ ! -s "$built_header" ]; then
    echo "stock vitaGL build did not produce its archive/header" >&2
    exit 6
fi
nm_symbols=$("$VITASDK/bin/arm-vita-eabi-nm" -g "$built_library")
case "$shader_cache:$nm_symbols" in
1:*vglGetIsaacShaderCacheStats*) ;;
1:*)
    echo "shader-cache stock vitaGL archive lacks its statistics endpoint" >&2
    exit 6
    ;;
0:*vglGetIsaacShaderCacheStats*)
    echo "shader-cache endpoint escaped its opt-in build" >&2
    exit 6
    ;;
0:*) ;;
esac
if [ "$coloroffset_optimizations" = 1 ]; then
    for endpoint in vglIsaacMarkColorOffsetShader vglGetIsaacColorOffsetStats; do
        if ! printf '%s\n' "$nm_symbols" | grep -Fq "$endpoint"; then
            echo "optimized stock vitaGL archive lacks $endpoint" >&2
            exit 6
        fi
    done
else
    if printf '%s\n' "$nm_symbols" |
            grep -Eq 'vglIsaacMarkColorOffsetShader|vglGetIsaacColorOffsetStats'; then
        echo "stock vitaGL archive contains disabled ColorOffset endpoints" >&2
        exit 6
    fi
fi
# The archive exports the FS probe statistics endpoint iff the probe is on.
if [ "$coloroffset_fs_probe" != 0 ]; then
    if ! printf '%s\n' "$nm_symbols" | grep -Fq vglGetIsaacColorOffsetFsProbeStats; then
        echo "ColorOffset FS probe archive lacks vglGetIsaacColorOffsetFsProbeStats" >&2
        exit 6
    fi
else
    if printf '%s\n' "$nm_symbols" | grep -Fq vglGetIsaacColorOffsetFsProbeStats; then
        echo "ColorOffset FS probe endpoint escaped its opt-in build" >&2
        exit 6
    fi
fi
if [ "$gpu_draw_optimizations" = 1 ]; then
    case "$nm_symbols" in
    *vglGetIsaacGpuDrawStats*) ;;
    *)
        echo "optimized stock vitaGL archive lacks its statistics endpoint" >&2
        exit 6
        ;;
    esac
else
    case "$nm_symbols" in
    *vglGetIsaacGpuDrawStats*)
        echo "default stock vitaGL archive contains the opt-in statistics endpoint" >&2
        exit 6
        ;;
    *) ;;
    esac
fi
case "$gl_time_profile:$nm_symbols" in
1:*vglIsaacSceneTimes*) ;;
1:*)
    echo "scene-timer stock vitaGL archive lacks vglIsaacSceneTimes" >&2
    exit 6
    ;;
0:*vglIsaacSceneTimes*)
    echo "scene-timer endpoint escaped its opt-in build" >&2
    exit 6
    ;;
0:*) ;;
esac
case "$gl_time_sdk_sparse:$nm_symbols" in
1:*vglIsaacSdkSparseTake*) ;;
1:*) echo "SDK-sparse profile endpoint missing from stock vitaGL archive" >&2; exit 6 ;;
0:*vglIsaacSdkSparseTake*) echo "SDK-sparse endpoint escaped its opt-in build" >&2; exit 6 ;;
0:*) ;;
esac
case "$laser_halo_profile:$nm_symbols" in
1:*vglGetIsaacLaserHaloStats*) ;;
1:*) echo "halo profile endpoint missing from stock vitaGL archive" >&2; exit 6 ;;
0:*vglGetIsaacLaserHaloStats*) echo "halo profile endpoint escaped its opt-in build" >&2; exit 6 ;;
0:*) ;;
esac
case "$gl_time_profile:$nm_symbols" in
1:*vglIsaacSceneWaits*) ;;
1:*)
    echo "scene-split stock vitaGL archive lacks vglIsaacSceneWaits" >&2
    exit 6
    ;;
0:*vglIsaacSceneWaits*)
    echo "scene-split endpoint escaped its opt-in build" >&2
    exit 6
    ;;
0:*) ;;
esac
# The native resource snapshot is exported only by its opt-in build.
case "$native_resource_profile:$nm_symbols" in
1:*vglIsaacNativeResourceProfileTake*) ;;
1:*)
    echo "native resource profile endpoint missing from stock vitaGL archive" >&2
    exit 6
    ;;
0:*vglIsaacNativeResourceProfileTake*)
    echo "native resource profile escaped its opt-in build" >&2
    exit 6
    ;;
0:*) ;;
esac
# The FBO render-target scenes symbol, not source text, proves the knob.
case "$fbo_rt_scenes:$nm_symbols" in
1:*vglIsaacSetupFboRenderTargetScenes*) ;;
1:*)
    echo "FBO render-target scenes archive lacks vglIsaacSetupFboRenderTargetScenes" >&2
    exit 6
    ;;
0:*vglIsaacSetupFboRenderTargetScenes*)
    echo "FBO render-target scenes endpoint escaped its opt-in build" >&2
    exit 6
    ;;
0:*) ;;
esac
# Both 0009 hooks (mode setup, counter take) are exported iff the flag is 1.
for endpoint in vglIsaacSetupFboValidRegion vglIsaacFboValidRegionStats; do
    case "$fbo_valid_region:$nm_symbols" in
    1:*"$endpoint"*) ;;
    1:*)
        echo "FBO valid-region archive lacks $endpoint" >&2
        exit 6
        ;;
    0:*"$endpoint"*)
        echo "FBO valid-region endpoint escaped its opt-in build: $endpoint" >&2
        exit 6
        ;;
    0:*) ;;
    esac
done
case "$canonical_quad_zero_copy:$nm_symbols" in
1:*vglIsaacDrawCanonicalQuads*) ;;
1:*)
    echo "optimized stock vitaGL archive lacks canonical quad endpoint" >&2
    exit 6
    ;;
0:*vglIsaacDrawCanonicalQuads*)
    echo "canonical quad endpoint escaped its opt-in build" >&2
    exit 6
    ;;
0:*) ;;
esac
for endpoint in isaacGxmSetVertexProgram isaacGxmSetFragmentProgram \
        isaacGxmSetFragmentTexture; do
    case "$gxm_state_shadow:$nm_symbols" in
    1:*"$endpoint"*) ;;
    1:*)
        echo "GXM state-shadow archive lacks $endpoint" >&2
        exit 6
        ;;
    0:*"$endpoint"*)
        echo "GXM state-shadow endpoint escaped its opt-in build: $endpoint" >&2
        exit 6
        ;;
    0:*) ;;
    esac
done
case "$fbo_rt_reuse:$nm_symbols" in
1:*isaacFboRtSwitch*) ;;
1:*) echo "FBO RT reuse implementation was not linked into native archive" >&2; exit 6 ;;
0:*isaacFboRtSwitch*) echo "FBO RT reuse escaped its opt-in build" >&2; exit 6 ;;
0:*) ;;
esac
case "$fbo_rt_reuse_lease:$nm_symbols" in
1:*vglIsaacFboRtDrainAfterFinish*) ;;
1:*) echo "Bounded FBO RT lease lacks its original-deadline-witnessed drain" >&2; exit 6 ;;
0:*vglIsaacFboRtDrainAfterFinish*) echo "Bounded FBO RT lease escaped its opt-in build" >&2; exit 6 ;;
0:*) ;;
esac
cp -- "$built_library" "$prefix/lib/libvitaGL.a"
cp -- "$built_header" "$prefix/include/vitaGL.h"
printf '%s\n' "$recipe" > "$output_root/recipe.txt"
{
    printf 'profile=%s\n' 'stock-vitagl-reference'
    printf 'source_commit=%s\n' "$source_commit"
    printf 'source_sha256=%s\n' "$source_sha"
    printf 'backported_oob_fix_commit=%s\n' "$oob_fix_commit"
    printf 'patch_sha256=%s\n' "$patch_sha"
    printf 'fbo_float_selector_sync=1\n'
    printf 'fbo_float_sync_patch_sha256=%s\n' "$fbo_float_sync_patch_sha"
    printf 'script_sha256=%s\n' "$script_sha"
    printf 'fbo_scissor_resize_invalidation=1\n'
    printf 'fbo_scissor_resize_patch_sha256=%s\n' "$fbo_scissor_resize_patch_sha"
    printf 'fbo_scissor_replay_sync=1\n'
    printf 'fbo_scissor_replay_patch_sha256=%s\n' "$fbo_scissor_replay_patch_sha"
    printf 'fbo_scissor_replay_region_patch_sha256=%s\n' "$fbo_scissor_replay_region_patch_sha"
    printf 'flags=%s\n' "$build_flags"
    printf 'gpu_draw_optimizations=%s\n' "$gpu_draw_optimizations"
    printf 'gpu_draw_patch_sha256=%s\n' "$gpu_draw_patch_sha"
    printf 'gpu_draw_policy_sha256=%s\n' "$gpu_draw_policy_sha"
    printf 'gpu_draw_policy=%s\n' "$gpu_draw_policy_name"
    printf 'canonical_quad_zero_copy=%s\n' "$canonical_quad_zero_copy"
    printf 'gxm_state_shadow=%s\n' "$gxm_state_shadow"
    printf 'gxm_state_policy_sha256=%s\n' "$gxm_state_policy_sha"
    printf 'coloroffset_gpu_optimizations=%s\n' "$coloroffset_optimizations"
    printf 'coloroffset_patch_sha256=%s\n' "$coloroffset_patch_sha"
    printf 'coloroffset_policy_sha256=%s\n' "$coloroffset_policy_sha"
    printf 'coloroffset_policy=%s\n' "$coloroffset_policy_name"
    printf 'coloroffset_fs_probe=%s\n' "$coloroffset_fs_probe"
    printf 'coloroffset_fs_probe_mode=%s\n' "$coloroffset_fs_probe_name"
    printf 'coloroffset_fs_probe_patch_sha256=%s\n' "$coloroffset_fs_probe_patch_sha"
    printf 'coloroffset_staging_proof=%s\n' "$coloroffset_staging_proof"
    printf 'coloroffset_staging_patch_sha256=%s\n' "$coloroffset_staging_patch_sha"
    printf 'coloroffset_staging_policy_sha256=%s\n' "$coloroffset_staging_policy_sha"
    printf 'coloroffset_single_final_bind=%s\n' "$coloroffset_single_final_bind"
    printf 'coloroffset_single_final_bind_patch_sha256=%s\n' "$coloroffset_single_final_bind_patch_sha"
    printf 'coloroffset_plain_fastpath=%s\n' "$coloroffset_plain_fastpath"
    printf 'coloroffset_staging_plain_fusion=%s\n' "$coloroffset_staging_plain_fusion"
    printf 'coloroffset_staging_finite_neon=%s\n' "$coloroffset_staging_finite_neon"
    printf 'coloroffset_staging_limits_neon=%s\n' "$coloroffset_staging_limits_neon"
    printf 'coloroffset_staging_outline=%s\n' "$coloroffset_staging_outline"
    printf 'coloroffset_plain_patch_sha256=%s\n' "$coloroffset_plain_patch_sha"
    printf 'coloroffset_plain_policy_sha256=%s\n' "$coloroffset_plain_policy_sha"
    printf 'coloroffset_plain_integration_sha256=%s\n' "$coloroffset_plain_integration_sha"
    printf 'coloroffset_plain_fp16=%s\n' "$coloroffset_plain_fp16"
    printf 'coloroffset_plain_fp16_header_sha256=%s\n' "$coloroffset_plain_fp16_header_sha"
    printf 'coloroffset_plain_vertex_pair=%s\n' "$coloroffset_plain_vertex_pair"
    printf 'coloroffset_plain_vertex_pair_patch_sha256=%s\n' "$coloroffset_plain_vertex_pair_patch_sha"
    printf 'coloroffset_plain_vertex_pair_integration_sha256=%s\n' "$coloroffset_plain_vertex_pair_integration_sha"
    printf 'coloroffset_transform_uniform=%s\n' "$coloroffset_transform_uniform"
    printf 'coloroffset_transform_uniform_patch_sha256=%s\n' "$coloroffset_transform_uniform_patch_sha"
    printf 'coloroffset_transform_uniform_header_sha256=%s\n' "$coloroffset_transform_uniform_header_sha"
    printf 'coloroffset_link_proof=%s\n' "$coloroffset_link_proof"
    printf 'coloroffset_link_proof_patch_sha256=%s\n' "$coloroffset_link_proof_patch_sha"
    printf 'skip_zero_fragment_uniform=%s\n' "$skip_zero_fragment_uniform"
    printf 'skip_zero_fragment_uniform_patch_sha256=%s\n' "$skip_zero_fragment_uniform_patch_sha"
    printf 'coloroffset_metadata_once=%s\n' "$coloroffset_metadata_once"
    printf 'coloroffset_metadata_once_patch_sha256=%s\n' "$coloroffset_metadata_once_patch_sha"
    printf 'coloroffset_metadata_once_policy=%s\n' 'draw-local-halo-no-tex-cache; float-change-recheck; no-cross-draw-state'
    printf 'phase_profile=%s\n' "$phase_profile"
    printf 'gl_time_profile=%s\n' "$gl_time_profile"
    printf 'gl_time_sdk_sparse=%s\n' "$gl_time_sdk_sparse"
    printf 'sdk_sparse_patch_sha256=%s\n' "$sdk_sparse_patch_sha"
    printf 'sdk_sparse_api_sha256=%s\n' "$sdk_sparse_api_sha"
    printf 'sdk_sparse_internal_sha256=%s\n' "$sdk_sparse_internal_sha"
    printf 'sdk_sparse_source_sha256=%s\n' "$sdk_sparse_source_sha"
    printf 'native_resource_profile=%s\n' "$native_resource_profile"
    printf 'fbo_rt_reuse=%s\n' "$fbo_rt_reuse"
    printf 'fbo_rt_reuse_patch_sha256=%s\n' "$fbo_rt_reuse_patch_sha"
    printf 'fbo_rt_reuse_header_sha256=%s\n' "$fbo_rt_reuse_header_sha"
    printf 'fbo_rt_reuse_source_sha256=%s\n' "$fbo_rt_reuse_source_sha"
    printf 'fbo_rt_reuse_lease=%s\n' "$fbo_rt_reuse_lease"
    printf 'fbo_rt_reuse_lease_patch_sha256=%s\n' "$fbo_rt_reuse_lease_patch_sha"
    printf 'fbo_rt_interlude_reuse=%s\n' "$fbo_rt_interlude_reuse"
    printf 'fbo_rt_interlude_patch_sha256=%s\n' "$fbo_rt_interlude_patch_sha"
    printf 'fbo_rt_reuse_lease_policy=%s\n' 'one-spare; one-extra-rotation; current-retirement-Collect-witness; Finish-is-not-EndScene'
    printf 'native_resource_patch_sha256=%s\n' "$native_resource_patch_sha"
    printf 'native_resource_api_sha256=%s\n' "$native_resource_api_sha"
    printf 'native_resource_internal_sha256=%s\n' "$native_resource_internal_sha"
    printf 'native_resource_source_sha256=%s\n' "$native_resource_source_sha"
    printf 'scene_timer_patch_sha256=%s\n' "$scene_timer_patch_sha"
    printf 'scene_split_patch_sha256=%s\n' "$scene_split_patch_sha"
    printf 'fbo_rt_scenes=%s\n' "$fbo_rt_scenes"
    printf 'fbo_rt_scenes_patch_sha256=%s\n' "$fbo_rt_scenes_patch_sha"
    printf 'p8_safe_upload=%s\n' "$p8_safe_upload"
    printf 'p8_safe_patch_sha256=%s\n' "$p8_safe_patch_sha"
    printf 'p8_safe_policy_sha256=%s\n' "$p8_safe_policy_sha"
    printf 'fbo_valid_region=%s\n' "$fbo_valid_region"
    printf 'fbo_valid_region_patch_sha256=%s\n' "$fbo_valid_region_patch_sha"
    printf 'laser_atlas_p8=%s\n' "$laser_atlas_p8"
    printf 'laser_p8_swizzle=%s\n' "$laser_p8_swizzle"
    printf 'laser_p8_patch_sha256=%s\n' "$laser_p8_patch_sha"
    printf 'laser_p8_policy_sha256=%s\n' "$laser_p8_policy_sha"
    printf 'laser_p8_reference_sha256=%s\n' "$laser_p8_reference_sha"
    printf 'laser_light_halo_clip=%s\n' "$laser_light_halo_clip"
    printf 'laser_light_halo_depth_approx=%s\n' "$laser_light_halo_depth_approx"
    printf 'laser_red_cap_side_clip=%s\n' "$laser_red_cap_side_clip"
    printf 'laser_light_producer_uv=%s\n' "$laser_light_producer_uv"
    printf 'laser_halo_proven_emit=%s\n' "$laser_halo_proven_emit"
    printf 'laser_light_halo_patch_sha256=%s\n' "$laser_light_halo_patch_sha"
    printf 'laser_light_halo_policy_sha256=%s\n' "$laser_light_halo_policy_sha"
    printf 'laser_light_halo_integration_sha256=%s\n' "$laser_light_halo_integration_sha"
    printf 'laser_halo_profile=%s\n' "$laser_halo_profile"
    printf 'laser_halo_profile_patch_sha256=%s\n' "$laser_halo_profile_patch_sha"
    printf 'laser_halo_profile_api_sha256=%s\n' "$laser_halo_profile_api_sha"
    printf 'laser_halo_profile_policy=%s\n' 'abi2; legacy11-abi1; packed-client-copy-opportunities; first-failure; modular-u32; no-clocks'
    printf 'white_census_patch_sha256=%s\n' "$white_census_patch_sha"
    printf 'white_census_enabled=%s\n' "$((laser_halo_profile * coloroffset_staging_plain_fusion))"
    printf 'laser_light_nearest=%s\n' "$laser_light_nearest"
    printf 'laser_light_nearest_patch_sha256=%s\n' "$laser_light_nearest_patch_sha"
    printf 'laser_light_nearest_policy_sha256=%s\n' "$laser_light_nearest_policy_sha"
    printf 'laser_atlas_nearest=%s\n' "$laser_atlas_nearest"
    printf 'laser_atlas_nearest_patch_sha256=%s\n' "$laser_atlas_nearest_patch_sha"
    printf 'laser_atlas_nearest_policy_sha256=%s\n' "$laser_atlas_nearest_policy_sha"
    printf 'shader_cache=%s\n' "$shader_cache"
    printf 'shader_cache_observer_compat=%s\n' "$shader_cache_observer_compat"
    printf 'shader_cache_identity=%s\n' "$shader_cache_identity"
    printf 'shader_cache_build_sha256=%s\n' "$shader_cache_recipe"
    printf 'shader_cache_patch_sha256=%s\n' "$shader_cache_patch_sha"
    printf 'shader_cache_policy_sha256=%s\n' "$shader_cache_policy_sha"
    printf 'shader_cache_integration_sha256=%s\n' \
        "$shader_cache_integration_sha"
    printf 'shader_cache_block_list_sha256=%s\n' \
        "$shader_cache_block_list_sha"
    printf 'shader_cache_root=%s\n' 'ux0:data/isaacr001/shader-cache/v2'
    printf 'shared_render_targets=%s\n' 'off'
    printf 'gxm_source_patch=%s\n' "$gxm_source_patch_name"
} > "$output_root/build-contract.txt"
sha256sum "$prefix/lib/libvitaGL.a" > "$output_root/libvitaGL.sha256"
sha256sum "$prefix/include/vitaGL.h" > "$output_root/vitaGL.h.sha256"
echo "Stock vitaGL reference: $prefix/lib/libvitaGL.a"
