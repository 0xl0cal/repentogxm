#!/usr/bin/env bash
set -eu

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=${ISAAC_SCREENSHOT_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
if [ -n "${ISAAC_SCREENSHOT_TEST_OUT:-}" ]; then
    work=$ISAAC_SCREENSHOT_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-vita-screenshot.XXXXXX")
fi

host_cc=${CC:-cc}
cc="$VITASDK/bin/arm-vita-eabi-gcc"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"
nm="$VITASDK/bin/arm-vita-eabi-nm"
objdump="$VITASDK/bin/arm-vita-eabi-objdump"
common_flags="-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror"
build_id=ssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssssss
build_define="-DISAAC_VITA_SCREENSHOT_BUILD_ID=\"$build_id\""

if [ "${#build_id}" -ne 96 ]; then
    echo "screenshot oracle build-id fixture is not 96 bytes" >&2
    exit 3
fi

for scenario in 0 1 2 3
do
    "$host_cc" -std=c11 -O2 -Wall -Wextra -Werror \
        -DISAAC_VITA_SCREENSHOT_ORACLE=1 \
        "-DISAAC_VITA_SCREENSHOT_ORACLE_SCENARIO=$scenario" \
        "$build_define" \
        -I"$root/runtime" \
        "$root/runtime/kage_vita_screenshot_probe.c" \
        "$root/runtime/kage_vita_screenshot_probe_oracle.c" \
        -o "$work/screenshot-host-oracle-$scenario"
    "$work/screenshot-host-oracle-$scenario"
done

"$cc" $common_flags \
    -DISAAC_VITA_SCREENSHOT_ORACLE=1 \
    -DISAAC_VITA_SCREENSHOT_ORACLE_SCENARIO=0 \
    "$build_define" \
    -I"$root/runtime" \
    "$root/runtime/kage_vita_screenshot_probe.c" \
    "$root/runtime/kage_vita_screenshot_probe_oracle.c" \
    -Wl,--gc-sections \
    -o "$work/screenshot-softfp-oracle.elf"

"$cc" $common_flags \
    -DGUEST_IMAGE_BASE=0x98000000u \
    -DGUEST_STACK_REQUIRED=1 \
    -DISAAC_VITA_HEAP_MB=81 \
    -DISAAC_VITA_HAS_RUNTIME=1 \
    -DISAAC_VITA_RAW_ALLOCATOR_GATE=1 \
    -DISAAC_VITA_SCREENSHOT_PROBE=1 \
    "$build_define" \
    -include "$root/vita/isaac_vita_raw_allocator_poison.h" \
    -I"$root/runtime" -I"$root/vita" \
    -c "$root/runtime/kage_vita_screenshot_probe.c" \
    -o "$work/kage_vita_screenshot_probe.production.o"

"$readelf" -h "$work/screenshot-softfp-oracle.elf" \
    > "$work/screenshot.header"
"$readelf" -A "$work/screenshot-softfp-oracle.elf" \
    > "$work/screenshot.attributes"
"$readelf" -A "$work/kage_vita_screenshot_probe.production.o" \
    > "$work/screenshot-production.attributes"
"$nm" -u "$work/kage_vita_screenshot_probe.production.o" \
    > "$work/screenshot-production.undefined"
"$objdump" -dr "$work/kage_vita_screenshot_probe.production.o" \
    > "$work/screenshot-production.disassembly"

cat "$work/screenshot.header"
cat "$work/screenshot.attributes"
cat "$work/screenshot-production.undefined"

if ! grep -q "soft-float ABI" "$work/screenshot.header"; then
    echo "Vita screenshot oracle is not marked soft-float ABI" >&2
    exit 4
fi
if grep -q "Tag_ABI_VFP_args" "$work/screenshot.attributes" ||
   grep -q "Tag_ABI_VFP_args" "$work/screenshot-production.attributes"; then
    echo "Vita screenshot probe unexpectedly advertises hardfp arguments" >&2
    exit 5
fi
for symbol in \
    isaac_vita_log sceScreenShotCapture sceSysmoduleLoadModule
do
    if ! grep -Eq "[[:space:]]U[[:space:]]+$symbol$" \
            "$work/screenshot-production.undefined"; then
        echo "production screenshot object lost dependency: $symbol" >&2
        exit 6
    fi
    if [ "$(grep -Ec "R_ARM_THM_CALL[[:space:]]+$symbol$" \
            "$work/screenshot-production.disassembly")" -ne 1 ]; then
        echo "production screenshot object does not call $symbol exactly once" >&2
        exit 7
    fi
done
if grep -Eq '[[:space:]]U[[:space:]]+(malloc|calloc|realloc|free|memalign|aligned_alloc|sceScreenShotEnable|sceSysmoduleUnloadModule|sceGxmDisplayQueueFinish)$' \
        "$work/screenshot-production.undefined"; then
    cat "$work/screenshot-production.undefined" >&2
    echo "screenshot probe retained a forbidden allocation/unload/queue edge" >&2
    exit 8
fi

python3 - "$root" <<'PY'
from pathlib import Path
import re
import sys

root = Path(sys.argv[1])
backend = (root / "runtime/kage_vita_backend.c").read_text(encoding="utf-8")
cmake = (root / "vita/CMakeLists.txt").read_text(encoding="utf-8")
probe = (root / "runtime/kage_vita_screenshot_probe.c").read_text(encoding="utf-8")
header = (root / "runtime/kage_vita_screenshot_probe.h").read_text(encoding="utf-8")

body_match = re.search(
    r"int kage_vita_backend_present\(void\)\s*\{(?P<body>.*?)\n\}",
    backend,
    re.S,
)
if body_match is None:
    raise SystemExit("cannot isolate kage_vita_backend_present")
body = body_match.group("body")
ordered = [
    body.find("vglSwapBuffers(GL_FALSE);"),
    body.find("s_present_count++;"),
    body.find("kage_vita_screenshot_probe_post_swap(s_present_count);"),
]
if any(position < 0 for position in ordered) or ordered != sorted(ordered):
    raise SystemExit(f"screenshot seam is not post-swap/post-count: {ordered}")
if backend.count("kage_vita_screenshot_probe_post_swap(s_present_count);") != 1:
    raise SystemExit("backend does not own exactly one screenshot call site")
if "#if defined(ISAAC_VITA_SCREENSHOT_PROBE)" not in backend:
    raise SystemExit("backend screenshot call is not compile-gated")

required_cmake = (
    'option(ISAAC_VITA_SCREENSHOT_PROBE',
    '"Capture once after vglSwapBuffers returns for post-loading present 600" OFF)',
    '"${ISAAC_RUNTIME}/kage_vita_screenshot_probe.c"',
    'ISAAC_VITA_SCREENSHOT_BUILD_ID=\\"${ISAAC_VITA_GUEST_LINK_ID}\\"',
    'SceScreenShot_stub_weak',
    'SceSysmodule_stub',
)
for needle in required_cmake:
    if needle not in cmake:
        raise SystemExit(f"production CMake lost screenshot contract: {needle}")
if re.search(r"(?m)^\s*SceScreenShot_stub\s*$", cmake):
    raise SystemExit("on-demand screenshot sysmodule gained a strong import")
if "target_compile_definitions(isaac_first_arm_fault PRIVATE\n    ISAAC_VITA_SCREENSHOT_PROBE" in cmake:
    raise SystemExit("screenshot selection leaked to the whole target")

if probe.count("sceScreenShotCapture(") != 1:
    raise SystemExit("probe does not make exactly one capture call")
for forbidden in ("sceSysmoduleUnloadModule", "sceGxmDisplayQueueFinish",
                  "sceScreenShotEnable", "vglSwapBuffers", "sceCtrl",
                  "sceClibPrintf"):
    if forbidden in probe:
        raise SystemExit(f"probe gained forbidden hot-path edge: {forbidden}")
for needle in (
    "SCE_SCREEN_SHOT_CAPTURE_MODE_FORCE_CAPTURE = 1",
    "sizeof(SceScreenShotCaptureMode) == 1u",
    "sizeof(SceScreenShotCaptureFileInfo) == 1024u",
    "offsetof(SceScreenShotCaptureFileInfo, path) == 0u",
):
    if needle not in header:
        raise SystemExit(f"local screenshot ABI lost assertion: {needle}")
PY

python3 "$root/vita/test_vita_raw_allocator_gate.py"

sha256sum \
    "$root/runtime/kage_vita_screenshot_probe.h" \
    "$root/runtime/kage_vita_screenshot_probe.c" \
    "$root/runtime/kage_vita_screenshot_probe_oracle.c" \
    "$root/runtime/kage_vita_backend.c" \
    "$root/vita/CMakeLists.txt" \
    "$root/vita/vita_raw_allocator_gate.py" \
    "$root/vita/test_vita_raw_allocator_gate.py" \
    "$root/vita/test_kage_vita_screenshot_probe.sh" \
    "$work/screenshot-softfp-oracle.elf" \
    "$work/kage_vita_screenshot_probe.production.o"
echo "Vita one-shot app-side screenshot: host fail-closed behavior + bounded durable log + local ABI + post-swap owner seam + source-scoped CMake + raw-allocator closure + ARM softfp compile/link: PASS"
