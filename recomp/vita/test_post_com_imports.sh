#!/usr/bin/env bash
set -eu

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
if [ -n "${ISAAC_POST_COM_TEST_OUT:-}" ]; then
    work=$ISAAC_POST_COM_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-post-com-imports.XXXXXX")
fi

cc="$VITASDK/bin/arm-vita-eabi-gcc"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"
nm="$VITASDK/bin/arm-vita-eabi-nm"
objdump="$VITASDK/bin/arm-vita-eabi-objdump"
flags="-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror"

"$cc" $flags -DGUEST_IMAGE_BASE=0x98000000u \
    -I"$root/runtime" -I"$root/vita" \
    "$root/runtime/host_vita_post_com.c" \
    "$root/runtime/vita_post_com_import_test.c" \
    -o "$work/vita-post-com-import-oracle.elf"

"$readelf" -h "$work/vita-post-com-import-oracle.elf"
"$readelf" -A "$work/vita-post-com-import-oracle.elf"
"$nm" -u "$work/vita-post-com-import-oracle.elf"
sha256sum \
    "$root/runtime/host_vita_post_com.h" \
    "$root/runtime/host_vita_post_com.c" \
    "$root/runtime/vita_post_com_import_test.c" \
    "$work/vita-post-com-import-oracle.elf"

# RENDER60 S6 (B2): the same oracle with the timeGetTime knobs, in the
# definition sets CMake gives host_vita_post_com.c (recipe A = reciprocal +
# OBSERVE with the phase profile, recipe B = reciprocal alone, plus ON).
s6_variants="
rec:-DISAAC_VITA_TIMEGETTIME_RECIPROCAL=1
rec-profile:-DISAAC_VITA_TIMEGETTIME_RECIPROCAL=1 -DISAAC_VITA_PHASE_PROFILE=1
latch-on:-DISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH_MODE=2
latch-on-rec:-DISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH_MODE=2 -DISAAC_VITA_TIMEGETTIME_RECIPROCAL=1
latch-observe-profile:-DISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH_MODE=1 -DISAAC_VITA_PHASE_PROFILE=1
latch-observe-profile-rec:-DISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH_MODE=1 -DISAAC_VITA_PHASE_PROFILE=1 -DISAAC_VITA_TIMEGETTIME_RECIPROCAL=1
"
echo "$s6_variants" | while IFS=: read -r name defs; do
    [ -n "$name" ] || continue
    # shellcheck disable=SC2086
    "$cc" $flags -DGUEST_IMAGE_BASE=0x98000000u $defs \
        -I"$root/runtime" -I"$root/vita" \
        "$root/runtime/host_vita_post_com.c" \
        "$root/runtime/vita_post_com_import_test.c" \
        -o "$work/vita-post-com-import-oracle-$name.elf"
    undefined=$("$nm" -u "$work/vita-post-com-import-oracle-$name.elf" |
                awk '{print $NF}' | sort | tr '\n' ' ')
    echo "S6 $name: undefined: $undefined"
    sha256sum "$work/vita-post-com-import-oracle-$name.elf"
done
# The latch OBSERVE mode without the phase profile must not compile (its
# counters are its only output).
if "$cc" $flags -DGUEST_IMAGE_BASE=0x98000000u \
        -DISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH_MODE=1 \
        -I"$root/runtime" -I"$root/vita" -c \
        "$root/runtime/host_vita_post_com.c" \
        -o "$work/observe-without-profile.o" 2>"$work/observe-without-profile.err"; then
    echo "S6 FAIL: OBSERVE without ISAAC_VITA_PHASE_PROFILE compiled" >&2
    exit 1
fi
grep -q "needs ISAAC_VITA_PHASE_PROFILE" "$work/observe-without-profile.err"

# RENDER60 S6 (B3): which functions of the handler TU call __aeabi_uldivmod.
# OFF: vita_post_com_timeGetTime does (the division leg 2 removes).  ON: the
# hot functions do not; only the cold self-test and fallback may.
uldivmod_owners() {
    "$objdump" -dr "$1" | awk '
        /^Disassembly of section / { section = $4; sub(/:$/, "", section) }
        /R_ARM_THM_(CALL|JUMP24)[ \t]+__aeabi_uldivmod/ { print section }
    ' | sort -u | tr '\n' ' '
}
for name in off rec rec-latch-on; do
    case $name in
        off) defs= ;;
        rec) defs=-DISAAC_VITA_TIMEGETTIME_RECIPROCAL=1 ;;
        rec-latch-on) defs="-DISAAC_VITA_TIMEGETTIME_RECIPROCAL=1 -DISAAC_VITA_CLEAR_EXPIRY_CLOCK_LATCH_MODE=2" ;;
    esac
    # shellcheck disable=SC2086
    "$cc" $flags -DGUEST_IMAGE_BASE=0x98000000u -DGUEST_STACK_REQUIRED=1 $defs \
        -I"$root/runtime" -I"$root/vita" -c \
        "$root/runtime/host_vita_post_com.c" -o "$work/post-com-$name.o"
    owners=$(uldivmod_owners "$work/post-com-$name.o")
    echo "S6 B3 $name: __aeabi_uldivmod callers: ${owners:-none}"
    case $name in
        off)
            case " $owners " in
                *" .text.vita_post_com_timeGetTime "*) ;;
                *) echo "S6 B3: OFF timeGetTime does not call __aeabi_uldivmod; leg 2 saves nothing (drop it)"; exit 1 ;;
            esac ;;
        *)
            for hot in .text.vita_post_com_timeGetTime .text.vita_post_com_fresh_ms \
                       .text.vita_post_com_clear_latch_ms; do
                case " $owners " in
                    *" $hot "*) echo "S6 B3 FAIL: $hot still calls __aeabi_uldivmod" >&2; exit 1 ;;
                esac
            done ;;
    esac
done
echo "Vita post-COM compile/link/static ABI check: PASS (6 handlers; complete four-symbol WINMM timer family; S6 knob variants and the uldivmod gate; execution is accepted only through Vita3K or hardware)"
