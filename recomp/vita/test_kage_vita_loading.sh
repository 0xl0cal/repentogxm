#!/usr/bin/env bash
set -eu

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=${ISAAC_LOADING_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
if [ -n "${ISAAC_LOADING_TEST_OUT:-}" ]; then
    work=$ISAAC_LOADING_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-kage-loading.XXXXXX")
fi

host_cc=${CC:-cc}
cmake_bin=${CMAKE:-cmake}
cc="$VITASDK/bin/arm-vita-eabi-gcc"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"
nm="$VITASDK/bin/arm-vita-eabi-nm"
objdump="$VITASDK/bin/arm-vita-eabi-objdump"
common_flags="-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror"

# Build the default mode from a deliberately clean source mirror.  The ignored
# local Specialist conversion is never copied into this tree; a successful
# host and ARM build therefore proves the redistributable source contract.
clean_runtime="$work/clean-source/runtime"
mkdir -p "$clean_runtime"
for source in \
    kage_vita_loading.c \
    kage_vita_loading.h \
    kage_vita_io_profile.h \
    kage_vita_stall_probe.h \
    kage_vita_loading_fallback.inc \
    kage_vita_loading_test_vitagl.h \
    kage_vita_loading_oracle.c
do
    cp "$root/runtime/$source" "$clean_runtime/$source"
done
if [ -e "$clean_runtime/kage_vita_loading_specialist.inc" ]; then
    echo "clean loading oracle copied the local Specialist include" >&2
    exit 10
fi

"$host_cc" -std=c11 -O2 -Wall -Wextra -Werror \
    -DISAAC_KAGE_VITA_LOADING_ORACLE=1 \
    -DISAAC_VITA_STALL_PROBE=1 \
    -I"$clean_runtime" \
    "$clean_runtime/kage_vita_loading.c" \
    "$clean_runtime/kage_vita_loading_oracle.c" \
    -o "$work/kage-vita-loading-host-oracle"
"$work/kage-vita-loading-host-oracle"

"$host_cc" -std=c11 -O2 -Wall -Wextra -Werror \
    -DISAAC_KAGE_VITA_LOADING_ORACLE=1 \
    -DISAAC_KAGE_VITA_LOADING_PRESENTATION=0 \
    -DISAAC_VITA_STALL_PROBE=1 \
    -I"$clean_runtime" \
    "$clean_runtime/kage_vita_loading.c" \
    "$clean_runtime/kage_vita_loading_oracle.c" \
    -o "$work/kage-vita-loading-disabled-host-oracle"
"$work/kage-vita-loading-disabled-host-oracle"

# This one legacy host oracle deliberately leaves GUEST_STACK_REQUIRED off so
# its zeroed CPU can exercise only the fread boundary.  Current host GCC warns
# about guest.h's legacy `guarded` out-parameter despite both successful paths
# assigning it.  Keep that warning visible and local; the exact production
# GUEST_STACK_REQUIRED=1 objects below retain unqualified -Werror.
"$host_cc" -std=c11 -O2 -Wall -Wextra -Werror \
    -Wno-error=maybe-uninitialized \
    -DISAAC_KAGE_VITA_LOADING_ORACLE=1 \
    -DGUEST_IMAGE_BASE=0x98000000u \
    -I"$clean_runtime" -I"$root/runtime" -I"$root/vita" \
    "$root/runtime/host_vita_first_fault.c" \
    "$clean_runtime/kage_vita_loading.c" \
    "$root/runtime/kage_vita_loading_override_oracle.c" \
    -o "$work/kage-vita-loading-override-host-oracle"
"$work/kage-vita-loading-override-host-oracle"

"$cc" $common_flags -DISAAC_KAGE_VITA_LOADING_ORACLE=1 \
    -DISAAC_VITA_STALL_PROBE=1 \
    -I"$clean_runtime" \
    "$clean_runtime/kage_vita_loading.c" \
    "$clean_runtime/kage_vita_loading_oracle.c" \
    -Wl,--gc-sections \
    -o "$work/kage-vita-loading-softfp-oracle.elf"
"$cc" $common_flags -DISAAC_KAGE_VITA_LOADING_ORACLE=1 \
    -DISAAC_KAGE_VITA_LOADING_PRESENTATION=0 \
    -DISAAC_VITA_STALL_PROBE=1 \
    -I"$clean_runtime" \
    "$clean_runtime/kage_vita_loading.c" \
    "$clean_runtime/kage_vita_loading_oracle.c" \
    -Wl,--gc-sections \
    -o "$work/kage-vita-loading-disabled-softfp-oracle.elf"

"$cc" $common_flags -DGUEST_IMAGE_BASE=0x98000000u \
    -DGUEST_STACK_REQUIRED=1 -DISAAC_VITA_HEAP_MB=64 \
    -DISAAC_VITA_HAS_RUNTIME=1 -DISAAC_VITA_STALL_PROBE=1 \
    -I"$clean_runtime" -I"$root/runtime" -I"$root/vita" \
    -c "$clean_runtime/kage_vita_loading.c" \
    -o "$work/kage_vita_loading.production.o"
"$cc" $common_flags -DGUEST_IMAGE_BASE=0x98000000u \
    -DGUEST_STACK_REQUIRED=1 -DISAAC_VITA_HEAP_MB=64 \
    -DISAAC_VITA_HAS_RUNTIME=1 -DISAAC_VITA_STALL_PROBE=1 \
    -DISAAC_KAGE_VITA_LOADING_PRESENTATION=1 \
    -I"$clean_runtime" -I"$root/runtime" -I"$root/vita" \
    -c "$clean_runtime/kage_vita_loading.c" \
    -o "$work/kage_vita_loading.production-explicit-on.o"
"$cc" $common_flags -DGUEST_IMAGE_BASE=0x98000000u \
    -DGUEST_STACK_REQUIRED=1 -DISAAC_VITA_HEAP_MB=64 \
    -DISAAC_VITA_HAS_RUNTIME=1 -DISAAC_VITA_STALL_PROBE=1 \
    -DISAAC_KAGE_VITA_LOADING_PRESENTATION=0 \
    -I"$clean_runtime" -I"$root/runtime" -I"$root/vita" \
    -c "$clean_runtime/kage_vita_loading.c" \
    -o "$work/kage_vita_loading.production-disabled.o"
if ! cmp -s "$work/kage_vita_loading.production.o" \
        "$work/kage_vita_loading.production-explicit-on.o"; then
    echo "explicit loading presentation ON changed the legacy object bytes" >&2
    exit 14
fi
"$cc" $common_flags -DGUEST_IMAGE_BASE=0x98000000u \
    -DGUEST_STACK_REQUIRED=1 -DISAAC_VITA_HEAP_MB=64 \
    -DISAAC_VITA_HAS_RUNTIME=1 -DISAAC_VITA_AUDIO=1 \
    -DISAAC_VITA_XINPUT=1 \
    -I"$root/runtime" -I"$root/vita" \
    -c "$root/runtime/host_vita_first_fault.c" \
    -o "$work/host_vita_first_fault.production.o"
"$cc" -r \
    "$work/host_vita_first_fault.production.o" \
    "$work/kage_vita_loading.production.o" \
    -o "$work/loading-strong-over-weak.o"

"$readelf" -h "$work/kage-vita-loading-softfp-oracle.elf" \
    > "$work/loading.header"
"$readelf" -A "$work/kage-vita-loading-softfp-oracle.elf" \
    > "$work/loading.attributes"
"$readelf" -h "$work/kage-vita-loading-disabled-softfp-oracle.elf" \
    > "$work/loading-disabled.header"
"$readelf" -A "$work/kage-vita-loading-disabled-softfp-oracle.elf" \
    > "$work/loading-disabled.attributes"
"$readelf" -A "$work/kage_vita_loading.production.o" \
    > "$work/loading-production.attributes"
"$readelf" -A "$work/kage_vita_loading.production-disabled.o" \
    > "$work/loading-production-disabled.attributes"
"$nm" -u "$work/kage-vita-loading-softfp-oracle.elf" \
    > "$work/loading.undefined"
"$nm" -u "$work/kage-vita-loading-disabled-softfp-oracle.elf" \
    > "$work/loading-disabled.undefined"
"$nm" -u "$work/kage_vita_loading.production.o" \
    > "$work/loading-production.undefined"
"$nm" -u "$work/kage_vita_loading.production-disabled.o" \
    > "$work/loading-production-disabled.undefined"
"$nm" "$work/kage_vita_loading.production-disabled.o" \
    > "$work/loading-production-disabled.symbols"
"$nm" "$work/loading-strong-over-weak.o" \
    > "$work/loading-combined.symbols"
"$objdump" -dr "$work/loading-strong-over-weak.o" \
    > "$work/loading-combined.disassembly"

"$cmake_bin" \
    "-DISAAC_LOADING_CMAKE_ROOT=$root" \
    "-DISAAC_LOADING_CMAKE_OUT=$work/cmake-selection" \
    -P "$root/vita/test_kage_vita_loading_cmake.cmake"

# The private conversion remains available for an explicit local-only oracle,
# but is neither required nor even inspected by the default run.
case "${ISAAC_LOADING_TEST_SPECIALIST:-0}" in
0) ;;
1)
    if [ ! -f "$root/runtime/kage_vita_loading_specialist.inc" ]; then
        echo "ISAAC_LOADING_TEST_SPECIALIST=1 requires the local ignored include" >&2
        exit 11
    fi
    "$host_cc" -std=c11 -O2 -Wall -Wextra -Werror \
        -DISAAC_KAGE_VITA_LOADING_ORACLE=1 \
        -DISAAC_KAGE_VITA_LOADING_SPECIALIST=1 \
        -DISAAC_VITA_STALL_PROBE=1 \
        -I"$root/runtime" \
        "$root/runtime/kage_vita_loading.c" \
        "$root/runtime/kage_vita_loading_oracle.c" \
        -o "$work/kage-vita-loading-specialist-local-oracle"
    "$work/kage-vita-loading-specialist-local-oracle"
    ;;
