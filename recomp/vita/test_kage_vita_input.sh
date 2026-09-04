#!/usr/bin/env bash
set -eu

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=${ISAAC_INPUT_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
if [ -n "${ISAAC_INPUT_TEST_OUT:-}" ]; then
    work=$ISAAC_INPUT_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-kage-input.XXXXXX")
fi

host_cc=${CC:-cc}
cc="$VITASDK/bin/arm-vita-eabi-gcc"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"
nm="$VITASDK/bin/arm-vita-eabi-nm"
common_flags="-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror"

"$host_cc" -std=c11 -O2 -Wall -Wextra -Werror \
    -DISAAC_KAGE_VITA_INPUT_ORACLE=1 \
    -I"$root/runtime" \
    "$root/runtime/kage_vita_input.c" \
    "$root/runtime/kage_vita_input_oracle.c" \
    -o "$work/kage-vita-input-host-oracle"
if command -v timeout >/dev/null 2>&1; then
    timeout 10s "$work/kage-vita-input-host-oracle"
else
    "$work/kage-vita-input-host-oracle"
fi

"$cc" $common_flags -I"$root/runtime" \
    -c "$root/runtime/kage_vita_input.c" \
    -o "$work/kage_vita_input.production.o"
"$cc" $common_flags -I"$root/runtime" -I"$root/vita" \
    -c "$root/runtime/kage_vita_generated_hooks.c" \
    -o "$work/kage_vita_generated_hooks.production.o"
"$cc" $common_flags -I"$root/runtime" -I"$root/vita" \
    -c "$root/runtime/kage_vita_backend.c" \
    -o "$work/kage_vita_backend.production.o"
"$cc" $common_flags -I"$root/runtime" \
    "$root/runtime/kage_vita_input.c" \
    "$root/runtime/kage_vita_touch.c" \
    "$root/runtime/kage_vita_input_link_oracle.c" \
    -Wl,--gc-sections -lSceCtrl_stub -lSceTouch_stub -lSceLibKernel_stub \
    -o "$work/kage-vita-input-softfp-link.elf"

"$readelf" -h "$work/kage-vita-input-softfp-link.elf" \
    > "$work/kage-vita-input-softfp-link.header"
"$readelf" -A "$work/kage-vita-input-softfp-link.elf" \
    > "$work/kage-vita-input-softfp-link.attributes"
"$nm" -u "$work/kage-vita-input-softfp-link.elf" \
    > "$work/kage-vita-input-softfp-link.undefined"
cat "$work/kage-vita-input-softfp-link.header"
cat "$work/kage-vita-input-softfp-link.attributes"
if ! grep -q "soft-float ABI" "$work/kage-vita-input-softfp-link.header"; then
    echo "Vita input oracle is not marked soft-float ABI" >&2
    exit 3
fi
if grep -q "Tag_ABI_VFP_args" "$work/kage-vita-input-softfp-link.attributes"; then
    echo "Vita input oracle unexpectedly advertises hardfp arguments" >&2
    exit 4
fi
if [ -s "$work/kage-vita-input-softfp-link.undefined" ]; then
    cat "$work/kage-vita-input-softfp-link.undefined" >&2
    echo "Vita input oracle has unresolved symbols" >&2
    exit 5
fi

sha256sum \
    "$root/runtime/kage_vita_input.h" \
    "$root/runtime/kage_vita_input.c" \
    "$root/runtime/kage_vita_input_test_ctrl.h" \
    "$root/runtime/kage_vita_input_oracle.c" \
    "$root/runtime/kage_vita_input_link_oracle.c" \
    "$root/runtime/kage_vita_touch.h" \
    "$root/runtime/kage_vita_touch.c" \
    "$root/runtime/kage_vita_generated_hooks.c" \
    "$root/runtime/kage_vita_backend.c" \
    "$root/vita/CMakeLists.txt" \
    "$root/vita/test_kage_vita_input.sh" \
    "$work/kage-vita-input-host-oracle" \
    "$work/kage-vita-input-softfp-link.elf"
echo "Vita KAGE pad/touch input host behavior + softfp compile/link/static ABI check: PASS (softfp ELF was not executed)"
