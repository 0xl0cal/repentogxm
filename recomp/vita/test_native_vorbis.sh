#!/usr/bin/env bash
# Host oracle and Vita compile checks for ISAAC_VITA_NATIVE_VORBIS.
#
# 1. Compiles recomp/runtime/host_vita_native_vorbis.c (which owns the
#    vendored stb_vorbis v1.04) for the Vita in its three configurations and
#    checks the __wrap_/__real_ symbol contract used by the eboot link.
# 2. Builds the host oracle twice (production semantics, then the VERIFY
#    build) and runs it against OGG files: those named in
#    ISAAC_NV_TEST_OGG (space separated) or, when ffmpeg with libvorbis is
#    available, four deterministic lavfi renders (stereo, mono, 5.1 and a
#    500 kbps stereo noise stress file).  Without either source the decode
#    proofs are SKIPPED and only the pure checks run.
set -eu

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=${ISAAC_NV_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
if [ -n "${ISAAC_NV_TEST_OUT:-}" ]; then
    work=$ISAAC_NV_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-native-vorbis.XXXXXX")
fi

cc="$VITASDK/bin/arm-vita-eabi-gcc"
nm="$VITASDK/bin/arm-vita-eabi-nm"
objdump="$VITASDK/bin/arm-vita-eabi-objdump"
host_cc=${HOST_CC:-cc}

common_flags="-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror"
# The eboot compiles every runtime unit under the raw-allocator poison gate.
production_flags="$common_flags -DGUEST_STACK_REQUIRED=1 -DGUEST_IMAGE_BASE=0x98000000u -DISAAC_VITA_HAS_RUNTIME=1 -DISAAC_VITA_RAW_ALLOCATOR_GATE=1 -include $root/vita/isaac_vita_raw_allocator_poison.h -DISAAC_VITA_NATIVE_VORBIS=1 -DISAAC_VITA_NATIVE_VORBIS_BUILD_ID=\"native-vorbis:test\""
host_flags="-std=gnu11 -O2 -fno-strict-aliasing -Wall -Wextra -Werror -Wno-error=maybe-uninitialized -pthread"

module="$root/runtime/host_vita_native_vorbis.c"

# 1. Vita compile: worker+receipt, verify, synchronous, and the ASYNC stage
#    (ISAAC_VITA_NATIVE_VORBIS_ASYNC) with and without receipts.
"$cc" $production_flags -DISAAC_VITA_NATIVE_VORBIS_WORKER=1 \
    -DISAAC_VITA_NATIVE_VORBIS_RECEIPT=1 \
    -I"$root/runtime" -I"$root/vita" -c "$module" -o "$work/nv-worker.o"
"$cc" $production_flags -DISAAC_VITA_NATIVE_VORBIS_WORKER=1 \
    -DISAAC_VITA_NATIVE_VORBIS_RECEIPT=1 -DISAAC_VITA_NATIVE_VORBIS_VERIFY=1 \
    -I"$root/runtime" -I"$root/vita" -c "$module" -o "$work/nv-verify.o"
"$cc" $production_flags \
    -I"$root/runtime" -I"$root/vita" -c "$module" -o "$work/nv-sync.o"
"$cc" $production_flags -DISAAC_VITA_NATIVE_VORBIS_WORKER=1 \
    -DISAAC_VITA_NATIVE_VORBIS_RECEIPT=1 -DISAAC_VITA_NATIVE_VORBIS_ASYNC=1 \
    -I"$root/runtime" -I"$root/vita" -c "$module" -o "$work/nv-async.o"
"$cc" $production_flags -DISAAC_VITA_NATIVE_VORBIS_WORKER=1 \
    -DISAAC_VITA_NATIVE_VORBIS_ASYNC=1 \
    -I"$root/runtime" -I"$root/vita" -c "$module" -o "$work/nv-async-noreceipt.o"
# ASYNC serves partial slots, which VERIFY's per-call frame comparison cannot
# follow, and it needs the worker: both combinations must refuse to compile.
if "$cc" $production_flags -DISAAC_VITA_NATIVE_VORBIS_WORKER=1 \
        -DISAAC_VITA_NATIVE_VORBIS_ASYNC=1 -DISAAC_VITA_NATIVE_VORBIS_VERIFY=1 \
        -I"$root/runtime" -I"$root/vita" -c "$module" \
        -o "$work/nv-async-verify.o" 2>/dev/null; then
    echo "ASYNC together with VERIFY must not compile" >&2
    exit 1
fi
if "$cc" $production_flags -DISAAC_VITA_NATIVE_VORBIS_ASYNC=1 \
        -I"$root/runtime" -I"$root/vita" -c "$module" \
        -o "$work/nv-async-sync.o" 2>/dev/null; then
    echo "ASYNC without the worker must not compile" >&2
    exit 1
fi
for object in nv-worker nv-verify nv-sync nv-async nv-async-noreceipt; do
    symbols=$("$nm" "$work/$object.o")
    for entry in sub_005bdf80 sub_005b7740 sub_005bb260 sub_005bd310; do
        echo "$symbols" | grep -q " T __wrap_$entry\$" ||
            { echo "missing __wrap_$entry in $object" >&2; exit 1; }
        echo "$symbols" | grep -q " U __real_$entry\$" ||
            { echo "missing __real_$entry reference in $object" >&2; exit 1; }
    done
    # the vendored decoder must not drag stdio into the decode path
    if echo "$symbols" | grep -Eq " U (fgetc|fread|fseek|ftell)\$"; then
        echo "vendored decoder still references stdio in $object" >&2
        exit 1
    fi
    # the stb_vorbis_open_file wrap (early clone) exists exactly with ASYNC
    case $object in
    nv-async*)
        echo "$symbols" | grep -q " T __wrap_sub_005bd740\$" ||
            { echo "missing __wrap_sub_005bd740 in $object" >&2; exit 1; }
        echo "$symbols" | grep -q " U __real_sub_005bd740\$" ||
            { echo "missing __real_sub_005bd740 reference in $object" >&2; exit 1; }
        ;;
    *)
        if echo "$symbols" | grep -q "sub_005bd740"; then
            echo "open_file wrap must exist only with ASYNC ($object)" >&2
            exit 1
        fi
        ;;
    esac
