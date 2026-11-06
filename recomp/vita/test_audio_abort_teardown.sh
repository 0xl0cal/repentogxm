#!/usr/bin/env bash
# ISAAC_VITA_ABORT_SKIP_AUDIO_TEARDOWN proof:
#   1. host oracle (knob ON): the abort-stop skip makes no OpenAL call, writes
#      no guest byte and leaves the owner; the Exit game teardown afterwards
#      keeps its census;
#   2. host adapter objects: the knob adds the skip symbol/receipt, its
#      absence has neither;
#   3. Vita objects (VITASDK set): entry_vita.c with the knob calls both the
#      skip and the full teardown between the KAGE deactivate and the final
#      telemetry, carries the "skipped" receipt and still ends in the same
#      coverage -> stack -> image -> process-exit order; without the knob it
#      has no skip relocation and no receipt; the knob with ISAAC_VITA_AUDIO=0
#      is refused at compile time.
set -eu

root=${ISAAC_AUDIO_ABORT_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
if [ -n "${ISAAC_AUDIO_ABORT_TEST_OUT:-}" ]; then
    work=$ISAAC_AUDIO_ABORT_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-audio-abort.XXXXXX")
fi
host_cc=${HOST_CC:-cc}
host_flags="-std=gnu11 -O2 -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror"
host_oracle_flags="$host_flags -Wno-error=maybe-uninitialized"
knob=-DISAAC_VITA_ABORT_SKIP_AUDIO_TEARDOWN=1
skip_symbol=isaac_vita_audio_abort_skip_teardown
abort_receipt="KAGE VITA AUDIO ABORT: owner=0x%08x context=%u device=%u names=%u/%u (OpenAL teardown skipped; process exit retires the device)"
skipped_receipt="KAGE VITA AUDIO SHUTDOWN: skipped (abort stop; OpenAL owner left to process exit)"
arm_status=SKIP

# 1. Execute the abort-stop skip against the production adapter on the host.
"$host_cc" $host_oracle_flags \
    -DISAAC_VITA_AUDIO_ORACLE=1 $knob \
    -DGUEST_IMAGE_BASE=0x27000000u \
    -I"$root/vita/audio_oracle_include" \
    -I"$root/runtime" -I"$root/vita" \
    "$root/runtime/host_vita_audio.c" \
    "$root/runtime/guest_stack_legacy_oracle_stub.c" \
    "$root/runtime/vita_audio_abort_teardown_oracle.c" \
    -Wl,--gc-sections \
    -o "$work/vita-audio-abort-host-oracle"
"$work/vita-audio-abort-host-oracle"

# 2. The knob is the only source of the skip symbol and its receipt.
"$host_cc" $host_oracle_flags -DISAAC_VITA_AUDIO_ORACLE=1 \
    -DGUEST_IMAGE_BASE=0x27000000u \
    -I"$root/vita/audio_oracle_include" -I"$root/runtime" -I"$root/vita" \
    -c "$root/runtime/host_vita_audio.c" -o "$work/host_vita_audio.host-off.o"
"$host_cc" $host_oracle_flags -DISAAC_VITA_AUDIO_ORACLE=1 $knob \
    -DGUEST_IMAGE_BASE=0x27000000u \
    -I"$root/vita/audio_oracle_include" -I"$root/runtime" -I"$root/vita" \
    -c "$root/runtime/host_vita_audio.c" -o "$work/host_vita_audio.host-on.o"
if nm "$work/host_vita_audio.host-off.o" | grep -q "[[:space:]]$skip_symbol$"; then
    echo "adapter without the knob defines $skip_symbol" >&2
    exit 3
fi
if strings "$work/host_vita_audio.host-off.o" | grep -Fq "KAGE VITA AUDIO ABORT:"; then
    echo "adapter without the knob carries the abort receipt" >&2
    exit 3
fi
if ! nm "$work/host_vita_audio.host-on.o" | grep -q "[[:space:]]T[[:space:]]*$skip_symbol$"; then
    echo "adapter with the knob does not define $skip_symbol" >&2
    exit 3
fi
if ! strings "$work/host_vita_audio.host-on.o" | grep -Fxq "$abort_receipt"; then
    echo "adapter with the knob lost the abort receipt" >&2
    exit 3
fi

# 3. Vita objects.
if [ -n "${VITASDK:-}" ]; then
    cc="$VITASDK/bin/arm-vita-eabi-gcc"
    nm_arm="$VITASDK/bin/arm-vita-eabi-nm"
    objdump="$VITASDK/bin/arm-vita-eabi-objdump"
    strings_arm="$VITASDK/bin/arm-vita-eabi-strings"
    common_flags="-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror"
    production_flags="$common_flags -DGUEST_STACK_REQUIRED=1"

    for mode in off on; do
        mode_flag=
        if [ "$mode" = on ]; then
            mode_flag=$knob
        fi
        "$cc" $production_flags -DGUEST_IMAGE_BASE=0x98000000u \
            -DISAAC_VITA_KAGE=1 -DISAAC_VITA_AUDIO=1 $mode_flag \
            -I"$root/runtime" -I"$root/vita" \
            -c "$root/runtime/entry_vita.c" \
            -o "$work/entry_vita.$mode.o"
        "$cc" $production_flags -DGUEST_IMAGE_BASE=0x98000000u \
            -DISAAC_VITA_AUDIO=1 $mode_flag \
            -I"$root/runtime" -I"$root/vita" \
            -c "$root/runtime/host_vita_audio.c" \
            -o "$work/host_vita_audio.$mode.o"
        "$objdump" -dr "$work/entry_vita.$mode.o" > "$work/entry_vita.$mode.disassembly"
        "$strings_arm" "$work/entry_vita.$mode.o" > "$work/entry_vita.$mode.strings"
        "$nm_arm" "$work/host_vita_audio.$mode.o" > "$work/host_vita_audio.$mode.symbols"
        "$strings_arm" "$work/host_vita_audio.$mode.o" > "$work/host_vita_audio.$mode.strings"
    done

    # The knob with the audio boundary compiled out is a configuration error
    # the unit itself refuses (CMake refuses it earlier).
    if "$cc" $production_flags -DGUEST_IMAGE_BASE=0x98000000u \
        -DISAAC_VITA_KAGE=1 -DISAAC_VITA_AUDIO=0 $knob \
        -I"$root/runtime" -I"$root/vita" \
        -c "$root/runtime/entry_vita.c" \
        -o "$work/entry_vita.audio-off-knob.o" 2> "$work/entry_vita.audio-off-knob.err"; then
        echo "entry accepted ISAAC_VITA_ABORT_SKIP_AUDIO_TEARDOWN with ISAAC_VITA_AUDIO=0" >&2
        exit 4
    fi
    if ! grep -q "ISAAC_VITA_ABORT_SKIP_AUDIO_TEARDOWN requires ISAAC_VITA_AUDIO=1" \
        "$work/entry_vita.audio-off-knob.err"; then
        cat "$work/entry_vita.audio-off-knob.err" >&2
        echo "entry refused the knob with ISAAC_VITA_AUDIO=0 for another reason" >&2
        exit 4
    fi

    entry_relocation_line()
    {
        awk -v symbol="$2" \
            '$0 ~ ("R_ARM_THM_(CALL|JUMP24)[[:space:]]+" symbol "$") {
                print NR
                exit
            }' "$1"
    }

    # OFF: no skip edge, no receipt; the full teardown edge as before.
    if grep -Eq "R_ARM_THM_(CALL|JUMP24)[[:space:]]+$skip_symbol$" \
            "$work/entry_vita.off.disassembly"; then
        echo "entry without the knob calls $skip_symbol" >&2
        exit 5
    fi
    if grep -Fq "KAGE VITA AUDIO SHUTDOWN: skipped" "$work/entry_vita.off.strings"; then
        echo "entry without the knob carries the skipped receipt" >&2
        exit 5
    fi
    if ! grep -Eq 'R_ARM_THM_(CALL|JUMP24)[[:space:]]+isaac_vita_audio_shutdown_active$' \
            "$work/entry_vita.off.disassembly"; then
        echo "entry without the knob lost the full teardown edge" >&2
        exit 5
    fi
    if grep -q "[[:space:]]$skip_symbol$" "$work/host_vita_audio.off.symbols" ||
       grep -Fq "KAGE VITA AUDIO ABORT:" "$work/host_vita_audio.off.strings"; then
        echo "Vita adapter without the knob carries the skip" >&2
        exit 5
    fi

    # ON: both edges between KAGE deactivate and the final telemetry, the
    # receipt present, the terminal order unchanged.
    if ! grep -Fxq "$skipped_receipt" "$work/entry_vita.on.strings"; then
        echo "entry with the knob lost the skipped receipt" >&2
        exit 6
    fi
    if ! grep -q "[[:space:]]T[[:space:]]*$skip_symbol$" "$work/host_vita_audio.on.symbols" ||
       ! grep -Fxq "$abort_receipt" "$work/host_vita_audio.on.strings"; then
        echo "Vita adapter with the knob lost the skip or its receipt" >&2
        exit 6
    fi
    # gcc duplicates the done: prologue per goto site once guest_exited is a
    # per-path constant (several kage_vita_backend_deactivate copies, each
    # branching to the one skip call or the one full-teardown call), so the
    # order is pinned on the single-instance edges: both audio calls come
    # after the first deactivate, and the shared tail after them is the
    # unchanged final telemetry -> coverage -> stack -> image -> exit.
    entry_relocation_count()
    {
        grep -cE "R_ARM_THM_(CALL|JUMP24)[[:space:]]+$2\$" "$1" || true
    }
    for single in $skip_symbol isaac_vita_audio_shutdown_active \
        isaac_vita_audio_log_final guest_coverage_shutdown guest_stack_free \
        guest_image_free sceKernelExitProcess; do
        if [ "$(entry_relocation_count "$work/entry_vita.on.disassembly" $single)" != 1 ]; then
            echo "entry with the knob does not call $single exactly once" >&2
            exit 6
        fi
    done
    on_kage=$(entry_relocation_line "$work/entry_vita.on.disassembly" kage_vita_backend_deactivate)
    on_skip=$(entry_relocation_line "$work/entry_vita.on.disassembly" $skip_symbol)
    on_full=$(entry_relocation_line "$work/entry_vita.on.disassembly" isaac_vita_audio_shutdown_active)
    on_final=$(entry_relocation_line "$work/entry_vita.on.disassembly" isaac_vita_audio_log_final)
    on_coverage=$(entry_relocation_line "$work/entry_vita.on.disassembly" guest_coverage_shutdown)
    on_stack=$(entry_relocation_line "$work/entry_vita.on.disassembly" guest_stack_free)
    on_image=$(entry_relocation_line "$work/entry_vita.on.disassembly" guest_image_free)
    on_exit=$(entry_relocation_line "$work/entry_vita.on.disassembly" sceKernelExitProcess)
    if [ -z "$on_kage" ]; then
        echo "entry with the knob lost the KAGE deactivate edge" >&2
        exit 6
    fi
    if [ "$on_kage" -ge "$on_skip" ] || [ "$on_kage" -ge "$on_full" ] ||
       [ "$on_final" -ge "$on_coverage" ] ||
       [ "$on_coverage" -ge "$on_stack" ] ||
       [ "$on_stack" -ge "$on_image" ] ||
       [ "$on_image" -ge "$on_exit" ]; then
        echo "entry with the knob is not KAGE -> {abort skip | full teardown} -> final telemetry -> coverage -> stack -> image -> process exit" >&2
        exit 6
    fi
    arm_status=PASS
fi

sha256sum \
    "$root/runtime/entry_vita.c" \
    "$root/runtime/host_vita_audio.h" \
    "$root/runtime/host_vita_audio.c" \
    "$root/runtime/vita_audio_test_openal.h" \
    "$root/runtime/vita_audio_abort_teardown_oracle.c" \
    "$root/vita/test_audio_abort_teardown.sh" \
    "$work/vita-audio-abort-host-oracle" \
    "$work/host_vita_audio.host-off.o" \
    "$work/host_vita_audio.host-on.o"
echo "Vita OpenAL abort teardown (ISAAC_VITA_ABORT_SKIP_AUDIO_TEARDOWN): PASS (host oracle; knob-only skip symbol/receipts; Vita entry/adapter edges=$arm_status)"
