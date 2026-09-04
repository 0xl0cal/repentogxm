#!/usr/bin/env bash
set -eu

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=${ISAAC_TEXTURE_MEMORY_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
work=${ISAAC_TEXTURE_MEMORY_TEST_OUT:-$(mktemp -d "${TMPDIR:-/tmp}/isaac-texture-memory.XXXXXX")}
mkdir -p "$work"
host_cc=${CC:-cc}
cc="$VITASDK/bin/arm-vita-eabi-gcc"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"
nm="$VITASDK/bin/arm-vita-eabi-nm"
defines="-DGUEST_IMAGE_BASE=0x98000000u -DISAAC_VITA_TEXEL_OOM_DIAGNOSTIC=1 -DISAAC_VITA_TEXTURE_ALIGN8_POLICY=1 -DISAAC_VITA_TEXTURE_MEMORY_ORACLE=1"
flags="-std=gnu11 -O2 -Wall -Wextra -Werror $defines -I$root/runtime -I$root/vita"
arm_flags="$flags -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -ffunction-sections -fdata-sections"

"$host_cc" $flags \
    "$root/runtime/kage_vita_texture_memory.c" \
    "$root/runtime/kage_vita_texture_memory_oracle.c" \
    -o "$work/kage-vita-texture-memory-host-oracle"
"$work/kage-vita-texture-memory-host-oracle"

"$cc" $arm_flags \
    "$root/runtime/kage_vita_texture_memory.c" \
    "$root/runtime/kage_vita_texture_memory_oracle.c" \
    -Wl,--gc-sections \
    -o "$work/kage-vita-texture-memory-softfp-oracle.elf"

production_flags="-std=gnu11 -O2 -Wall -Wextra -Werror -DGUEST_IMAGE_BASE=0x98000000u -DGUEST_STACK_REQUIRED=1 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -ffunction-sections -fdata-sections -I$root/runtime -I$root/vita"
"$cc" $production_flags -DISAAC_VITA_TEXEL_OOM_DIAGNOSTIC=1 -c \
    "$root/runtime/kage_vita_texture_memory.c" \
    -o "$work/kage-vita-texel-oom.production.o"
"$cc" $production_flags -DISAAC_VITA_TEXTURE_ALIGN8_POLICY=1 -c \
    "$root/runtime/kage_vita_texture_memory.c" \
    -o "$work/kage-vita-align8.production.o"
"$cc" $production_flags -c \
    "$root/runtime/kage_vita_texture_memory.c" \
    -o "$work/kage-vita-texture-memory.off.o"
"$cc" $production_flags -DISAAC_VITA_TEXEL_OOM_DIAGNOSTIC=1 \
    -DISAAC_VITA_HEAP_RANGE_LEASE=1 -c \
    "$root/runtime/host_vita_heap.c" \
    -o "$work/host-vita-heap.texel-oom.o"
"$cc" $production_flags -DISAAC_VITA_HAS_RUNTIME=1 \
    -DISAAC_VITA_KAGE=1 -DISAAC_VITA_TEXTURE_ALIGN8_POLICY=1 -c \
    "$root/runtime/entry_vita.c" \
    -o "$work/entry-vita.align8.o"
"$cc" -r "$work/host-vita-heap.texel-oom.o" \
    "$work/kage-vita-texel-oom.production.o" \
    -o "$work/texel-oom-seam.combined.o"
"$cc" -r "$work/entry-vita.align8.o" \
    "$work/kage-vita-align8.production.o" \
    -o "$work/align8-seam.combined.o"

"$readelf" -h "$work/kage-vita-texture-memory-softfp-oracle.elf" > "$work/header"
"$readelf" -A "$work/kage-vita-texture-memory-softfp-oracle.elf" > "$work/attributes"
"$nm" -u "$work/kage-vita-texture-memory-softfp-oracle.elf" > "$work/undefined"
cat "$work/header"
cat "$work/attributes"
if ! grep -q "soft-float ABI" "$work/header"; then
    echo "texture-memory oracle is not marked soft-float ABI" >&2
    exit 3
fi
if grep -q "Tag_ABI_VFP_args" "$work/attributes"; then
    echo "texture-memory oracle advertises hardfp arguments" >&2
    exit 4
fi
if [ -s "$work/undefined" ]; then
    cat "$work/undefined" >&2
    echo "texture-memory oracle has unresolved symbols" >&2
    exit 5
fi
if "$nm" "$work/kage-vita-texture-memory-softfp-oracle.elf" | \
        grep -Eq '__atomic|libatomic'; then
    echo "texture-memory oracle unexpectedly depends on libatomic" >&2
    exit 6
fi
if ! "$nm" "$work/kage-vita-texel-oom.production.o" | \
        grep -q ' T kage_vita_texel_oom_diagnostic$' || \
   "$nm" "$work/kage-vita-texel-oom.production.o" | \
        grep -q 'kage_vita_texture_align8_policy_apply'; then
    echo "texel OOM production gate leaked or omitted a policy" >&2
    exit 7
fi
if ! "$nm" "$work/kage-vita-align8.production.o" | \
        grep -q ' T kage_vita_texture_align8_policy_apply$' || \
   "$nm" "$work/kage-vita-align8.production.o" | \
        grep -q 'kage_vita_texel_oom_diagnostic'; then
    echo "align8 production gate leaked or omitted a policy" >&2
    exit 8
fi
if "$nm" "$work/kage-vita-texture-memory.off.o" | \
        grep -q 'kage_vita_'; then
    echo "default-OFF texture memory object retained runtime symbols" >&2
    exit 9
fi
if "$nm" -u "$work/texel-oom-seam.combined.o" | \
        grep -q 'kage_vita_texel_oom_diagnostic'; then
    echo "heap-to-texel-OOM diagnostic seam did not resolve" >&2
    exit 10
fi
if "$nm" -u "$work/align8-seam.combined.o" | \
        grep -q 'kage_vita_texture_align8_policy_apply'; then
    echo "entry-to-align8 policy seam did not resolve" >&2
    exit 11
fi

sha256sum \
    "$root/runtime/kage_vita_texture_memory.h" \
    "$root/runtime/kage_vita_texture_memory.c" \
    "$root/runtime/kage_vita_texture_memory_oracle.c" \
    "$root/vita/test_kage_vita_texture_memory.sh" \
    "$work/kage-vita-texture-memory-host-oracle" \
    "$work/kage-vita-texture-memory-softfp-oracle.elf" \
    "$work/kage-vita-texel-oom.production.o" \
    "$work/kage-vita-align8.production.o" \
    "$work/kage-vita-texture-memory.off.o" \
    "$work/texel-oom-seam.combined.o" \
    "$work/align8-seam.combined.o"
echo "Vita KAGE texture memory host behavior + softfp static ABI: PASS (ARM ELF was not executed)"
