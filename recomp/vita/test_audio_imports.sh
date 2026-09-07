#!/usr/bin/env bash
set -eu

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=${ISAAC_AUDIO_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
if [ -n "${ISAAC_AUDIO_TEST_OUT:-}" ]; then
    work=$ISAAC_AUDIO_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-audio-imports.XXXXXX")
fi

cc="$VITASDK/bin/arm-vita-eabi-gcc"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"
nm="$VITASDK/bin/arm-vita-eabi-nm"
objdump="$VITASDK/bin/arm-vita-eabi-objdump"
strings="$VITASDK/bin/arm-vita-eabi-strings"
host_cc=${HOST_CC:-cc}
a005_site_status=SKIP
stream_contract_status=SKIP

if [ -n "${ISAAC_AUDIO_TEST_PE:-}" ]; then
    "${ISAAC_AUDIO_TEST_PYTHON:-python3}" \
        "$root/test_audio_a005_sites.py" --pe "$ISAAC_AUDIO_TEST_PE"
    a005_site_status=PASS
    "${ISAAC_AUDIO_TEST_PYTHON:-python3}" \
        "$root/test_audio_stream_starvation_contract.py" \
        --pe "$ISAAC_AUDIO_TEST_PE"
    stream_contract_status=PASS
fi

common_flags="-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror"
production_flags="$common_flags -DGUEST_STACK_REQUIRED=1"
host_flags="-std=gnu11 -O2 -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror"
oracle_flags="$common_flags -Wno-error=maybe-uninitialized"
host_oracle_flags="$host_flags -Wno-error=maybe-uninitialized"

# Execute the ownership state machine on the build host.  The test-only manual
# compile seam retains the real SoundInitialize body and excludes graphics.
"$host_cc" $host_oracle_flags \
    -DISAAC_VITA_AUDIO_ORACLE=1 \
    -DISAAC_VITA_AUDIO_MANUAL_ORACLE=1 \
    -DISAAC_VITA_AUDIO=1 \
    -DGUEST_IMAGE_BASE=0x27000000u \
    -I"$root/vita/audio_oracle_include" \
    -I"$root/runtime" -I"$root/vita" \
    "$root/runtime/host_vita_audio.c" \
    "$root/runtime/manual_kage_vita.c" \
    "$root/runtime/guest_stack_legacy_oracle_stub.c" \
    "$root/runtime/vita_audio_import_oracle.c" \
    -Wl,--gc-sections \
    -o "$work/vita-audio-host-oracle"
"$work/vita-audio-host-oracle"

# Static sample scopes use the same actual OpenAL adapters; the existing ABI
# fixture supplies original-loader and clock mocks, not a second WAV decoder.
"$host_cc" $host_oracle_flags \
    -DISAAC_VITA_AUDIO_ORACLE=1 -DISAAC_VITA_STATIC_SFX_PROFILE=1 \
    -DGUEST_IMAGE_BASE=0x27000000u \
    -I"$root/vita/audio_oracle_include" \
    -I"$root/runtime" -I"$root/vita" \
    "$root/runtime/host_vita_audio.c" \
    "$root/runtime/host_vita_static_sfx_profile.c" \
    "$root/runtime/guest_stack_legacy_oracle_stub.c" \
    "$root/runtime/vita_audio_import_oracle.c" \
    -Wl,--gc-sections \
    -o "$work/vita-audio-static-sfx-host-oracle"
"$work/vita-audio-static-sfx-host-oracle"

# Execute both pre-OpenAL initialization edges with a deterministic pool stub.
# The ordinary oracle above deliberately keeps the pool router out so it also
# remains the exact audio-OFF/A-B regression fixture.
"$host_cc" $host_oracle_flags \
    -DISAAC_VITA_AUDIO_ORACLE=1 \
    -DISAAC_VITA_OPENAL_POOL_ROUTER=1 \
    -DISAAC_VITA_OPENAL_POOL_ROUTER_ORACLE=1 \
    -DGUEST_IMAGE_BASE=0x27000000u \
    -I"$root/vita/audio_oracle_include" \
    -I"$root/runtime" \
    "$root/runtime/host_vita_audio.c" \
    "$root/runtime/guest_stack_legacy_oracle_stub.c" \
    "$root/runtime/vita_audio_import_oracle.c" \
    -Wl,--gc-sections \
    -o "$work/vita-audio-pool-router-host-oracle"
"$work/vita-audio-pool-router-host-oracle"

# Replay the frozen StreamSource::Update query pattern against the stream
# refill pacer (ISAAC_VITA_AUDIO_WORKER).  The decision is a pure function,
# so it runs on the build host without OpenAL or platform services.
"$host_cc" $host_oracle_flags \
    -DGUEST_IMAGE_BASE=0x27000000u \
    -I"$root/runtime" \
    "$root/runtime/host_vita_audio_refill.c" \
    "$root/runtime/vita_audio_refill_oracle.c" \
    -Wl,--gc-sections \
    -o "$work/vita-audio-refill-host-oracle"
"$work/vita-audio-refill-host-oracle"

# Drive the pacer and receipts through the real x86 cdecl adapters with the
# return marker pushed as generated code pushes it (bare RVA), plus the based
# form.  This is the check that catches a GUEST_IMAGE_BASE offset mismatch in
# the return-site filter, which the pure decision oracle above cannot see.
"$host_cc" $host_oracle_flags \
    -DISAAC_VITA_AUDIO_ORACLE=1 \
    -DISAAC_VITA_AUDIO_REFILL_HOOKS=1 \
    -DISAAC_VITA_AUDIO_WORKER=1 \
    -DISAAC_VITA_AUDIO_REFILL_SPACING_US=20000U \
    -DGUEST_IMAGE_BASE=0x27000000u \
    -I"$root/vita/audio_oracle_include" \
    -I"$root/runtime" -I"$root/vita" \
    "$root/runtime/host_vita_audio.c" \
    "$root/runtime/host_vita_audio_refill.c" \
    "$root/runtime/guest_stack_legacy_oracle_stub.c" \
    "$root/runtime/vita_audio_import_oracle.c" \
    -Wl,--gc-sections \
    -o "$work/vita-audio-refill-hook-host-oracle"
"$work/vita-audio-refill-hook-host-oracle"

# The replay receipt (ISAAC_VITA_AUDIO_REFILL_REPLAYS, defined together with
# ISAAC_VITA_NATIVE_VORBIS_ASYNC): alSourcePlay from QueueData with the
# source AL_STOPPED is a stream that ran dry; partial stream refills count.
"$host_cc" $host_oracle_flags \
    -DISAAC_VITA_AUDIO_ORACLE=1 \
    -DISAAC_VITA_AUDIO_REFILL_HOOKS=1 \
    -DISAAC_VITA_AUDIO_WORKER=1 \
    -DISAAC_VITA_AUDIO_REFILL_SPACING_US=20000U \
    -DISAAC_VITA_AUDIO_REFILL_REPLAYS=1 \
    -DGUEST_IMAGE_BASE=0x27000000u \
    -I"$root/vita/audio_oracle_include" \
    -I"$root/runtime" -I"$root/vita" \
    "$root/runtime/host_vita_audio.c" \
    "$root/runtime/host_vita_audio_refill.c" \
    "$root/runtime/guest_stack_legacy_oracle_stub.c" \
    "$root/runtime/vita_audio_import_oracle.c" \
    -Wl,--gc-sections \
    -o "$work/vita-audio-refill-replays-host-oracle"
"$work/vita-audio-refill-replays-host-oracle"

# The production pacer/receipt module and the adapter owner must compile for
# the Vita with the hooks on, in both the receipt-only and worker shapes.
"$cc" $production_flags -DGUEST_IMAGE_BASE=0x98000000u \
    -DISAAC_VITA_AUDIO=1 -DISAAC_VITA_AUDIO_REFILL_HOOKS=1 \
    -I"$root/runtime" -I"$root/vita" \
    -c "$root/runtime/host_vita_audio.c" \
    -o "$work/host_vita_audio.refill-hooks.o"
"$cc" $production_flags -DGUEST_IMAGE_BASE=0x98000000u \
    -DISAAC_VITA_AUDIO=1 -DISAAC_VITA_AUDIO_REFILL_HOOKS=1 \
    -I"$root/runtime" -I"$root/vita" \
    -c "$root/runtime/host_vita_audio_refill.c" \
    -o "$work/host_vita_audio_refill.receipt.o"
"$cc" $production_flags -DGUEST_IMAGE_BASE=0x98000000u \
    -DISAAC_VITA_AUDIO=1 -DISAAC_VITA_AUDIO_REFILL_HOOKS=1 \
    -DISAAC_VITA_AUDIO_WORKER=1 -DISAAC_VITA_AUDIO_REFILL_SPACING_US=20000U \
    -I"$root/runtime" -I"$root/vita" \
    -c "$root/runtime/host_vita_audio_refill.c" \
    -o "$work/host_vita_audio_refill.worker.o"
"$nm" "$work/host_vita_audio_refill.worker.o" | grep -q " D _oal_thread_affinity" || {
    echo "worker module must define the OpenAL Vita backend affinity override" >&2
    exit 1
}
"$cc" $production_flags -DGUEST_IMAGE_BASE=0x98000000u \
    -DISAAC_VITA_AUDIO=1 -DISAAC_VITA_AUDIO_REFILL_HOOKS=1 \
    -DISAAC_VITA_AUDIO_REFILL_REPLAYS=1 \
    -I"$root/runtime" -I"$root/vita" \
    -c "$root/runtime/host_vita_audio.c" \
    -o "$work/host_vita_audio.refill-replays.o"
"$cc" $production_flags -DGUEST_IMAGE_BASE=0x98000000u \
    -DISAAC_VITA_AUDIO=1 -DISAAC_VITA_AUDIO_REFILL_HOOKS=1 \
    -DISAAC_VITA_AUDIO_WORKER=1 -DISAAC_VITA_AUDIO_REFILL_SPACING_US=20000U \
    -DISAAC_VITA_AUDIO_REFILL_REPLAYS=1 \
    -I"$root/runtime" -I"$root/vita" \
    -c "$root/runtime/host_vita_audio_refill.c" \
    -o "$work/host_vita_audio_refill.replays.o"

"$cc" $oracle_flags -DGUEST_IMAGE_BASE=0x98000000u \
    -I"$root/runtime" \
    "$root/runtime/host_vita_audio.c" \
    "$root/runtime/guest_stack_legacy_oracle_stub.c" \
    "$root/runtime/vita_audio_import_link_probe.c" \
    -Wl,--gc-sections -Wl,-Map="$work/vita-audio-import-link.map" \
    -o "$work/vita-audio-import-link.elf" \
    -lopenal -lpthread -lm -lSceAudio_stub -lSceAudioIn_stub

"$cc" $common_flags \
    "$root/runtime/vita_audio_link_probe.c" \
    -Wl,--gc-sections -Wl,-Map="$work/vita-audio-library-link.map" \
    -o "$work/vita-audio-library-link.elf" \
    -lopenal -lpthread -lm -lSceAudio_stub -lSceAudioIn_stub

# Compile the production integration edges without touching the frozen
# generated corpus, then make ld resolve their internal audio references in a
# relocatable link.  Other runtime/graphics services intentionally remain U.
"$cc" $production_flags -DGUEST_IMAGE_BASE=0x98000000u \
    -DISAAC_VITA_AUDIO=1 \
    -I"$root/runtime" -I"$root/vita" \
    -c "$root/runtime/host_vita_audio.c" \
    -o "$work/host_vita_audio.production.o"
"$cc" $production_flags -DGUEST_IMAGE_BASE=0x98000000u \
    -DISAAC_VITA_KAGE=1 -DISAAC_VITA_AUDIO=1 \
    -I"$root/runtime" -I"$root/vita" \
    -c "$root/runtime/entry_vita.c" \
    -o "$work/entry_vita.audio-on.o"
"$cc" $production_flags -DGUEST_IMAGE_BASE=0x98000000u \
    -DISAAC_VITA_KAGE=1 -DISAAC_VITA_AUDIO=0 \
    -I"$root/runtime" -I"$root/vita" \
    -c "$root/runtime/entry_vita.c" \
    -o "$work/entry_vita.audio-off.o"
"$cc" $production_flags -DGUEST_IMAGE_BASE=0x98000000u \
    -DISAAC_VITA_AUDIO=1 \
    -I"$root/runtime" -I"$root/vita" \
    -c "$root/runtime/host_vita_first_fault.c" \
    -o "$work/host_vita_first_fault.audio-on.o"
"$cc" $production_flags -DGUEST_IMAGE_BASE=0x98000000u \
    -DISAAC_VITA_AUDIO=1 \
    -I"$root/runtime" -I"$root/vita" \
    -I"$root/vita/audio_oracle_include" \
    -c "$root/runtime/manual_kage_vita.c" \
    -o "$work/manual_kage_vita.audio-on.o"
"$cc" $production_flags -DGUEST_IMAGE_BASE=0x98000000u \
    -DISAAC_VITA_AUDIO=0 \
    -I"$root/runtime" -I"$root/vita" \
    -I"$root/vita/audio_oracle_include" \
    -c "$root/runtime/manual_kage_vita.c" \
    -o "$work/manual_kage_vita.audio-off.o"
"$cc" -r \
    "$work/host_vita_audio.production.o" \
    "$work/entry_vita.audio-on.o" \
    "$work/host_vita_first_fault.audio-on.o" \
    "$work/manual_kage_vita.audio-on.o" \
    -lSceLibKernel_stub \
    -o "$work/vita-audio-integration.o"

"$readelf" -h "$work/vita-audio-import-link.elf" > \
    "$work/vita-audio-import-link.header"
"$readelf" -A "$work/vita-audio-import-link.elf" > \
    "$work/vita-audio-import-link.attributes"
"$nm" "$work/vita-audio-import-link.elf" > \
    "$work/vita-audio-import-link.symbols"
"$nm" -u "$work/vita-audio-import-link.elf" > \
    "$work/vita-audio-import-link.undefined"
"$strings" "$work/vita-audio-import-link.elf" > \
    "$work/vita-audio-import-link.strings"
"$readelf" -A "$work/host_vita_audio.production.o" > \
    "$work/host_vita_audio.production.attributes"
"$readelf" -A "$work/entry_vita.audio-on.o" > \
    "$work/entry_vita.audio-on.attributes"
"$readelf" -A "$work/entry_vita.audio-off.o" > \
    "$work/entry_vita.audio-off.attributes"
"$readelf" -A "$work/host_vita_first_fault.audio-on.o" > \
    "$work/host_vita_first_fault.audio-on.attributes"
"$readelf" -A "$work/manual_kage_vita.audio-on.o" > \
    "$work/manual_kage_vita.audio-on.attributes"
"$nm" -u "$work/vita-audio-integration.o" > \
    "$work/vita-audio-integration.undefined"
"$nm" -u "$work/manual_kage_vita.audio-off.o" > \
    "$work/manual_kage_vita.audio-off.undefined"
"$nm" -u "$work/entry_vita.audio-off.o" > \
    "$work/entry_vita.audio-off.undefined"
"$nm" "$work/entry_vita.audio-on.o" > \
    "$work/entry_vita.audio-on.symbols"
"$nm" "$work/entry_vita.audio-off.o" > \
    "$work/entry_vita.audio-off.symbols"
"$nm" "$work/vita-audio-integration.o" > \
    "$work/vita-audio-integration.symbols"
"$objdump" -dr "$work/entry_vita.audio-on.o" > \
    "$work/entry_vita.audio-on.disassembly"
"$objdump" -dr "$work/entry_vita.audio-off.o" > \
    "$work/entry_vita.audio-off.disassembly"
"$strings" "$work/entry_vita.audio-on.o" > \
    "$work/entry_vita.audio-on.strings"
"$strings" "$work/entry_vita.audio-off.o" > \
    "$work/entry_vita.audio-off.strings"
"$objdump" -dr "$work/host_vita_first_fault.audio-on.o" > \
    "$work/host_vita_first_fault.audio-on.disassembly"
"$objdump" -dr "$work/manual_kage_vita.audio-on.o" > \
    "$work/manual_kage_vita.audio-on.disassembly"
"$strings" "$work/manual_kage_vita.audio-on.o" > \
    "$work/manual_kage_vita.audio-on.strings"
"$strings" "$work/manual_kage_vita.audio-off.o" > \
    "$work/manual_kage_vita.audio-off.strings"

if ! grep -Fq "Version5 EABI, soft-float ABI" \
    "$work/vita-audio-import-link.header"
then
    echo "audio proof is not an ARM EABI5 softfp executable" >&2
    exit 3
fi
if [ -s "$work/vita-audio-import-link.undefined" ]; then
    echo "audio proof retained unresolved native symbols" >&2
    cat "$work/vita-audio-import-link.undefined" >&2
    exit 4
fi
for attributes in \
    "$work/host_vita_audio.production.attributes" \
    "$work/entry_vita.audio-on.attributes" \
    "$work/entry_vita.audio-off.attributes" \
    "$work/host_vita_first_fault.audio-on.attributes" \
    "$work/manual_kage_vita.audio-on.attributes"
do
    if grep -q "Tag_ABI_VFP_args" "$attributes"; then
        echo "audio integration object unexpectedly uses hardfp arguments" >&2
        exit 7
    fi
done
if grep -Eq '[[:space:]]U[[:space:]]+isaac_vita_audio_' \
        "$work/vita-audio-integration.undefined"; then
    cat "$work/vita-audio-integration.undefined" >&2
    echo "audio integration retained an unresolved internal audio edge" >&2
    exit 8
fi
if grep -Eq '[[:space:]]U[[:space:]]+sceKernelExitProcess$' \
        "$work/vita-audio-integration.undefined"; then
    cat "$work/vita-audio-integration.undefined" >&2
    echo "entry terminal handoff did not resolve through SceLibKernel_stub" >&2
    exit 8
fi
if grep -Eq '[[:space:]]sceGxmTerminate$' \
        "$work/vita-audio-integration.symbols"; then
    echo "entry terminal handoff retained the forbidden guest GXM teardown" >&2
    exit 8
fi
if grep -Eq '[[:space:]]U[[:space:]]+isaac_vita_audio_' \
        "$work/manual_kage_vita.audio-off.undefined"; then
    cat "$work/manual_kage_vita.audio-off.undefined" >&2
    echo "ISAAC_VITA_AUDIO=0 still depends on the OpenAL backend" >&2
    exit 9
fi
if grep -Eq '[[:space:]]U[[:space:]]+isaac_vita_audio_' \
        "$work/entry_vita.audio-off.undefined"; then
    cat "$work/entry_vita.audio-off.undefined" >&2
    echo "ISAAC_VITA_AUDIO=0 entry teardown still depends on OpenAL" >&2
    exit 9
fi
if ! grep -Eq 'R_ARM_THM_(CALL|JUMP24)[[:space:]]+isaac_vita_audio_shutdown_active$' \
        "$work/entry_vita.audio-on.disassembly"; then
    echo "audio-on entry has no relocation to active manager teardown" >&2
    exit 10
fi
if ! grep -Eq 'R_ARM_THM_(CALL|JUMP24)[[:space:]]+isaac_vita_audio_log_final$' \
        "$work/entry_vita.audio-on.disassembly"; then
    echo "audio-on entry has no relocation to process-final pool telemetry" >&2
    exit 10
fi
for shutdown_message in \
    "KAGE VITA AUDIO SHUTDOWN: complete" \
    "KAGE VITA AUDIO SHUTDOWN: incomplete (device close failed; owner retained)"
do
    if ! grep -Fxq "$shutdown_message" "$work/entry_vita.audio-on.strings"; then
        echo "audio-on entry lost shutdown status: $shutdown_message" >&2
        exit 10
    fi
done
if grep -Fq "KAGE VITA AUDIO SHUTDOWN:" \
        "$work/entry_vita.audio-off.strings"; then
    echo "audio-off entry retained OpenAL shutdown telemetry" >&2
    exit 10
fi
if ! grep -Eq 'R_ARM_THM_(CALL|JUMP24)[[:space:]]+isaac_vita_audio_manager_is_active$' \
        "$work/entry_vita.audio-on.disassembly"; then
    echo "audio-on materialisation wrapper has no host-owner relocation" >&2
    exit 10
fi
for entry_symbols in \
    "$work/entry_vita.audio-on.symbols" \
    "$work/entry_vita.audio-off.symbols"
do
    if ! grep -Eq '[[:space:]]T[[:space:]]+isaac_vita_audio_materialization_ready$' \
            "$entry_symbols"; then
        echo "entry variant lost the generated-code materialisation ABI" >&2
        exit 10
    fi
done
for entry_disassembly in \
    "$work/entry_vita.audio-on.disassembly" \
    "$work/entry_vita.audio-off.disassembly"
do
    if ! grep -Eq 'R_ARM_THM_(CALL|JUMP24)[[:space:]]+kage_vita_backend_deactivate$' \
            "$entry_disassembly"; then
        echo "KAGE entry has no relocation to callback/input teardown" >&2
        exit 10
    fi
    if ! grep -Eq 'R_ARM_THM_(CALL|JUMP24)[[:space:]]+sceKernelExitProcess$' \
            "$entry_disassembly"; then
        echo "KAGE entry has no relocation to terminal process exit" >&2
        exit 10
    fi
    if grep -Eq 'R_ARM_THM_(CALL|JUMP24)[[:space:]]+(sceGxmTerminate|kage_vita_backend_prepare_process_exit)$' \
            "$entry_disassembly"; then
        echo "KAGE entry retained the forbidden final swap/GXM path" >&2
        exit 10
    fi
done
entry_relocation_line()
{
    awk -v symbol="$2" \
        '$0 ~ ("R_ARM_THM_(CALL|JUMP24)[[:space:]]+" symbol "$") {
            print NR
            exit
        }' "$1"
}
on_kage=$(entry_relocation_line \
    "$work/entry_vita.audio-on.disassembly" kage_vita_backend_deactivate)
