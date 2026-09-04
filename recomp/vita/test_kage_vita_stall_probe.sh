#!/usr/bin/env bash
set -eu

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=${ISAAC_STALL_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
work=${ISAAC_STALL_TEST_OUT:-$(mktemp -d "${TMPDIR:-/tmp}/isaac-stall.XXXXXX")}
mkdir -p "$work"
host_cc=${CC:-cc}
cc="$VITASDK/bin/arm-vita-eabi-gcc"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"
nm="$VITASDK/bin/arm-vita-eabi-nm"
objdump="$VITASDK/bin/arm-vita-eabi-objdump"
oracle_build_id=oracle:stall:0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdefxyz
if [ "${#oracle_build_id}" -ne 96 ]; then
    echo "stall oracle build id is not the intended 96 characters" >&2
    exit 2
fi
flags="-std=gnu11 -O2 -Wall -Wextra -Werror -DISAAC_KAGE_VITA_STALL_ORACLE=1 -DISAAC_VITA_STALL_BUILD_ID=\"$oracle_build_id\" -I$root/runtime"
arm_flags="$flags -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -ffunction-sections -fdata-sections"

"$host_cc" $flags \
    "$root/runtime/kage_vita_stall_probe.c" \
    "$root/runtime/kage_vita_stall_probe_oracle.c" \
    -o "$work/kage-vita-stall-host-oracle"
"$work/kage-vita-stall-host-oracle"

"$cc" $arm_flags \
    "$root/runtime/kage_vita_stall_probe.c" \
    "$root/runtime/kage_vita_stall_probe_oracle.c" \
    -Wl,--gc-sections \
    -o "$work/kage-vita-stall-softfp-oracle.elf"

production_flags="-std=gnu11 -O2 -Wall -Wextra -Werror -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -ffunction-sections -fdata-sections -I$root/runtime"
"$cc" $production_flags \
    -c "$root/runtime/kage_vita_stall_probe.c" \
    -o "$work/kage-vita-stall.production.o"
"$cc" $production_flags \
    -c "$root/runtime/kage_vita_stall_gate_oracle.c" \
    -o "$work/kage-vita-stall-gate-off.o"
"$cc" $production_flags -DISAAC_VITA_STALL_PROBE=1 \
    -c "$root/runtime/kage_vita_stall_gate_oracle.c" \
    -o "$work/kage-vita-stall-gate-on.o"

"$readelf" -h "$work/kage-vita-stall-softfp-oracle.elf" \
    > "$work/header"
"$readelf" -A "$work/kage-vita-stall-softfp-oracle.elf" \
    > "$work/attributes"
"$nm" -u "$work/kage-vita-stall-softfp-oracle.elf" \
    > "$work/oracle.undefined"
"$nm" -u "$work/kage-vita-stall.production.o" \
    > "$work/production.undefined"
"$nm" -u "$work/kage-vita-stall-gate-off.o" \
    > "$work/gate-off.undefined"
"$nm" -u "$work/kage-vita-stall-gate-on.o" \
    > "$work/gate-on.undefined"
"$objdump" -dr "$work/kage-vita-stall-gate-off.o" \
    > "$work/gate-off.disassembly"
"$objdump" -dr "$work/kage-vita-stall-gate-on.o" \
    > "$work/gate-on.disassembly"
cat "$work/header"
cat "$work/attributes"
cat "$work/production.undefined"

if ! grep -q "soft-float ABI" "$work/header"; then
    echo "stall probe oracle is not marked soft-float ABI" >&2
    exit 3
fi
if grep -q "Tag_ABI_VFP_args" "$work/attributes"; then
    echo "stall probe oracle unexpectedly advertises hardfp arguments" >&2
    exit 4
fi
if [ -s "$work/oracle.undefined" ]; then
    cat "$work/oracle.undefined" >&2
    echo "stall probe oracle has unresolved symbols" >&2
    exit 5
fi
if "$nm" "$work/kage-vita-stall-softfp-oracle.elf" \
        "$work/kage-vita-stall.production.o" | grep -Eq '__atomic|libatomic'; then
    echo "32-bit stall probe unexpectedly depends on libatomic" >&2
    exit 6
fi
if [ -s "$work/gate-off.undefined" ] ||
   grep -q 'R_ARM.*kage_vita_stall_' "$work/gate-off.disassembly"; then
    cat "$work/gate-off.undefined" >&2
    cat "$work/gate-off.disassembly" >&2
    echo "stall probe OFF gate retained diagnostic work" >&2
    exit 8
fi
for symbol in \
    kage_vita_stall_probe_start kage_vita_stall_probe_stop \
    kage_vita_stall_note_loop kage_vita_stall_note_update \
    kage_vita_stall_note_render kage_vita_stall_note_render_return \
    kage_vita_stall_note_present_enter \
    kage_vita_stall_note_present_return \
    kage_vita_stall_note_guest_site kage_vita_stall_note_dispatch \
    kage_vita_stall_note_logical_fread \
    kage_vita_stall_note_loading_swap \
    kage_vita_stall_trace_sync
do
    if ! grep -Eq "[[:space:]]U[[:space:]]+$symbol$" \
            "$work/gate-on.undefined"; then
        cat "$work/gate-on.undefined" >&2
        echo "stall probe ON gate is missing owner relocation: $symbol" >&2
        exit 9
    fi
done
for symbol in \
    isaac_vita_log sceKernelCreateThread sceKernelDelayThread \
    sceKernelDeleteThread sceKernelGetProcessTimeWide sceKernelGetThreadId \
    sceKernelGetThreadInfo sceKernelStartThread sceKernelWaitThreadEnd \
    sceKernelPowerTick \
    kage_vita_preloop_phase_reset \
    kage_vita_preloop_phase_get_snapshot
do
    if ! grep -Eq "[[:space:]]U[[:space:]]+$symbol$" \
            "$work/production.undefined"; then
        echo "production stall probe is missing expected dependency: $symbol" >&2
        exit 10
    fi
done
if ! grep -Fq \
    'ISAAC_VITA_STALL_BUILD_ID=\"${ISAAC_VITA_GUEST_LINK_ID}\"' \
    "$root/vita/CMakeLists.txt"; then
    echo "production stall probe lost the configured executable build stamp" >&2
    exit 11
fi
if ! grep -Fq 'SceProcessmgr_stub' "$root/vita/CMakeLists.txt"; then
    echo "stall probe lost the power-tick link provider" >&2
    exit 11
fi
if grep -q 'sceClibPrintf' "$root/runtime/kage_vita_stall_probe.c"; then
    echo "stall records bypass the durable isaac_vita_log path" >&2
    exit 12
fi
require_one_wiring()
{
    source_file=$1
    marker_call=$2
    if [ ! -f "$source_file" ]; then
        echo "stall lifecycle owner is missing: $source_file" >&2
        exit 13
    fi
    call_count=$(grep -Fc "$marker_call" "$source_file" || true)
    case $call_count in
        ''|*[!0-9]*)
            echo "stall lifecycle count is not numeric: $marker_call" >&2
            exit 13
            ;;
    esac
    if [ "$call_count" -ne 1 ]; then
        echo "stall lifecycle wiring count $call_count, expected 1: $marker_call" >&2
        exit 13
    fi
}
require_one_wiring "$root/runtime/kage_vita_generated_hooks.c" \
    'KAGE_VITA_STALL_NOTE_LOOP();'
