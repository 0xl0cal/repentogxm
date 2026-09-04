#!/usr/bin/env bash
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if [ -n "${ISAAC_FILE_LOCK_TEST_OUT:-}" ]; then
    work=$ISAAC_FILE_LOCK_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-file-lock-imports.XXXXXX")
fi

cc="$VITASDK/bin/arm-vita-eabi-gcc"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"
nm="$VITASDK/bin/arm-vita-eabi-nm"
flags="-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror"

"$cc" $flags -DGUEST_IMAGE_BASE=0x98000000u \
    -I"$root/runtime" -I"$root/vita" \
    "$root/runtime/host_vita_file_lock.c" \
    "$root/runtime/vita_file_lock_import_test.c" \
    -o "$work/vita-file-lock-import-oracle.elf"

"$readelf" -h "$work/vita-file-lock-import-oracle.elf"
"$readelf" -A "$work/vita-file-lock-import-oracle.elf"
"$nm" -u "$work/vita-file-lock-import-oracle.elf"
sha256sum \
    "$root/runtime/host_vita_file_lock.h" \
    "$root/runtime/host_vita_file_lock.c" \
    "$root/runtime/vita_file_lock_import_test.c" \
    "$work/vita-file-lock-import-oracle.elf"
echo "Vita file-lock compile/link/static ABI check: PASS (2 handlers; owned CRT fd validation only; execution is accepted only through Vita3K or hardware)"
