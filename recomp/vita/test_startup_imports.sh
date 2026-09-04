#!/usr/bin/env bash
set -eu

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=${ISAAC_STARTUP_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
if [ -n "${ISAAC_STARTUP_TEST_OUT:-}" ]; then
    work=$ISAAC_STARTUP_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-startup-imports.XXXXXX")
fi

cc="$VITASDK/bin/arm-vita-eabi-gcc"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"
host_cc=${CC:-cc}

host_flags="-std=gnu11 -O2 -fno-pie -no-pie -fno-strict-aliasing -Wall -Wextra -Werror -Wno-maybe-uninitialized"

"$host_cc" $host_flags -DGUEST_IMAGE_BASE=0x98000000u \
    -I"$root/runtime" -I"$root/vita" \
    "$root/runtime/host_vita_startup.c" \
    "$root/runtime/guest_stack_legacy_oracle_stub.c" \
    "$root/runtime/vita_startup_import_test.c" \
    -o "$work/vita-startup-host-oracle"
"$work/vita-startup-host-oracle"

common_flags="-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror -Wno-maybe-uninitialized"

"$cc" $common_flags -DGUEST_IMAGE_BASE=0x98000000u \
    -I"$root/runtime" -I"$root/vita" \
    "$root/runtime/host_vita_startup.c" \
    "$root/runtime/guest_stack_legacy_oracle_stub.c" \
    "$root/runtime/vita_startup_import_test.c" \
    -o "$work/vita-startup-import-oracle.elf"

"$readelf" -h "$work/vita-startup-import-oracle.elf"
"$readelf" -A "$work/vita-startup-import-oracle.elf"
sha256sum \
    "$root/runtime/host_vita_startup.h" \
    "$root/runtime/host_vita_startup.c" \
    "$root/runtime/vita_startup_import_test.c" \
    "$work/vita-startup-host-oracle" \
    "$work/vita-startup-import-oracle.elf"
echo "Vita startup-platform host semantics + compile/link/static ABI check: PASS (11 handlers including GetLastError; all bounded GetEnvironmentVariableA names absent without output access; fixed desktop fallback through LoadLibraryA(winmm.dll) #526; softfp ELF was not executed)"
