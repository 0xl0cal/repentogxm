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
shader_cache_patch="$script_dir/0004-hardened-custom-shader-cache.patch"
shader_cache_policy="$script_dir/isaac_shader_cache_policy.h"
shader_cache_integration="$script_dir/isaac_shader_cache_vitagl.h"
shader_cache_block_list="$script_dir/isaac_shader_cache_block_list.h"
scene_timer_patch="$script_dir/0005-isaac-scene-timer.patch"
gpu_draw_optimizations=${ISAAC_GPU_DRAW_OPTIMIZATIONS:-0}
shader_cache=${ISAAC_SHADER_CACHE:-0}
canonical_quad_zero_copy=${ISAAC_CANONICAL_QUAD_ZERO_COPY:-0}
gxm_state_shadow=${ISAAC_GXM_STATE_SHADOW:-0}
coloroffset_optimizations=${ISAAC_COLOROFFSET_GPU_OPTIMIZATIONS:-0}
phase_profile=${ISAAC_PHASE_PROFILE:-0}
gl_time_profile=${ISAAC_GL_TIME_PROFILE:-0}

for value in \
    "gpu_draw_optimizations:$gpu_draw_optimizations" \
    "shader_cache:$shader_cache" \
    "canonical_quad_zero_copy:$canonical_quad_zero_copy" \
    "gxm_state_shadow:$gxm_state_shadow" \
    "coloroffset_optimizations:$coloroffset_optimizations" \
    "phase_profile:$phase_profile" \
    "gl_time_profile:$gl_time_profile"; do
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
# The scene-timer patch is written against gxm.c as left by the 0002 draw
# patch and only has a consumer through the phase profiler.
if [ "$gl_time_profile" = 1 ] &&
   { [ "$gpu_draw_optimizations" != 1 ] || [ "$phase_profile" != 1 ]; }; then
    echo "GL time profile requires ISAAC_GPU_DRAW_OPTIMIZATIONS=1 and ISAAC_PHASE_PROFILE=1" >&2
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
    # The stock tree declares text=auto.  Without this pin, an ambient Windows
    # core.autocrlf=true (or a hostile core.eol) rewrites every touched source
    # to CRLF and changes the whole-file evidence hashes even though the patch
    # content is identical.
    (cd "$source_dir" &&
        git -c core.autocrlf=false -c core.eol=lf \
            apply --check "$patch")
    (cd "$source_dir" &&
        git -c core.autocrlf=false -c core.eol=lf \
            apply "$patch")
}

download "$source_url" "$source_archive" "$source_sha"
patch_sha=$(sha256sum "$contract_patch" | awk '{print $1}')
gpu_draw_patch_sha=$(sha256sum "$gpu_draw_patch" | awk '{print $1}')
gpu_draw_policy_sha=$(sha256sum "$gpu_draw_policy" | awk '{print $1}')
gxm_state_policy_sha=$(sha256sum "$gxm_state_policy" | awk '{print $1}')
coloroffset_patch_sha=$(sha256sum "$coloroffset_patch" | awk '{print $1}')
coloroffset_policy_sha=$(sha256sum "$coloroffset_policy" | awk '{print $1}')
shader_cache_patch_sha=$(sha256sum "$shader_cache_patch" | awk '{print $1}')
shader_cache_policy_sha=$(sha256sum "$shader_cache_policy" | awk '{print $1}')
shader_cache_integration_sha=$(sha256sum "$shader_cache_integration" | awk '{print $1}')
shader_cache_block_list_sha=$(sha256sum "$shader_cache_block_list" | awk '{print $1}')
scene_timer_patch_sha=$(sha256sum "$scene_timer_patch" | awk '{print $1}')
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
if [ "$phase_profile" = 1 ]; then
    build_flags="$build_flags HAVE_ISAAC_PHASE_PROFILE=1"
fi
gxm_source_patch_name=none
if [ "$gl_time_profile" = 1 ]; then
    build_flags="$build_flags HAVE_ISAAC_GL_TIME_PROFILE=1"
    gxm_source_patch_name=0005-isaac-scene-timer
fi
recipe=$(printf '%s\n' \
    "$source_commit" "$source_sha" "$patch_sha" "$script_sha" \
    "$build_flags" "$oob_fix_commit" "$gpu_draw_optimizations" \
    "$gpu_draw_patch_sha" "$gpu_draw_policy_sha" \
    "$canonical_quad_zero_copy" "$gxm_state_shadow" \
    "$gxm_state_policy_sha" \
    "$coloroffset_optimizations" "$coloroffset_patch_sha" \
    "$coloroffset_policy_sha" "$phase_profile" "$shader_cache" \
    "$shader_cache_patch_sha" "$shader_cache_policy_sha" \
    "$shader_cache_integration_sha" "$shader_cache_block_list_sha" \
    "$gl_time_profile" "$scene_timer_patch_sha" |
    sha256sum | awk '{print $1}')