on_audio=$(entry_relocation_line \
    "$work/entry_vita.audio-on.disassembly" isaac_vita_audio_shutdown_active)
on_audio_final=$(entry_relocation_line \
    "$work/entry_vita.audio-on.disassembly" isaac_vita_audio_log_final)
on_coverage=$(entry_relocation_line \
    "$work/entry_vita.audio-on.disassembly" guest_coverage_shutdown)
on_stack=$(entry_relocation_line \
    "$work/entry_vita.audio-on.disassembly" guest_stack_free)
on_image=$(entry_relocation_line \
    "$work/entry_vita.audio-on.disassembly" guest_image_free)
on_exit=$(entry_relocation_line \
    "$work/entry_vita.audio-on.disassembly" sceKernelExitProcess)
if [ -z "$on_kage$on_audio$on_audio_final$on_coverage$on_stack$on_image$on_exit" ] ||
   [ "$on_kage" -ge "$on_audio" ] ||
   [ "$on_audio" -ge "$on_audio_final" ] ||
   [ "$on_audio_final" -ge "$on_coverage" ] ||
   [ "$on_coverage" -ge "$on_stack" ] ||
   [ "$on_stack" -ge "$on_image" ] ||
   [ "$on_image" -ge "$on_exit" ]; then
    echo "audio-on entry teardown is not KAGE -> audio -> final telemetry -> coverage -> stack -> image -> process exit" >&2
    exit 10
