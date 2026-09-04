#!/usr/bin/env bash
set -eu

root=${ISAAC_ARCHIVE_RECEIPT_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
packed=${ISAAC_ARCHIVE_RECEIPT_PACKED_DIR:-${1:-}}
work=${ISAAC_ARCHIVE_RECEIPT_TEST_OUT:-$(mktemp -d "${TMPDIR:-/tmp}/isaac-archive-receipt.XXXXXX")}
host_cc=${CC:-cc}

if [ -z "$packed" ]; then
    echo "usage: ISAAC_ARCHIVE_RECEIPT_PACKED_DIR=/path/to/resources/packed $0" >&2
    exit 2
fi
for archive in animations.a afterbirth.a; do
    if [ ! -f "$packed/$archive" ]; then
        echo "missing exact archive fixture: $packed/$archive" >&2
        exit 2
    fi
done

mkdir -p "$work/data"
"$host_cc" -std=c11 -O2 -Wall -Wextra -Werror \
    -DISAAC_VITA_ARCHIVE_RECEIPT_ORACLE=1 \
    -I"$root/runtime" -I"$root/vita" \
    "$root/runtime/host_vita_archive_receipt.c" \
    "$root/runtime/host_vita_archive_receipt_oracle.c" \
    -o "$work/archive-receipt-host-oracle"

"$work/archive-receipt-host-oracle" \
    "$work/data" "$packed/animations.a" "$packed/afterbirth.a"
