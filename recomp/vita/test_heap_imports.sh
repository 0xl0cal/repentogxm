#!/usr/bin/env bash
set -eu

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=${ISAAC_HEAP_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
if [ -n "${ISAAC_HEAP_TEST_OUT:-}" ]; then
    work=$ISAAC_HEAP_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-heap-imports.XXXXXX")
fi

cc="$VITASDK/bin/arm-vita-eabi-gcc"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"
nm="$VITASDK/bin/arm-vita-eabi-nm"
host_cc=${HOST_CC:-cc}
fake_include="$root/vita/anm2_scratch_oracle_include"
host_san_flags=(-O1 -fsanitize=address,undefined -fno-omit-frame-pointer)

flags="-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror -Wno-maybe-uninitialized"

"$cc" $flags -DGUEST_IMAGE_BASE=0x98000000u -I"$root/runtime" \
    -c "$root/runtime/host_vita_heap.c" \
    -o "$work/host-vita-heap-policy-off.o"
if "$nm" "$work/host-vita-heap-policy-off.o" | \
     grep -q 'isaac_vita_guest_heap_lease'; then
    echo "default-OFF heap object retained range-lease policy" >&2
    exit 1
fi

for ledger_mode in legacy backshift; do
    backshift_flags=()
    if [ "$ledger_mode" = backshift ]; then
        backshift_flags=(-DISAAC_VITA_HEAP_LEDGER_BACKSHIFT=1)
    fi
    "$host_cc" -std=gnu11 "${host_san_flags[@]}" \
        -Wall -Wextra -Werror -Wno-maybe-uninitialized -pthread \
        -ffunction-sections -fdata-sections \
        -DGUEST_IMAGE_BASE=0x98000000u -DISAAC_VITA_HEAP_TESTING=1 \
        -DISAAC_VITA_HEAP_RANGE_LEASE=1 \
        -DISAAC_VITA_PNG_NATIVE_UNFILTER=1 "${backshift_flags[@]}" \
        -I"$root/runtime" -c "$root/runtime/host_vita_heap.c" \
        -o "$work/host-vita-heap-lease-$ledger_mode.o"
    "$host_cc" -std=gnu11 "${host_san_flags[@]}" \
        -Wall -Wextra -Werror -pthread -ffunction-sections -fdata-sections \
        -DISAAC_VITA_HEAP_TESTING=1 -DISAAC_VITA_HEAP_RANGE_LEASE=1 \
        -DISAAC_VITA_PNG_NATIVE_UNFILTER=1 \
        -I"$root/runtime" -c "$root/runtime/host_vita_heap_lease_oracle.c" \
        -o "$work/host-vita-heap-lease-oracle-$ledger_mode.o"
    "$host_cc" "${host_san_flags[@]}" -pthread \
        "$work/host-vita-heap-lease-$ledger_mode.o" \
        "$work/host-vita-heap-lease-oracle-$ledger_mode.o" \
        -Wl,--gc-sections -o "$work/host-vita-heap-lease-oracle-$ledger_mode"
    "$work/host-vita-heap-lease-oracle-$ledger_mode"
done

# Exercise the production USER_RW storage path with dirty pages and injected
# alloc/get/free failures in both production table layouts.  Interpose only
# heap.c's calloc/free calls so a successful rehash proves that ledger
# generations never touch newlib.
"$host_cc" -std=gnu11 "${host_san_flags[@]}" -Wall -Wextra -Werror \
    -ffunction-sections -fdata-sections \
    -I"$fake_include" -I"$root/runtime" \
    -c "$root/runtime/host_vita_heap_ledger_memblock.c" \
    -o "$work/host-vita-heap-ledger-storage.o"
for lease_mode in exact-base range-lease; do
    lease_flags=()
    if [ "$lease_mode" = range-lease ]; then
        lease_flags=(-DISAAC_VITA_HEAP_RANGE_LEASE=1)
    fi
    for ledger_mode in legacy backshift; do
        backshift_flags=()
        if [ "$ledger_mode" = backshift ]; then
            backshift_flags=(-DISAAC_VITA_HEAP_LEDGER_BACKSHIFT=1)
        fi
        variant="$lease_mode-$ledger_mode"
        "$host_cc" -std=gnu11 "${host_san_flags[@]}" \
            -Wall -Wextra -Werror -Wno-maybe-uninitialized \
            -ffunction-sections -fdata-sections \
            -DGUEST_IMAGE_BASE=0x98000000u -DISAAC_VITA_HEAP_TESTING=1 \
            -DISAAC_VITA_HEAP_LEDGER_MEMBLOCK=1 \
            -Dcalloc=oracle_calloc -Dfree=oracle_free \
            -Drealloc=oracle_realloc "${lease_flags[@]}" \
            "${backshift_flags[@]}" -I"$fake_include" -I"$root/runtime" \
            -c "$root/runtime/host_vita_heap.c" \
            -o "$work/host-vita-heap-ledger-$variant.o"
        "$host_cc" -std=gnu11 "${host_san_flags[@]}" \
            -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
            -DISAAC_VITA_HEAP_TESTING=1 \
            -DISAAC_VITA_HEAP_LEDGER_MEMBLOCK=1 \
            "${lease_flags[@]}" -I"$fake_include" -I"$root/runtime" \
            -c "$root/runtime/host_vita_heap_ledger_memblock_oracle.c" \
            -o "$work/host-vita-heap-ledger-oracle-$variant.o"
        "$host_cc" "${host_san_flags[@]}" \
            "$work/host-vita-heap-ledger-$variant.o" \
            "$work/host-vita-heap-ledger-storage.o" \
            "$work/host-vita-heap-ledger-oracle-$variant.o" \
            -Wl,--gc-sections \
            -o "$work/host-vita-heap-ledger-oracle-$variant"
        "$work/host-vita-heap-ledger-oracle-$variant"
    done
done

# Re-run the existing cross-thread lease/free/realloc oracle with the USER_RW
# backend active.  The link-probe half of the transaction oracle contributes
# only the dirty fake sysmem implementation and allocator counters, not main.
"$host_cc" -std=gnu11 "${host_san_flags[@]}" -Wall -Wextra -Werror \
    -ffunction-sections -fdata-sections \
    -DISAAC_VITA_HEAP_LEDGER_MEMBLOCK_LINK_PROBE=1 \
    -I"$fake_include" -I"$root/runtime" \
    -c "$root/runtime/host_vita_heap_ledger_memblock_oracle.c" \
    -o "$work/host-vita-heap-ledger-fake-sysmem.o"
"$host_cc" -std=gnu11 "${host_san_flags[@]}" -Wall -Wextra -Werror \
    -Wno-maybe-uninitialized -pthread \
    -ffunction-sections -fdata-sections \
    -DISAAC_VITA_HEAP_TESTING=1 -DISAAC_VITA_HEAP_RANGE_LEASE=1 \
    -DISAAC_VITA_HEAP_LEDGER_MEMBLOCK=1 \
    -I"$root/runtime" \
    -c "$root/runtime/host_vita_heap_lease_oracle.c" \
    -o "$work/host-vita-heap-lease-memblock-oracle.o"
"$host_cc" "${host_san_flags[@]}" -pthread \
    "$work/host-vita-heap-ledger-range-lease-backshift.o" \
    "$work/host-vita-heap-ledger-storage.o" \
    "$work/host-vita-heap-ledger-fake-sysmem.o" \
    "$work/host-vita-heap-lease-memblock-oracle.o" \
    -Wl,--gc-sections -o "$work/host-vita-heap-lease-memblock-oracle"
"$work/host-vita-heap-lease-memblock-oracle"

# Build every ARM table-layout/deletion-policy combination.  The backshift A/B
# definition is deliberately applied only to heap.c, matching production's
# source-scoped CMake contract.
"$cc" $flags -I"$fake_include" -I"$root/runtime" \
    -c "$root/runtime/host_vita_heap_ledger_memblock.c" \
    -o "$work/heap-ledger-storage.arm.o"
"$cc" $flags -DISAAC_VITA_HEAP_TESTING=1 \
    -DISAAC_VITA_HEAP_LEDGER_MEMBLOCK=1 \
    -DISAAC_VITA_HEAP_LEDGER_MEMBLOCK_LINK_PROBE=1 \
    -I"$fake_include" -I"$root/runtime" \
    -c "$root/runtime/host_vita_heap_ledger_memblock_oracle.c" \
    -o "$work/heap-ledger-fake-sysmem.arm.o"
"$cc" $flags -DGUEST_IMAGE_BASE=0x98000000u \
    -DISAAC_VITA_HEAP_TESTING=1 -DISAAC_VITA_HEAP_RANGE_LEASE=1 \
    -I"$root/runtime" -c "$root/runtime/vita_heap_import_test.c" \
    -o "$work/vita-heap-import-range-main.arm.o"
"$cc" $flags -DISAAC_VITA_HEAP_TESTING=1 \
    -DISAAC_VITA_HEAP_LEDGER_MEMBLOCK=1 \
    -DISAAC_VITA_HEAP_LEDGER_MEMBLOCK_ARM_LINK_MAIN=1 \
    -I"$fake_include" -I"$root/runtime" \
    -c "$root/runtime/host_vita_heap_ledger_memblock_oracle.c" \
    -o "$work/vita-heap-import-exact-main.arm.o"

for lease_mode in exact-base range-lease; do
    lease_flags=()
    if [ "$lease_mode" = range-lease ]; then
        lease_flags=(-DISAAC_VITA_HEAP_RANGE_LEASE=1)
    fi
    for ledger_mode in legacy backshift; do
        backshift_flags=()
        if [ "$ledger_mode" = backshift ]; then
            backshift_flags=(-DISAAC_VITA_HEAP_LEDGER_BACKSHIFT=1)
        fi
        variant="$lease_mode-$ledger_mode"
        "$cc" $flags -DGUEST_IMAGE_BASE=0x98000000u \
            -DISAAC_VITA_HEAP_TESTING=1 \
            -DISAAC_VITA_HEAP_LEDGER_MEMBLOCK=1 \
            "${lease_flags[@]}" "${backshift_flags[@]}" \
            -I"$root/runtime" -c "$root/runtime/host_vita_heap.c" \
            -o "$work/vita-heap-$variant.arm.o"
        if [ "$lease_mode" = range-lease ]; then
            arm_main="$work/vita-heap-import-range-main.arm.o"
            arm_sysmem="$work/heap-ledger-fake-sysmem.arm.o"
        else
            arm_main="$work/vita-heap-import-exact-main.arm.o"
            arm_sysmem=
        fi
        "$cc" $flags "$work/vita-heap-$variant.arm.o" \
            "$work/heap-ledger-storage.arm.o" ${arm_sysmem:+"$arm_sysmem"} \
            "$arm_main" -Wl,--gc-sections \
            -o "$work/vita-heap-import-oracle-$variant.elf"
    done
done

# The production storage object must compile identically against the real
# VitaSDK declaration and the pinned fake declaration used by the oracle.
"$cc" $flags -I"$root/runtime" \
    -c "$root/runtime/host_vita_heap_ledger_memblock.c" \
    -o "$work/heap-ledger-storage.actual-header.o"
"$cc" $flags -I"$fake_include" -I"$root/runtime" \
    -c "$root/runtime/host_vita_heap_ledger_memblock.c" \
    -o "$work/heap-ledger-storage.fake-header.o"
if ! cmp -s "$work/heap-ledger-storage.actual-header.o" \
              "$work/heap-ledger-storage.fake-header.o"; then
    echo "fake sysmem header changed the production heap-ledger object" >&2
    exit 3
fi

# Storage is allowed exactly the three sysmem calls plus compiler-emitted
# memset.  Any allocator or atomic helper is therefore an immediate failure.
"$nm" -u "$work/heap-ledger-storage.actual-header.o" \
    > "$work/heap-ledger-storage.undefined"
awk '$1 == "U" { print $2 }' "$work/heap-ledger-storage.undefined" | \
    LC_ALL=C sort > "$work/heap-ledger-storage.undefined.names"
printf '%s\n' memset sceKernelAllocMemBlock sceKernelFreeMemBlock \
    sceKernelGetMemBlockBase | LC_ALL=C sort \
    > "$work/heap-ledger-storage.expected.names"
if ! cmp -s "$work/heap-ledger-storage.expected.names" \
              "$work/heap-ledger-storage.undefined.names"; then
    echo "heap-ledger storage unresolved-symbol allow-list changed" >&2
    diff -u "$work/heap-ledger-storage.expected.names" \
        "$work/heap-ledger-storage.undefined.names" >&2 || true
    exit 1
fi
if grep -Eq '^(malloc|calloc|realloc|free)$' \
       "$work/heap-ledger-storage.undefined.names"; then
    echo "heap-ledger storage imported a newlib allocator" >&2
    exit 1
fi

for lease_mode in exact-base range-lease; do
    for ledger_mode in legacy backshift; do
        variant="$lease_mode-$ledger_mode"
        arm_elf="$work/vita-heap-import-oracle-$variant.elf"
        "$readelf" -h "$arm_elf" > "$work/header-$variant"
        "$readelf" -A "$arm_elf" > "$work/attributes-$variant"
        cat "$work/header-$variant"
        cat "$work/attributes-$variant"
        grep -q 'Machine:[[:space:]]*ARM$' "$work/header-$variant"
        grep -q 'Version5 EABI, soft-float ABI' "$work/header-$variant"
        if grep -q 'Tag_ABI_VFP_args' "$work/attributes-$variant"; then
            echo "$variant ARM heap oracle gained a hardfp VFP-args tag" >&2
            exit 1
        fi
        "$nm" "$arm_elf" > "$work/vita-heap-import-oracle-$variant.nm"
        if grep -Eq ' U (__atomic|__sync)' \
               "$work/vita-heap-import-oracle-$variant.nm"; then
            echo "$variant Vita heap oracle gained an atomic helper" >&2
            exit 1
        fi
        if [ "$lease_mode" = exact-base ]; then
            if grep -q ' T isaac_vita_guest_heap_lease' \
                   "$work/vita-heap-import-oracle-$variant.nm"; then
                echo "$variant retained range-lease policy" >&2
                exit 1
            fi
        else
            grep -q ' T isaac_vita_guest_heap_lease_containing$' \
                "$work/vita-heap-import-oracle-$variant.nm"
            grep -q ' T isaac_vita_guest_heap_lease_release$' \
                "$work/vita-heap-import-oracle-$variant.nm"
        fi
    done
done

sha256sum \
    "$root/runtime/host_vita_heap.h" \
    "$root/runtime/host_vita_heap.c" \
    "$root/runtime/host_vita_heap_ledger_memblock.h" \
    "$root/runtime/host_vita_heap_ledger_memblock.c" \
    "$root/runtime/host_vita_heap_ledger_memblock_oracle.c" \
    "$root/runtime/host_vita_heap_lease_oracle.c" \
    "$root/runtime/vita_heap_import_test.c" \
    "$fake_include/psp2/kernel/sysmem.h" \
    "$root/vita/test_heap_imports.sh" \
    "$work/vita-heap-import-oracle-exact-base-legacy.elf" \
    "$work/vita-heap-import-oracle-exact-base-backshift.elf" \
    "$work/vita-heap-import-oracle-range-lease-legacy.elf" \
    "$work/vita-heap-import-oracle-range-lease-backshift.elf"
echo "Vita UCRT heap compile/link/static artifact check: PASS (both layouts and both deletion policies built; ARM runtime evidence still requires Vita3K or hardware)"