fi
off_kage=$(entry_relocation_line \
    "$work/entry_vita.audio-off.disassembly" kage_vita_backend_deactivate)
off_coverage=$(entry_relocation_line \
    "$work/entry_vita.audio-off.disassembly" guest_coverage_shutdown)
off_stack=$(entry_relocation_line \
    "$work/entry_vita.audio-off.disassembly" guest_stack_free)
off_image=$(entry_relocation_line \
    "$work/entry_vita.audio-off.disassembly" guest_image_free)
off_exit=$(entry_relocation_line \
    "$work/entry_vita.audio-off.disassembly" sceKernelExitProcess)
if [ -z "$off_kage$off_coverage$off_stack$off_image$off_exit" ] ||
   [ "$off_kage" -ge "$off_coverage" ] ||
   [ "$off_coverage" -ge "$off_stack" ] ||
   [ "$off_stack" -ge "$off_image" ] ||
   [ "$off_image" -ge "$off_exit" ]; then
    echo "audio-off entry teardown is not KAGE -> coverage -> stack -> image -> process exit" >&2
    exit 10
fi
if ! grep -Eq 'R_ARM_THM_(CALL|JUMP24)[[:space:]]+isaac_vita_audio_import_counted$' \
        "$work/host_vita_first_fault.audio-on.disassembly"; then
    echo "production dispatcher has no relocation to the audio delegate" >&2
    exit 10
