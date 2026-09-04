#!/usr/bin/env bash
set -eu

source_commit=f4b23b61c84e8ba59de542832f8e507b3660f994
source_short=f4b23b6
source_epoch=1787407070
source_sha=01629941b75fbf228a3b4b0bd5a57bebecc62e367aa05fafa104694df6e0879f
draw_first_fix_commit=a136dd9ff73534a53bb45dcbeceb7c5ffafdb038

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
contract_patch="$script_dir/0001-isaac-build-contract.patch"
display_queue_probe=${ISAAC_DISPLAY_QUEUE_PROBE:-0}
known_color_probe=${ISAAC_KNOWN_COLOR_PROBE:-0}

case "$display_queue_probe" in
    0|1) ;;
    *)
        echo "ISAAC_DISPLAY_QUEUE_PROBE must be 0 or 1" >&2
        exit 2
        ;;
esac
case "$known_color_probe" in
    0|1) ;;
    *)
        echo "ISAAC_KNOWN_COLOR_PROBE must be 0 or 1" >&2
        exit 2
        ;;
esac
if [ "$known_color_probe" -eq 1 ] && [ "$display_queue_probe" -ne 1 ]; then
    echo "ISAAC_KNOWN_COLOR_PROBE=1 requires ISAAC_DISPLAY_QUEUE_PROBE=1" >&2
    exit 2
fi
if [ "$display_queue_probe" -eq 1 ]; then
    display_queue_cpu_sentinel=16x16-at-16x16-v1
else
    display_queue_cpu_sentinel=off
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
   [ ! -x "$VITASDK/bin/arm-vita-eabi-gcc-ar" ]; then
    echo "VITASDK lacks the ARM compiler/archive tools: $VITASDK" >&2
    exit 2
fi
if ! command -v git >/dev/null 2>&1; then
    echo "git is required to apply the transactional vitaGL contract patch" >&2
    exit 2