require_one_wiring "$root/runtime/kage_vita_generated_hooks.c" \
    'KAGE_VITA_STALL_NOTE_UPDATE();'
require_one_wiring "$root/runtime/kage_vita_generated_hooks.c" \
    'KAGE_VITA_STALL_NOTE_RENDER();'
require_one_wiring "$root/runtime/kage_vita_generated_hooks.c" \
    'KAGE_VITA_STALL_NOTE_RENDER_RETURN();'
require_one_wiring "$root/runtime/kage_vita_backend.c" \
    'KAGE_VITA_STALL_NOTE_PRESENT_ENTER(s_present_count);'
require_one_wiring "$root/runtime/kage_vita_backend.c" \
    'KAGE_VITA_STALL_NOTE_PRESENT_RETURN(s_present_count);'
require_one_wiring "$root/runtime/host_vita_first_fault.c" \
    'KAGE_VITA_STALL_NOTE_LOGICAL_FREAD(completed);'
require_one_wiring "$root/runtime/kage_vita_loading.c" \
    'KAGE_VITA_STALL_NOTE_LOADING_SWAP(s_loading_swaps);'

sha256sum \
    "$root/runtime/kage_vita_stall_probe.h" \
    "$root/runtime/kage_vita_stall_probe.c" \
    "$root/runtime/kage_vita_stall_probe_oracle.c" \
    "$root/runtime/kage_vita_stall_gate_oracle.c" \
    "$root/vita/test_kage_vita_stall_probe.sh" \
    "$work/kage-vita-stall-host-oracle" \
    "$work/kage-vita-stall-softfp-oracle.elf" \
    "$work/kage-vita-stall.production.o" \
    "$work/kage-vita-stall-gate-off.o" \
    "$work/kage-vita-stall-gate-on.o"
echo "Vita pre-loop heartbeat + first-only stage markers + bounded pre/post-present watchdog + softfp ABI + ON/OFF owner gate: PASS (ARM ELF was not executed)"
