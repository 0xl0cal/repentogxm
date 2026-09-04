#!/usr/bin/env bash
set -eu

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=${ISAAC_ANM2_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
work=${ISAAC_ANM2_TEST_OUT:-$(mktemp -d "${TMPDIR:-/tmp}/isaac-anm2-scratch.XXXXXX")}
platform_include=${ISAAC_ANM2_PLATFORM_INCLUDE:-$root/vita}
guest_include=${ISAAC_ANM2_GUEST_INCLUDE:-$root/runtime}
heap_source=${ISAAC_ANM2_HEAP_SOURCE:-$root/runtime/host_vita_heap.c}
mkdir -p "$work"

host_cc=${CC:-cc}
host_nm=${NM:-nm}
cc="$VITASDK/bin/arm-vita-eabi-gcc"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"
nm="$VITASDK/bin/arm-vita-eabi-nm"
fake_include="$root/vita/anm2_scratch_oracle_include"
common="-std=gnu11 -O2 -Wall -Wextra -Werror -I$root/runtime -I$platform_include"
arm="$common -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -ffunction-sections -fdata-sections"

"$host_cc" $common -pthread -DISAAC_VITA_ANM2_SCRATCH_ORACLE=1 \
    -I"$fake_include" \
    "$root/runtime/host_vita_anm2_scratch.c" \
    "$root/runtime/host_vita_anm2_scratch_oracle.c" \
    -o "$work/anm2-scratch-host-oracle"
"$work/anm2-scratch-host-oracle"

# Compile the actual heap-import owner with both default-OFF scratch seams and
# the diagnostic ON.  This also compiles the cross-header fixed-map assertions.
# The routing oracle freezes their order: exact raw ANM2 owner first, exact PNG
# EBP chain second, ordinary guest malloc and its diagnostic last.
routing_common="-std=gnu11 -O2 -Wall -Wextra -Werror \
-DGUEST_IMAGE_BASE=0x98000000u -DGUEST_STACK_REQUIRED=1 \
-ffunction-sections -fdata-sections \
-I$guest_include -I$root/runtime -I$platform_include -I$fake_include"
"$host_cc" $routing_common -DISAAC_VITA_ANM2_SCRATCH=1 \
    -DISAAC_VITA_TEXEL_SCRATCH=1 \
    -DISAAC_VITA_TEXEL_OOM_DIAGNOSTIC=1 \
    -DISAAC_VITA_HEAP_RANGE_LEASE=1 \
    -Dmalloc=oracle_guest_malloc -c "$heap_source" \
    -o "$work/host-vita-heap.routing.o"
"$host_cc" $routing_common -c "$root/runtime/guest.c" \
    -o "$work/guest.routing.o"
"$host_cc" $routing_common -DISAAC_VITA_ANM2_SCRATCH_ORACLE=1 \
    -c "$root/runtime/host_vita_anm2_scratch.c" \
    -o "$work/anm2-scratch.routing-state.o"
"$host_cc" $routing_common -DISAAC_VITA_TEXEL_SCRATCH_ORACLE=1 \
    -c "$root/runtime/host_vita_texel_scratch.c" \
    -o "$work/texel-scratch.routing-state.o"
"$host_cc" $routing_common -DISAAC_VITA_ANM2_SCRATCH_ORACLE=1 \
    -DISAAC_VITA_TEXEL_SCRATCH_ORACLE=1 \
    -c "$root/runtime/host_vita_anm2_scratch_routing_oracle.c" \
    -o "$work/anm2-scratch.routing-oracle.o"
"$host_cc" -pthread \
    "$work/host-vita-heap.routing.o" \
    "$work/guest.routing.o" \
    "$work/anm2-scratch.routing-state.o" \
    "$work/texel-scratch.routing-state.o" \
    "$work/anm2-scratch.routing-oracle.o" \
    -Wl,--gc-sections \
    -o "$work/anm2-scratch-routing-oracle"
"$work/anm2-scratch-routing-oracle"

# The behavioral routing executable must resolve the heap owner's REQUIRED1
# stack edges from the actual production runtime, not from oracle stubs.
"$host_nm" --defined-only "$work/guest.routing.o" \
    > "$work/guest.routing.defined"
for symbol in guest_stack_address gpop_at guest_stack_violation
do
    if ! grep -Eq "[[:space:]]T[[:space:]]+$symbol$" \
            "$work/guest.routing.defined"; then
        cat "$work/guest.routing.defined" >&2
        echo "production guest routing object misses $symbol" >&2
        exit 10
    fi
done

# Compile once against the real VitaSDK declaration and once against the
# project-local fake.  The source uses only the NULL-option API surface, so
# byte-identical objects pin the fake header to the real contract it models.
"$cc" $arm -c "$root/runtime/host_vita_anm2_scratch.c" \
    -o "$work/anm2-scratch.actual-header.o"
"$cc" $arm -I"$fake_include" \
    -c "$root/runtime/host_vita_anm2_scratch.c" \
    -o "$work/anm2-scratch.fake-header.o"
if ! cmp -s "$work/anm2-scratch.actual-header.o" \
              "$work/anm2-scratch.fake-header.o"; then
    echo "fake sysmem header changed the production object" >&2
    exit 3
fi

"$cc" $arm -I"$fake_include" \
    "$root/runtime/host_vita_anm2_scratch.c" \
    "$root/runtime/host_vita_anm2_scratch_link_probe.c" \
    -Wl,--gc-sections -o "$work/anm2-scratch-softfp-link.elf"

"$readelf" -h "$work/anm2-scratch-softfp-link.elf" > "$work/header"
"$readelf" -A "$work/anm2-scratch-softfp-link.elf" > "$work/attributes"
"$nm" -u "$work/anm2-scratch.actual-header.o" > "$work/production.undefined"
"$nm" -u "$work/anm2-scratch-softfp-link.elf" > "$work/link.undefined"
cat "$work/header"
cat "$work/attributes"

if ! grep -q "Version5 EABI, soft-float ABI" "$work/header"; then
    echo "ANM2 scratch link is not ARM EABI5 softfp" >&2
    exit 4
fi
if grep -q "Tag_ABI_VFP_args" "$work/attributes"; then
    echo "ANM2 scratch link advertises hardfp arguments" >&2
    exit 5
fi
if [ -s "$work/link.undefined" ]; then
    cat "$work/link.undefined" >&2
    echo "fake-sysmem ARM link has unresolved symbols" >&2
    exit 6
fi

for symbol in isaac_vita_log sceKernelAllocMemBlock \
              sceKernelFreeMemBlock sceKernelGetMemBlockBase
do
    if ! grep -Eq "[[:space:]]U[[:space:]]+$symbol$" \
            "$work/production.undefined"; then
        cat "$work/production.undefined" >&2
        echo "production ANM2 object misses dependency: $symbol" >&2
        exit 7
    fi
done
if [ "$(wc -l < "$work/production.undefined")" -ne 4 ]; then
    cat "$work/production.undefined" >&2
    echo "production ANM2 object gained an unexpected dependency" >&2
    exit 8
fi
if "$nm" "$work/anm2-scratch.actual-header.o" \
        "$work/anm2-scratch-softfp-link.elf" | \
        grep -Eq '(__atomic|libatomic|[[:space:]](malloc|calloc|realloc|free)$)'; then
    echo "ANM2 scratch reached libatomic or a libc allocator" >&2
    exit 9
fi

python3 "$root/test_linker_map_contract.py"

if [ -n "${ISAAC_ANM2_PE_PATH:-}" ] && \
   [ -n "${ISAAC_ANM2_GENERATED_DIR:-}" ]; then
    python3 "$root/test_ogg_queue_error_cleanup.py" \
        --pe "$ISAAC_ANM2_PE_PATH" \
        --generated "$ISAAC_ANM2_GENERATED_DIR" \
        --exercise-generator
    if [ -n "${ISAAC_ANM2_MAP_PATH:-}" ]; then
        python3 "$root/test_anm2_scratch_census.py" \
            --pe "$ISAAC_ANM2_PE_PATH" \
            --generated "$ISAAC_ANM2_GENERATED_DIR" \
            --header "$root/runtime/host_vita_anm2_scratch.h" \
            --map "$ISAAC_ANM2_MAP_PATH" \
            --source-root "$root/.." \
            --vitasdk "$VITASDK"
    else
        python3 "$root/test_anm2_scratch_census.py" \
            --pe "$ISAAC_ANM2_PE_PATH" \
            --generated "$ISAAC_ANM2_GENERATED_DIR" \
            --header "$root/runtime/host_vita_anm2_scratch.h"
    fi
else
    echo "ANM2/Ogg frozen census: SKIP (set ISAAC_ANM2_PE_PATH and ISAAC_ANM2_GENERATED_DIR)"
fi

sha256sum \
    "$root/runtime/host_vita_anm2_scratch.h" \
    "$root/runtime/host_vita_anm2_scratch.c" \
    "$root/runtime/host_vita_anm2_scratch_oracle.c" \
    "$root/runtime/host_vita_anm2_scratch_routing_oracle.c" \
    "$root/runtime/host_vita_texel_scratch.h" \
    "$root/runtime/host_vita_texel_scratch.c" \
    "$root/vita/anm2_scratch_oracle_include/windows.h" \
    "$root/runtime/host_vita_anm2_scratch_link_probe.c" \
    "$root/vita/anm2_scratch_oracle_include/psp2/kernel/sysmem.h" \
    "$root/vita/test_anm2_scratch.sh" \
    "$root/linker_map_contract.py" \
    "$root/test_linker_map_contract.py" \
    "$root/test_anm2_scratch_census.py" \
    "$root/test_ogg_queue_error_cleanup.py" \
    "$work/anm2-scratch-host-oracle" \
    "$work/anm2-scratch-routing-oracle" \
    "$work/guest.routing.o" \
    "$work/anm2-scratch.actual-header.o" \
    "$work/anm2-scratch-softfp-link.elf"
echo "Vita ANM2 scratch host lifecycle + memblock contract + ARM EABI5 softfp/link: PASS (ARM ELF was not executed)"
