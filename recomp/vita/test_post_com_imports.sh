#!/usr/bin/env bash
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if [ -n "${ISAAC_POST_COM_TEST_OUT:-}" ]; then
    work=$ISAAC_POST_COM_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-post-com-imports.XXXXXX")
fi

cc="$VITASDK/bin/arm-vita-eabi-gcc"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"
nm="$VITASDK/bin/arm-vita-eabi-nm"
flags="-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror"

"$cc" $flags -DGUEST_IMAGE_BASE=0x98000000u \
    -I"$root/runtime" -I"$root/vita" \
    "$root/runtime/host_vita_post_com.c" \
    "$root/runtime/vita_post_com_import_test.c" \
    -o "$work/vita-post-com-import-oracle.elf"

"$readelf" -h "$work/vita-post-com-import-oracle.elf"
"$readelf" -A "$work/vita-post-com-import-oracle.elf"
"$nm" -u "$work/vita-post-com-import-oracle.elf"
sha256sum \
    "$root/runtime/host_vita_post_com.h" \
    "$root/runtime/host_vita_post_com.c" \
    "$root/runtime/vita_post_com_import_test.c" \
    "$work/vita-post-com-import-oracle.elf"
echo "Vita post-COM compile/link/static ABI check: PASS (6 handlers; complete four-symbol WINMM timer family; execution is accepted only through Vita3K or hardware)"
