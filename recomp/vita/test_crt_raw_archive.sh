#!/usr/bin/env bash
set -eu

root=${ISAAC_CRT_RAW_ARCHIVE_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
work=${ISAAC_CRT_RAW_ARCHIVE_TEST_OUT:-$(mktemp -d "${TMPDIR:-/tmp}/isaac-crt-raw-archive.XXXXXX")}
host_cc=${CC:-cc}
common="-std=gnu11 -O2 -fno-pie -ffunction-sections -fdata-sections -Wall -Wextra -Werror -Wno-maybe-uninitialized -DGUEST_IMAGE_BASE=0x98000000u"
includes="-I$root/vita/qsort_oracle_include -I$root/runtime -I$root/vita"

mkdir -p "$work"

ISAAC_ARCHIVE_CACHE_TEST_OUT="$work/archive-cache" \
    bash "$root/vita/test_archive_cache.sh"

# Twice: without ISAAC_VITA_CRT_DESCRIPTOR_RECOVER (the pre-existing fatal
# ENODEV path, identity) and with it (one reopen at the cursor, redo).
for recover in off on; do
recover_flag=
if [ "$recover" = on ]; then
    recover_flag=-DISAAC_VITA_CRT_DESCRIPTOR_RECOVER=1
fi
"$host_cc" $common $includes $recover_flag \
    "$root/runtime/guest_stack_legacy_oracle_stub.c" \
    "$root/runtime/host_vita_crt_raw_archive_oracle.c" \
    -Wl,--gc-sections -no-pie \
    -o "$work/vita-crt-raw-archive-host-$recover"

if command -v timeout >/dev/null 2>&1; then
    timeout 20s "$work/vita-crt-raw-archive-host-$recover"
else
    "$work/vita-crt-raw-archive-host-$recover"
fi
done

sha256sum \
    "$root/runtime/host_vita_archive_cache.h" \
    "$root/runtime/host_vita_archive_cache.c" \
    "$root/runtime/host_vita_crt.c" \
    "$root/runtime/host_vita_crt_raw_archive_oracle.c" \
    "$root/vita/test_archive_cache.sh" \
    "$root/vita/test_crt_raw_archive.sh" \
    "$work/vita-crt-raw-archive-host-off" \
    "$work/vita-crt-raw-archive-host-on"
echo "Vita CRT exact-ENOMEM packed-archive raw SceIo host behavior: PASS"
