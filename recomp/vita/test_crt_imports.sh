#!/usr/bin/env bash
set -eu

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=${ISAAC_CRT_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
if [ -n "${ISAAC_CRT_TEST_OUT:-}" ]; then
    work=$ISAAC_CRT_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-crt-imports.XXXXXX")
fi

cc="$VITASDK/bin/arm-vita-eabi-gcc"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"

common_flags="-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror -Wno-maybe-uninitialized ${ISAAC_CRT_TEST_CFLAGS:-}"

ISAAC_CRT_RAW_ARCHIVE_TEST_ROOT="$root" \
ISAAC_CRT_RAW_ARCHIVE_TEST_OUT="$work/raw-archive" \
    bash "$root/vita/test_crt_raw_archive.sh"

ISAAC_CRT_VFPRINTF_TEST_ROOT="$root" \
ISAAC_CRT_VFPRINTF_TEST_OUT="$work/vfprintf" \
    bash "$root/vita/test_crt_vfprintf.sh"

ISAAC_CRT_SEEK_SHADOW_TEST_ROOT="$root" \
ISAAC_CRT_SEEK_SHADOW_TEST_OUT="$work/seek-shadow" \
    bash "$root/vita/test_crt_seek_shadow.sh"

"$cc" $common_flags -DGUEST_IMAGE_BASE=0x98000000u \
    -DISAAC_VITA_ARCHIVE_FILE_CACHE=1 \
    -DISAAC_VITA_CRT_DESCRIPTOR_RECOVER=1 \
    -DISAAC_VITA_CRT_FREAD_ORACLE=1 \
    -I"$root/runtime" -I"$root/vita" \
    "$root/runtime/guest_stack_legacy_oracle_stub.c" \
    "$root/runtime/host_vita_first_fault.c" \
    "$root/runtime/host_vita_archive_cache.c" \
    "$root/runtime/host_vita_crt.c" \
    "$root/runtime/vita_crt_import_test.c" \
    -lSceRtc_stub -lSceIofilemgr_stub \
    -o "$work/vita-crt-import-oracle.elf"

"$readelf" -h "$work/vita-crt-import-oracle.elf"
"$readelf" -A "$work/vita-crt-import-oracle.elf"
sha256sum \
    "$root/runtime/guest_stack_legacy_oracle_stub.c" \
    "$root/runtime/host_vita_first_fault.c" \
    "$root/runtime/vita_boot_imports.h" \
    "$root/runtime/host_vita_archive_cache.h" \
    "$root/runtime/host_vita_archive_cache.c" \
    "$root/runtime/host_vita_crt.h" \
    "$root/runtime/host_vita_crt.c" \
    "$root/runtime/host_vita_crt_raw_archive_oracle.c" \
    "$root/runtime/host_vita_crt_seek_shadow_oracle.c" \
    "$root/runtime/host_vita_memory.h" \
    "$root/runtime/vita_crt_import_test.c" \
    "$root/vita/test_crt_raw_archive.sh" \
    "$root/vita/test_crt_seek_shadow.sh" \
    "$work/vita-crt-import-oracle.elf"
echo "Vita CRT import compile/link/static ABI check: PASS (60 handlers; exact TLS/_exit/exit family with four exit call sites and source oracle; utility qsort is the exact 7-site/6-comparator guest-callback implementation; complete 6-slot convert DLL with exact 456-physical-call numeric census: atoi 191 direct + 46 register loads feeding 102 unique register calls = 293 physical calls; EDX:EAX/x87 oracle; complete contiguous 16-slot stdio family, no direct raw allocation in bounded vfprintf streaming, exact missing 8-slot/92-call census with fwrite #2772, fflush #2773 and predicted fread #2793 pinned, one-newlib locked FILE registry, 16 KiB read buffering, optional read-only SEEK_END shadow, one-idle packed-archive cache, exact-ENOMEM raw SceIo packed-archive fallback, and fread/fseek hot dispatch; exact 5-import/22-call UCRT time family; source oracle built but execution is accepted only through Vita3K or hardware)"