done
# ASYNC without receipts: nothing may call the CRC any more (the definition
# stays exported for the oracle); with receipts the CRC runs on the producer
# side and on the logged slots only, which the host oracle proves.
if "$objdump" -dr "$work/nv-async-noreceipt.o" |
        grep -Eq "R_ARM_[A-Z0-9_]+[[:space:]]+isaac_nv_crc32"; then
    echo "ASYNC without receipt still references isaac_nv_crc32" >&2
    exit 1
fi
echo "native vorbis Vita compile: PASS (worker, verify, sync, async, async-noreceipt; ASYNC+VERIFY and ASYNC-without-worker refused; open_file wrap only with ASYNC; no CRC reference without receipt)"

# 2. Host oracle.
oracle_sources="$module $root/runtime/vita_native_vorbis_oracle.c $root/runtime/guest_stack_legacy_oracle_stub.c"
"$host_cc" $host_flags -DGUEST_IMAGE_BASE=0x27000000u \
    -DISAAC_VITA_NATIVE_VORBIS_ORACLE=1 -DISAAC_VITA_NATIVE_VORBIS=1 \
    -I"$root/runtime" -I"$root/vita" $oracle_sources -lm \
    -o "$work/vita-native-vorbis-oracle"
"$host_cc" $host_flags -DGUEST_IMAGE_BASE=0x27000000u \
    -DISAAC_VITA_NATIVE_VORBIS_ORACLE=1 -DISAAC_VITA_NATIVE_VORBIS=1 \
    -DISAAC_VITA_NATIVE_VORBIS_VERIFY=1 -DNV_ORACLE_EXPECT_VERIFY=1 \
    -I"$root/runtime" -I"$root/vita" $oracle_sources -lm \
    -o "$work/vita-native-vorbis-oracle-verify"
# ASYNC semantics (partial slots, budget, early clone, producer-side CRC),
# with the production defaults of the knobs.
"$host_cc" $host_flags -DGUEST_IMAGE_BASE=0x27000000u \
    -DISAAC_VITA_NATIVE_VORBIS_ORACLE=1 -DISAAC_VITA_NATIVE_VORBIS=1 \
    -DISAAC_VITA_NATIVE_VORBIS_ASYNC=1 \
    -I"$root/runtime" -I"$root/vita" $oracle_sources -lm \
    -o "$work/vita-native-vorbis-oracle-async"

oggs=${ISAAC_NV_TEST_OGG:-}
if [ -z "$oggs" ] && command -v ffmpeg >/dev/null 2>&1 &&
        ffmpeg -hide_banner -encoders 2>/dev/null | grep -q libvorbis; then
    render() { ffmpeg -hide_banner -loglevel error -y "$@"; }
    render -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=6" \
        -f lavfi -i "sine=frequency=660:sample_rate=44100:duration=6" \
        -filter_complex "[0:a][1:a]join=inputs=2:channel_layout=stereo[a]" \
        -map "[a]" -c:a libvorbis -q:a 6 "$work/stereo_q6.ogg"
    render -f lavfi -i "sine=frequency=330:sample_rate=44100:duration=4" \
        -c:a libvorbis -q:a 3 "$work/mono_q3.ogg"
    render -f lavfi -i "anoisesrc=color=pink:sample_rate=44100:duration=30:seed=7" \
        -f lavfi -i "anoisesrc=color=brown:sample_rate=44100:duration=30:seed=9" \
        -filter_complex "[0:a][1:a]join=inputs=2:channel_layout=stereo[a]" \
        -map "[a]" -c:a libvorbis -q:a 10 "$work/noise_q10.ogg"
    render -f lavfi -i "sine=frequency=220:sample_rate=48000:duration=3" \
        -f lavfi -i "sine=frequency=330:sample_rate=48000:duration=3" \
        -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=3" \
        -f lavfi -i "sine=frequency=550:sample_rate=48000:duration=3" \
        -f lavfi -i "sine=frequency=660:sample_rate=48000:duration=3" \
        -f lavfi -i "sine=frequency=770:sample_rate=48000:duration=3" \
        -filter_complex "[0:a][1:a][2:a][3:a][4:a][5:a]join=inputs=6:channel_layout=5.1[a]" \
        -map "[a]" -c:a libvorbis -q:a 4 "$work/six_q4.ogg"
    oggs="$work/stereo_q6.ogg $work/mono_q3.ogg $work/six_q4.ogg $work/noise_q10.ogg"
fi

if [ -z "$oggs" ]; then
    "$work/vita-native-vorbis-oracle"
    "$work/vita-native-vorbis-oracle-verify"
    "$work/vita-native-vorbis-oracle-async"
    echo "native vorbis decode proofs: SKIP (no OGG input; set ISAAC_NV_TEST_OGG or install ffmpeg+libvorbis)"
else
    # shellcheck disable=SC2086
    "$work/vita-native-vorbis-oracle" $oggs
    # shellcheck disable=SC2086
    "$work/vita-native-vorbis-oracle-verify" $oggs
    # shellcheck disable=SC2086
    "$work/vita-native-vorbis-oracle-async" $oggs
    echo "native vorbis decode proofs: PASS (production, verify, async)"
fi
echo "native vorbis oracle: PASS ($work)"
