#!/usr/bin/env bash
set -eu

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=${ISAAC_QSORT_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
if [ -n "${ISAAC_QSORT_TEST_OUT:-}" ]; then
    work=$ISAAC_QSORT_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-qsort-import.XXXXXX")
fi

host_cc=${CC:-cc}
cc="$VITASDK/bin/arm-vita-eabi-gcc"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"
nm="$VITASDK/bin/arm-vita-eabi-nm"
objdump="$VITASDK/bin/arm-vita-eabi-objdump"

common="-std=gnu11 -O2 -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror -Wno-maybe-uninitialized -DGUEST_IMAGE_BASE=0x98000000u"
arm="$common -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb"

# Execute the exact exported production handler.  -no-pie keeps the static
# guest arena in the x86-64 executable's low 32-bit address range.
"$host_cc" $common -fno-pie -DISAAC_VITA_CRT_QSORT_HOST_ORACLE=1 \
    -I"$root/vita/qsort_oracle_include" \
    -I"$root/runtime" -I"$root/vita" \
    "$root/runtime/guest_stack_legacy_oracle_stub.c" \
    "$root/runtime/host_vita_crt.c" \
    "$root/runtime/vita_qsort_import_oracle.c" \
    -Wl,--gc-sections -no-pie \
    -o "$work/vita-qsort-host-oracle"
if command -v timeout >/dev/null 2>&1; then
    timeout 10s "$work/vita-qsort-host-oracle"
else
    "$work/vita-qsort-host-oracle"
fi
if nm -D "$work/vita-qsort-host-oracle" 2>/dev/null |
        grep -Eq '[[:space:]]qsort(@|$)'; then
    echo "host oracle accidentally imports native libc qsort" >&2
    exit 3
fi

# Link the same handler/oracle for Vita but do not execute that ELF: the
# bundled GNU simulator is not positive runtime evidence for Vita startup.
"$cc" $arm -I"$root/runtime" -I"$root/vita" \
    "$root/runtime/guest_stack_legacy_oracle_stub.c" \
    "$root/runtime/host_vita_crt.c" \
    "$root/runtime/vita_qsort_import_oracle.c" \
    -Wl,--gc-sections \
    -o "$work/vita-qsort-softfp-oracle.elf"
"$cc" $arm -I"$root/runtime" -I"$root/vita" \
    -c "$root/runtime/host_vita_crt.c" \
    -o "$work/host_vita_crt.production.o"

"$readelf" -h "$work/vita-qsort-softfp-oracle.elf" \
    > "$work/header"
"$readelf" -A "$work/vita-qsort-softfp-oracle.elf" \
    > "$work/attributes"
"$nm" -u "$work/vita-qsort-softfp-oracle.elf" \
    > "$work/oracle.undefined"
"$nm" -u "$work/host_vita_crt.production.o" \
    > "$work/production.undefined"
"$objdump" -dr "$work/host_vita_crt.production.o" \
    > "$work/production.disassembly"
cat "$work/header"
cat "$work/attributes"

if ! grep -q "soft-float ABI" "$work/header"; then
    echo "qsort oracle is not marked soft-float ABI" >&2
    exit 4
fi
if grep -q "Tag_ABI_VFP_args" "$work/attributes"; then
    echo "qsort oracle unexpectedly advertises hardfp arguments" >&2
    exit 5
fi
if [ -s "$work/oracle.undefined" ]; then
    cat "$work/oracle.undefined" >&2
    echo "qsort softfp oracle has unresolved symbols" >&2
    exit 6
fi
if ! "$nm" "$work/host_vita_crt.production.o" |
        grep -Eq '[[:space:]]T[[:space:]]+isaac_vita_crt_qsort$'; then
    echo "production CRT object does not export isaac_vita_crt_qsort" >&2
    exit 7
fi
if grep -Eq '[[:space:]]U[[:space:]]+qsort$' \
        "$work/production.undefined"; then
    echo "production CRT object delegates to native qsort" >&2
    exit 8
fi
if ! grep -Eq '[[:space:]]U[[:space:]]+guest_call$' \
        "$work/production.undefined" ||
   ! grep -Eq '[[:space:]]U[[:space:]]+guest_fault$' \
        "$work/production.undefined"; then
    cat "$work/production.undefined" >&2
    echo "production CRT object lost the guest callback/fault boundary" >&2
    exit 9
fi

sha256sum \
    "$root/runtime/guest_stack_legacy_oracle_stub.c" \
    "$root/runtime/host_vita_crt.h" \
    "$root/runtime/host_vita_crt.c" \
    "$root/runtime/vita_qsort_import_oracle.c" \
    "$root/vita/qsort_oracle_include/psp2/rtc.h" \
    "$root/vita/test_qsort_import.sh" \
    "$work/vita-qsort-host-oracle" \
    "$work/vita-qsort-softfp-oracle.elf" \
    "$work/host_vita_crt.production.o"
echo "Vita qsort guest-callback behavior + frozen 7-site/6-comparator census + softfp static ABI gate: PASS (ARM ELF was not executed)"
