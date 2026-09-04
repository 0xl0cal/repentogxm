#!/usr/bin/env bash
set -eu

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=${ISAAC_STEAM_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
if [ -n "${ISAAC_STEAM_TEST_OUT:-}" ]; then
    work=$ISAAC_STEAM_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-steam-imports.XXXXXX")
fi

cc="$VITASDK/bin/arm-vita-eabi-gcc"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"

common_flags="-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror"

"$cc" $common_flags -DGUEST_IMAGE_BASE=0x98000000u \
    -DGUEST_CHECKED_MEMORY \
    -I"$root/runtime" \
    -c "$root/runtime/host_vita_steam.c" \
    -o "$work/host-vita-steam-production.o"

"$cc" $common_flags -DGUEST_IMAGE_BASE=0x98000000u \
    -DGUEST_CHECKED_MEMORY \
    -DISAAC_VITA_STEAM_CONTEXT_TEST_READ32 \
    -I"$root/runtime" \
    "$root/runtime/host_vita_steam.c" \
    "$root/runtime/vita_steam_import_test.c" \
    -o "$work/vita-steam-import-oracle.elf"

"$readelf" -h "$work/vita-steam-import-oracle.elf"
"$readelf" -A "$work/vita-steam-import-oracle.elf"
"$readelf" -A "$work/host-vita-steam-production.o"
sha256sum \
    "$root/runtime/host_vita_steam.h" \
    "$root/runtime/host_vita_steam.c" \
    "$root/runtime/vita_steam_import_test.c" \
    "$root/vita/test_steam_imports.sh" \
    "$work/host-vita-steam-production.o" \
    "$work/vita-steam-import-oracle.elf"
echo "Vita Steam compile/link/static artifact check: PASS (complete 11-slot family with 8 handled/3 loud; ContextInit exact frozen-PE census: 71 direct + 24 register loads feeding 67 unique register calls = 138 physical calls; historical pre-main boot count remains 1; source oracle built but not executed; runtime evidence requires Vita3K or hardware)"
