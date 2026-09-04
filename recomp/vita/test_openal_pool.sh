#!/usr/bin/env bash
set -euo pipefail

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=${ISAAC_OPENAL_POOL_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
if [ -n "${ISAAC_OPENAL_POOL_TEST_OUT:-}" ]; then
    work=$ISAAC_OPENAL_POOL_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-openal-pool.XXXXXX")
fi

host_gcc=${HOST_CC:-gcc}
host_clang=${HOST_CLANG:-clang}
gcc_path=$(command -v "$host_gcc")
clang_path=$(command -v "$host_clang")
if [ "$(readlink -f "$gcc_path")" = "$(readlink -f "$clang_path")" ]; then
    echo "OpenAL pool gate requires distinct GCC and Clang compilers" >&2
    exit 2
fi
if ! "$gcc_path" --version | head -1 | grep -Eqi 'gcc|free software foundation'; then
    echo "HOST_CC is not GCC: $gcc_path" >&2
    exit 2
fi
if ! "$clang_path" --version | head -1 | grep -qi clang; then
    echo "HOST_CLANG is not Clang: $clang_path" >&2
    exit 2
fi

cc="$VITASDK/bin/arm-vita-eabi-gcc"
nm="$VITASDK/bin/arm-vita-eabi-nm"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"
fake="$work/fake-include"
mkdir -p "$fake/psp2/kernel/threadmgr" "$fake/psp2/kernel" "$fake/psp2"

cat > "$fake/psp2/types.h" <<'EOF'
#ifndef ISAAC_OPENAL_POOL_FAKE_TYPES_H
#define ISAAC_OPENAL_POOL_FAKE_TYPES_H
#include <stdint.h>
typedef int32_t SceUID;
typedef uint32_t SceSize;
typedef uint32_t SceUInt;
#endif
EOF

cat > "$fake/psp2/kernel/sysmem.h" <<'EOF'
#ifndef ISAAC_OPENAL_POOL_FAKE_SYSMEM_H
#define ISAAC_OPENAL_POOL_FAKE_SYSMEM_H
#include <psp2/types.h>
typedef int SceKernelMemBlockType;
typedef struct SceKernelAllocMemBlockOpt {
    SceSize size;
    uint32_t opaque[15];
} SceKernelAllocMemBlockOpt;
enum { SCE_KERNEL_MEMBLOCK_TYPE_USER_RW = 0x0c20d060 };
SceUID sceKernelAllocMemBlock(const char *, SceKernelMemBlockType, SceSize,
                              SceKernelAllocMemBlockOpt *);
int sceKernelGetMemBlockBase(SceUID, void **);
int sceKernelFreeMemBlock(SceUID);
#endif
EOF

cat > "$fake/psp2/kernel/clib.h" <<'EOF'
#ifndef ISAAC_OPENAL_POOL_FAKE_CLIB_H
#define ISAAC_OPENAL_POOL_FAKE_CLIB_H
#include <psp2/types.h>
typedef void *SceClibMspace;
SceClibMspace sceClibMspaceCreate(void *, SceSize);
void sceClibMspaceDestroy(SceClibMspace);
void *sceClibMspaceMalloc(SceClibMspace, SceSize);
void sceClibMspaceFree(SceClibMspace, void *);
#endif
EOF

cat > "$fake/psp2/kernel/threadmgr/mutex.h" <<'EOF'
#ifndef ISAAC_OPENAL_POOL_FAKE_MUTEX_H
#define ISAAC_OPENAL_POOL_FAKE_MUTEX_H
#include <psp2/types.h>
typedef struct SceKernelMutexOptParam {
    SceSize size;
    uint32_t opaque[7];
} SceKernelMutexOptParam;
SceUID sceKernelCreateMutex(const char *, SceUInt, int,
                            SceKernelMutexOptParam *);
int sceKernelDeleteMutex(SceUID);
int sceKernelLockMutex(SceUID, int, unsigned int *);
int sceKernelUnlockMutex(SceUID, int);
#endif
EOF

host_flags=(
    -std=gnu11 -O1 -g -Wall -Wextra -Werror -fno-omit-frame-pointer
    -fsanitize=address,undefined -pthread
    -DISAAC_VITA_OPENAL_POOL_TESTING=1
    -I"$fake" -I"$root/runtime"
)
wrap_flags=(
    -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc
    -Wl,--wrap=free -Wl,--wrap=aligned_alloc
)

for compiler in "$gcc_path" "$clang_path"; do
    family=$(basename "$compiler")
    active="$work/openal-pool-$family"
    "$compiler" "${host_flags[@]}" \
        -DISAAC_VITA_OPENAL_POOL_ACTIVE=1 \
        "$root/runtime/host_vita_openal_pool.c" \
        "$root/runtime/host_vita_openal_pool_oracle.c" \
        "${wrap_flags[@]}" -o "$active"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
        "$active"

    disabled="$work/openal-pool-$family-disabled"
    "$compiler" "${host_flags[@]}" \
        -DISAAC_VITA_OPENAL_POOL_ORACLE_OFF=1 \
        "$root/runtime/host_vita_openal_pool.c" \
        "$root/runtime/host_vita_openal_pool_oracle.c" \
        "${wrap_flags[@]}" -o "$disabled"
    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
    UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
        "$disabled"
done

tsan="$work/openal-pool-clang-tsan"
"$clang_path" \
    -std=gnu11 -O1 -g -Wall -Wextra -Werror -Wno-unused-function \
    -fno-omit-frame-pointer \
    -fsanitize=thread -pthread \
    -DISAAC_VITA_OPENAL_POOL_TESTING=1 \
    -DISAAC_VITA_OPENAL_POOL_ACTIVE=1 \
    -DISAAC_VITA_OPENAL_POOL_ORACLE_TSAN=1 \
    -I"$fake" -I"$root/runtime" \
    "$root/runtime/host_vita_openal_pool.c" \
    "$root/runtime/host_vita_openal_pool_oracle.c" \
    -o "$tsan"
TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1 "$tsan"

arm_flags=(
    -std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb
    -ffunction-sections -fdata-sections -Wall -Wextra -Werror
    -Wno-maybe-uninitialized
)

"$cc" "${arm_flags[@]}" \
    -DISAAC_VITA_OPENAL_POOL_ACTIVE=1 \
    -DISAAC_VITA_OPENAL_POOL_TESTING=1 \
    -I"$root/runtime" \
    -c "$root/runtime/host_vita_openal_pool.c" \
    -o "$work/openal-pool.real-header.o"
"$cc" "${arm_flags[@]}" \
    -DISAAC_VITA_OPENAL_POOL_ACTIVE=1 \
    -DISAAC_VITA_OPENAL_POOL_TESTING=1 \
    -I"$fake" -I"$root/runtime" \
    -c "$root/runtime/host_vita_openal_pool.c" \
    -o "$work/openal-pool.fake-header.o"
if ! cmp -s "$work/openal-pool.real-header.o" \
              "$work/openal-pool.fake-header.o"; then
    echo "fake Vita headers changed the OpenAL pool object" >&2
    exit 3
fi

"$cc" "${arm_flags[@]}" \
    -DISAAC_VITA_OPENAL_POOL_ACTIVE=1 \
    -I"$root/runtime" \
    -c "$root/runtime/host_vita_openal_pool.c" \
    -o "$work/openal-pool.production.o"

undefined_names()
{
    "$nm" -u "$1" | awk '$1 == "U" { print $2 }' | LC_ALL=C sort -u
}

undefined_names "$work/openal-pool.production.o" \
    > "$work/openal-pool.production.undefined"
cat > "$work/openal-pool.production.expected" <<'EOF'
aligned_alloc
calloc
free
malloc
memcpy
memset
realloc
sceClibMspaceCreate
sceClibMspaceFree
sceClibMspaceMalloc
sceKernelAllocMemBlock
sceKernelCreateMutex
sceKernelDeleteMutex
sceKernelFreeMemBlock
sceKernelGetMemBlockBase
sceKernelLockMutex
sceKernelUnlockMutex
strlen
EOF
LC_ALL=C sort -o "$work/openal-pool.production.expected" \
    "$work/openal-pool.production.expected"
if ! cmp -s "$work/openal-pool.production.expected" \
              "$work/openal-pool.production.undefined"; then
    echo "OpenAL pool production undefined-symbol allow-list changed" >&2
    diff -u "$work/openal-pool.production.expected" \
        "$work/openal-pool.production.undefined" >&2 || true
    exit 1
fi

undefined_names "$work/openal-pool.real-header.o" \
    > "$work/openal-pool.testing.undefined"
cp "$work/openal-pool.production.expected" \
   "$work/openal-pool.testing.expected"
printf '%s\n' sceClibMspaceDestroy >> "$work/openal-pool.testing.expected"
LC_ALL=C sort -u -o "$work/openal-pool.testing.expected" \
    "$work/openal-pool.testing.expected"
if ! cmp -s "$work/openal-pool.testing.expected" \
              "$work/openal-pool.testing.undefined"; then
    echo "OpenAL pool testing undefined-symbol allow-list changed" >&2
    diff -u "$work/openal-pool.testing.expected" \
        "$work/openal-pool.testing.undefined" >&2 || true
    exit 1
fi
if grep -Eq '^(__atomic|__sync|mspace_)' \
       "$work/openal-pool.testing.undefined"; then
    echo "OpenAL pool object gained a forbidden helper/import" >&2
    exit 1
fi

cat > "$work/arm-main.c" <<'EOF'
#include <stdint.h>
#include "host_vita_openal_pool.h"

int main(void)
{
    isaac_vita_openal_pool_snapshot snapshot;
    void *pointer;

    (void)isaac_vita_openal_pool_initialize(0x98000000U, 0x9885f000U);
    pointer = isaac_vita_openal_pool_malloc(32U);
    pointer = isaac_vita_openal_pool_realloc(pointer, 64U);
    isaac_vita_openal_pool_free(pointer);
    pointer = isaac_vita_openal_pool_calloc(2U, 32U);
    isaac_vita_openal_pool_free(pointer);
    pointer = isaac_vita_openal_pool_aligned_alloc(16U, 64U);
    isaac_vita_openal_pool_free(pointer);
    pointer = isaac_vita_openal_pool_strdup("vita");
    isaac_vita_openal_pool_free(pointer);
    (void)isaac_vita_openal_pool_snapshot_get(&snapshot);
    return snapshot.state == ISAAC_VITA_OPENAL_POOL_DRAIN_ONLY;
}
EOF

"$cc" "${arm_flags[@]}" \
    -DISAAC_VITA_OPENAL_POOL_ACTIVE=1 \
    -I"$root/runtime" \
    "$root/runtime/host_vita_openal_pool.c" "$work/arm-main.c" \
    -Wl,--gc-sections \
    -lSceLibKernel_stub -lSceSysmem_stub -lSceKernelThreadMgr_stub \
    -o "$work/openal-pool.elf"

"$readelf" -h "$work/openal-pool.elf" > "$work/openal-pool.elf.header"
"$readelf" -A "$work/openal-pool.elf" > "$work/openal-pool.elf.attributes"
grep -q 'Machine:[[:space:]]*ARM$' "$work/openal-pool.elf.header"
grep -q 'Version5 EABI, soft-float ABI' "$work/openal-pool.elf.header"
if grep -q 'Tag_ABI_VFP_args' "$work/openal-pool.elf.attributes"; then
    echo "OpenAL pool ARM link gained a hardfp VFP-args tag" >&2
    exit 1
fi
"$nm" "$work/openal-pool.elf" > "$work/openal-pool.elf.nm"
if grep -Eq ' U (__atomic|__sync|mspace_)' "$work/openal-pool.elf.nm"; then
    echo "OpenAL pool ARM link gained a forbidden helper" >&2
    exit 1
fi

bash -n "$root/vita/openal-overlay/build.sh"
sha256sum \
    "$root/runtime/host_vita_openal_pool.h" \
    "$root/runtime/host_vita_openal_pool.c" \
    "$root/runtime/host_vita_openal_pool_oracle.c" \
    "$root/vita/openal-overlay/openal_pool_redirect.h" \
    "$root/vita/openal-overlay/build.sh" \
    "$root/vita/test_openal_pool.sh" \
    "$work/openal-pool.production.o" \
    "$work/openal-pool.elf"
echo "Vita OpenAL dedicated mspace host/ARM gate: PASS (ARM linked, not executed)"
