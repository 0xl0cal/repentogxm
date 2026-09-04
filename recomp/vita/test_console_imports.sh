#!/usr/bin/env bash
set -eu

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=${ISAAC_CONSOLE_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
work=${ISAAC_CONSOLE_TEST_OUT:-$(mktemp -d "${TMPDIR:-/tmp}/isaac-console-imports.XXXXXX")}
mkdir -p "$work"
cc="$VITASDK/bin/arm-vita-eabi-gcc"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"
flags="-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror"

"$cc" $flags -DGUEST_IMAGE_BASE=0x98000000u -I"$root/runtime" -I"$root/vita" \
    "$root/runtime/host_vita_console.c" \
    "$root/runtime/vita_console_import_test.c" \
    -o "$work/vita-console-import-oracle.elf"
"$readelf" -h "$work/vita-console-import-oracle.elf"
"$readelf" -A "$work/vita-console-import-oracle.elf"
sha256sum "$root/runtime/host_vita_console.h" \
    "$root/runtime/host_vita_console.c" \
    "$root/runtime/vita_console_import_test.c" \
    "$work/vita-console-import-oracle.elf"
echo "Vita console compile/link/static artifact check: PASS (source oracle built but not executed; runtime evidence requires Vita3K or hardware)"