source_parent="$work/source-$recipe"
source_dir="$source_parent/vitaGL-$source_commit"
marker="$source_parent/.isaac-stock-vitagl-recipe"

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
    if [ "$shader_cache" = 1 ]; then
        apply_source_patch "$shader_cache_patch"
        cp -- "$shader_cache_policy" \
            "$source_dir/source/isaac_shader_cache_policy.h"
        cp -- "$shader_cache_integration" \
            "$source_dir/source/isaac_shader_cache_vitagl.h"
        cp -- "$shader_cache_block_list" \
            "$source_dir/source/isaac_shader_cache_block_list.h"
    fi
    if [ "$gl_time_profile" = 1 ]; then
        apply_source_patch "$scene_timer_patch"
    fi
    printf '%s\n' "$recipe" > "$marker"
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
    case "$gpu_draw_optimizations:$coloroffset_optimizations" in
    0:0)
        shader_source_sha=51377c7a9f75a27a093c86ba3106314fb3a08dff824fe6228ebbd796a106c9ed
        shader_header_sha=a0c2141c568048797a9431c3f3947d1975a013448d4327edb53316894d990a4e
        ;;
    1:0)
        shader_source_sha=1a8285ee3377855359268306d65a879f9697e2235e337aa9be4f496108da14d5
        shader_header_sha=4ed165659b6909b4a329e974b78ff48cab1d777dcd638a3dc066128dc1260bcc
        ;;
    1:1)
        shader_source_sha=6a9ca09632ab40e86af8d6752789bb75c6c0a10ebd03174339264e306c78f70e
        shader_header_sha=59e81f903cf6e6a0f5fd8f5b8785bea3b3d5ee65d5f1724f3e54d0808e6f0224
        ;;
    *)
        echo "unsupported shader-cache source combination" >&2
        exit 5
        ;;
    esac
    # Contextual application plus these whole-file gates make placement and
    # resulting source identity fail closed for every supported combination.
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
else
    if grep -Fq 'HAVE_ISAAC_COLOROFFSET_GPU_OPTIMIZATIONS' \
            "$source_dir/source/custom_shaders.c" ||
       [ -e "$source_dir/source/isaac_coloroffset_gpu_policy.h" ]; then
        echo "stock vitaGL source unexpectedly contains ColorOffset optimizations" >&2
        exit 5
    fi
fi
if [ "$gl_time_profile" = 1 ]; then
    for evidence in \
        'HAVE_ISAAC_GL_TIME_PROFILE' \
        'vglIsaacSceneTimes' \
        'ISAAC_SCENE_TIME_END_BEGIN_SCENE' \
        'ISAAC_SCENE_TIME_END_END_SCENE'; do
        if ! grep -Fq "$evidence" "$source_dir/source/gxm.c"; then
            echo "stock vitaGL scene-timer patch is incomplete: $evidence" >&2
            exit 5
        fi
    done
    # vitaGL.h must stay byte-identical: the hook is declared by the consumer.
    if grep -Fq 'vglIsaacSceneTimes' "$source_dir/source/vitaGL.h"; then
        echo "scene-timer hook leaked into vitaGL.h" >&2
        exit 5
    fi
else
    if grep -Fq 'HAVE_ISAAC_GL_TIME_PROFILE' "$source_dir/source/gxm.c"; then
        echo "stock vitaGL source unexpectedly contains the scene timer" >&2
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
if [ "$phase_profile" = 1 ]; then
    extra_cflags="$extra_cflags -DHAVE_ISAAC_PHASE_PROFILE=1"
fi
if [ "$gl_time_profile" = 1 ]; then
    extra_cflags="$extra_cflags -DHAVE_ISAAC_GL_TIME_PROFILE=1"
fi
if [ "$shader_cache" = 1 ]; then
    extra_cflags="$extra_cflags -DHAVE_ISAAC_SHADER_CACHE=1 -DISAAC_SHADER_CACHE_BUILD_HEX=\\\"$recipe\\\""
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
cp -- "$built_library" "$prefix/lib/libvitaGL.a"
cp -- "$built_header" "$prefix/include/vitaGL.h"
printf '%s\n' "$recipe" > "$output_root/recipe.txt"
{
    printf 'profile=%s\n' 'stock-vitagl-reference'
    printf 'source_commit=%s\n' "$source_commit"
    printf 'source_sha256=%s\n' "$source_sha"
    printf 'backported_oob_fix_commit=%s\n' "$oob_fix_commit"
    printf 'patch_sha256=%s\n' "$patch_sha"
    printf 'script_sha256=%s\n' "$script_sha"
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
    printf 'phase_profile=%s\n' "$phase_profile"
    printf 'gl_time_profile=%s\n' "$gl_time_profile"
    printf 'scene_timer_patch_sha256=%s\n' "$scene_timer_patch_sha"
    printf 'shader_cache=%s\n' "$shader_cache"
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
