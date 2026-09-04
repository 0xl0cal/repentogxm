#!/usr/bin/env bash
set -euo pipefail

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=${ISAAC_FIOS_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
if [ -n "${ISAAC_FIOS_TEST_OUT:-}" ]; then
    work=$ISAAC_FIOS_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-fios-cache.XXXXXX")
fi

fake="$work/fake-include"
mkdir -p "$fake/psp2/kernel" "$fake/psp2"
cat > "$fake/psp2/types.h" <<'EOF'
#ifndef ISAAC_FIOS_FAKE_TYPES_H
#define ISAAC_FIOS_FAKE_TYPES_H
#include <stdint.h>
typedef int32_t SceUID;
typedef uint32_t SceSize;
#endif
EOF
cat > "$fake/psp2/kernel/sysmem.h" <<'EOF'
#ifndef ISAAC_FIOS_FAKE_SYSMEM_H
#define ISAAC_FIOS_FAKE_SYSMEM_H
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

host_gcc=${HOST_CC:-gcc}
host_clang=${HOST_CLANG:-clang}
gcc_path=$(command -v "$host_gcc")
clang_path=$(command -v "$host_clang")
if [ "$(readlink -f "$gcc_path")" = "$(readlink -f "$clang_path")" ]; then
    echo "FIOS cache gate requires distinct GCC and Clang compilers" >&2
    exit 2
fi
scenarios=(
    success cold-shutdown alloc-fail getbase-fail getbase-null layout-fail
    initialize-fail filter-fail filter-free-fail shutdown-free-fail
)
for compiler in "$host_gcc" "$host_clang"; do
    output="$work/fios-$(basename "$compiler")"
    "$compiler" -std=gnu11 -O1 -g -Wall -Wextra -Werror \
        -fno-omit-frame-pointer -fsanitize=address,undefined \
        -I"$fake" -I"$root/runtime" -I"$root/vita" \
        "$root/runtime/host_vita_fios_cache.c" \
        "$root/runtime/host_vita_fios_cache_oracle.c" \
        -o "$output"
    for scenario in "${scenarios[@]}"; do
        ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
        UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1 \
            "$output" "$scenario"
    done
done

cc="$VITASDK/bin/arm-vita-eabi-gcc"
nm="$VITASDK/bin/arm-vita-eabi-nm"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"
arm_flags=(
    -std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb
    -ffunction-sections -fdata-sections -Wall -Wextra -Werror
)
"$cc" "${arm_flags[@]}" -I"$root/runtime" -I"$root/vita" \
    -c "$root/runtime/host_vita_fios_cache.c" \
    -o "$work/host_vita_fios_cache.o"

"$nm" -u "$work/host_vita_fios_cache.o" | \
    awk '$1 == "U" { print $2 }' | LC_ALL=C sort -u \
    > "$work/undefined.actual"
cat > "$work/undefined.expected" <<'EOF'
memset
sceFiosIOFilterAdd
sceFiosIOFilterCache
sceFiosInitialize
sceFiosTerminate
sceKernelAllocMemBlock
sceKernelFreeMemBlock
sceKernelGetMemBlockBase
EOF
if ! cmp -s "$work/undefined.expected" "$work/undefined.actual"; then
    echo "FIOS production undefined-symbol closure changed" >&2
    diff -u "$work/undefined.expected" "$work/undefined.actual" >&2 || true
    exit 1
fi

cat > "$work/arm-main.c" <<'EOF'
#include "host_vita_fios_cache.h"
int main(void)
{
    isaac_vita_fios_cache_snapshot snapshot;
    (void)isaac_vita_fios_cache_initialize();
    isaac_vita_fios_cache_snapshot_get(&snapshot);
    isaac_vita_fios_cache_shutdown();
    return snapshot.memblock_bytes != ISAAC_VITA_FIOS_MEMBLOCK_BYTES;
}
EOF
"$cc" "${arm_flags[@]}" -I"$root/runtime" -I"$root/vita" \
    "$root/runtime/host_vita_fios_cache.c" "$work/arm-main.c" \
    -Wl,--gc-sections -lSceFios2_stub -lSceSysmem_stub \
    -o "$work/fios-cache.elf"
"$readelf" -h "$work/fios-cache.elf" > "$work/header"
"$readelf" -A "$work/fios-cache.elf" > "$work/attributes"
cat "$work/header"
cat "$work/attributes"
grep -q 'Machine:[[:space:]]*ARM$' "$work/header"
grep -q 'Version5 EABI, soft-float ABI' "$work/header"
if grep -q 'Tag_ABI_VFP_args' "$work/attributes"; then
    echo "FIOS cache ARM link advertises hardfp arguments" >&2
    exit 1
fi
echo "FIOS cache host fault matrix / ARM EABI5 softfp / import closure: PASS (ARM ELF was not executed)"
