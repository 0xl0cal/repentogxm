#!/usr/bin/env bash
set -eu

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if [ -n "${ISAAC_RTTI_TEST_OUT:-}" ]; then
    work=$ISAAC_RTTI_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-rtti-imports.XXXXXX")
fi

cc="$VITASDK/bin/arm-vita-eabi-gcc"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"
nm="$VITASDK/bin/arm-vita-eabi-nm"
flags="-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror"

"$cc" $flags -DGUEST_IMAGE_BASE=0x98000000u -DGUEST_CHECKED_MEMORY \
    -I"$root/runtime" -I"$root/vita" \
    -c "$root/runtime/host_vita_rtti.c" \
    -o "$work/host_vita_rtti.o"
"$cc" $flags -DGUEST_IMAGE_BASE=0x98000000u -DGUEST_CHECKED_MEMORY \
    -I"$root/runtime" -I"$root/vita" \
    -c "$root/runtime/vita_rtti_import_test.c" \
    -o "$work/vita_rtti_import_test.o"
"$cc" $flags \
    "$work/host_vita_rtti.o" "$work/vita_rtti_import_test.o" \
    -o "$work/vita-rtti-import-oracle.elf"

"$readelf" -h "$work/vita-rtti-import-oracle.elf"
"$readelf" -A "$work/vita-rtti-import-oracle.elf" \
    > "$work/vita-rtti-import-oracle.attributes.txt"
cat "$work/vita-rtti-import-oracle.attributes.txt"
if grep -q 'Tag_ABI_VFP_args: VFP registers' \
        "$work/vita-rtti-import-oracle.attributes.txt"; then
    echo "Vita RTTI oracle unexpectedly linked as hardfp" >&2
    exit 1
fi
"$nm" -u "$work/vita-rtti-import-oracle.elf"
"$nm" -g --defined-only "$work/host_vita_rtti.o"
sha256sum \
    "$root/runtime/host_vita_rtti.h" \
    "$root/runtime/host_vita_rtti.c" \
    "$root/runtime/vita_rtti_import_test.c" \
    "$root/vita/test_rtti_imports.sh" \
    "$work/host_vita_rtti.o" \
    "$work/vita-rtti-import-oracle.elf"
echo "Vita RTTI compile/link/static ABI check: PASS (1 handler; 662-site frozen-PE census; cdecl5 SI/null/success/failure/name-fallback/reference-loud source oracle built but not executed)"
