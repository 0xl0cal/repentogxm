#!/usr/bin/env bash
set -eu

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=${ISAAC_TOUCH_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
if [ -n "${ISAAC_TOUCH_TEST_OUT:-}" ]; then
    work=$ISAAC_TOUCH_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-kage-touch.XXXXXX")
fi

host_cc=${CC:-cc}
cc="$VITASDK/bin/arm-vita-eabi-gcc"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"
nm="$VITASDK/bin/arm-vita-eabi-nm"
common_flags="-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror"

"$host_cc" -std=c11 -O2 -Wall -Wextra -Werror \
    -DISAAC_KAGE_VITA_TOUCH_ORACLE=1 \
    -I"$root/runtime" \
    "$root/runtime/kage_vita_touch.c" \
    "$root/runtime/kage_vita_touch_oracle.c" \
    -o "$work/kage-vita-touch-host-oracle"
"$work/kage-vita-touch-host-oracle"

"$cc" $common_flags -I"$root/runtime" \
    -c "$root/runtime/kage_vita_touch.c" \
    -o "$work/kage_vita_touch.production.o"
"$cc" $common_flags -I"$root/runtime" \
    "$root/runtime/kage_vita_touch.c" \
    "$root/runtime/kage_vita_touch_link_oracle.c" \
    -Wl,--gc-sections -lSceTouch_stub -lSceLibKernel_stub \
    -o "$work/kage-vita-touch-softfp-link.elf"

"$readelf" -h "$work/kage-vita-touch-softfp-link.elf" \
    > "$work/kage-vita-touch-softfp-link.header"
"$readelf" -A "$work/kage-vita-touch-softfp-link.elf" \
    > "$work/kage-vita-touch-softfp-link.attributes"
"$nm" "$work/kage_vita_touch.production.o" \
    > "$work/kage_vita_touch.production.symbols"
"$nm" -u "$work/kage_vita_touch.production.o" \
    > "$work/kage_vita_touch.production.undefined"
"$nm" -u "$work/kage-vita-touch-softfp-link.elf" \
    > "$work/kage-vita-touch-softfp-link.undefined"

cat "$work/kage-vita-touch-softfp-link.header"
cat "$work/kage-vita-touch-softfp-link.attributes"
if ! grep -q "soft-float ABI" "$work/kage-vita-touch-softfp-link.header"; then
    echo "Vita touch oracle is not marked soft-float ABI" >&2
    exit 3
fi
if grep -q "Tag_ABI_VFP_args" "$work/kage-vita-touch-softfp-link.attributes"; then
    echo "Vita touch oracle unexpectedly advertises hardfp arguments" >&2
    exit 4
fi
if [ -s "$work/kage-vita-touch-softfp-link.undefined" ]; then
    cat "$work/kage-vita-touch-softfp-link.undefined" >&2
    echo "Vita touch oracle has unresolved symbols" >&2
    exit 5
fi

for symbol in \
    kage_vita_touch_initialize \
    kage_vita_touch_deactivate \
    kage_vita_touch_sample \
    kage_vita_touch_snapshot_read
do
    if ! grep -q "[[:space:]]$symbol$" \
        "$work/kage_vita_touch.production.symbols"
    then
        echo "missing production Vita touch symbol: $symbol" >&2
        exit 6
    fi
done
for import in \
    sceTouchGetPanelInfo \
    sceTouchPeek \
    sceTouchSetSamplingState
do
    if ! grep -q "[[:space:]]$import$" \
        "$work/kage_vita_touch.production.undefined"
    then
        echo "production Vita touch object lost import: $import" >&2
        exit 7
    fi
done

if grep -q "SCE_TOUCH_PORT_BACK" "$root/runtime/kage_vita_touch.c"; then
    echo "production Vita touch backend references the rear panel" >&2
    exit 8
fi

sha256sum \
    "$root/runtime/kage_vita_touch.h" \
    "$root/runtime/kage_vita_touch.c" \
    "$root/runtime/kage_vita_touch_test_touch.h" \
    "$root/runtime/kage_vita_touch_oracle.c" \
    "$root/runtime/kage_vita_touch_link_oracle.c" \
    "$root/vita/test_kage_vita_touch.sh" \
    "$work/kage-vita-touch-host-oracle" \
    "$work/kage_vita_touch.production.o" \
    "$work/kage-vita-touch-softfp-link.elf"
echo "Vita front-touch active/map/pocket levels + softfp compile/link/static ABI check: PASS (softfp ELF was not executed)"
