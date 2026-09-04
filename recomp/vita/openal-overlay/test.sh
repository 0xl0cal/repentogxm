#!/usr/bin/env bash
set -euo pipefail

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
runtime_dir=$(CDPATH= cd -- "$script_dir/../../runtime" && pwd)
vita_dir=$(CDPATH= cd -- "$script_dir/.." && pwd)
archive_verifier="$script_dir/verify_deterministic_archive.py"

if [ "$#" -ne 1 ]; then
    echo "usage: $0 OUTPUT_ROOT" >&2
    exit 2
fi
if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

case "$1" in
    /*) output_root=$1 ;;
    *) output_root=$(pwd)/$1 ;;
esac
library="$output_root/prefix/lib/libopenal.a"
proof="$output_root/proof"
mkdir -p "$proof"

cc="$VITASDK/bin/arm-vita-eabi-gcc"
ar="$VITASDK/bin/arm-vita-eabi-ar"
nm="$VITASDK/bin/arm-vita-eabi-nm"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"
objdump="$VITASDK/bin/arm-vita-eabi-objdump"

if [ ! -s "$library" ]; then
    echo "missing patched OpenAL archive: $library" >&2
    exit 3
fi
if [ ! -f "$output_root/libopenal.archive.json" ]; then
    echo "missing deterministic OpenAL archive receipt" >&2
    exit 3
fi
python3 -B "$archive_verifier" --expected-regular-members 53 "$library" \
    > "$proof/libopenal.archive.live.json"
if ! cmp -s "$output_root/libopenal.archive.json" \
          "$proof/libopenal.archive.live.json"; then
    echo "deterministic OpenAL archive receipt is stale" >&2
    exit 3
fi

# Re-prove the allocator closure from the supplied archive.  The build recipe
# has the same check, but this gate must reject an old/unredirected archive on
# its own instead of passing merely because newlib resolves the raw symbols.
"$nm" -A -u "$library" > "$proof/libopenal.undefined"
awk '{ print $NF }' "$proof/libopenal.undefined" > \
    "$proof/libopenal.undefined.names"
if grep -Eq '^(malloc|calloc|realloc|free|aligned_alloc|strdup)$' \
        "$proof/libopenal.undefined.names"; then
    echo "OpenAL overlay retained a raw direct allocator import" >&2
    exit 3
fi
for symbol in \
    isaac_vita_openal_pool_malloc \
    isaac_vita_openal_pool_calloc \
    isaac_vita_openal_pool_realloc \
    isaac_vita_openal_pool_free \
    isaac_vita_openal_pool_aligned_alloc \
    isaac_vita_openal_pool_strdup; do
    if ! grep -Fxq "$symbol" "$proof/libopenal.undefined.names"; then
        echo "OpenAL overlay does not import $symbol" >&2
        exit 3
    fi
done

"$ar" p "$library" ALc.c.obj > "$proof/ALc.c.obj"
"$ar" p "$library" alSource.c.obj > "$proof/alSource.c.obj"
"$readelf" -h "$proof/ALc.c.obj" > "$proof/ALc.header"
"$readelf" -A "$proof/ALc.c.obj" > "$proof/ALc.attributes"
"$nm" "$library" > "$proof/libopenal.symbols"
"$objdump" -dr --disassemble=AllocateVoices "$proof/ALc.c.obj" > \
    "$proof/AllocateVoices.disassembly"
"$objdump" -dr --disassemble=alcCreateContext "$proof/ALc.c.obj" > \
    "$proof/alcCreateContext.disassembly"
"$objdump" -dr --disassemble=alGenSources "$proof/alSource.c.obj" > \
    "$proof/alGenSources.disassembly"

if ! grep -Fq 'Version5 EABI' "$proof/ALc.header" ||
   grep -q 'Tag_ABI_VFP_args' "$proof/ALc.attributes"; then
    echo "OpenAL overlay is not ARM EABI5 softfp" >&2
    exit 4
fi
for symbol in AllocateVoices alcOpenDevice alcCreateContext alGenSources; do
    if ! grep -Eq "[[:space:]][Tt][[:space:]]+$symbol$" \
            "$proof/libopenal.symbols"; then
        echo "OpenAL overlay lost required symbol: $symbol" >&2
        exit 5
    fi
done

# The pinned 1.19.1 ARM layout has a 30,080-byte base voice, 1,484 bytes per
# auxiliary send, a 164-byte property base and 24 bytes per send.  The archive
# disassembly is checked before using those constants as a memory proof.
if ! grep -Eq 'movw[[:space:]]+fp, #1484' \
        "$proof/AllocateVoices.disassembly" ||
   ! grep -Eq 'movw[[:space:]]+r3, #30095' \
        "$proof/AllocateVoices.disassembly"; then
    echo "OpenAL voice layout drifted; memory proof is no longer valid" >&2
    exit 6
fi

# alSource.c grows its name store in 64-object slabs.  Pin both the 208-byte
# source layout and the resulting 13,312-byte slab before charging the second
# slab that sources 65..80 make reachable.
if ! grep -Eq 'movs[[:space:]]+r[0-9]+, #208' \
        "$proof/alGenSources.disassembly" ||
   ! grep -Eq 'mov\.w[[:space:]]+r[0-9]+, #13312' \
        "$proof/alGenSources.disassembly" ||
   ! grep -Eq 'movs[[:space:]]+r1, #24' \
        "$proof/alGenSources.disassembly"; then
    echo "OpenAL source slab layout drifted; headroom cost is no longer valid" >&2
    exit 6
fi

sends=2
voice=$(( (30080 + 1484 * sends + 15) / 16 * 16 ))
props=$(( (164 + 24 * sends + 15) / 16 * 16 ))
per_voice=$(( 4 + voice + props ))
old_bytes=$(( per_voice * 256 ))
manager_voice_bytes=$(( per_voice * 64 ))
new_bytes=$(( per_voice * 80 ))
saved_bytes=$(( old_bytes - new_bytes ))
voice_headroom_bytes=$(( new_bytes - manager_voice_bytes ))
source_object_bytes=208
source_slab_entries=64
source_slab_bytes=$(( source_object_bytes * source_slab_entries ))
source_sublist_bytes=16
source_vector_bytes_64=$(( 8 + source_sublist_bytes ))
source_vector_bytes_80=$(( 8 + source_sublist_bytes * 2 ))
source_vector_headroom_bytes=$(( source_vector_bytes_80 - source_vector_bytes_64 ))
fixed_headroom_bytes=$(( voice_headroom_bytes + source_slab_bytes + source_vector_headroom_bytes ))
if [ "$old_bytes" -ne 8520704 ] ||
   [ "$manager_voice_bytes" -ne 2130176 ] ||
   [ "$new_bytes" -ne 2662720 ] ||
   [ "$saved_bytes" -ne 5857984 ] ||
   [ "$voice_headroom_bytes" -ne 532544 ] ||
   [ "$source_slab_bytes" -ne 13312 ] ||
   [ "$source_vector_headroom_bytes" -ne 16 ] ||
   [ "$fixed_headroom_bytes" -ne 545872 ]; then
    echo "OpenAL 64-manager/80-device memory arithmetic drifted" >&2
    exit 6
fi

common_flags='-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -ffunction-sections -fdata-sections -Wall -Wextra -Werror -Wno-error=maybe-uninitialized'
"$cc" $common_flags \
    -DISAAC_VITA_OPENAL_POOL_ACTIVE=1 \
    -I"$runtime_dir" \
    "$runtime_dir/host_vita_openal_pool.c" \
    "$runtime_dir/vita_audio_link_probe.c" \
    -Wl,--gc-sections -Wl,-Map="$proof/overlay-link.map" \
    -o "$proof/overlay-link.elf" \
    "$library" -lpthread -lm -lSceAudio_stub -lSceAudioIn_stub \
    -lSceLibKernel_stub -lSceSysmem_stub -lSceKernelThreadMgr_stub
"$readelf" -h "$proof/overlay-link.elf" > "$proof/overlay-link.header"
"$readelf" -A "$proof/overlay-link.elf" > "$proof/overlay-link.attributes"
"$nm" -u "$proof/overlay-link.elf" > "$proof/overlay-link.undefined"
if [ -s "$proof/overlay-link.undefined" ] ||
   grep -q 'Tag_ABI_VFP_args' "$proof/overlay-link.attributes"; then
    echo "OpenAL overlay softfp link retained an ABI or symbol failure" >&2
    cat "$proof/overlay-link.undefined" >&2
    exit 7
fi

# Link the production adapter too: its pre-context guard adds alcGetIntegerv,
# which a generic OpenAL smoke probe would not prove is present in this exact
# archive/link set.
"$cc" $common_flags -DGUEST_IMAGE_BASE=0x98000000u \
    -DISAAC_VITA_OPENAL_POOL_ACTIVE=1 \
    -DISAAC_VITA_OPENAL_POOL_ROUTER=1 \
    -I"$runtime_dir" \
    "$runtime_dir/host_vita_openal_pool.c" \
    "$runtime_dir/host_vita_audio.c" \
    "$runtime_dir/guest_stack_legacy_oracle_stub.c" \
    "$runtime_dir/vita_audio_import_link_probe.c" \
    -Wl,--gc-sections -Wl,-Map="$proof/overlay-host-link.map" \
    -o "$proof/overlay-host-link.elf" \
    "$library" -lpthread -lm -lSceAudio_stub -lSceAudioIn_stub \
    -lSceLibKernel_stub -lSceSysmem_stub -lSceKernelThreadMgr_stub
"$readelf" -A "$proof/overlay-host-link.elf" > \
    "$proof/overlay-host-link.attributes"
"$nm" "$proof/overlay-host-link.elf" > "$proof/overlay-host-link.symbols"
"$nm" -u "$proof/overlay-host-link.elf" > \
    "$proof/overlay-host-link.undefined"
if [ -s "$proof/overlay-host-link.undefined" ] ||
   grep -q 'Tag_ABI_VFP_args' "$proof/overlay-host-link.attributes" ||
   ! grep -Eq '[[:space:]][Tt][[:space:]]+alcGetIntegerv$' \
       "$proof/overlay-host-link.symbols" ||
   ! grep -Eq '[[:space:]][Tt][[:space:]]+isaac_vita_openal_pool_initialize$' \
       "$proof/overlay-host-link.symbols"; then
    echo "production audio adapter failed its patched OpenAL softfp link" >&2
    cat "$proof/overlay-host-link.undefined" >&2
    exit 8
fi

# Configure only the tiny scaffold and inspect the actual packaging command.
# Checking the source file contents is insufficient: OpenAL opens the exact
# app0:/alsoft.conf name, and a one-character VPK destination typo disables
# audio before alcCreateContext via the fail-closed capacity guard.
package_build="$proof/package-cmake"
cmake -S "$vita_dir" -B "$package_build" -G Ninja \
    -DISAAC_VITA_SCAFFOLD_ONLY=ON \
    -DSOURCE_DATE_EPOCH=1700000000 \
    -DISAAC_VITA_KAGE=ON \
    -DISAAC_VITA_AUDIO=ON \
    -DISAAC_VITA_OPENAL_OVERLAY_DIR="$output_root"
ninja -C "$package_build" -t commands > "$proof/package.commands"
mapping="$vita_dir/alsoft.conf=alsoft.conf"
mapping_count=$(awk -v expected="$mapping" \
    '{ for (i = 1; i <= NF; i++) if ($i == expected) count++ }
     END { print count + 0 }' \
    "$proof/package.commands")
source_count=$(awk -v prefix="$vita_dir/alsoft.conf=" \
    '{ for (i = 1; i <= NF; i++) if (index($i, prefix) == 1) count++ }
     END { print count + 0 }' \
    "$proof/package.commands")
if [ "$mapping_count" -ne 1 ] || [ "$source_count" -ne 1 ]; then
    echo "VPK does not package the exact app0:/alsoft.conf destination" >&2
    grep -F "$vita_dir/alsoft.conf=" "$proof/package.commands" >&2 || true
    exit 9
fi

printf 'Vita OpenAL overlay: PASS (softfp host link + exact app0:/alsoft.conf package mapping + pre-context 80-source guard; manager 64 + stream headroom 16; initial voices 256 -> 80; %s -> %s bytes; saved %s bytes; fixed headroom cost %s bytes)\n' \
    "$old_bytes" "$new_bytes" "$saved_bytes" "$fixed_headroom_bytes"