*)
    echo "ISAAC_LOADING_TEST_SPECIALIST must be 0 or 1" >&2
    exit 12
    ;;
esac

cat "$work/loading.header"
cat "$work/loading.attributes"
cat "$work/loading-production.undefined"

if ! grep -q "soft-float ABI" "$work/loading.header"; then
    echo "Vita loading oracle is not marked soft-float ABI" >&2
    exit 3
fi
if ! grep -q "soft-float ABI" "$work/loading-disabled.header"; then
    echo "disabled Vita loading oracle is not marked soft-float ABI" >&2
    exit 18
fi
if grep -q "Tag_ABI_VFP_args" "$work/loading.attributes" ||
   grep -q "Tag_ABI_VFP_args" "$work/loading-disabled.attributes" ||
   grep -q "Tag_ABI_VFP_args" "$work/loading-production.attributes" ||
   grep -q "Tag_ABI_VFP_args" \
        "$work/loading-production-disabled.attributes"; then
    echo "Vita loading object unexpectedly advertises hardfp arguments" >&2
    exit 4
fi
if [ -s "$work/loading.undefined" ]; then
    cat "$work/loading.undefined" >&2
    echo "Vita loading oracle has unresolved symbols" >&2
    exit 5
fi
if [ -s "$work/loading-disabled.undefined" ]; then
    cat "$work/loading-disabled.undefined" >&2
    echo "disabled Vita loading oracle has unresolved symbols" >&2
    exit 19
