#!/usr/bin/env bash
set -eu

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=${ISAAC_TEARDOWN_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
if [ -n "${ISAAC_TEARDOWN_TEST_OUT:-}" ]; then
    work=$ISAAC_TEARDOWN_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-kage-teardown.XXXXXX")
fi

host_cc=${CC:-cc}
cc="$VITASDK/bin/arm-vita-eabi-gcc"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"
nm="$VITASDK/bin/arm-vita-eabi-nm"
objdump="$VITASDK/bin/arm-vita-eabi-objdump"
strings="$VITASDK/bin/arm-vita-eabi-strings"
common_flags="-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror"
production_flags="$common_flags -DGUEST_STACK_REQUIRED=1"

"$host_cc" -std=c11 -O2 -Wall -Wextra -Werror \
    -DISAAC_KAGE_VITA_BACKEND_ORACLE=1 \
    -DISAAC_VITA_STALL_PROBE=1 \
    -I"$root/runtime" \
    "$root/runtime/kage_vita_backend.c" \
    "$root/runtime/kage_vita_backend_teardown_oracle.c" \
    -o "$work/kage-vita-teardown-host-oracle"
"$work/kage-vita-teardown-host-oracle"

# Link the same process-lifetime behavior oracle as ARM/Thumb softfp without
# executing it.
"$cc" $common_flags -DISAAC_KAGE_VITA_BACKEND_ORACLE=1 \
    -DISAAC_VITA_STALL_PROBE=1 \
    -I"$root/runtime" \
    "$root/runtime/kage_vita_backend.c" \
    "$root/runtime/kage_vita_backend_teardown_oracle.c" \
    -Wl,--gc-sections \
    -o "$work/kage-vita-teardown-softfp-oracle.elf"

# Compile the real Vita header surface and both terminal-entry variants.  The
# audio choice must not restore a final swap or guest sceGxmTerminate call.
"$cc" $production_flags -DGUEST_IMAGE_BASE=0x98000000u \
    -I"$root/runtime" -I"$root/vita" \
    -c "$root/runtime/kage_vita_backend.c" \
    -o "$work/kage_vita_backend.production.o"
for audio in on off
do
    if [ "$audio" = on ]; then
        audio_define=1
    else
        audio_define=0
    fi
    "$cc" $production_flags -DGUEST_IMAGE_BASE=0x98000000u \
        -DISAAC_VITA_KAGE=1 -DISAAC_VITA_AUDIO=$audio_define \
        -I"$root/runtime" -I"$root/vita" \
        -c "$root/runtime/entry_vita.c" \
        -o "$work/entry_vita.audio-$audio.o"
    "$objdump" -dr "$work/entry_vita.audio-$audio.o" \
        > "$work/entry_vita.audio-$audio.disassembly"
done

"$readelf" -h "$work/kage-vita-teardown-softfp-oracle.elf" \
    > "$work/teardown.header"
"$readelf" -A "$work/kage-vita-teardown-softfp-oracle.elf" \
    > "$work/teardown.attributes"
"$readelf" -A "$work/kage_vita_backend.production.o" \
    > "$work/teardown-production.attributes"
"$nm" -u "$work/kage-vita-teardown-softfp-oracle.elf" \
    > "$work/teardown.undefined"
"$nm" -u "$work/kage_vita_backend.production.o" \
    > "$work/teardown-production.undefined"
"$objdump" -dr "$work/kage_vita_backend.production.o" \
    > "$work/kage_vita_backend.production.disassembly"
"$strings" "$work/kage_vita_backend.production.o" \
    > "$work/kage_vita_backend.production.strings"

if ! grep -Fq "Version5 EABI, soft-float ABI" "$work/teardown.header"; then
    echo "backend teardown oracle is not an ARM EABI5 softfp executable" >&2
    exit 3
fi
if grep -q "Tag_ABI_VFP_args" "$work/teardown.attributes" ||
   grep -q "Tag_ABI_VFP_args" "$work/teardown-production.attributes"; then
    echo "backend teardown object unexpectedly advertises hardfp arguments" >&2
    exit 4
fi
if [ -s "$work/teardown.undefined" ]; then
    cat "$work/teardown.undefined" >&2
    echo "backend teardown softfp oracle retained unresolved symbols" >&2
    exit 5
fi
if ! grep -Eq '[[:space:]]U[[:space:]]+vglSwapBuffers$' \
        "$work/teardown-production.undefined"; then
    echo "production backend lost its ordinary Present swap dependency" >&2
    exit 6
fi
if grep -Eq '[[:space:]]U[[:space:]]+kage_vita_stall_' \
        "$work/teardown-production.undefined"; then
    echo "default-OFF production backend retained stall-probe edges" >&2
    exit 6
fi
if grep -Fq "final GXM scene seal" \
        "$work/kage_vita_backend.production.strings"; then
    echo "production backend retained the dead final-seal surface" >&2
    exit 7
fi

entry_relocation_lines()
{
    awk -v symbol="$2" \
        '$0 ~ ("R_ARM_THM_(CALL|JUMP24)[[:space:]]+" symbol "$") { print NR }' \
        "$1"
}

entry_has_terminal_self_loop()
{
    awk '
        /R_ARM_THM_(CALL|JUMP24)[[:space:]]+sceKernelExitProcess$/ {
            after_exit = 1
            next
        }
        after_exit && /^[[:space:]]*[0-9a-f]+:/ {
            if ($2 == "e7fe" && $3 ~ /^b(\.n)?$/)
                terminal = 1
            exit
        }
        END { exit terminal ? 0 : 1 }
    ' "$1"
}

for audio in on off
do
    disassembly="$work/entry_vita.audio-$audio.disassembly"
    deactivate=$(entry_relocation_lines "$disassembly" kage_vita_backend_deactivate)
    seal=$(entry_relocation_lines "$disassembly" kage_vita_backend_prepare_process_exit)
    terminate=$(entry_relocation_lines "$disassembly" sceGxmTerminate)
    coverage=$(entry_relocation_lines "$disassembly" guest_coverage_shutdown)
    stack=$(entry_relocation_lines "$disassembly" guest_stack_free)
    image=$(entry_relocation_lines "$disassembly" guest_image_free)
    process_exit=$(entry_relocation_lines "$disassembly" sceKernelExitProcess)
    if [ -n "$seal$terminate" ] ||
       [ "$(printf '%s\n' "$process_exit" | grep -c .)" -ne 1 ] ||
       [ -z "$deactivate$coverage$stack$image" ] ||
       [ "$coverage" -ge "$stack" ] ||
       [ "$deactivate" -ge "$coverage" ] ||
       [ "$stack" -ge "$image" ] ||
       [ "$image" -ge "$process_exit" ] ||
       ! entry_has_terminal_self_loop "$disassembly"; then
        echo "audio-$audio teardown is not deactivate -> coverage -> stack -> image -> one terminal process exit (with no seal/GXM)" >&2
        exit 8
    fi
done

audio_shutdown=$(entry_relocation_lines \
    "$work/entry_vita.audio-on.disassembly" isaac_vita_audio_shutdown_active)
audio_deactivate=$(entry_relocation_lines \
    "$work/entry_vita.audio-on.disassembly" kage_vita_backend_deactivate)
audio_coverage=$(entry_relocation_lines \
    "$work/entry_vita.audio-on.disassembly" guest_coverage_shutdown)
if [ -z "$audio_shutdown" ] ||
   [ "$audio_deactivate" -ge "$audio_shutdown" ] ||
   [ "$audio_shutdown" -ge "$audio_coverage" ]; then
    echo "audio-on teardown lost deactivate -> audio -> coverage order" >&2
    exit 9
fi

# No runtime owner may resurrect the dead final-seal helper.
if grep -R -n --include='*.c' --include='*.h' \
       'kage_vita_backend_prepare_process_exit' "$root/runtime"; then
    echo "runtime retained the dead final process-exit seal" >&2
    exit 10
fi

sha256sum \
    "$root/runtime/entry_vita.c" \
    "$root/runtime/kage_vita_backend.h" \
    "$root/runtime/kage_vita_backend.c" \
    "$root/runtime/kage_vita_backend_test_vitagl.h" \
    "$root/runtime/kage_vita_backend_teardown_oracle.c" \
    "$root/vita/test_kage_vita_teardown.sh" \
    "$work/kage-vita-teardown-host-oracle" \
    "$work/kage-vita-teardown-softfp-oracle.elf" \
    "$work/kage_vita_backend.production.o" \
    "$work/entry_vita.audio-on.o" \
    "$work/entry_vita.audio-off.o"
echo "Vita terminal process-exit host behavior + ARM softfp/static teardown-order gate: PASS (softfp ELF was not executed)"
