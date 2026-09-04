#!/usr/bin/env bash
set -eu

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=${ISAAC_FLS_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
if [ -n "${ISAAC_FLS_TEST_OUT:-}" ]; then
    work=$ISAAC_FLS_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-fls-imports.XXXXXX")
fi

cc="$VITASDK/bin/arm-vita-eabi-gcc"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"

common_flags="-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror"

"$cc" $common_flags -DGUEST_IMAGE_BASE=0x98000000u \
    -I"$root/runtime" \
    "$root/runtime/host_vita_fls.c" \
    "$root/runtime/vita_fls_import_test.c" \
    -o "$work/vita-fls-import-oracle.elf"

"$readelf" -h "$work/vita-fls-import-oracle.elf"
"$readelf" -A "$work/vita-fls-import-oracle.elf"
sha256sum \
    "$root/runtime/host_vita_fls.h" \
    "$root/runtime/host_vita_fls.c" \
    "$root/runtime/vita_fls_import_test.c" \
    "$work/vita-fls-import-oracle.elf"
echo "Vita KERNEL32 FLS compile/link/static artifact check: PASS (source oracle built but not executed; runtime evidence requires Vita3K or hardware)"