fi
if ! grep -Eq 'R_ARM_THM_(CALL|JUMP24)[[:space:]]+isaac_vita_audio_manager_initialize$' \
        "$work/manual_kage_vita.audio-on.disassembly"; then
    echo "manual SoundInitialize has no relocation to native manager init" >&2
    exit 11
fi
if ! grep -Eq 'R_ARM_THM_(CALL|JUMP24)[[:space:]]+isaac_vita_sync_cs_initialize_plain$' \
        "$work/manual_kage_vita.audio-on.disassembly"; then
    echo "manual SoundInitialize lost the plain critical-section helper" >&2
    exit 11
fi
if grep -Eq 'R_ARM_THM_(CALL|JUMP24)[[:space:]]+isaac_vita_sync_cs_initialize$' \
        "$work/manual_kage_vita.audio-on.disassembly"; then
    echo "manual SoundInitialize borrowed the SpinCount failure attribution" >&2
    exit 11
fi
if ! grep -Fq \
        "manager_sources=%u device_sources=%u stream_headroom=%u buffers=%u pump=main-loop+room-cooperative guest_thread=off" \
        "$work/manual_kage_vita.audio-on.strings"; then
    echo "audio-on object lost the explicit 64+16 source/frame-pump policy" >&2
    exit 12
fi
if ! grep -Fq \
        "status=disabled stage=compile-option" \
        "$work/manual_kage_vita.audio-off.strings"; then
    echo "audio-off object lost the measured fallback marker" >&2
    exit 13
