#!/usr/bin/env bash
set -eu

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=${ISAAC_XINPUT_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
if [ -n "${ISAAC_XINPUT_TEST_OUT:-}" ]; then
    work=$ISAAC_XINPUT_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-vita-xinput.XXXXXX")
fi

host_cc=${CC:-cc}
cc="$VITASDK/bin/arm-vita-eabi-gcc"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"
nm="$VITASDK/bin/arm-vita-eabi-nm"
strings="$VITASDK/bin/arm-vita-eabi-strings"
common_flags="-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror -Wno-maybe-uninitialized"
xinput_binding_status=SKIP

if [ -n "${ISAAC_FROZEN_PE:-}" ]; then
    python3 "$root/test_vita_xinput_bindings.py" --pe "$ISAAC_FROZEN_PE"
    xinput_binding_status=PASS
else
    echo "Frozen Vita XInput binding contract: SKIP (ISAAC_FROZEN_PE unset)"
fi

"$host_cc" -std=c11 -O2 -Wall -Wextra -Werror \
    -Wno-maybe-uninitialized \
    -DISAAC_VITA_XINPUT_ORACLE=1 \
    -I"$root/runtime" \
    "$root/runtime/host_vita_xinput.c" \
    "$root/runtime/vita_xinput_mapping_oracle.c" \
    "$root/runtime/guest_stack_legacy_oracle_stub.c" \
    -o "$work/vita-xinput-mapping-host-oracle"
"$work/vita-xinput-mapping-host-oracle"

"$host_cc" -std=c11 -O2 -Wall -Wextra -Werror \
    -DISAAC_KAGE_VITA_INPUT_ORACLE=1 \
    -I"$root/runtime" \
    "$root/runtime/kage_vita_input.c" \
    "$root/runtime/kage_vita_input_oracle.c" \
    -o "$work/kage-vita-input-cache-host-oracle"
"$work/kage-vita-input-cache-host-oracle"

"$cc" $common_flags -DGUEST_IMAGE_BASE=0x98000000u \
    -I"$root/runtime" -I"$root/vita" \
    -c "$root/runtime/host_vita_xinput.c" \
    -o "$work/host_vita_xinput.production.o"
"$cc" $common_flags -DGUEST_IMAGE_BASE=0x98000000u \
    -I"$root/runtime" \
    -c "$root/runtime/kage_vita_input.c" \
    -o "$work/kage_vita_input.production.o"
"$cc" $common_flags -DGUEST_IMAGE_BASE=0x98000000u \
    -I"$root/runtime" -I"$root/vita" \
    -c "$root/runtime/host_vita_first_fault.c" \
    -o "$work/host_vita_first_fault.xinput-off.o"
"$cc" $common_flags -DGUEST_IMAGE_BASE=0x98000000u \
    -DISAAC_VITA_XINPUT=1 \
    -I"$root/runtime" -I"$root/vita" \
    -c "$root/runtime/host_vita_first_fault.c" \
    -o "$work/host_vita_first_fault.xinput-on.o"
"$cc" $common_flags -DGUEST_IMAGE_BASE=0x98000000u \
    -I"$root/runtime" -I"$root/vita" \
    "$root/runtime/host_vita_xinput.c" \
    "$root/runtime/kage_vita_input.c" \
    "$root/runtime/kage_vita_touch.c" \
    "$root/runtime/vita_xinput_import_test.c" \
    "$root/runtime/guest_stack_legacy_oracle_stub.c" \
    -Wl,--gc-sections -lSceCtrl_stub -lSceTouch_stub -lSceLibKernel_stub \
    -o "$work/vita-xinput-import-oracle.elf"

"$readelf" -h "$work/vita-xinput-import-oracle.elf" \
    > "$work/vita-xinput-import-oracle.header"
"$readelf" -A "$work/vita-xinput-import-oracle.elf" \
    > "$work/vita-xinput-import-oracle.attributes"
"$nm" "$work/vita-xinput-import-oracle.elf" \
    > "$work/vita-xinput-import-oracle.symbols"
"$nm" "$work/host_vita_xinput.production.o" \
    > "$work/host_vita_xinput.production.symbols"
"$nm" "$work/kage_vita_input.production.o" \
    > "$work/kage_vita_input.production.symbols"
"$nm" -u "$work/host_vita_first_fault.xinput-off.o" \
    > "$work/host_vita_first_fault.xinput-off.undefined"
"$nm" -u "$work/host_vita_first_fault.xinput-on.o" \
    > "$work/host_vita_first_fault.xinput-on.undefined"
"$nm" -u "$work/vita-xinput-import-oracle.elf" \
    > "$work/vita-xinput-import-oracle.undefined"
"$strings" "$work/vita-xinput-import-oracle.elf" \
    > "$work/vita-xinput-import-oracle.strings"

cat "$work/vita-xinput-import-oracle.header"
cat "$work/vita-xinput-import-oracle.attributes"
if ! grep -q "soft-float ABI" "$work/vita-xinput-import-oracle.header"; then
    echo "Vita XInput oracle is not marked soft-float ABI" >&2
    exit 3
fi
if grep -q "Tag_ABI_VFP_args" "$work/vita-xinput-import-oracle.attributes"; then
    echo "Vita XInput oracle unexpectedly advertises hardfp arguments" >&2
    exit 4
fi
if [ -s "$work/vita-xinput-import-oracle.undefined" ]; then
    cat "$work/vita-xinput-import-oracle.undefined" >&2
    echo "Vita XInput oracle has unresolved symbols" >&2
    exit 5
fi

for symbol in \
    isaac_vita_xinput_module_for_name \
    isaac_vita_xinput_token_for_name \
    isaac_vita_xinput_token_for_ordinal \
    isaac_vita_xinput_axis \
    isaac_vita_xinput_build_state \
    isaac_vita_xinput_build_capabilities \
    isaac_vita_xinput_import \
    isaac_vita_xinput_import_counted \
    isaac_vita_xinput_dynamic \
    isaac_vita_xinput_dynamic_counted
do
    if ! grep -q "[[:space:]]$symbol$" \
        "$work/host_vita_xinput.production.symbols"
    then
        echo "missing production Vita XInput symbol: $symbol" >&2
        exit 6
    fi
done
for delegate in \
    isaac_vita_xinput_import_counted \
    isaac_vita_xinput_dynamic_counted
do
    if grep -q "[[:space:]]$delegate$" \
        "$work/host_vita_first_fault.xinput-off.undefined"
    then
        echo "non-KAGE dispatcher unexpectedly retained XInput: $delegate" >&2
        exit 6
    fi
    if ! grep -q "[[:space:]]$delegate$" \
        "$work/host_vita_first_fault.xinput-on.undefined"
    then
        echo "KAGE dispatcher lost XInput delegate: $delegate" >&2
        exit 6
    fi
done

python3 - "$root/vita/CMakeLists.txt" <<'PY'
import pathlib
import sys

text = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
begin = text.index("  set(ISAAC_VITA_RUNTIME_SOURCES")
end = text.index("  foreach(ISAAC_SOURCE", begin)
block = text[begin:end]
kage = block.index("  if(ISAAC_VITA_KAGE)")
off = block.index("  else()", kage)
source = block.index('"${ISAAC_RUNTIME}/host_vita_xinput.c"')
touch = block.index('"${ISAAC_RUNTIME}/kage_vita_touch.c"')
switch = block.index("APPEND PROPERTY COMPILE_DEFINITIONS ISAAC_VITA_XINPUT=1")
if not (kage < source < off and kage < touch < off and kage < switch < off):
    raise SystemExit("XInput source/dispatcher switch escaped KAGE-only CMake block")
if block.count('"${ISAAC_RUNTIME}/host_vita_xinput.c"') != 1:
    raise SystemExit("XInput CMake source count changed")
if block.count('"${ISAAC_RUNTIME}/kage_vita_touch.c"') != 1:
    raise SystemExit("touch CMake source count changed")
PY
for symbol in \
    kage_vita_input_gamepad_snapshot \
    kage_vita_input_set_xinput_active
do
    if ! grep -q "[[:space:]]$symbol$" \
        "$work/kage_vita_input.production.symbols"
    then
        echo "missing production Vita input-cache symbol: $symbol" >&2
        exit 6
    fi
done
for symbol in \
    isaac_vita_xinput_import_counted \
    isaac_vita_xinput_dynamic_counted \
    kage_vita_input_gamepad_snapshot \
    kage_vita_input_set_xinput_active
do
    if ! grep -q "[[:space:]]$symbol$" \
        "$work/vita-xinput-import-oracle.symbols"
    then
        echo "missing linked Vita XInput path symbol: $symbol" >&2
        exit 6
    fi
done

for evidence in \
    "XInput1_4.dll" \
    "XInput1_3.dll" \
    "bin\\XInput1_3.dll" \
    "XInputGetState" \
    "XInputGetCapabilities" \
    "XInputGetState received a null output" \
    "XInputGetCapabilities flags outside frozen ABI" \
    "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
do
    if ! grep -Fq "$evidence" \
        "$work/vita-xinput-import-oracle.strings"
    then
        echo "missing Vita XInput policy evidence: $evidence" >&2
        exit 7
    fi
done

sha256sum \
    "$root/runtime/kage_vita_input.h" \
    "$root/runtime/kage_vita_input.c" \
    "$root/runtime/kage_vita_input_test_ctrl.h" \
    "$root/runtime/kage_vita_input_oracle.c" \
    "$root/runtime/kage_vita_touch.h" \
    "$root/runtime/kage_vita_touch.c" \
    "$root/runtime/host_vita_xinput.h" \
    "$root/runtime/host_vita_xinput.c" \
    "$root/runtime/vita_xinput_mapping_oracle.c" \
    "$root/runtime/vita_xinput_import_test.c" \
    "$root/runtime/host_vita_first_fault.c" \
    "$root/test_vita_xinput_bindings.py" \
    "$root/test_vita_pacing_contract.py" \
    "$root/vita/CMakeLists.txt" \
    "$root/vita/test_xinput_imports.sh" \
    "$work/vita-xinput-mapping-host-oracle" \
    "$work/kage-vita-input-cache-host-oracle" \
    "$work/host_vita_xinput.production.o" \
    "$work/kage_vita_input.production.o" \
    "$work/host_vita_first_fault.xinput-off.o" \
    "$work/host_vita_first_fault.xinput-on.o" \
    "$work/vita-xinput-import-oracle.elf"
echo "Vita synthetic XInput host behavior + direct-R/touch LT + Start-prefix/touch RB/BACK/R controls + coherent-cache + KAGE-only dispatcher/CMake switch + softfp compile/link/static ABI check: PASS (frozen bindings=$xinput_binding_status)"
