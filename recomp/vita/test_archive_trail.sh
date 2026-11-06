#!/usr/bin/env bash
# Host oracle for the packed-archive read trail accumulator
# (ISAAC_VITA_ARCHIVE_READ_TRAIL): builds host_vita_archive_trail.c and runs
# runtime/host_vita_archive_trail_oracle.c twice, once with a small record
# cap (the store overflows inside a scenario) and once at the header default
# the shipped configure uses, so the cap an eboot carries is the one proven.
set -eu

root=${ISAAC_ARCHIVE_TRAIL_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
work=${ISAAC_ARCHIVE_TRAIL_TEST_OUT:-$(mktemp -d "${TMPDIR:-/tmp}/isaac-archive-trail.XXXXXX")}
host_cc=${CC:-cc}
flags="-std=gnu11 -O2 -Wall -Wextra -Werror -Wconversion -DISAAC_VITA_ARCHIVE_READ_TRAIL=1"

for cap in "-DISAAC_VITA_ARCHIVE_READ_TRAIL_CAP=8U" ""; do
    "$host_cc" $flags $cap -I"$root/runtime" \
        "$root/runtime/host_vita_archive_trail.c" \
        "$root/runtime/host_vita_archive_trail_oracle.c" \
        -o "$work/archive-trail-host-oracle"
    "$work/archive-trail-host-oracle"
done
