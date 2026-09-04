#!/usr/bin/env bash
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if [ -n "${ISAAC_MATH_TEST_OUT:-}" ]; then
    work=$ISAAC_MATH_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-math-imports.XXXXXX")
fi

cc="$VITASDK/bin/arm-vita-eabi-gcc"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"
nm="$VITASDK/bin/arm-vita-eabi-nm"
host_cc=${CC:-cc}
flags="-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror"

"$host_cc" -std=gnu11 -O2 -fno-builtin -fno-pie -no-pie \
    -fno-strict-aliasing \
    -ffunction-sections -fdata-sections -Wall -Wextra -Werror \
    -DGUEST_IMAGE_BASE=0x00400000u -I"$root/runtime" -I"$root/vita" \
    "$root/runtime/host_vita_math.c" \
    "$root/runtime/vita_math_import_test.c" -Wl,--gc-sections -lm \
    -o "$work/vita-math-import-host-oracle"
if command -v timeout >/dev/null 2>&1; then
    timeout 10s "$work/vita-math-import-host-oracle"
else
    "$work/vita-math-import-host-oracle"
fi

"$cc" $flags -DGUEST_IMAGE_BASE=0x98000000u \
    -I"$root/runtime" -I"$root/vita" \
    -c "$root/runtime/host_vita_math.c" \
    -o "$work/host_vita_math.o"
"$cc" $flags -DGUEST_IMAGE_BASE=0x98000000u \
    -I"$root/runtime" -I"$root/vita" \
    -c "$root/runtime/vita_math_import_test.c" \
    -o "$work/vita_math_import_test.o"
"$cc" $flags \
    "$work/host_vita_math.o" "$work/vita_math_import_test.o" -lm \
    -o "$work/vita-math-import-oracle.elf"

"$readelf" -h "$work/vita-math-import-oracle.elf"
"$readelf" -A "$work/vita-math-import-oracle.elf" \
    > "$work/vita-math-import-oracle.attributes.txt"
cat "$work/vita-math-import-oracle.attributes.txt"
if grep -q 'Tag_ABI_VFP_args: VFP registers' \
        "$work/vita-math-import-oracle.attributes.txt"; then
    echo "Vita math oracle unexpectedly linked as hardfp" >&2
    exit 1
fi
"$nm" -u "$work/vita-math-import-oracle.elf"
"$nm" -g --defined-only "$work/host_vita_math.o"
sha256sum \
    "$root/runtime/host_vita_math.h" \
    "$root/runtime/host_vita_math.c" \
    "$root/runtime/vita_math_import_test.c" \
    "$root/vita/test_math_imports.sh" \
    "$work/vita-math-import-host-oracle" \
    "$work/host_vita_math.o" \
    "$work/vita-math-import-oracle.elf"
echo "Vita math host behavior + softfp compile/link/static ABI check: PASS (21/21 handlers, all 593 PE sites; softfp ELF was not executed)"
