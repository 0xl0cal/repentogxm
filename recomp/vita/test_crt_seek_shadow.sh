#!/usr/bin/env bash
set -eu

root=${ISAAC_CRT_SEEK_SHADOW_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
work=${ISAAC_CRT_SEEK_SHADOW_TEST_OUT:-$(mktemp -d "${TMPDIR:-/tmp}/isaac-crt-seek-shadow.XXXXXX")}
host_cc=${CC:-cc}
common="-std=gnu11 -O2 -fno-pie -ffunction-sections -fdata-sections -Wall -Wextra -Werror -Wno-maybe-uninitialized -DGUEST_IMAGE_BASE=0x98000000u"
includes="-I$root/vita/qsort_oracle_include -I$root/runtime -I$root/vita"

mkdir -p "$work"

"$host_cc" $common $includes \
    "$root/runtime/guest_stack_legacy_oracle_stub.c" \
    "$root/runtime/host_vita_crt_seek_shadow_oracle.c" \
    -Wl,--gc-sections -no-pie \
    -o "$work/vita-crt-seek-shadow-host"

# Two randomized 20,000-operation sequences plus the pinned cases; the oracle
# writes and removes its own fixture below $work.
if command -v timeout >/dev/null 2>&1; then
    timeout 60s "$work/vita-crt-seek-shadow-host" "$work" 20000
else
    "$work/vita-crt-seek-shadow-host" "$work" 20000
fi

sha256sum \
    "$root/runtime/host_vita_crt.h" \
    "$root/runtime/host_vita_crt.c" \
    "$root/runtime/host_vita_crt_seek_shadow_oracle.c" \
    "$root/vita/test_crt_seek_shadow.sh" \
    "$work/vita-crt-seek-shadow-host"
echo "Vita CRT read-only SEEK_END shadow host behavior (ISAAC_VITA_CRT_SEEK_SHADOW): PASS"
