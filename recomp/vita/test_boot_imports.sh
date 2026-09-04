#!/usr/bin/env bash
set -eu

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-boot-imports.XXXXXX")
trap 'rm -rf "$work"' EXIT HUP INT TERM

cc="$VITASDK/bin/arm-vita-eabi-gcc"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"

common_flags="-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra"

# Directory creation belongs to the reset boundary, not every durable record.
# Keep this source-level closure beside the real platform-object compile so a
# later logger edit cannot silently restore per-line mkdir overhead.
if [ "$(grep -c 'sceIoMkdir(ISAAC_LOG_DIR' "$root/vita/platform.c")" -ne 1 ]; then
    echo "platform logger must create its directory exactly once in reset" >&2
    exit 1
fi

# Execute the exact production handlers as ARM code.  The bundled GNU
# simulator mistakes Cortex-A9's .ARM.attributes value for an unsupported
# machine subtype, so only the simulator copy loses that metadata; the
# production-ABI ELF remains untouched and is inspected below.
"$cc" $common_flags -DGUEST_IMAGE_BASE=0x98000000u \
    -I"$root/runtime" -I"$root/vita" \
    "$root/runtime/host_vita_first_fault.c" \
    "$root/runtime/guest_stack_legacy_oracle_stub.c" \
    "$root/runtime/vita_boot_import_test.c" \
    -o "$work/boot-import-oracle.elf"
"$readelf" -h "$work/boot-import-oracle.elf" \
    >"$work/boot-import-oracle.header"
if ! grep -q "Machine:.*ARM" "$work/boot-import-oracle.header" ||
   ! grep -q "soft-float ABI" "$work/boot-import-oracle.header"; then
    cat "$work/boot-import-oracle.header" >&2
    echo "boot-import oracle is not a softfp ARM ELF" >&2
    exit 1
fi
# Compile the production frontier verdict too.  It needs no generated body at
# this stage; the exact IAT/name/return constants are shared with the handler.
"$cc" $common_flags -DGUEST_IMAGE_BASE=0x98000000u \
    -I"$root/runtime" -I"$root/vita" \
    -c "$root/runtime/entry_vita.c" -o "$work/entry_vita.o"

# Compile the real SceRtc/provider edge and link the scaffold target against
# the same Vita stubs without offering CMake any generated corpus.
"$cc" $common_flags -DISAAC_VITA_PLATFORM_HEAP_MB=32 \
    -I"$root/runtime" -I"$root/vita" \
    -c "$root/vita/platform.c" -o "$work/platform.o"
if ! cmake -S "$root/vita" -B "$work/scaffold" -G Ninja \
        -DISAAC_VITA_SCAFFOLD_ONLY=ON \
        -DSOURCE_DATE_EPOCH=1700000000 \
        -DISAAC_VITA_HEAP_MB=32 >"$work/cmake-configure.log" 2>&1; then
    cat "$work/cmake-configure.log" >&2
    exit 1
fi
if ! cmake --build "$work/scaffold" --target isaac_first_arm_fault \
        >"$work/cmake-build.log" 2>&1; then
    cat "$work/cmake-build.log" >&2
    exit 1
fi

echo "Vita boot-import compile/link/static artifact check: PASS (source oracle built but not executed; runtime evidence requires Vita3K or hardware)"