fi
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
        echo "refusing unsafe vitaGL overlay output root: $output_root" >&2
        exit 2
        ;;
    "$sdk_root"|"$sdk_root"/*)
        echo "refusing to place the vitaGL overlay inside VitaSDK: $output_root" >&2
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

download "$source_url" "$source_archive" "$source_sha"
patch_sha=$(sha256sum "$contract_patch" | awk '{print $1}')
build_flags="SOFTFP_ABI=1 NO_DEBUG=1 NO_SPLASHSCREEN=1 SINGLE_THREADED_GC=1 SHARED_RENDERTARGETS=1 ISAAC_DISPLAY_QUEUE_PROBE=$display_queue_probe ISAAC_KNOWN_COLOR_PROBE=$known_color_probe"
recipe=$(printf '%s\n%s\n%s\n%s\n%s\n' \
    "$source_commit" "$source_sha" "$patch_sha" "$build_flags" \
    "$draw_first_fix_commit" | \
    sha256sum | awk '{print $1}')
source_parent="$work/source-$recipe"
source_dir="$source_parent/vitaGL-$source_commit"
marker="$source_parent/.isaac-vitagl-recipe"

if [ ! -f "$marker" ]; then
    if [ -e "$source_parent" ]; then
        echo "unmarked vitaGL source directory already exists: $source_parent" >&2
        exit 4
    fi
    mkdir -p "$source_parent"
    tar -xf "$source_archive" -C "$source_parent"
    # This contract deliberately contains successive diffs for the same file.
    # git-apply handles those transactionally even when later hunks revisit an
    # earlier source range; GNU patch rejects that valid ordering as a backward
    # hunk and can leave a partially modified tree.
    (cd "$source_dir" && git apply --check "$contract_patch")
    (cd "$source_dir" && git apply "$contract_patch")
    printf '%s\n' "$recipe" > "$marker"
fi
if [ "$(sed -n '1p' "$marker")" != "$recipe" ]; then
    echo "vitaGL source recipe marker mismatch: $marker" >&2
    exit 4
fi
for evidence in \
    'isaac_vitagl_single_threaded_gc_enabled' \
    'isaac_vitagl_shared_render_targets_mode' \
    'Isaac vitaGL overlay pins SHARED_RENDERTARGETS=1'; do
    if ! grep -Fq "$evidence" "$source_dir/source/gxm.c"; then
        echo "vitaGL build-contract patch is incomplete: $evidence" >&2
        exit 5
    fi
done
for draw_fix_evidence in \
    'ptrs[0] = (void *)target_vbo->ptr + first * streams[0].stride;' \
    'ptrs[0] = (void *)target_vbo->ptr + lowest * streams[0].stride;' \
    'gpu_alloc_mapped_temp((highest - lowest) * streams[0].stride)'; do
    if ! grep -Fq "$draw_fix_evidence" \
            "$source_dir/source/custom_shaders.c"; then
        echo "vitaGL pin lost packed-VBO first/range fix $draw_first_fix_commit" >&2
        exit 5
    fi
done
for scene_evidence in \
    'isaac_vitagl_report_scene_failure' \
    '"scene_reset:display"' \
    '"scene_reset:framebuffer"'; do
    if ! grep -Fq "$scene_evidence" "$source_dir/source/gxm.c"; then
        echo "vitaGL scene-begin failure guard is incomplete: $scene_evidence" >&2
        exit 5
    fi
done
for known_color_evidence in \
    'isaac_vita_vitagl_known_color_present_index' \
    'isaac_vita_vitagl_known_color_probe' \
    'SCE_GXM_TRANSFER_FORMAT_U8U8U8U8_ABGR' \
    'sceGxmTransferFill(' \
    'sceGxmTransferFinish()'; do
    if ! grep -Fq "$known_color_evidence" "$source_dir/source/gxm.c"; then
        echo "vitaGL known-color source contract is incomplete: $known_color_evidence" >&2
        exit 5
    fi
done
for display_surface_evidence in \
    'gxm_color_surface_memblocks' \
    'sceKernelAllocMemBlock(' \
    'SCE_KERNEL_MEMBLOCK_TYPE_USER_CDRAM_RW' \
    'isaac_color_surface_dedicated' \
    'isaac_color_surface_init_results' \
    'isaac_color_surface_sync_results' \
    'isaac_vita_vitagl_display_surface_status' \
    'isaac_vita_vitagl_display_surface_lifecycle_failure' \
    'isaac_vitagl_release_display_color_surface' \
    'GLboolean may_free = GL_TRUE;' \
    'init-rollback:unmap-scanout' \
    'init-rollback:free-scanout' \
    'action=fail-closed' \
    'status_copy_count > ISAAC_VITAGL_DISPLAY_SURFACE_COUNT' \
    'int32_t queue_finish_result = sceGxmDisplayQueueFinish();' \
    'NULL, SCE_DISPLAY_SETBUF_NEXTFRAME' \
    'int32_t wait_result = sceDisplayWaitSetFrameBuf();' \
    'int32_t destroy_result =' \
    'int32_t unmap_result = sceGxmUnmapMemory(address);' \
    'int32_t free_result = sceKernelFreeMemBlock(uid);' \
    'for (marker_y = 16; marker_y < 32; marker_y++)'; do
    if ! grep -Fq "$display_surface_evidence" \
            "$source_dir/source/gxm.c"; then
        echo "vitaGL dedicated display transaction is incomplete: $display_surface_evidence" >&2
        exit 5
    fi
done
if [ "$(grep -c 'isaac_vitagl_report_scene_failure' \
             "$source_dir/source/gxm.c")" -ne 3 ]; then
    echo "vitaGL scene-begin failure guard census drifted" >&2
    exit 5
fi
for render_target_evidence in \
    'SCE_GXM_ERROR_OUT_OF_RENDER_TARGETS' \
    'isaac_vitagl_reserve_render_target_once' \
    'ISAAC_RESERVED_RT_WIDTH 1024' \
    'scene_reset:framebuffer:reserved' \
    'isaac_reserved_render_target = NULL;' \
    'SceGxmRenderTarget *target = NULL;' \
    'isaac_vitagl_count_pending_render_targets(event)' \
    'if (!pending_total)' \
    'isaac_vitagl_has_duplicate_pending_render_target()' \
    'event->finish_called = 1;' \
    'if (event->destroy_result)' \
    'event->retry_attempted = 1;' \
    'isaac_vitagl_publish_render_target' \
    '_Static_assert(FRAME_PURGE_FREQ == 4' \
    'isaac_vitagl_rearm_scene_after_failure'; do
    if ! grep -Fq "$render_target_evidence" "$source_dir/source/gxm.c"; then
        echo "vitaGL render-target recovery is incomplete: $render_target_evidence" >&2
        exit 5
    fi
done
if [ "$(grep -c 'isaac_vitagl_report_render_target_event' \
             "$source_dir/source/gxm.c")" -ne 6 ] ||
   [ "$(grep -c 'setup_render_target(&target' \
             "$source_dir/source/gxm.c")" -ne 3 ]; then
    echo "vitaGL transactional render-target recovery census drifted" >&2
    exit 5
fi
if ! grep -Fq 'fb->target = NULL;' \
        "$source_dir/source/framebuffers.c"; then
    echo "vitaGL framebuffer release lost its shared-target disarm" >&2
    exit 5
fi
for reserve_evidence in \
    'isaac_vitagl_report_uniform_reserve_failure' \
    '"update_scissor_test:mask"' \
    '"update_scissor_test:restore"'; do
    if ! grep -Fq "$reserve_evidence" "$source_dir/source/tests.c"; then
        echo "vitaGL reserve-failure guard is incomplete: $reserve_evidence" >&2
        exit 5
    fi
done
for reserve_evidence in \
    '"glClear:vertex"' \
    '"glClear:fragment"'; do
    if ! grep -Fq "$reserve_evidence" "$source_dir/source/misc.c"; then
        echo "vitaGL clear reserve-failure guard is incomplete: $reserve_evidence" >&2
        exit 5
    fi
done
if [ "$(grep -c 'sceGxmReserveVertexDefaultUniformBuffer' \
             "$source_dir/source/tests.c")" -ne 2 ] ||
   [ "$(grep -c 'isaac_vitagl_report_uniform_reserve_failure' \
             "$source_dir/source/tests.c")" -ne 2 ] ||
   [ "$(grep -c 'sceGxmReserve.*DefaultUniformBuffer' \
             "$source_dir/source/misc.c")" -ne 2 ] ||
   [ "$(grep -c 'isaac_vitagl_report_uniform_reserve_failure' \
             "$source_dir/source/misc.c")" -ne 2 ]; then
    echo "vitaGL live reserve/guard census drifted" >&2
    exit 5
fi

jobs=${ISAAC_VITAGL_JOBS:-}
if [ -z "$jobs" ]; then
    jobs=$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '1')
fi
debug_prefix="-fdebug-prefix-map=$source_dir=/usr/src/vitaGL-$source_commit"
file_prefix="-ffile-prefix-map=$source_dir=/usr/src/vitaGL-$source_commit"

# Deliberately build only the archive target.  Upstream's install target writes
# into VITASDK; this overlay must leave that archive and header untouched.
SOURCE_DATE_EPOCH=$source_epoch make --directory="$source_dir" \
    --jobs="$jobs" \
    VGL_GIT_HASH="$source_short" \
    EXTRA_CFLAGS="$debug_prefix $file_prefix" \
    SOFTFP_ABI=1 \
    NO_DEBUG=1 \
    NO_SPLASHSCREEN=1 \
    SINGLE_THREADED_GC=1 \
    SHARED_RENDERTARGETS=1 \
    ISAAC_DISPLAY_QUEUE_PROBE="$display_queue_probe" \
    ISAAC_KNOWN_COLOR_PROBE="$known_color_probe" \
    libvitaGL.a

built_library="$source_dir/libvitaGL.a"
built_header="$source_dir/source/vitaGL.h"
if [ ! -s "$built_library" ] || [ ! -s "$built_header" ]; then
    echo "pinned vitaGL build did not produce its archive/header" >&2
    exit 6
fi
cp -- "$built_library" "$prefix/lib/libvitaGL.a"
cp -- "$built_header" "$prefix/include/vitaGL.h"
printf '%s\n' "$recipe" > "$output_root/recipe.txt"
{
    printf 'source_commit=%s\n' "$source_commit"
    printf 'source_sha256=%s\n' "$source_sha"
    printf 'required_draw_first_fix_commit=%s\n' "$draw_first_fix_commit"
    printf 'patch_sha256=%s\n' "$patch_sha"
    printf 'flags=%s\n' "$build_flags"
    printf 'uniform_reserve_guard=%s\n' 'fail-closed-v1'
    printf 'scene_begin_guard=%s\n' 'fail-closed-v1'
    printf 'render_target_recovery=%s\n' 'out-of-render-targets-v1'
    printf 'render_target_early_reserve=%s\n' '1024x1024-scenes8-none-v1'
    printf 'render_target_scene_capacity=%s\n' 'display8-fbo8-v1'
    printf 'render_target_telemetry=%s\n' 'bounded-381-v1'
    printf 'display_surface_allocation=%s\n' 'dedicated-cdram-transaction-v2'
    printf 'display_surface_init_gate=%s\n' 'bounded-100-fail-closed-v1'
    printf 'display_surface_lifecycle_log=%s\n' 'action-fail-closed-v1'
    printf 'display_surface_resize_release=%s\n' \
        'queue-finish-null-nextframe-wait-retire-v2'
    printf 'display_queue_cpu_sentinel=%s\n' \
        "$display_queue_cpu_sentinel"
    printf 'display_queue_probe=%s\n' "$display_queue_probe"
    printf 'known_color_probe=%s\n' "$known_color_probe"
    if [ "$known_color_probe" -eq 1 ]; then
        printf 'known_color_contract=%s\n' \
            'p2-green-clear-transfer-fill-64-64-512-256-v1'
    else
        printf 'known_color_contract=%s\n' 'off'
    fi
} > "$output_root/build-contract.txt"
sha256sum "$prefix/lib/libvitaGL.a" > "$output_root/libvitaGL.sha256"
sha256sum "$prefix/include/vitaGL.h" > "$output_root/vitaGL.h.sha256"
echo "Pinned Isaac vitaGL overlay: $prefix/lib/libvitaGL.a"
