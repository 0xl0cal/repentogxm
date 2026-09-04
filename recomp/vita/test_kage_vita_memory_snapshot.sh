#!/usr/bin/env bash
set -eu

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=${ISAAC_MEMORY_SNAPSHOT_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
if [ -n "${ISAAC_MEMORY_SNAPSHOT_TEST_OUT:-}" ]; then
    work=$ISAAC_MEMORY_SNAPSHOT_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-memory-snapshot.XXXXXX")
fi

host_cc=${HOST_CC:-cc}
cc="$VITASDK/bin/arm-vita-eabi-gcc"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"
nm="$VITASDK/bin/arm-vita-eabi-nm"
strings="$VITASDK/bin/arm-vita-eabi-strings"
common_flags="-std=gnu11 -O2 -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror"
arm_flags="$common_flags -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb"
profile_id=0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef
profile_id_define="-DISAAC_VITA_IO_PROFILE_BUILD_ID=\"$profile_id\""
profile_flags="-DISAAC_VITA_IO_PROFILE=1 $profile_id_define"
vitagl_include_flag=
if [ -n "${ISAAC_VITAGL_INCLUDE:-}" ]; then
    if [ ! -f "$ISAAC_VITAGL_INCLUDE/vitaGL.h" ]; then
        echo "ISAAC_VITAGL_INCLUDE does not contain vitaGL.h" >&2
        exit 2
    fi
    vitagl_include_flag="-I$ISAAC_VITAGL_INCLUDE"
fi

"$host_cc" $common_flags $profile_flags \
    -DISAAC_KAGE_VITA_BACKEND_ORACLE=1 \
    -DISAAC_VITA_STALL_PROBE=1 \
    -I"$root/runtime" \
    "$root/runtime/kage_vita_backend.c" \
    "$root/runtime/kage_vita_backend_teardown_oracle.c" \
    -Wl,--gc-sections -o "$work/memory-snapshot-host-oracle"
"$work/memory-snapshot-host-oracle"

"$cc" $arm_flags $profile_flags \
    -DISAAC_KAGE_VITA_BACKEND_ORACLE=1 \
    -DISAAC_VITA_STALL_PROBE=1 \
    -I"$root/runtime" \
    "$root/runtime/kage_vita_backend.c" \
    "$root/runtime/kage_vita_backend_teardown_oracle.c" \
    -Wl,--gc-sections -o "$work/memory-snapshot-softfp-oracle.elf"

# Compile both sides of the diagnostic gate against the real Vita headers.
# OFF must not retain a query or durable-log edge; ON must retain all five.
"$cc" $arm_flags -DGUEST_STACK_REQUIRED=1 \
    $vitagl_include_flag \
    -I"$root/runtime" -I"$root/vita" \
    -c "$root/runtime/kage_vita_backend.c" \
    -o "$work/kage_vita_backend.release.o"
"$cc" $arm_flags $profile_flags -DGUEST_STACK_REQUIRED=1 \
    $vitagl_include_flag \
    -I"$root/runtime" -I"$root/vita" \
    -c "$root/runtime/kage_vita_backend.c" \
    -o "$work/kage_vita_backend.profile.o"

"$readelf" -h "$work/memory-snapshot-softfp-oracle.elf" \
    > "$work/oracle.header"
"$readelf" -A "$work/memory-snapshot-softfp-oracle.elf" \
    > "$work/oracle.attributes"
"$readelf" -A "$work/kage_vita_backend.profile.o" \
    > "$work/profile.attributes"
"$nm" -u "$work/kage_vita_backend.release.o" > "$work/release.undefined"
"$nm" -u "$work/kage_vita_backend.profile.o" > "$work/profile.undefined"
"$strings" "$work/kage_vita_backend.profile.o" > "$work/profile.strings"

if ! grep -Fq "Version5 EABI, soft-float ABI" "$work/oracle.header"; then
    echo "memory snapshot oracle is not ARM EABI5 softfp" >&2
    exit 3
fi
if grep -q "Tag_ABI_VFP_args" \
        "$work/oracle.attributes" "$work/profile.attributes"; then
    echo "memory snapshot object unexpectedly advertises hardfp arguments" >&2
    exit 4
fi

for symbol in \
    isaac_vita_log \
    sceKernelGetFreeMemorySize \
    sceGxmGetRenderTargetMemSize \
    vglMemFree \
    vglMemTotal
do
    if grep -Eq "[[:space:]]U[[:space:]]+$symbol$" \
            "$work/release.undefined"; then
        echo "default-OFF backend retained diagnostic import: $symbol" >&2
        exit 5
    fi
    if ! grep -Eq "[[:space:]]U[[:space:]]+$symbol$" \
            "$work/profile.undefined"; then
        echo "profile backend lost diagnostic import: $symbol" >&2
        exit 6
    fi
done

for evidence in \
    "$profile_id" \
    "first-1024-fbo-event" \
    "vitagl-init-return" \
    "vf/t(r/v/p/b)=" \
    "init=0/960/544/16777216/0"
do
    if ! grep -Fq "$evidence" "$work/profile.strings"; then
        echo "profile backend lost memory evidence: $evidence" >&2
        exit 7
    fi
done

sha256sum \
    "$root/runtime/kage_vita_backend.h" \
    "$root/runtime/kage_vita_backend.c" \
    "$root/runtime/kage_vita_backend_test_vitagl.h" \
    "$root/runtime/kage_vita_backend_teardown_oracle.c" \
    "$root/vita/test_kage_vita_memory_snapshot.sh" \
    "$work/memory-snapshot-host-oracle" \
    "$work/memory-snapshot-softfp-oracle.elf" \
    "$work/kage_vita_backend.release.o" \
    "$work/kage_vita_backend.profile.o"
echo "Vita memory snapshot host + ARM softfp + default-OFF import gate: PASS"
