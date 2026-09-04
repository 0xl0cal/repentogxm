#!/usr/bin/env bash
set -eu

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=${ISAAC_EXCEPTION_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
if [ -n "${ISAAC_EXCEPTION_TEST_OUT:-}" ]; then
    work=$ISAAC_EXCEPTION_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-exception-imports.XXXXXX")
fi

cc="$VITASDK/bin/arm-vita-eabi-gcc"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"

common_flags="-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror"

"$cc" $common_flags -DGUEST_IMAGE_BASE=0x98000000u \
    -DGUEST_CHECKED_MEMORY \
    -I"$root/runtime" \
    "$root/runtime/host_vita_exception.c" \
    "$root/runtime/vita_exception_import_test.c" \
    -o "$work/vita-exception-import-oracle.elf"

"$readelf" -h "$work/vita-exception-import-oracle.elf"
"$readelf" -A "$work/vita-exception-import-oracle.elf"
sha256sum \
    "$root/runtime/host_vita_exception.h" \
    "$root/runtime/host_vita_exception.c" \
    "$root/runtime/vita_exception_import_test.c" \
    "$work/vita-exception-import-oracle.elf"
echo "Vita exception compile/link/static artifact check: PASS (source oracle built but not executed; runtime evidence requires Vita3K or hardware)"
