#!/usr/bin/env bash
set -eu

root=${ISAAC_AUDIO_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-audio-cooperative.XXXXXX")
trap 'rm -rf "$work"' EXIT
host_cc=${HOST_CC:-cc}

"$host_cc" -std=gnu11 -O2 -fno-strict-aliasing -Wall -Wextra -Werror \
    -Wno-error=maybe-uninitialized \
    -DISAAC_VITA_AUDIO=1 -DISAAC_VITA_AUDIO_STREAM_RECEIPT=1 \
    -DGUEST_IMAGE_BASE=0x27000000U \
    -I"$root/runtime" -I"$root/vita" \
    "$root/runtime/host_vita_audio_cooperative.c" \
    "$root/runtime/guest_stack_legacy_oracle_stub.c" \
    "$root/runtime/vita_audio_cooperative_oracle.c" \
    -o "$work/oracle"
"$work/oracle"