fi
if ! grep -Fq \
        "status=already-active stage=openal" \
        "$work/manual_kage_vita.audio-on.strings"; then
    echo "audio-on object lost the duplicate-init ownership marker" >&2
    exit 13
fi

for symbol in \
    isaac_vita_audio_import \
    isaac_vita_audio_import_counted \
    isaac_vita_audio_manager_initialize \
    isaac_vita_audio_manager_close \
    isaac_vita_audio_manager_status_name \
    alSourcePlay \
    alGetSourcei \
    alSourceStop \
    alGenBuffers \
    alSourceQueueBuffers \
    alSourcePause \
    alSourceUnqueueBuffers \
    alcMakeContextCurrent \
    alcDestroyContext \
    alcOpenDevice \
    alcCreateContext \
    alcCloseDevice \
    alListener3f \
    alSourcei \
    alcProcessContext \
    alGetSourcef \
    alGenSources \
    alGetError \
    alDeleteBuffers \
    alListenerfv \
    alDeleteSources \
    alSource3f \
    alSourcef \
    alBufferData \
    pthread_create \
    sceAudioOutOutput \
    sceAudioInInput
do
    if ! grep -q "[[:space:]]$symbol$" \
        "$work/vita-audio-import-link.symbols"
    then
        echo "missing linked Vita audio symbol: $symbol" >&2
        exit 5
    fi
