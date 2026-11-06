#!/usr/bin/env bash
set -eu

root=${ISAAC_CRT_FREAD_COMPLETE_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
work=${ISAAC_CRT_FREAD_COMPLETE_TEST_OUT:-$(mktemp -d "${TMPDIR:-/tmp}/isaac-crt-fread-complete.XXXXXX")}
host_cc=${CC:-cc}
common="-std=gnu11 -O2 -fno-pie -ffunction-sections -fdata-sections -Wall -Wextra -Werror -Wno-maybe-uninitialized -DGUEST_IMAGE_BASE=0x98000000u"
includes="-I$root/vita/qsort_oracle_include -I$root/runtime -I$root/vita"

mkdir -p "$work"

# Four builds: ISAAC_VITA_CRT_FREAD_COMPLETE off (identity: a short read on
# a packed-archive token reaches the guest unchanged and the block chain
# faults like tour 1) and on (the read is completed from the cursor it left,
# bounded, with receipts and counters), each alone and with the release CRT
# set (seek shadow, descriptor recovery, lookup hint).
for lanes in plain shadow; do
for complete in off on; do
flags=
if [ "$complete" = on ]; then
    flags="$flags -DISAAC_VITA_CRT_FREAD_COMPLETE=1"
fi
if [ "$lanes" = shadow ]; then
    flags="$flags -DISAAC_VITA_CRT_SEEK_SHADOW=1 -DISAAC_VITA_CRT_DESCRIPTOR_RECOVER=1 -DISAAC_VITA_CRT_FILE_LOOKUP_HINT=1"
fi
"$host_cc" $common $includes $flags \
    "$root/runtime/guest_stack_legacy_oracle_stub.c" \
    "$root/runtime/host_vita_crt_fread_complete_oracle.c" \
    -Wl,--gc-sections -no-pie \
    -o "$work/vita-crt-fread-complete-host-$lanes-$complete"

# The pinned cases plus two randomized 2,000-operation sequences against a
# plain libc FILE; the oracle writes and removes its own fixture below $work.
if command -v timeout >/dev/null 2>&1; then
    timeout 60s "$work/vita-crt-fread-complete-host-$lanes-$complete" "$work" 2000
else
    "$work/vita-crt-fread-complete-host-$lanes-$complete" "$work" 2000
fi
done
done

sha256sum \
    "$root/runtime/host_vita_crt.h" \
    "$root/runtime/host_vita_crt.c" \
    "$root/runtime/host_vita_crt_fread_complete_oracle.c" \
    "$root/vita/test_crt_fread_complete.sh" \
    "$work/vita-crt-fread-complete-host-plain-off" \
    "$work/vita-crt-fread-complete-host-plain-on" \
    "$work/vita-crt-fread-complete-host-shadow-off" \
    "$work/vita-crt-fread-complete-host-shadow-on"
echo "Vita CRT short packed-archive fread completion host behavior (ISAAC_VITA_CRT_FREAD_COMPLETE off/on, alone and with the seek shadow, descriptor recovery and lookup hint): PASS"
