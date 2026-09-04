#!/usr/bin/env bash
set -eu

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=${ISAAC_OGG_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
work=${ISAAC_OGG_TEST_OUT:-$(mktemp -d "${TMPDIR:-/tmp}/isaac-ogg-emergency.XXXXXX")}
fake_include=${ISAAC_OGG_FAKE_INCLUDE:-$root/vita/anm2_scratch_oracle_include}
mkdir -p "$work"

host_cc=${HOST_CC:-cc}
host_nm=${HOST_NM:-nm}
cc="$VITASDK/bin/arm-vita-eabi-gcc"
ld="$VITASDK/bin/arm-vita-eabi-ld"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"
nm="$VITASDK/bin/arm-vita-eabi-nm"

common="-std=gnu11 -O2 -Wall -Wextra -Werror -Wno-maybe-uninitialized \
-DGUEST_IMAGE_BASE=0x98000000u -DGUEST_STACK_REQUIRED=1 \
-ffunction-sections -fdata-sections -I$root/runtime -I$root/vita"

"$host_cc" $common -DISAAC_VITA_OGG_EMERGENCY=1 \
    -Dmalloc=oracle_guest_malloc -Dcalloc=oracle_guest_calloc \
    -I"$fake_include" -c "$root/runtime/host_vita_heap.c" \
    -o "$work/host-vita-heap.ogg-routing.o"
"$host_cc" $common -DISAAC_VITA_OGG_EMERGENCY_ORACLE=1 \
    -I"$fake_include" -c "$root/runtime/host_vita_ogg_emergency.c" \
    -o "$work/ogg-emergency.state.o"
"$host_cc" $common -I"$fake_include" -c "$root/runtime/guest.c" \
    -o "$work/guest.ogg-routing.o"
"$host_cc" $common -DISAAC_VITA_OGG_EMERGENCY_ORACLE=1 \
    -I"$fake_include" -c "$root/runtime/host_vita_ogg_emergency_oracle.c" \
    -o "$work/ogg-emergency.oracle.o"
"$host_cc" -pthread -Wl,--gc-sections \
    "$work/host-vita-heap.ogg-routing.o" \
    "$work/ogg-emergency.state.o" \
    "$work/guest.ogg-routing.o" \
    "$work/ogg-emergency.oracle.o" \
    -o "$work/ogg-emergency-host-oracle"
"$work/ogg-emergency-host-oracle"

"$host_nm" -u "$work/host-vita-heap.ogg-routing.o" \
    > "$work/host-vita-heap.ogg-routing.undefined"
for symbol in isaac_vita_ogg_emergency_malloc \
              isaac_vita_ogg_emergency_free \
              isaac_vita_ogg_emergency_realloc
do
    if ! grep -Eq "[[:space:]]U[[:space:]]+$symbol$" \
            "$work/host-vita-heap.ogg-routing.undefined"; then
        cat "$work/host-vita-heap.ogg-routing.undefined" >&2
        echo "heap routing object misses $symbol" >&2
        exit 3
    fi
done

pe_contract=SKIP
if [ -n "${ISAAC_OGG_PE_PATH:-}" ]; then
    python3 "$root/test_ogg_emergency_contract.py" \
        --pe "$ISAAC_OGG_PE_PATH" \
        --header "$root/runtime/host_vita_ogg_emergency.h"
    pe_contract=PASS
else
    echo "OGG frozen PE contract: SKIP (set ISAAC_OGG_PE_PATH)"
fi

arm="$common -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb"
"$cc" $arm -c "$root/runtime/host_vita_ogg_emergency.c" \
    -o "$work/ogg-emergency.actual-header.o"
"$cc" $arm -I"$fake_include" \
    -c "$root/runtime/host_vita_ogg_emergency.c" \
    -o "$work/ogg-emergency.fake-header.o"
if ! cmp -s "$work/ogg-emergency.actual-header.o" \
              "$work/ogg-emergency.fake-header.o"; then
    echo "fake sysmem header changed the OGG production object" >&2
    exit 4
fi

"$cc" $arm -I"$fake_include" \
    "$root/runtime/host_vita_ogg_emergency.c" \
    "$root/runtime/host_vita_ogg_emergency_link_probe.c" \
    -Wl,--gc-sections -o "$work/ogg-emergency-softfp-link.elf"

"$readelf" -h "$work/ogg-emergency-softfp-link.elf" > "$work/header"
"$readelf" -A "$work/ogg-emergency-softfp-link.elf" > "$work/attributes"
"$nm" -u "$work/ogg-emergency.actual-header.o" \
    > "$work/production.undefined"
"$nm" -u "$work/ogg-emergency-softfp-link.elf" \
    > "$work/link.undefined"
cat "$work/header"
cat "$work/attributes"

if ! grep -q "Version5 EABI, soft-float ABI" "$work/header"; then
    echo "OGG emergency link is not ARM EABI5 softfp" >&2
    exit 5
fi
if grep -q "Tag_ABI_VFP_args" "$work/attributes"; then
    echo "OGG emergency link advertises hardfp arguments" >&2
    exit 6
fi
if [ -s "$work/link.undefined" ]; then
    cat "$work/link.undefined" >&2
    echo "OGG fake-sysmem ARM link has unresolved symbols" >&2
    exit 7
fi
for symbol in isaac_vita_log sceKernelAllocMemBlock \
              sceKernelFreeMemBlock sceKernelGetMemBlockBase
do
    if ! grep -Eq "[[:space:]]U[[:space:]]+$symbol$" \
            "$work/production.undefined"; then
        cat "$work/production.undefined" >&2
        echo "production OGG object misses dependency: $symbol" >&2
        exit 8
    fi
done
if [ "$(wc -l < "$work/production.undefined")" -ne 4 ]; then
    cat "$work/production.undefined" >&2
    echo "production OGG object gained an unexpected dependency" >&2
    exit 9
fi
if "$nm" "$work/ogg-emergency.actual-header.o" \
        "$work/ogg-emergency-softfp-link.elf" | \
        grep -Eq '(__atomic|libatomic|[[:space:]](malloc|calloc|realloc|free)$)'; then
    echo "OGG emergency reached libatomic or a libc allocator" >&2
    exit 10
fi

"$cc" $arm -DISAAC_VITA_OGG_EMERGENCY=1 \
    -c "$root/runtime/host_vita_heap.c" \
    -o "$work/host-vita-heap.ogg-arm.o"
"$ld" -r "$work/host-vita-heap.ogg-arm.o" \
    "$work/ogg-emergency.actual-header.o" \
    -o "$work/ogg-emergency-heap-owner.o"
"$nm" -u "$work/ogg-emergency-heap-owner.o" \
    > "$work/ogg-emergency-heap-owner.undefined"
if grep -Eq 'isaac_vita_ogg_emergency_(malloc|free|realloc)$' \
        "$work/ogg-emergency-heap-owner.undefined"; then
    cat "$work/ogg-emergency-heap-owner.undefined" >&2
    echo "OGG heap/module partial link is unresolved" >&2
    exit 11
fi

sha256sum \
    "$root/runtime/host_vita_ogg_emergency.h" \
    "$root/runtime/host_vita_ogg_emergency.c" \
    "$root/runtime/host_vita_ogg_emergency_oracle.c" \
    "$root/runtime/host_vita_ogg_emergency_link_probe.c" \
    "$root/test_ogg_emergency_contract.py" \
    "$root/vita/test_ogg_emergency.sh" \
    "$work/ogg-emergency-host-oracle" \
    "$work/ogg-emergency-softfp-link.elf" \
    "$work/ogg-emergency-heap-owner.o"
echo "Vita OGG emergency host/ARM EABI5 softfp gates: PASS; frozen PE contract: $pe_contract (ARM ELF was not executed)"
