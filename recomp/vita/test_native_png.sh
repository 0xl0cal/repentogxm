#!/usr/bin/env bash
# Host oracle and Vita compile checks for ISAAC_VITA_NATIVE_PNG.
#
# 1. Compiles recomp/runtime/host_vita_native_png.c for the Vita in its
#    three configurations (plain, receipt, verify) under the eboot's flags
#    and checks the __wrap_/__real_ symbol contract used by the link.
# 2. Builds the host oracle (decoder core + in-tree tinfl) and runs
#    recomp/test_vita_native_png.py: a Pillow-generated corpus covering the
#    8-bit colour types, tRNS, multi-IDAT chunking and empty IDAT chunks,
#    plus every *.png under ISAAC_NP_TEST_CORPUS (colon separated) is decoded
#    by the core and by an independent zlib + filter-equation reference and
#    compared byte for byte; the fault cases assert the fallback codes.
set -eu

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=${ISAAC_NP_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
if [ -n "${ISAAC_NP_TEST_OUT:-}" ]; then
    work=$ISAAC_NP_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-native-png.XXXXXX")
fi

cc="$VITASDK/bin/arm-vita-eabi-gcc"
nm="$VITASDK/bin/arm-vita-eabi-nm"
host_cc=${HOST_CC:-cc}
python=${PYTHON:-python3}

common_flags="-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror"
production_flags="$common_flags -DGUEST_STACK_REQUIRED=1 -DGUEST_IMAGE_BASE=0x98000000u -DISAAC_VITA_HAS_RUNTIME=1 -DISAAC_VITA_RAW_ALLOCATOR_GATE=1 -include $root/vita/isaac_vita_raw_allocator_poison.h -DISAAC_VITA_NATIVE_PNG=1 -DISAAC_VITA_NATIVE_PNG_BUILD_ID=\"native-png:test\""
host_flags="-std=gnu11 -O2 -fno-strict-aliasing -Wall -Wextra -Werror"

module="$root/runtime/host_vita_native_png.c"
tinfl="$root/runtime/host_vita_archive_miniz_native.c"

# 1. Vita compile: plain, receipt, verify.
"$cc" $production_flags -I"$root/runtime" -I"$root/vita" -c "$module" -o "$work/np-plain.o"
"$cc" $production_flags -DISAAC_VITA_NATIVE_PNG_RECEIPT=1 \
    -I"$root/runtime" -I"$root/vita" -c "$module" -o "$work/np-receipt.o"
"$cc" $production_flags -DISAAC_VITA_NATIVE_PNG_RECEIPT=1 -DISAAC_VITA_NATIVE_PNG_VERIFY=1 \
    -I"$root/runtime" -I"$root/vita" -c "$module" -o "$work/np-verify.o"
for object in np-plain np-receipt np-verify; do
    symbols=$("$nm" "$work/$object.o")
    echo "$symbols" | grep -q " T __wrap_sub_005b1500\$" ||
        { echo "missing __wrap_sub_005b1500 in $object" >&2; exit 1; }
    echo "$symbols" | grep -q " U __real_sub_005b1500\$" ||
        { echo "missing __real_sub_005b1500 reference in $object" >&2; exit 1; }
    # the decoder must not touch the poisoned libc allocators
    if echo "$symbols" | grep -Eq " U (malloc|calloc|realloc|free)\$"; then
        echo "native png module references a raw libc allocator in $object" >&2
        exit 1
    fi
    # the Up filter must have been vectorised
    if ! "$VITASDK/bin/arm-vita-eabi-objdump" -d "$work/$object.o" | grep -q "vadd.i8"; then
        echo "native png module has no NEON Up filter in $object" >&2
        exit 1
    fi
done
echo "native png Vita compile: PASS (plain, receipt, verify)"

# 2. Host oracle.
"$host_cc" $host_flags -DISAAC_VITA_NATIVE_PNG_ORACLE=1 -DISAAC_VITA_NATIVE_PNG=1 \
    -I"$root/runtime" -I"$root/vita" "$module" "$tinfl" \
    "$root/runtime/vita_native_png_oracle.c" -lm -o "$work/vita-native-png-oracle"

corpus_args=()
if [ -n "${ISAAC_NP_TEST_CORPUS:-}" ]; then
    IFS=: read -r -a dirs <<< "$ISAAC_NP_TEST_CORPUS"
    for d in "${dirs[@]}"; do
        corpus_args+=(--corpus "$d")
    done
fi
"$python" "$root/test_vita_native_png.py" --oracle "$work/vita-native-png-oracle" \
    --keep "$work/corpus" ${corpus_args[@]+"${corpus_args[@]}"}
echo "native png host oracle: PASS ($work)"
