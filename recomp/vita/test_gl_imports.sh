#!/usr/bin/env bash
set -eu

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=${ISAAC_GL_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
if [ -n "${ISAAC_GL_TEST_OUT:-}" ]; then
    work=$ISAAC_GL_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-gl-imports.XXXXXX")
fi

cc="$VITASDK/bin/arm-vita-eabi-gcc"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"
nm="$VITASDK/bin/arm-vita-eabi-nm"
strings="$VITASDK/bin/arm-vita-eabi-strings"

common_flags="-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror"
production_flags="$common_flags -DGUEST_STACK_REQUIRED=1"
oracle_flags="$common_flags -Wno-error=maybe-uninitialized"

"$cc" $production_flags -DGUEST_IMAGE_BASE=0x98000000u \
    -I"$root/runtime" -I"$root/vita" \
    -c "$root/runtime/host_vita_first_fault.c" \
    -o "$work/host_vita_first_fault.o"

"$cc" $oracle_flags -DGUEST_IMAGE_BASE=0x98000000u \
    -I"$root/runtime" -I"$root/vita" \
    "$root/runtime/gl_bridge.c" \
    "$root/runtime/host_vita_gl.c" \
    "$root/runtime/guest_stack_legacy_oracle_stub.c" \
    "$root/runtime/vita_gl_import_test.c" \
    -o "$work/vita-gl-import-oracle.elf"

"$readelf" -h "$work/vita-gl-import-oracle.elf"
"$readelf" -A "$work/vita-gl-import-oracle.elf"
"$readelf" -A "$work/host_vita_first_fault.o"
"$nm" "$work/vita-gl-import-oracle.elf" > \
    "$work/vita-gl-import-oracle.symbols"
"$nm" -u "$work/host_vita_first_fault.o" > \
    "$work/host_vita_first_fault.undefined"
"$strings" "$work/vita-gl-import-oracle.elf" > \
    "$work/vita-gl-import-oracle.strings"

for symbol in \
    isaac_vita_gl_import \
    isaac_vita_gl_import_counted \
    isaac_vita_gl_dynamic \
    isaac_vita_gl_dynamic_counted \
    guest_gl_resolve \
    guest_gl_resolve_guest \
    guest_gl_resolve_provider \
    guest_gl_dispatch
do
    if ! grep -q "[[:space:]]$symbol$" \
        "$work/vita-gl-import-oracle.symbols"
    then
        echo "missing linked Vita GL resolver symbol: $symbol" >&2
        exit 3
    fi
done

for delegate in \
    isaac_vita_file_lock_import_counted \
    isaac_vita_filesystem_import_counted \
    isaac_vita_math_import_counted \
    isaac_vita_rtti_import_counted \
    isaac_vita_gl_import_counted \
    isaac_vita_user32_import_counted \
    isaac_vita_gl_dynamic_counted
do
    if ! grep -q "[[:space:]]$delegate$" \
        "$work/host_vita_first_fault.undefined"
    then
        echo "first-fault dispatcher lost Vita GL delegate: $delegate" >&2
        exit 4
    fi
done

for evidence in \
    "OPENGL32.dll!wglGetProcAddress" \
    "libGLESv1_CM.so.1" \
    "libGLESv2.so.2" \
    "libEGL.so.1" \
    "glCreateShader" \
    "typed Vita GL token was rejected by gl_bridge" \
    "unsupported GL backend symbol: glGetString"
do
    if ! grep -Fqx "$evidence" \
        "$work/vita-gl-import-oracle.strings"
    then
        echo "missing Vita GL policy evidence: $evidence" >&2
        exit 5
    fi
done

sha256sum \
    "$root/runtime/gl_bridge.h" \
    "$root/runtime/gl_bridge.c" \
    "$root/runtime/host_vita_gl.h" \
    "$root/runtime/host_vita_gl.c" \
    "$root/runtime/host_vita_first_fault.c" \
    "$root/runtime/vita_gl_import_test.c" \
    "$root/vita/test_gl_imports.sh" \
    "$work/host_vita_first_fault.o" \
    "$work/vita-gl-import-oracle.elf"
echo "Vita GL resolver compile/link/static ABI check: PASS (four epoxy module aliases share one token; bounded canonical-name provider helper preserves caller cleanup; unknown modules and foreign handles delegate mutation-free; typed dynamic dispatch counted before fault; execution only through Vita3K or hardware)"
