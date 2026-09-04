#!/usr/bin/env bash
set -eu

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=${ISAAC_SETJMP_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
if [ -n "${ISAAC_SETJMP_TEST_OUT:-}" ]; then
    work=$ISAAC_SETJMP_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-setjmp-runtime.XXXXXX")
fi

cc="$VITASDK/bin/arm-vita-eabi-gcc"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"

common_flags="-std=gnu11 -O2 -DGUEST_STACK_REQUIRED=1 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror"

"$cc" $common_flags \
    -I"$root/runtime" \
    "$root/runtime/guest.c" \
    "$root/runtime/setjmp_runtime_oracle.c" \
    -Wl,--gc-sections \
    -lSceLibKernel_stub -lSceKernelThreadMgr_stub \
    -o "$work/vita-setjmp-runtime-oracle.elf"

"$readelf" -h "$work/vita-setjmp-runtime-oracle.elf"
"$readelf" -A "$work/vita-setjmp-runtime-oracle.elf"
sha256sum \
    "$root/runtime/guest.c" \
    "$root/runtime/guest.h" \
    "$root/runtime/setjmp_runtime_oracle.c" \
    "$work/vita-setjmp-runtime-oracle.elf"
echo "Vita setjmp runtime compile/link/static artifact check: PASS (same behavioural oracle executes on PC; target execution requires Vita3K or hardware)"
