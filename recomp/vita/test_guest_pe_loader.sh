#!/usr/bin/env bash
set -eu

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=${ISAAC_GUEST_PE_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
if [ -n "${ISAAC_GUEST_PE_TEST_OUT:-}" ]; then
    work=$ISAAC_GUEST_PE_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-guest-pe-loader.XXXXXX")
fi

cc="$VITASDK/bin/arm-vita-eabi-gcc"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"
nm="$VITASDK/bin/arm-vita-eabi-nm"

flags="-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror"

# The same exhaustive source executes on PC.  The ARM edge is deliberately a
# compile/link gate only: it proves the pure SHA/parser/map/transaction code is
# valid softfp Vita C without installing or launching a candidate.
"$cc" $flags -I"$root/runtime" \
    "$root/runtime/guest_pe_loader_oracle.c" \
    -Wl,--gc-sections \
    -lSceLibKernel_stub -lSceKernelThreadMgr_stub \
    -o "$work/vita-guest-pe-loader-oracle.elf"

"$readelf" -h "$work/vita-guest-pe-loader-oracle.elf"
"$readelf" -A "$work/vita-guest-pe-loader-oracle.elf"

# Keep guest_image_load and its __vita__ fixed-allocation/file/TLS path alive
# through the final link.  The fixed allocator here is a link seam only; this
# ELF is never executed and never replaces the candidate's fixed_image.c.
"$cc" $flags -DGUEST_IMAGE_BASE=0x98000000u -I"$root/runtime" \
    "$root/runtime/guest.c" \
    "$root/runtime/guest_pe_vita_link_probe.c" \
    -Wl,--gc-sections \
    -lSceLibKernel_stub -lSceKernelThreadMgr_stub \
    -o "$work/vita-guest-pe-integration-link.elf"

"$readelf" -h "$work/vita-guest-pe-integration-link.elf"
"$readelf" -A "$work/vita-guest-pe-integration-link.elf"
if "$nm" -u "$work/vita-guest-pe-integration-link.elf" | grep -Eq \
        'isaac_vita_fixed_|guest_pe_|__atomic|libatomic'; then
    "$nm" -u "$work/vita-guest-pe-integration-link.elf" >&2
    echo "Vita PE loader integration retained a forbidden unresolved symbol" >&2
    exit 1
fi
sha256sum \
    "$root/runtime/guest_pe.h" \
    "$root/runtime/guest_pe_loader_oracle.c" \
    "$root/runtime/guest_pe_vita_link_probe.c" \
    "$work/vita-guest-pe-loader-oracle.elf" \
    "$work/vita-guest-pe-integration-link.elf"
echo "Vita bounded PE loader compile/link/static artifact check: PASS (behavioural oracle executes on PC)"