fi
if grep -Eq '(__atomic|libatomic)' "$work/loading-production.undefined"; then
    echo "32-bit loading snapshot unexpectedly depends on libatomic" >&2
    exit 6
fi
if ! grep -Eq '^[[:xdigit:]]+[[:space:]]+T[[:space:]]+kage_vita_loading_note_fread$' \
        "$work/loading-combined.symbols"; then
    cat "$work/loading-combined.symbols" >&2
    echo "strong loading hook did not override the non-KAGE weak stub" >&2
    exit 7
fi
if ! grep -Eq 'R_ARM_THM_CALL[[:space:]]+kage_vita_loading_note_fread$' \
        "$work/loading-combined.disassembly"; then
    echo "actual fread dispatcher does not relocate its call to the strong hook" >&2
    exit 9
fi
for symbol in \
    sceClibPrintf sceGxmDisplayQueueFinish sceKernelGetProcessTimeWide \
    sceKernelGetThreadId vglSetDisplayCallback vglSwapBuffers \
    kage_vita_stall_trace_sync
do
    if ! grep -Eq "[[:space:]]U[[:space:]]+$symbol$" \
            "$work/loading-production.undefined"; then
        echo "production loading object is missing expected dependency: $symbol" >&2
        exit 8
    fi
done

for symbol in \
    sceGxmDisplayQueueFinish vglSetDisplayCallback vglSwapBuffers \
    kage_vita_stall_trace_sync
