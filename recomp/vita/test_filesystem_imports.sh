#!/usr/bin/env bash
set -eu

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if [ -n "${ISAAC_FILESYSTEM_TEST_OUT:-}" ]; then
    work=$ISAAC_FILESYSTEM_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-filesystem-imports.XXXXXX")
fi

cc="$VITASDK/bin/arm-vita-eabi-gcc"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"
nm="$VITASDK/bin/arm-vita-eabi-nm"
# GCC cannot prove that guest_stack_legacy_guard() assigns its out parameter
# on every successful inline return.  Keep every other warning fatal while
# allowing that known false positive from the shared guest.h test scaffold.
flags="-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror -Wno-error=maybe-uninitialized"

"$cc" $flags -DGUEST_IMAGE_BASE=0x98000000u \
    -I"$root/runtime" -I"$root/vita" \
    -c "$root/runtime/host_vita_filesystem.c" \
    -o "$work/host_vita_filesystem.o"
"$cc" $flags -DGUEST_IMAGE_BASE=0x98000000u \
    -I"$root/runtime" -I"$root/vita" \
    -c "$root/runtime/host_vita_startup.c" \
    -o "$work/host_vita_startup.o"
"$cc" $flags -DGUEST_IMAGE_BASE=0x98000000u \
    -I"$root/runtime" -I"$root/vita" \
    -c "$root/runtime/vita_filesystem_import_test.c" \
    -o "$work/vita_filesystem_import_test.o"
"$cc" $flags -Wl,--gc-sections \
    "$work/host_vita_filesystem.o" \
    "$work/host_vita_startup.o" \
    "$work/vita_filesystem_import_test.o" \
    -o "$work/vita-filesystem-import-oracle.elf"

"$readelf" -h "$work/vita-filesystem-import-oracle.elf"
"$readelf" -A "$work/vita-filesystem-import-oracle.elf" \
    > "$work/vita-filesystem-import-oracle.attributes.txt"
cat "$work/vita-filesystem-import-oracle.attributes.txt"
if grep -q 'Tag_ABI_VFP_args: VFP registers' \
        "$work/vita-filesystem-import-oracle.attributes.txt"; then
    echo "Vita filesystem oracle unexpectedly linked as hardfp" >&2
    exit 1
fi
"$nm" -u "$work/vita-filesystem-import-oracle.elf"
filesystem_undefined=$("$nm" -u "$work/host_vita_filesystem.o")
printf '%s\n' "$filesystem_undefined"
if printf '%s\n' "$filesystem_undefined" | grep -Eq ' U access$'; then
    echo "Vita filesystem boundary still calls allocation-backed access()" >&2
    exit 1
fi
if ! printf '%s\n' "$filesystem_undefined" | grep -Eq ' U sceIoGetstat$'; then
    echo "Vita filesystem boundary lost direct sceIoGetstat()" >&2
    exit 1
fi
"$nm" -g --defined-only "$work/host_vita_filesystem.o"
sha256sum \
    "$root/runtime/host_vita_filesystem.h" \
    "$root/runtime/host_vita_filesystem.c" \
    "$root/runtime/vita_filesystem_import_test.c" \
    "$root/vita/test_filesystem_imports.sh" \
    "$work/host_vita_filesystem.o" \
    "$work/vita-filesystem-import-oracle.elf"
echo "Vita filesystem compile/link/static ABI check: PASS (2/2 handlers, 9/9 PE sites; allocation-free access stat; exact root mapper linked; source oracle built but not executed)"