done

for symbol in \
    isaac_vita_audio_manager_is_active \
    isaac_vita_audio_materialization_ready
do
    if ! grep -Eq "[[:space:]]T[[:space:]]+$symbol$" \
            "$work/vita-audio-integration.symbols"; then
        echo "missing linked materialisation-policy symbol: $symbol" >&2
        exit 5
    fi
done

for evidence in \
    "OpenAL32.dll!alSourcePlay" \
    "OpenAL32.dll!alcOpenDevice" \
    "OpenAL32.dll!alBufferData"
do
    if ! grep -Fqx "$evidence" \
        "$work/vita-audio-import-link.strings"
    then
        echo "missing frozen Vita audio evidence: $evidence" >&2
        exit 6
    fi
done

sha256sum \
    "$root/runtime/entry_vita.c" \
    "$root/runtime/host_vita_audio.h" \
    "$root/runtime/host_vita_audio.c" \
    "$root/runtime/host_vita_openal_pool.h" \
    "$root/runtime/host_vita_first_fault.c" \
    "$root/runtime/manual_kage_vita.c" \
    "$root/runtime/vita_audio_import_link_probe.c" \
    "$root/runtime/vita_audio_link_probe.c" \
    "$root/runtime/vita_audio_test_openal.h" \
    "$root/runtime/vita_audio_import_oracle.c" \
    "$root/test_audio_a005_sites.py" \
    "$root/test_audio_stream_starvation_contract.py" \
    "$root/vita/audio_oracle_include/guest_coverage_generated.h" \
    "$root/vita/CMakeLists.txt" \
    "$root/vita/test_audio_imports.sh" \
    "$work/vita-audio-host-oracle" \
    "$work/vita-audio-pool-router-host-oracle" \
    "$work/host_vita_audio.production.o" \
    "$work/entry_vita.audio-on.o" \
    "$work/entry_vita.audio-off.o" \
    "$work/host_vita_first_fault.audio-on.o" \
    "$work/manual_kage_vita.audio-on.o" \
    "$work/manual_kage_vita.audio-off.o" \
    "$work/vita-audio-integration.o" \
    "$work/vita-audio-import-link.elf" \
    "$work/vita-audio-library-link.elf"
echo "Vita OpenAL softfp integration: PASS (24/24 imports; A005 frozen sites=$a005_site_status; OGG starvation contract=$stream_contract_status; bounded attribution/folding; executed indexed/manager pool-order + immediate manager-failure + retry-safe final telemetry oracle; duplicate/direct/partial owner oracle; manual duplicate no-op; terminal KAGE/audio ON/OFF edges; explicit SceLibKernel+openal+pthread+SceAudio+SceAudioIn)"