do
    if grep -Eq "[[:space:]]U[[:space:]]+$symbol$" \
            "$work/loading-production-disabled.undefined"; then
        echo "presentation-OFF loading object retained display dependency: $symbol" >&2
        exit 15
    fi
done
for symbol in \
    sceClibPrintf sceKernelGetProcessTimeWide sceKernelGetThreadId \
    kage_vita_stall_note_loading_swap
do
    if ! grep -Eq "[[:space:]]U[[:space:]]+$symbol$" \
            "$work/loading-production-disabled.undefined"; then
        echo "presentation-OFF loading object lost lifecycle dependency: $symbol" >&2
        exit 16
    fi
done
for symbol in kage_vita_io_profile_begin kage_vita_io_profile_report
do
    if ! grep -Eq "^[[:xdigit:]]+[[:space:]]+W[[:space:]]+$symbol$" \
            "$work/loading-production-disabled.symbols"; then
        echo "presentation-OFF loading object lost weak profile edge: $symbol" >&2
        exit 17
    fi
done

if [ "$(grep -Fc 'vglSwapBuffers(GL_FALSE);' \
        "$root/runtime/kage_vita_loading.c")" -ne 1 ]; then
    echo "loading swap trace no longer owns one central raw swap call" >&2
    exit 12
fi
if [ "$(grep -Fc 'queue_finish_result = sceGxmDisplayQueueFinish();' \
        "$root/runtime/kage_vita_loading.c")" -ne 1 ] ||
   ! grep -Fq '"queue_finish=0x%08x\n"' \
        "$root/runtime/kage_vita_loading.c"; then
    echo "loading handoff no longer captures and reports display-queue finish rc" >&2
    exit 13
fi

sha256sum \
    "$root/runtime/kage_vita_loading.h" \
    "$root/runtime/kage_vita_loading.c" \
    "$root/runtime/kage_vita_io_profile.h" \
    "$root/runtime/kage_vita_stall_probe.h" \
    "$root/runtime/kage_vita_loading_fallback.inc" \
    "$root/runtime/kage_vita_loading_test_vitagl.h" \
    "$root/runtime/kage_vita_loading_oracle.c" \
    "$root/runtime/kage_vita_loading_override_oracle.c" \
    "$root/runtime/host_vita_first_fault.c" \
    "$root/runtime/host_vita_import_id.h" \
    "$root/runtime/kage_vita_generated_hooks.c" \
    "$root/runtime/kage_vita_backend.c" \
    "$root/vita/CMakeLists.txt" \
    "$root/vita/kage_vita_loading_selection.cmake" \
    "$root/vita/test_kage_vita_loading.sh" \
    "$root/vita/test_kage_vita_loading_cmake.cmake" \
    "$work/kage-vita-loading-host-oracle" \
    "$work/kage-vita-loading-disabled-host-oracle" \
    "$work/kage-vita-loading-override-host-oracle" \
    "$work/kage-vita-loading-softfp-oracle.elf" \
    "$work/kage-vita-loading-disabled-softfp-oracle.elf" \
    "$work/kage_vita_loading.production.o" \
    "$work/kage_vita_loading.production-disabled.o"
echo "Vita loading presentation ON byte-stable + OFF no-display + clean-clone fallback + CMake selection + host behavior + softfp compile/link/static ABI check: PASS (softfp ELF was not executed)"
