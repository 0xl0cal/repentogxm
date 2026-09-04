#!/usr/bin/env bash
set -eu

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=${ISAAC_HEAP_OVERFLOW_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
if [ -n "${ISAAC_HEAP_OVERFLOW_TEST_OUT:-}" ]; then
    work=$ISAAC_HEAP_OVERFLOW_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-heap-overflow.XXXXXX")
fi

host_cc=${HOST_CC:-cc}
cc="$VITASDK/bin/arm-vita-eabi-gcc"
nm="$VITASDK/bin/arm-vita-eabi-nm"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"
fake_include="$work/fake-include"
mkdir -p "$fake_include/psp2/kernel" "$fake_include/psp2"

cat > "$fake_include/psp2/types.h" <<'EOF'
#ifndef ISAAC_OVERFLOW_ORACLE_PSP2_TYPES_H
#define ISAAC_OVERFLOW_ORACLE_PSP2_TYPES_H
#include <stdint.h>
typedef int32_t SceUID;
typedef uint32_t SceSize;
#endif
EOF

cat > "$fake_include/psp2/kernel/sysmem.h" <<'EOF'
#ifndef ISAAC_OVERFLOW_ORACLE_PSP2_SYSMEM_H
#define ISAAC_OVERFLOW_ORACLE_PSP2_SYSMEM_H
#include <psp2/types.h>
typedef int SceKernelMemBlockType;
typedef struct SceKernelAllocMemBlockOpt {
    SceSize size;
    uint32_t opaque[15];
} SceKernelAllocMemBlockOpt;
enum { SCE_KERNEL_MEMBLOCK_TYPE_USER_RW = 0x0c20d060 };
SceUID sceKernelAllocMemBlock(const char *name, SceKernelMemBlockType type,
                              SceSize size,
                              SceKernelAllocMemBlockOpt *option);
int sceKernelGetMemBlockBase(SceUID uid, void **base);
int sceKernelFreeMemBlock(SceUID uid);
#endif
EOF

cat > "$fake_include/psp2/kernel/clib.h" <<'EOF'
#ifndef ISAAC_OVERFLOW_ORACLE_PSP2_CLIB_H
#define ISAAC_OVERFLOW_ORACLE_PSP2_CLIB_H
#include <psp2/types.h>
typedef void *SceClibMspace;
SceClibMspace sceClibMspaceCreate(void *memblock, SceSize size);
void sceClibMspaceDestroy(SceClibMspace mspace);
void *sceClibMspaceMalloc(SceClibMspace mspace, SceSize size);
void *sceClibMspaceCalloc(SceClibMspace mspace, SceSize count, SceSize size);
void *sceClibMspaceRealloc(SceClibMspace mspace, void *pointer, SceSize size);
void sceClibMspaceFree(SceClibMspace mspace, void *pointer);
#endif
EOF

host_flags="-std=gnu11 -O1 -g -Wall -Wextra -Werror -fno-omit-frame-pointer -fsanitize=address,undefined"
"$host_cc" $host_flags \
    -DISAAC_VITA_HEAP_OVERFLOW_MSPACE_TESTING=1 \
    -I"$fake_include" -I"$root/runtime" \
    "$root/runtime/host_vita_heap_overflow_mspace.c" \
    "$root/runtime/host_vita_heap_overflow_mspace_oracle.c" \
    -o "$work/host-vita-heap-overflow-mspace-oracle"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    "$work/host-vita-heap-overflow-mspace-oracle"
ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
    "$work/host-vita-heap-overflow-mspace-oracle" --internal-stranded

arm_flags="-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -ffunction-sections -fdata-sections -Wall -Wextra -Werror"

# Compile the full test lifecycle against both declaration sets.  Identical
# objects prove that the pinned oracle prototypes have not hidden ABI drift in
# the real VitaSDK headers.
"$cc" $arm_flags -DISAAC_VITA_HEAP_OVERFLOW_MSPACE_TESTING=1 \
    -I"$root/runtime" \
    -c "$root/runtime/host_vita_heap_overflow_mspace.c" \
    -o "$work/overflow-mspace.real-header.o"
"$cc" $arm_flags -DISAAC_VITA_HEAP_OVERFLOW_MSPACE_TESTING=1 \
    -I"$fake_include" -I"$root/runtime" \
    -c "$root/runtime/host_vita_heap_overflow_mspace.c" \
    -o "$work/overflow-mspace.fake-header.o"
if ! cmp -s "$work/overflow-mspace.real-header.o" \
              "$work/overflow-mspace.fake-header.o"; then
    echo "fake SceClib/SceSysmem headers changed the overflow-mspace object" >&2
    exit 3
fi

"$cc" $arm_flags -I"$root/runtime" \
    -c "$root/runtime/host_vita_heap_overflow_mspace.c" \
    -o "$work/overflow-mspace.production.o"

undefined_names () {
    "$nm" -u "$1" | awk '$1 == "U" { print $2 }' | LC_ALL=C sort
}

undefined_names "$work/overflow-mspace.real-header.o" \
    > "$work/overflow-mspace.test.undefined.names"
printf '%s\n' \
    sceClibMspaceCalloc \
    sceClibMspaceCreate \
    sceClibMspaceDestroy \
    sceClibMspaceFree \
    sceClibMspaceMalloc \
    sceClibMspaceRealloc \
    sceKernelAllocMemBlock \
    sceKernelFreeMemBlock \
    sceKernelGetMemBlockBase | LC_ALL=C sort \
    > "$work/overflow-mspace.test.expected.names"
if ! cmp -s "$work/overflow-mspace.test.expected.names" \
              "$work/overflow-mspace.test.undefined.names"; then
    echo "overflow-mspace test unresolved-symbol allow-list changed" >&2
    diff -u "$work/overflow-mspace.test.expected.names" \
        "$work/overflow-mspace.test.undefined.names" >&2 || true
    exit 1
fi

undefined_names "$work/overflow-mspace.production.o" \
    > "$work/overflow-mspace.production.undefined.names"
grep -v '^sceClibMspaceDestroy$' \
    "$work/overflow-mspace.test.expected.names" \
    > "$work/overflow-mspace.production.expected.names"
if ! cmp -s "$work/overflow-mspace.production.expected.names" \
              "$work/overflow-mspace.production.undefined.names"; then
    echo "overflow-mspace production unresolved-symbol allow-list changed" >&2
    diff -u "$work/overflow-mspace.production.expected.names" \
        "$work/overflow-mspace.production.undefined.names" >&2 || true
    exit 1
fi

if grep -Eq '^(malloc|calloc|realloc|free|mspace_|__atomic|__sync)' \
       "$work/overflow-mspace.test.undefined.names"; then
    echo "overflow-mspace imported a raw libc/mspace/atomic helper" >&2
    exit 1
fi

cat > "$work/arm-link-main.c" <<'EOF'
#include <stddef.h>
#include "host_vita_heap_overflow_mspace.h"

int main(void)
{
    isaac_vita_heap_overflow_forbidden_ranges ranges = {
        { 0x81000000U, 0x82000000U },
        { 0x83000000U, 0x84000000U },
        { 0x85000000U, 0x86000000U }
    };
    isaac_vita_heap_overflow_mspace_snapshot snapshot;
    isaac_vita_heap_overflow_mspace_realloc_status realloc_status;
    void *pointer;
    void *internal_page;

    (void)isaac_vita_heap_overflow_mspace_init(&ranges);
    internal_page =
        isaac_vita_heap_overflow_mspace_internal_page_malloc();
    (void)isaac_vita_heap_overflow_mspace_internal_page_free(internal_page);
    pointer = isaac_vita_heap_overflow_mspace_malloc(32U);
    (void)isaac_vita_heap_overflow_mspace_calloc(2U, 16U);
    pointer = isaac_vita_heap_overflow_mspace_realloc(
        pointer, 64U, &realloc_status);
    (void)isaac_vita_heap_overflow_mspace_contains(pointer);
    (void)isaac_vita_heap_overflow_mspace_free(pointer);
    (void)isaac_vita_heap_overflow_mspace_snapshot_get(&snapshot);
    (void)isaac_vita_heap_overflow_mspace_test_reset();
    return snapshot.state == ISAAC_VITA_HEAP_OVERFLOW_MSPACE_ORPHANED;
}
EOF

"$cc" $arm_flags -DISAAC_VITA_HEAP_OVERFLOW_MSPACE_TESTING=1 \
    -I"$root/runtime" \
    "$root/runtime/host_vita_heap_overflow_mspace.c" \
    "$work/arm-link-main.c" \
    -Wl,--gc-sections -lSceLibKernel_stub -lSceSysmem_stub \
    -o "$work/vita-heap-overflow-mspace.elf"

"$readelf" -h "$work/vita-heap-overflow-mspace.elf" \
    > "$work/overflow-mspace.elf.header"
"$readelf" -A "$work/vita-heap-overflow-mspace.elf" \
    > "$work/overflow-mspace.elf.attributes"
cat "$work/overflow-mspace.elf.header"
cat "$work/overflow-mspace.elf.attributes"
grep -q 'Machine:[[:space:]]*ARM$' "$work/overflow-mspace.elf.header"
grep -q 'Version5 EABI, soft-float ABI' "$work/overflow-mspace.elf.header"
if grep -q 'Tag_ABI_VFP_args' "$work/overflow-mspace.elf.attributes"; then
    echo "overflow-mspace ARM link gained a hardfp VFP-args tag" >&2
    exit 1
fi
"$nm" "$work/vita-heap-overflow-mspace.elf" \
    > "$work/vita-heap-overflow-mspace.nm"
if grep -Eq ' U (__atomic|__sync|mspace_|malloc$|calloc$|realloc$|free$)' \
       "$work/vita-heap-overflow-mspace.nm"; then
    echo "overflow-mspace ARM link gained a forbidden allocator/helper" >&2
    exit 1
fi

sha256sum \
    "$root/runtime/host_vita_heap_overflow_mspace.h" \
    "$root/runtime/host_vita_heap_overflow_mspace.c" \
    "$root/runtime/host_vita_heap_overflow_mspace_oracle.c" \
    "$root/vita/test_heap_overflow_mspace.sh" \
    "$fake_include/psp2/types.h" \
    "$fake_include/psp2/kernel/sysmem.h" \
    "$fake_include/psp2/kernel/clib.h" \
    "$VITASDK/arm-vita-eabi/include/psp2/kernel/sysmem.h" \
    "$VITASDK/arm-vita-eabi/include/psp2/kernel/clib.h" \
    "$work/overflow-mspace.production.o" \
    "$work/vita-heap-overflow-mspace.elf"
echo "Vita heap overflow SceClibMspace host/ARM gate: PASS (ARM linked, not executed)"
