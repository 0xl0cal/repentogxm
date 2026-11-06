#!/usr/bin/env bash
# ISAAC_VITA_AUDIO_STARVATION_SHIELD / ISAAC_VITA_AUDIO_MUSIC_DROP_RECEIPT proof:
#   1. host oracles through the real cdecl adapters and the linker seam: the
#      shield alone, the drop receipt alone, both together on the release
#      hook shape (refill hooks + worker + replay counter);
#   2. host adapter objects: the knobs are the only source of the shield
#      receipt and of the --wrap=sub_0056e9a0 seam;
#   3. Vita objects (VITASDK set): both adapter objects compile with the
#      knobs on top of the release hook shape and carry the seam symbols and
#      receipts; a knob without the refill layer is refused at compile time.
set -eu

root=${ISAAC_AUDIO_SHIELD_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
if [ -n "${ISAAC_AUDIO_SHIELD_TEST_OUT:-}" ]; then
    work=$ISAAC_AUDIO_SHIELD_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-audio-shield.XXXXXX")
fi
host_cc=${HOST_CC:-cc}
host_flags="-std=gnu11 -O2 -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror"
host_oracle_flags="$host_flags -Wno-error=maybe-uninitialized"
shield=-DISAAC_VITA_AUDIO_STARVATION_SHIELD=1
drop=-DISAAC_VITA_AUDIO_MUSIC_DROP_RECEIPT=1
hooks="-DISAAC_VITA_AUDIO_REFILL_HOOKS=1 -DISAAC_VITA_AUDIO_WORKER=1 -DISAAC_VITA_AUDIO_REFILL_SPACING_US=20000U"
replays=-DISAAC_VITA_AUDIO_REFILL_REPLAYS=1
oracle_includes="-I$root/vita/audio_oracle_include -I$root/runtime -I$root/vita"
shield_receipt="KAGE VITA AUDIO SHIELD: n=%u replays=%u starved=%u src=%u queued=%u processed=%u"
drop_receipt="KAGE VITA AUDIO MUSIC DROP: slot=%u stream=0x%08x src=%u state=%d queued=%d processed=%d eof=%u loop=%u last_refill_ms=%u"
arm_status=SKIP

# 1. Execute the decision tables on the host.
run_host_oracle()
{
    name=$1
    shift
    "$host_cc" $host_oracle_flags \
        -DISAAC_VITA_AUDIO_ORACLE=1 -DGUEST_IMAGE_BASE=0x27000000u \
        $hooks "$@" $oracle_includes \
        "$root/runtime/host_vita_audio.c" \
        "$root/runtime/host_vita_audio_refill.c" \
        "$root/runtime/guest_stack_legacy_oracle_stub.c" \
        "$root/runtime/vita_audio_import_oracle.c" \
        -Wl,--gc-sections \
        -o "$work/$name"
    "$work/$name"
}
run_host_oracle vita-audio-shield-host-oracle $shield
run_host_oracle vita-audio-drop-host-oracle $drop
run_host_oracle vita-audio-shield-replays-host-oracle $replays $shield
run_host_oracle vita-audio-shield-drop-replays-host-oracle $replays $shield $drop

# 2. The knobs are the only source of the receipts and of the seam.
"$host_cc" $host_oracle_flags -DISAAC_VITA_AUDIO_ORACLE=1 \
    -DGUEST_IMAGE_BASE=0x27000000u $hooks $replays $oracle_includes \
    -c "$root/runtime/host_vita_audio.c" -o "$work/host_vita_audio.host-off.o"
"$host_cc" $host_oracle_flags -DGUEST_IMAGE_BASE=0x27000000u $hooks $replays \
    $oracle_includes \
    -c "$root/runtime/host_vita_audio_refill.c" -o "$work/host_vita_audio_refill.host-off.o"
"$host_cc" $host_oracle_flags -DISAAC_VITA_AUDIO_ORACLE=1 \
    -DGUEST_IMAGE_BASE=0x27000000u $hooks $replays $shield $drop $oracle_includes \
    -c "$root/runtime/host_vita_audio.c" -o "$work/host_vita_audio.host-on.o"
"$host_cc" $host_oracle_flags -DGUEST_IMAGE_BASE=0x27000000u $hooks $replays $shield $drop \
    $oracle_includes \
    -c "$root/runtime/host_vita_audio_refill.c" -o "$work/host_vita_audio_refill.host-on.o"
if nm "$work/host_vita_audio.host-off.o" | grep -q "__wrap_sub_0056e9a0$"; then
    echo "adapter without the knobs defines the slot reset seam" >&2
    exit 3
fi
if strings "$work/host_vita_audio.host-off.o" | grep -Fq "KAGE VITA AUDIO MUSIC DROP"; then
    echo "adapter without the knobs carries the music drop receipt" >&2
    exit 3
fi
if strings "$work/host_vita_audio_refill.host-off.o" | grep -Fq "KAGE VITA AUDIO SHIELD"; then
    echo "refill module without the knobs carries the shield receipt" >&2
    exit 3
fi
if ! nm "$work/host_vita_audio.host-on.o" | grep -q "[[:space:]]T[[:space:]]*__wrap_sub_0056e9a0$" ||
   ! nm "$work/host_vita_audio.host-on.o" | grep -q "[[:space:]]U[[:space:]]*__real_sub_0056e9a0$"; then
    echo "adapter with the knobs does not define the --wrap seam over the translated body" >&2
    exit 3
fi
if ! strings "$work/host_vita_audio.host-on.o" | grep -Fxq "$drop_receipt"; then
    echo "adapter with the knobs lost the music drop receipt" >&2
    exit 3
fi
if ! strings "$work/host_vita_audio_refill.host-on.o" | grep -Fxq "$shield_receipt"; then
    echo "refill module with the knobs lost the shield receipt" >&2
    exit 3
fi

# 3. Vita objects.
if [ -n "${VITASDK:-}" ]; then
    cc="$VITASDK/bin/arm-vita-eabi-gcc"
    nm_arm="$VITASDK/bin/arm-vita-eabi-nm"
    strings_arm="$VITASDK/bin/arm-vita-eabi-strings"
    common_flags="-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror"
    production_flags="$common_flags -DGUEST_STACK_REQUIRED=1"

    for mode in off on; do
        mode_flags=
        if [ "$mode" = on ]; then
            mode_flags="$shield $drop"
        fi
        # the adapter owner sees hooks + replays (the release shape), never
        # the worker macro; the refill module sees the worker as well
        "$cc" $production_flags -DGUEST_IMAGE_BASE=0x98000000u \
            -DISAAC_VITA_AUDIO=1 -DISAAC_VITA_AUDIO_REFILL_HOOKS=1 $replays $mode_flags \
            -I"$root/runtime" -I"$root/vita" \
            -c "$root/runtime/host_vita_audio.c" \
            -o "$work/host_vita_audio.$mode.o"
        "$cc" $production_flags -DGUEST_IMAGE_BASE=0x98000000u \
            -DISAAC_VITA_AUDIO=1 $hooks $replays $mode_flags \
            -I"$root/runtime" -I"$root/vita" \
            -c "$root/runtime/host_vita_audio_refill.c" \
            -o "$work/host_vita_audio_refill.$mode.o"
        "$nm_arm" "$work/host_vita_audio.$mode.o" > "$work/host_vita_audio.$mode.symbols"
        "$strings_arm" "$work/host_vita_audio.$mode.o" > "$work/host_vita_audio.$mode.strings"
        "$strings_arm" "$work/host_vita_audio_refill.$mode.o" > "$work/host_vita_audio_refill.$mode.strings"
    done
    # the shield alone must also compile without the replay counter
    # (ISAAC_VITA_NATIVE_VORBIS_ASYNC=OFF configurations)
    "$cc" $production_flags -DGUEST_IMAGE_BASE=0x98000000u \
        -DISAAC_VITA_AUDIO=1 -DISAAC_VITA_AUDIO_REFILL_HOOKS=1 $shield \
        -I"$root/runtime" -I"$root/vita" \
        -c "$root/runtime/host_vita_audio.c" \
        -o "$work/host_vita_audio.shield-only.o"
    "$cc" $production_flags -DGUEST_IMAGE_BASE=0x98000000u \
        -DISAAC_VITA_AUDIO=1 -DISAAC_VITA_AUDIO_REFILL_HOOKS=1 $shield \
        -I"$root/runtime" -I"$root/vita" \
        -c "$root/runtime/host_vita_audio_refill.c" \
        -o "$work/host_vita_audio_refill.shield-only.o"

    # A knob without the refill layer is a configuration error the adapter
    # itself refuses (CMake refuses it earlier).
    for knob in $shield $drop; do
        if "$cc" $production_flags -DGUEST_IMAGE_BASE=0x98000000u \
            -DISAAC_VITA_AUDIO=1 $knob \
            -I"$root/runtime" -I"$root/vita" \
            -c "$root/runtime/host_vita_audio.c" \
            -o "$work/host_vita_audio.no-hooks.o" 2> "$work/host_vita_audio.no-hooks.err"; then
            echo "adapter accepted $knob without the refill layer" >&2
            exit 4
        fi
        if ! grep -q "require the stream refill layer" "$work/host_vita_audio.no-hooks.err"; then
            cat "$work/host_vita_audio.no-hooks.err" >&2
            echo "adapter refused $knob without the refill layer for another reason" >&2
            exit 4
        fi
    done

    if grep -q "__wrap_sub_0056e9a0$" "$work/host_vita_audio.off.symbols" ||
       grep -Fq "KAGE VITA AUDIO MUSIC DROP" "$work/host_vita_audio.off.strings" ||
       grep -Fq "KAGE VITA AUDIO SHIELD" "$work/host_vita_audio_refill.off.strings"; then
        echo "Vita objects without the knobs carry the seam or a receipt" >&2
        exit 5
    fi
    if ! grep -q "[[:space:]]T[[:space:]]*__wrap_sub_0056e9a0$" "$work/host_vita_audio.on.symbols" ||
       ! grep -q "[[:space:]]U[[:space:]]*__real_sub_0056e9a0$" "$work/host_vita_audio.on.symbols"; then
        echo "Vita adapter with the knobs lost the --wrap seam" >&2
        exit 6
    fi
    if ! grep -Fxq "$drop_receipt" "$work/host_vita_audio.on.strings" ||
       ! grep -Fxq "$shield_receipt" "$work/host_vita_audio_refill.on.strings"; then
        echo "Vita objects with the knobs lost a receipt" >&2
        exit 6
    fi
    arm_status=PASS
fi

sha256sum \
    "$root/runtime/host_vita_audio.h" \
    "$root/runtime/host_vita_audio.c" \
    "$root/runtime/host_vita_audio_refill.h" \
    "$root/runtime/host_vita_audio_refill.c" \
    "$root/runtime/vita_audio_test_openal.h" \
    "$root/runtime/vita_audio_import_oracle.c" \
    "$root/vita/test_audio_starvation_shield.sh" \
    "$work/vita-audio-shield-host-oracle" \
    "$work/vita-audio-drop-host-oracle" \
    "$work/vita-audio-shield-replays-host-oracle" \
    "$work/vita-audio-shield-drop-replays-host-oracle" \
    "$work/host_vita_audio.host-off.o" \
    "$work/host_vita_audio.host-on.o" \
    "$work/host_vita_audio_refill.host-off.o" \
    "$work/host_vita_audio_refill.host-on.o"
echo "Vita OpenAL starvation shield / music drop receipt (ISAAC_VITA_AUDIO_STARVATION_SHIELD, ISAAC_VITA_AUDIO_MUSIC_DROP_RECEIPT): PASS (host oracles x4; knob-only receipts and --wrap seam; Vita objects=$arm_status)"
