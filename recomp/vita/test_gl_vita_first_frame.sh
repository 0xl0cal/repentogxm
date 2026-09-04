#!/usr/bin/env bash
set -eu

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=${ISAAC_FIRST_FRAME_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
work=${ISAAC_FIRST_FRAME_TEST_OUT:-$(mktemp -d "${TMPDIR:-/tmp}/isaac-first-frame.XXXXXX")}
vitagl_inc=${ISAAC_FIRST_FRAME_VITAGL_INCLUDE:-$VITASDK/arm-vita-eabi/include}
generated_inc=${ISAAC_FIRST_FRAME_GENERATED_INCLUDE:-}
mkdir -p "$work"

cc="$VITASDK/bin/arm-vita-eabi-gcc"
nm="$VITASDK/bin/arm-vita-eabi-nm"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"
strings="$VITASDK/bin/arm-vita-eabi-strings"
objdump="$VITASDK/bin/arm-vita-eabi-objdump"
flags="-std=gnu11 -O2 -Wall -Wextra -Werror -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -ffunction-sections -fdata-sections -DGUEST_STACK_REQUIRED=1 -DGUEST_IMAGE_BASE=0x98000000u -I$root/runtime -I$root/vita -I$vitagl_inc"
on_flags="$flags -DISAAC_VITA_FIRST_FRAME_PROBE=1 -DISAAC_VITA_FIRST_FRAME_BUILD_ID=\"oracle:first-frame\""
direct_flags="$on_flags -DISAAC_VITA_DIRECT_DEFAULT=1"

for source in gl_vita_backend kage_vita_backend kage_vita_generated_hooks
do
    "$cc" $on_flags -c "$root/runtime/$source.c" -o "$work/$source.on.o"
    "$cc" $flags -c "$root/runtime/$source.c" -o "$work/$source.off.o"
done
for source in gl_vita_backend kage_vita_generated_hooks
do
    "$cc" $direct_flags -c "$root/runtime/$source.c" \
        -o "$work/$source.direct.o"
done
on_objects="$work/gl_vita_backend.on.o $work/kage_vita_backend.on.o $work/kage_vita_generated_hooks.on.o"
direct_objects="$work/gl_vita_backend.direct.o $work/kage_vita_backend.on.o $work/kage_vita_generated_hooks.direct.o"
off_objects="$work/gl_vita_backend.off.o $work/kage_vita_backend.off.o $work/kage_vita_generated_hooks.off.o"
if [ -n "$generated_inc" ]; then
    "$cc" $on_flags -I"$generated_inc" \
        -c "$root/runtime/manual_kage_vita.c" \
        -o "$work/manual_kage_vita.on.o"
    "$cc" $flags -I"$generated_inc" \
        -c "$root/runtime/manual_kage_vita.c" \
        -o "$work/manual_kage_vita.off.o"
    on_objects="$on_objects $work/manual_kage_vita.on.o"
    direct_objects="$direct_objects $work/manual_kage_vita.on.o"
    off_objects="$off_objects $work/manual_kage_vita.off.o"
fi
"$cc" -nostdlib -r $on_objects \
    -o "$work/first-frame-softfp-linked.o"
"$cc" -nostdlib -r $direct_objects \
    -o "$work/direct-default-softfp-linked.o"

"$readelf" -h "$work/first-frame-softfp-linked.o" > "$work/header"
"$readelf" -A "$work/first-frame-softfp-linked.o" > "$work/attributes"
"$nm" -u "$work/kage_vita_backend.on.o" > "$work/kage-on.undefined"
"$nm" "$work/gl_vita_backend.on.o" > "$work/gl-on.symbols"
"$objdump" -dr -j .text.isaac_vitagl_display_queue_probe \
    "$work/gl_vita_backend.on.o" > "$work/queue-hook.dis"
"$nm" "$work/kage_vita_generated_hooks.on.o" > "$work/hooks-on.symbols"
"$nm" $off_objects > "$work/off.symbols"
"$strings" $off_objects > "$work/off.strings"
"$strings" "$work/gl_vita_backend.direct.o" \
    "$work/kage_vita_generated_hooks.direct.o" > "$work/direct.strings"
cat "$work/header"
cat "$work/attributes"

if grep -q "Tag_ABI_VFP_args" "$work/attributes"; then
    echo "first-frame object unexpectedly advertises hardfp arguments" >&2
    exit 3
fi
if grep -Eqi 'first_frame|postprocess_bypass|display_queue_probe' \
        "$work/off.symbols" ||
   grep -Fq 'KAGE VITA FIRST FRAME' "$work/off.strings"; then
    echo "default-OFF objects retained first-frame probe edges" >&2
    exit 4
fi
grep -Fq 'phase=true-direct-default-enter' "$work/direct.strings"
grep -Fq 'mode=true-direct-default probes=passive' "$work/direct.strings"
if grep -Eq 'phase=(control-clear|explicit-blit|postprocess-bypass)' \
        "$work/direct.strings"; then
    echo "direct-default objects retained an active diagnostic phase" >&2
    exit 8
fi
grep -Eq '[[:space:]]U[[:space:]]+gl_vita_backend_first_frame_before_present$' \
    "$work/kage-on.undefined"
if grep -E 'R_ARM_(THM_)?CALL' "$work/queue-hook.dis" | \
        grep -Ev '[[:space:]]memcpy$'; then
    echo "display-queue callback hook gained a non-copy call" >&2
    exit 6
fi
if "$nm" -u "$work/gl_vita_backend.on.o" | grep -Eq '__atomic|__sync'; then
    echo "display-queue callback hook introduced an atomic runtime edge" >&2
    exit 6
fi
if [ -n "$generated_inc" ]; then
    "$nm" -u "$work/manual_kage_vita.on.o" | grep -Eq \
        '[[:space:]]U[[:space:]]+gl_vita_backend_first_frame_set_manager_framebuffer$'
fi
for symbol in \
    gl_vita_backend_first_frame_before_present \
    gl_vita_backend_first_frame_queue_begin \
    gl_vita_backend_first_frame_queue_end \
    gl_vita_backend_first_frame_set_manager_framebuffer \
    gl_vita_backend_first_frame_snapshot \
    isaac_vitagl_display_queue_probe
do
    grep -Eq "[[:space:]][Tt][[:space:]]+$symbol$" "$work/gl-on.symbols"
done
"$strings" "$work/kage_vita_generated_hooks.on.o" | \
    grep -Fq 'phase=postprocess-bypass'
grep -Fq 'isaac_vita_first_frame_postprocess_bypass_active(present)' \
    "$root/runtime/kage_vita_generated_hooks.c"
grep -Fq 'isaac_vita_first_frame_postprocess_bypass_complete(present_count)' \
    "$root/runtime/kage_vita_generated_hooks.c"
grep -Fq 'isaac_vita_first_frame_postprocess_restored(present_count)' \
    "$root/runtime/kage_vita_generated_hooks.c"
grep -Fq 'case 360u: return 4;' "$root/runtime/gl_vita_backend.c"
for lineage_boundary in \
    'case 2u: return 0;' \
    'case 3u: return 1;' \
    'case 4u: return 2;'; do
    grep -Fq "$lineage_boundary" "$root/runtime/gl_vita_backend.c"
done
grep -Fq 'GL_VITA_FF_QUEUE_STOP_PRESENT  8u' \
    "$root/runtime/gl_vita_backend.c"
grep -Fq 'sizeof(IsaacVitaGlDisplayQueueProbe) == 96u' \
    "$root/runtime/gl_vita_backend.h"
grep -Fq 'phase=queue-scene' "$root/runtime/gl_vita_backend.c"
grep -Fq 'expected=0x3f' "$root/runtime/gl_vita_backend.c"
grep -Fq 'GL_VITA_FF_LOGICAL_WIDTH / 2' \
    "$root/runtime/gl_vita_backend.c"
grep -Fq 'GL_VITA_FF_DISPLAY_HEIGHT / 2' \
    "$root/runtime/gl_vita_backend.c"
grep -Fq '(GUEST_IMAGE_BASE + 0x007fd680u)' \
    "$root/runtime/kage_vita_generated_hooks.c"
grep -Fq 'KAGE_VITA_GAME_POINTER_SLOT == 0x987fd680u' \
    "$root/runtime/kage_vita_generated_hooks.c"
test "$(grep -Fc 'st8(' "$root/runtime/kage_vita_generated_hooks.c")" -eq 2

cmake="$root/vita/CMakeLists.txt"
python3 - "$cmake" <<'PY'
import pathlib
import re
import sys

text = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
options = re.findall(
    r"option\(ISAAC_VITA_FIRST_FRAME_PROBE\s+.*?\s+(ON|OFF)\)",
    text,
    re.DOTALL,
)
if options != ["OFF"]:
    raise SystemExit("first-frame diagnostic must default exactly OFF")
if not re.search(
    r"if\(ISAAC_VITA_FIRST_FRAME_PROBE AND\s+"
    r"\(ISAAC_VITA_SCAFFOLD_ONLY OR NOT ISAAC_VITA_KAGE\)\).*?"
    r"message\(FATAL_ERROR",
    text,
    re.DOTALL,
):
    raise SystemExit("first-frame diagnostic lost its KAGE dependency check")
lines = text.splitlines()
markers = [
    i for i, line in enumerate(lines)
    if re.fullmatch(r"\s*ISAAC_VITA_FIRST_FRAME_PROBE=1\)", line)
]
if len(markers) != 1:
    raise SystemExit(f"expected one first-frame source definition: {markers}")
starts = [
    i for i in range(markers[0], -1, -1)
    if "set_property(SOURCE" in lines[i]
]
block = "\n".join(lines[starts[0]:markers[0] + 1])
owners = re.findall(r'"\$\{ISAAC_RUNTIME\}/([^\"]+)"', block)
expected = [
    "gl_vita_backend.c",
    "kage_vita_backend.c",
    "kage_vita_generated_hooks.c",
    "manual_kage_vita.c",
]
if owners != expected or "ISAAC_GENERATED_C" in block:
    raise SystemExit(f"first-frame diagnostic escaped exact owners: {owners}")
direct_options = re.findall(
    r"option\(ISAAC_VITA_DIRECT_DEFAULT\s+.*?\s+(ON|OFF)\)",
    text,
    re.DOTALL,
)
if direct_options != ["OFF"]:
    raise SystemExit("direct-default path must default exactly OFF")
direct_markers = [
    i for i, line in enumerate(lines)
    if re.fullmatch(r"\s*ISAAC_VITA_DIRECT_DEFAULT=1\)", line)
]
if len(direct_markers) != 1:
    raise SystemExit(
        f"expected one direct-default source definition: {direct_markers}"
    )
direct_starts = [
    i for i in range(direct_markers[0], -1, -1)
    if "set_property(SOURCE" in lines[i]
]
direct_block = "\n".join(
    lines[direct_starts[0]:direct_markers[0] + 1]
)
direct_owners = re.findall(
    r'"\$\{ISAAC_RUNTIME\}/([^\"]+)"', direct_block
)
if direct_owners != ["gl_vita_backend.c", "kage_vita_generated_hooks.c"]:
    raise SystemExit(
        f"direct-default policy escaped exact native owners: {direct_owners}"
    )
if "ISAAC_GENERATED_C" in direct_block:
    raise SystemExit("direct-default macro leaked into generated sources")
if text.count(
    '"ISAAC_DISPLAY_QUEUE_PROBE=${ISAAC_VITA_DISPLAY_QUEUE_PROBE}"'
) != 1:
    raise SystemExit("vitaGL overlay lost exact queue-probe configure mode")
if "isaac-vitagl-display-queue-probe.mode" not in text or text.count(
    '"${ISAAC_VITA_DISPLAY_QUEUE_PROBE_MODE_FILE}"'
) < 2:
    raise SystemExit("vitaGL overlay mode no longer invalidates its stamp")
print("first-frame CMake default/dependency/source/direct/overlay-mode: PASS")
PY
grep -Fq 'option(ISAAC_VITA_FIRST_FRAME_PROBE' "$cmake"
test "$(grep -Fc 'ISAAC_VITA_FIRST_FRAME_PROBE=1' "$cmake")" -eq 1
grep -Fq '"${ISAAC_RUNTIME}/gl_vita_backend.c"' "$cmake"
grep -Fq '"${ISAAC_RUNTIME}/kage_vita_backend.c"' "$cmake"
grep -Fq '"${ISAAC_RUNTIME}/kage_vita_generated_hooks.c"' "$cmake"
grep -Fq '"${ISAAC_RUNTIME}/manual_kage_vita.c"' "$cmake"
grep -Fq '"ISAAC_DISPLAY_QUEUE_PROBE=${ISAAC_VITA_DISPLAY_QUEUE_PROBE}"' \
    "$cmake"
grep -Fq '"${ISAAC_VITA_VITAGL_OVERLAY_DIR}/overlay-ready.stamp"' "$cmake"
grep -Fq 'isaac-vitagl-display-queue-probe.mode' "$cmake"
grep -Fq '"${ISAAC_VITA_DISPLAY_QUEUE_PROBE_MODE_FILE}"' "$cmake"
if grep -F 'ISAAC_VITA_FIRST_FRAME_PROBE' "$cmake" | grep -Fq 'ISAAC_GENERATED_C'; then
    echo "first-frame probe leaked into generated translation units" >&2
    exit 5
fi

overlay_patch="$root/vita/vitagl-overlay/0001-isaac-build-contract.patch"
overlay_build="$root/vita/vitagl-overlay/build.sh"
grep -Fq 'ISAAC_VITAGL_DISPLAY_QUEUE_PROBE_DEFINED' "$overlay_patch"
grep -Fq '{ 480, 271 }' "$overlay_patch"
for lineage_evidence in \
    'isaac_vitagl_note_display_begin' \
    'isaac_vitagl_note_display_end' \
    'sceGxmColorSurfaceGetData' \
    'back_memblock_uid' \
    'back_get_base_result' \
    'back_map_result' \
    'back_dedicated' \
    'IsaacVitaGlDisplaySurfaceStatus' \
    'isaac_vita_vitagl_display_surface_status' \
    'isaac_vita_vitagl_display_surface_lifecycle_failure' \
    'GLboolean may_free = GL_TRUE;' \
    'init-rollback:unmap-scanout' \
    'init-rollback:free-scanout' \
    'action=fail-closed' \
    'status_copy_count > ISAAC_VITAGL_DISPLAY_SURFACE_COUNT' \
    'int32_t wait_result = sceDisplayWaitSetFrameBuf();' \
    'isaac_vitagl_prepare_display_queue_callback' \
    'for (marker_y = 16; marker_y < 32; marker_y++)' \
    'display_result = sceDisplaySetFrameBuf' \
    'sizeof(IsaacVitaGlDisplayQueueProbe) == 96u'; do
    grep -Fq "$lineage_evidence" "$overlay_patch"
done
grep -Fq 'action=fail-closed' "$root/runtime/gl_vita_backend.c"
for reserve_evidence in \
    '#define KAGE_VITA_CDRAM_THRESHOLD 0x00800000' \
    '#define KAGE_VITA_CDIALOG_THRESHOLD 0x008c6000' \
    'gl_vita_backend_get_display_surface_status(&display_status)' \
    'vglInitWithCustomThreshold('; do
    grep -Fq "$reserve_evidence" "$root/runtime/kage_vita_backend.c"
done
grep -Fq 'display_queue_probe=${ISAAC_DISPLAY_QUEUE_PROBE:-0}' \
    "$overlay_build"
grep -Fq 'ISAAC_DISPLAY_QUEUE_PROBE=$display_queue_probe' "$overlay_build"
grep -Fq 'ISAAC_DISPLAY_QUEUE_PROBE="$display_queue_probe"' "$overlay_build"
grep -Fq "printf 'display_queue_probe=%s\\n'" "$overlay_build"
if ISAAC_DISPLAY_QUEUE_PROBE=invalid bash "$overlay_build" \
        "$work/invalid-overlay-mode" > "$work/invalid-mode.out" 2>&1; then
    echo "vitaGL overlay accepted an invalid display-queue mode" >&2
    exit 7
fi
grep -Fq 'ISAAC_DISPLAY_QUEUE_PROBE must be 0 or 1' \
    "$work/invalid-mode.out"

sha256sum \
    "$root/runtime/gl_vita_backend.c" \
    "$root/runtime/gl_vita_backend.h" \
    "$root/runtime/kage_vita_backend.c" \
    "$root/runtime/kage_vita_generated_hooks.c" \
    "$root/runtime/manual_kage_vita.c" \
    "$root/vita/test_gl_vita_first_frame.sh" \
    "$work/first-frame-softfp-linked.o" \
    "$work/direct-default-softfp-linked.o"
echo "first-frame softfp ON/OFF owner gate + hook pin/link: PASS (ARM object was not executed)"
