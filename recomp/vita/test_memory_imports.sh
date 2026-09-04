#!/usr/bin/env bash
set -eu

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=${ISAAC_MEMORY_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
if [ -n "${ISAAC_MEMORY_TEST_OUT:-}" ]; then
    work=$ISAAC_MEMORY_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-memory-imports.XXXXXX")
fi

cc="$VITASDK/bin/arm-vita-eabi-gcc"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"
nm="$VITASDK/bin/arm-vita-eabi-nm"
host_cc=${HOST_CC:-cc}

arm_base_flags="-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror -Wno-maybe-uninitialized"
common_flags="$arm_base_flags -DISAAC_VITA_MEMORY_RANGE_GUARD=1"
# guest.h's legacy-stack inline has an established GCC maybe-uninitialized
# false positive.  The range-query outputs themselves are explicitly zeroed.
host_flags="-std=gnu11 -O2 -fno-strict-aliasing -ffunction-sections -fdata-sections -Wall -Wextra -Werror -Wno-maybe-uninitialized -no-pie -DISAAC_VITA_MEMORY_RANGE_GUARD=1 -DISAAC_VITA_MEMORY_RANGE_ORACLE=1 -DISAAC_VITA_MEMORY_HOST_LOW_ORACLE=1"

python3 - "$root/vita/CMakeLists.txt" <<'PY'
import pathlib
import re
import sys

text = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
options = re.findall(
    r"option\(ISAAC_VITA_MEMORY_RANGE_GUARD\s+.*?\s+(ON|OFF)\)",
    text,
    re.DOTALL,
)
if options != ["OFF"]:
    raise SystemExit("memory range guard must default exactly OFF")
if not re.search(
    r"if\(ISAAC_VITA_MEMORY_RANGE_GUARD AND "
    r"ISAAC_VITA_SCAFFOLD_ONLY\).*?message\(FATAL_ERROR",
    text,
    re.DOTALL,
):
    raise SystemExit("memory range guard lost its translated-runtime check")
lines = text.splitlines()
markers = [
    index
    for index, line in enumerate(lines)
    if re.fullmatch(r"\s*ISAAC_VITA_MEMORY_RANGE_GUARD=1\)", line)
]
if len(markers) != 1:
    raise SystemExit(
        f"expected one source definition for memory guard, found {len(markers)}"
    )
marker = markers[0]
starts = [
    index
    for index in range(marker, -1, -1)
    if "set_property(SOURCE" in lines[index]
]
if not starts:
    raise SystemExit("memory range guard definition is not source-scoped")
block = "\n".join(lines[starts[0] : marker + 1])
owners = re.findall(r'"\$\{ISAAC_RUNTIME\}/([^\"]+)"', block)
if owners != ["host_vita_memory.c"]:
    raise SystemExit(f"memory range guard escaped its owner: {owners}")
print("Vita catastrophic memory guard CMake default/scope: PASS")
PY

# The ordinary object must not acquire either a sysmem hot-path dependency or
# the diagnostic logger.  Conversely, the opted-in object must reference both.
"$cc" $arm_base_flags -DGUEST_IMAGE_BASE=0x98000000u \
    -I"$root/runtime" -c "$root/runtime/host_vita_memory.c" \
    -o "$work/host_vita_memory.default.o"
"$nm" -u "$work/host_vita_memory.default.o" \
    > "$work/host_vita_memory.default.undefined"
if grep -Eq 'sceKernelGetMemBlockInfoByAddr|isaac_vita_log' \
        "$work/host_vita_memory.default.undefined"; then
    echo "default memory object unexpectedly retained diagnostic imports" >&2
    exit 1
fi

"$cc" $common_flags -DGUEST_IMAGE_BASE=0x98000000u \
    -I"$root/runtime" -c "$root/runtime/host_vita_memory.c" \
    -o "$work/host_vita_memory.guard.o"
"$nm" -u "$work/host_vita_memory.guard.o" \
    > "$work/host_vita_memory.guard.undefined"
for symbol in sceKernelGetMemBlockInfoByAddr isaac_vita_log; do
    if ! grep -Eq "[[:space:]]U[[:space:]]+$symbol$" \
            "$work/host_vita_memory.guard.undefined"; then
        echo "enabled memory object lost $symbol" >&2
        exit 1
    fi
done

# A non-PIE host executable keeps its static test buffers below 4 GiB, which
# lets the exact identity-pointer implementation run instead of merely link.
"$host_cc" $host_flags -DGUEST_IMAGE_BASE=0x98000000u \
    -I"$root/runtime" \
    "$root/runtime/host_vita_memory.c" \
    "$root/runtime/guest_stack_legacy_oracle_stub.c" \
    "$root/runtime/vita_memory_import_test.c" \
    -o "$work/vita-memory-import-host-oracle"
"$work/vita-memory-import-host-oracle"

"$cc" $common_flags -DGUEST_IMAGE_BASE=0x98000000u \
    -I"$root/runtime" \
    "$root/runtime/host_vita_memory.c" \
    "$root/runtime/guest_stack_legacy_oracle_stub.c" \
    "$root/runtime/vita_memory_import_test.c" \
    -o "$work/vita-memory-import-oracle.elf"

"$readelf" -h "$work/vita-memory-import-oracle.elf"
"$readelf" -A "$work/vita-memory-import-oracle.elf"
sha256sum \
    "$root/runtime/host_vita_memory.h" \
    "$root/runtime/host_vita_memory.c" \
    "$root/runtime/guest_stack_legacy_oracle_stub.h" \
    "$root/runtime/guest_stack_legacy_oracle_stub.c" \
    "$root/runtime/vita_memory_import_test.c" \
    "$work/vita-memory-import-host-oracle" \
    "$work/vita-memory-import-oracle.elf"
echo "Vita VCRUNTIME memory host execution + ARM compile/link/static artifact check: PASS"
