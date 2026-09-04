#!/usr/bin/env bash
set -eu

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=${ISAAC_USER32_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
if [ -n "${ISAAC_USER32_TEST_OUT:-}" ]; then
    work=$ISAAC_USER32_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-user32-imports.XXXXXX")
fi

cc="$VITASDK/bin/arm-vita-eabi-gcc"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"
nm="$VITASDK/bin/arm-vita-eabi-nm"
flags="-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror"

"$cc" $flags -DGUEST_IMAGE_BASE=0x98000000u \
    -I"$root/runtime" -I"$root/vita" \
    -c "$root/runtime/host_vita_user32.c" \
    -o "$work/host_vita_user32.o"
"$cc" $flags -DGUEST_IMAGE_BASE=0x98000000u \
    -I"$root/runtime" -I"$root/vita" \
    -c "$root/runtime/vita_user32_import_test.c" \
    -o "$work/vita_user32_import_test.o"
"$cc" $flags -Wl,--gc-sections \
    "$work/host_vita_user32.o" \
    "$work/vita_user32_import_test.o" \
    -o "$work/vita-user32-import-oracle.elf"

"$readelf" -h "$work/vita-user32-import-oracle.elf"
"$readelf" -A "$work/vita-user32-import-oracle.elf" \
    > "$work/vita-user32-import-oracle.attributes.txt"
cat "$work/vita-user32-import-oracle.attributes.txt"
if grep -q 'Tag_ABI_VFP_args: VFP registers' \
        "$work/vita-user32-import-oracle.attributes.txt"; then
    echo "Vita USER32 oracle unexpectedly linked as hardfp" >&2
    exit 1
fi
"$nm" -u "$work/vita-user32-import-oracle.elf"
"$nm" -g --defined-only "$work/host_vita_user32.o"
sha256sum \
    "$root/runtime/host_vita_user32.h" \
    "$root/runtime/host_vita_user32.c" \
    "$root/runtime/vita_user32_import_test.c" \
    "$root/vita/test_user32_imports.sh" \
    "$work/host_vita_user32.o" \
    "$work/vita-user32-import-oracle.elf"
echo "Vita USER32 compile/link/static ABI check: PASS (6 handlers, 31 physical sites; bounded fullscreen windowless policy; source oracle built but not executed)"
