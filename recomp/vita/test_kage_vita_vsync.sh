#!/usr/bin/env bash
set -eu

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi
if [ -z "${ISAAC_PACING_PE_PATH:-${ISAAC_PE_PATH:-}}" ]; then
    echo "set ISAAC_PACING_PE_PATH (or ISAAC_PE_PATH) to the frozen Repentance PE" >&2
    exit 2
fi

root=${ISAAC_VSYNC_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
if [ -n "${ISAAC_VSYNC_TEST_OUT:-}" ]; then
    work=$ISAAC_VSYNC_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-kage-vsync.XXXXXX")
fi

host_cc=${HOST_CC:-cc}
python=${PYTHON:-python3}
cc="$VITASDK/bin/arm-vita-eabi-gcc"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"
nm="$VITASDK/bin/arm-vita-eabi-nm"
strings="$VITASDK/bin/arm-vita-eabi-strings"
pe=${ISAAC_PACING_PE_PATH:-${ISAAC_PE_PATH}}

host_flags="-std=gnu11 -O2 -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror"
arm_flags="-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror"
production_flags="$arm_flags -DGUEST_STACK_REQUIRED=1"

"$host_cc" $host_flags -DISAAC_KAGE_VITA_BACKEND_ORACLE=1 \
    -I"$root/runtime" \
    "$root/runtime/kage_vita_backend.c" \
    "$root/runtime/kage_vita_vsync_oracle.c" \
    -Wl,--gc-sections -o "$work/kage-vita-vsync-host-oracle"
"$work/kage-vita-vsync-host-oracle"

"$cc" $arm_flags -DISAAC_KAGE_VITA_BACKEND_ORACLE=1 \
    -I"$root/runtime" \
    "$root/runtime/kage_vita_backend.c" \
    "$root/runtime/kage_vita_vsync_oracle.c" \
    -Wl,--gc-sections -o "$work/kage-vita-vsync-softfp-oracle.elf"

# Compile the exact two production owners against VitaSDK plus the focused
# generated-coverage mirror.  The ARM ELF above is link-only and is not run.
"$cc" $production_flags -DGUEST_IMAGE_BASE=0x98000000u \
    -I"$root/runtime" -I"$root/vita" \
    -c "$root/runtime/kage_vita_backend.c" \
    -o "$work/kage_vita_backend.vsync-production.o"
"$cc" $production_flags -DGUEST_IMAGE_BASE=0x98000000u \
    -DISAAC_VITA_AUDIO=0 \
    -I"$root/vita/audio_oracle_include" \
    -I"$root/runtime" -I"$root/vita" \
    -c "$root/runtime/manual_kage_vita.c" \
    -o "$work/manual_kage_vita.vsync-production.o"

"$readelf" -h "$work/kage-vita-vsync-softfp-oracle.elf" > "$work/header"
"$readelf" -A "$work/kage-vita-vsync-softfp-oracle.elf" > "$work/attributes"
"$readelf" -A "$work/kage_vita_backend.vsync-production.o" \
    > "$work/backend-production.attributes"
"$readelf" -A "$work/manual_kage_vita.vsync-production.o" \
    > "$work/manual-production.attributes"
"$nm" -u "$work/kage-vita-vsync-softfp-oracle.elf" > "$work/undefined"
"$strings" "$work/kage_vita_backend.vsync-production.o" \
    > "$work/backend-production.strings"
"$strings" "$work/manual_kage_vita.vsync-production.o" \
    > "$work/manual-production.strings"

if ! grep -Fq "Version5 EABI, soft-float ABI" "$work/header"; then
    echo "VSync oracle is not an ARM EABI5 softfp executable" >&2
    exit 3
fi
if grep -q "Tag_ABI_VFP_args" "$work/attributes" \
   "$work/backend-production.attributes" "$work/manual-production.attributes"; then
    echo "VSync objects unexpectedly advertise hardfp arguments" >&2
    exit 4
fi
if [ -s "$work/undefined" ]; then
    cat "$work/undefined" >&2
    echo "VSync softfp oracle retained unresolved symbols" >&2
    exit 5
fi
if ! grep -Fq "vsync request=%u actual=0 policy=software-60hz" \
        "$work/backend-production.strings"; then
    echo "production backend lost requested/actual VSync evidence" >&2
    exit 6
fi
if ! grep -Fq "KAGE VITA VSYNC: requested=%u actual=%u" \
        "$work/manual-production.strings"; then
    echo "production manual KAGE lost durable VSync evidence" >&2
    exit 6
fi

"$python" "$root/test_vita_pacing_contract.py" --pe "$pe"

sha256sum \
    "$root/runtime/manual_kage_vita_vsync.h" \
    "$root/runtime/manual_kage_vita.c" \
    "$root/runtime/kage_vita_backend.h" \
    "$root/runtime/kage_vita_backend.c" \
    "$root/runtime/kage_vita_vsync_oracle.c" \
    "$root/test_vita_pacing_contract.py" \
    "$root/vita/test_kage_vita_vsync.sh" \
    "$work/kage-vita-vsync-host-oracle" \
    "$work/kage-vita-vsync-softfp-oracle.elf" \
    "$work/kage_vita_backend.vsync-production.o" \
    "$work/manual_kage_vita.vsync-production.o"
echo "Vita requested/actual VSync host + ARM softfp + frozen-loop pacing gate: PASS (ARM ELF was not executed)"
