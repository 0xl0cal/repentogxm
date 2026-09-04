#!/usr/bin/env bash
set -eu

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=${ISAAC_TEXEL_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
work=${ISAAC_TEXEL_TEST_OUT:-$(mktemp -d "${TMPDIR:-/tmp}/isaac-texel-scratch.XXXXXX")}
platform_include=${ISAAC_TEXEL_PLATFORM_INCLUDE:-$root/vita}
mkdir -p "$work"

host_cc=${CC:-cc}
cc="$VITASDK/bin/arm-vita-eabi-gcc"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"
nm="$VITASDK/bin/arm-vita-eabi-nm"
fake_include="$root/vita/anm2_scratch_oracle_include"
common="-std=gnu11 -O2 -Wall -Wextra -Werror -I$root/runtime -I$platform_include"
arm="$common -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb -ffunction-sections -fdata-sections"

python3 - "$root/vita/CMakeLists.txt" <<'PY'
import pathlib
import re
import sys

path = pathlib.Path(sys.argv[1])
text = path.read_text(encoding="utf-8")
options = re.findall(
    r"option\(ISAAC_VITA_TEXEL_SCRATCH_8M\s+.*?\s+(ON|OFF)\)",
    text,
    re.DOTALL,
)
if options != ["OFF"]:
    raise SystemExit("8 MiB texel diagnostic option must default exactly OFF")
if not re.search(
    r"if\(ISAAC_VITA_TEXEL_SCRATCH_8M AND NOT "
    r"ISAAC_VITA_TEXEL_SCRATCH\).*?message\(FATAL_ERROR",
    text,
    re.DOTALL,
):
    raise SystemExit("8 MiB texel option lost its scratch dependency")
lines = text.splitlines()
markers = [
    index
    for index, line in enumerate(lines)
    if re.fullmatch(r"\s*ISAAC_VITA_TEXEL_SCRATCH_8M=1\)", line)
]
if len(markers) != 1:
    raise SystemExit(
        f"expected one 8 MiB source definition, found {len(markers)}"
    )
marker = markers[0]
starts = [
    index
    for index in range(marker, -1, -1)
    if "set_property(SOURCE" in lines[index]
]
if not starts:
    raise SystemExit("8 MiB definition is not source-scoped")
block = "\n".join(lines[starts[0] : marker + 1])
owners = re.findall(r'"\$\{ISAAC_RUNTIME\}/([^\"]+)"', block)
if owners != ["host_vita_texel_scratch.c"]:
    raise SystemExit(
        f"8 MiB texel definition escaped its owner: {owners}"
    )
print("Vita texel scratch 8 MiB CMake default/dependency/source scope: PASS")
PY

"$host_cc" $common -pthread -DISAAC_VITA_TEXEL_SCRATCH_ORACLE=1 \
    -I"$fake_include" \
    "$root/runtime/host_vita_texel_scratch.c" \
    "$root/runtime/host_vita_texel_scratch_oracle.c" \
    -o "$work/texel-scratch-host-oracle"
"$work/texel-scratch-host-oracle"
"$host_cc" $common -pthread -DISAAC_VITA_TEXEL_SCRATCH_ORACLE=1 \
    -DISAAC_VITA_TEXEL_SCRATCH_8M=1 -I"$fake_include" \
    "$root/runtime/host_vita_texel_scratch.c" \
    "$root/runtime/host_vita_texel_scratch_oracle.c" \
    -o "$work/texel-scratch-8m-host-oracle"
"$work/texel-scratch-8m-host-oracle"

# Freeze the actual heap-import order and clean-failure semantics.  A failed
# USER_RW reservation must continue through ordinary guest malloc; an exact
# PNG stack whose fallback malloc also fails must still reach the one-shot
# texel diagnostic.  A successful reservation bypasses both.
routing_common="-std=gnu11 -O2 -Wall -Wextra -Werror \
-DGUEST_IMAGE_BASE=0x98000000u -DGUEST_STACK_REQUIRED=1 \
-ffunction-sections -fdata-sections \
-I$root/runtime -I$platform_include -I$fake_include"
"$host_cc" $routing_common -DISAAC_VITA_TEXEL_SCRATCH=1 \
    -DISAAC_VITA_TEXEL_OOM_DIAGNOSTIC=1 \
    -DISAAC_VITA_HEAP_RANGE_LEASE=1 \
    -Dmalloc=oracle_guest_malloc \
    -c "$root/runtime/host_vita_heap.c" \
    -o "$work/host-vita-heap.texel-routing.o"
"$host_cc" $routing_common -c "$root/runtime/guest.c" \
    -o "$work/guest.texel-routing.o"
"$host_cc" $routing_common -DISAAC_VITA_TEXEL_SCRATCH_ORACLE=1 \
    -c "$root/runtime/host_vita_texel_scratch.c" \
    -o "$work/texel-scratch.routing-state.o"
"$host_cc" $routing_common -DISAAC_VITA_TEXEL_SCRATCH_ORACLE=1 \
    -DISAAC_VITA_TEXEL_SCRATCH_ROUTING_ORACLE=1 \
    -c "$root/runtime/host_vita_texel_scratch_oracle.c" \
    -o "$work/texel-scratch.routing-oracle.o"
"$host_cc" \
    "$work/host-vita-heap.texel-routing.o" \
    "$work/guest.texel-routing.o" \
    "$work/texel-scratch.routing-state.o" \
    "$work/texel-scratch.routing-oracle.o" \
    -Wl,--gc-sections \
    -o "$work/texel-scratch-routing-oracle"
"$work/texel-scratch-routing-oracle"

"$host_cc" $routing_common -DISAAC_VITA_TEXEL_SCRATCH_ORACLE=1 \
    -DISAAC_VITA_TEXEL_SCRATCH_8M=1 \
    -c "$root/runtime/host_vita_texel_scratch.c" \
    -o "$work/texel-scratch-8m.routing-state.o"
"$host_cc" $routing_common -DISAAC_VITA_TEXEL_SCRATCH_ORACLE=1 \
    -DISAAC_VITA_TEXEL_SCRATCH_ROUTING_ORACLE=1 \
    -DISAAC_VITA_TEXEL_SCRATCH_8M=1 \
    -c "$root/runtime/host_vita_texel_scratch_oracle.c" \
    -o "$work/texel-scratch-8m.routing-oracle.o"
"$host_cc" \
    "$work/host-vita-heap.texel-routing.o" \
    "$work/guest.texel-routing.o" \
    "$work/texel-scratch-8m.routing-state.o" \
    "$work/texel-scratch-8m.routing-oracle.o" \
    -Wl,--gc-sections \
    -o "$work/texel-scratch-8m-routing-oracle"
"$work/texel-scratch-8m-routing-oracle"

if [ -n "${ISAAC_TEXEL_PE_PATH:-}" ] && \
   [ -n "${ISAAC_TEXEL_GENERATED_DIR:-}" ]; then
    python3 "$root/test_texel_scratch_census.py" \
        --pe "$ISAAC_TEXEL_PE_PATH" \
        --generated "$ISAAC_TEXEL_GENERATED_DIR" \
        --header "$root/runtime/kage_vita_texture_memory.h" \
        --source "$root/runtime/host_vita_texel_scratch.c" \
        --oracle "$root/runtime/host_vita_texel_scratch_oracle.c"
elif [ -n "${ISAAC_TEXEL_PE_PATH:-}" ] || \
     [ -n "${ISAAC_TEXEL_GENERATED_DIR:-}" ]; then
    echo "set both ISAAC_TEXEL_PE_PATH and ISAAC_TEXEL_GENERATED_DIR" >&2
    exit 2
else
    echo "Texel scratch procedural frozen census: SKIP (set ISAAC_TEXEL_PE_PATH and ISAAC_TEXEL_GENERATED_DIR)"
fi

# Compile the production source against both the real VitaSDK sysmem header
# and the project-local fake.  This module uses only the NULL-option API
# surface, so identical objects pin the fake declaration to the real one.
"$cc" $arm -c "$root/runtime/host_vita_texel_scratch.c" \
    -o "$work/texel-scratch.actual-header.o"
"$cc" $arm -I"$fake_include" \
    -c "$root/runtime/host_vita_texel_scratch.c" \
    -o "$work/texel-scratch.fake-header.o"
if ! cmp -s "$work/texel-scratch.actual-header.o" \
              "$work/texel-scratch.fake-header.o"; then
    echo "fake sysmem header changed the production texel object" >&2
    exit 3
fi

"$cc" $arm -DISAAC_VITA_TEXEL_SCRATCH_8M=1 \
    -c "$root/runtime/host_vita_texel_scratch.c" \
    -o "$work/texel-scratch-8m.actual-header.o"
"$cc" $arm -DISAAC_VITA_TEXEL_SCRATCH_8M=1 -I"$fake_include" \
    -c "$root/runtime/host_vita_texel_scratch.c" \
    -o "$work/texel-scratch-8m.fake-header.o"
if ! cmp -s "$work/texel-scratch-8m.actual-header.o" \
              "$work/texel-scratch-8m.fake-header.o"; then
    echo "fake sysmem header changed the 8 MiB texel object" >&2
    exit 3
fi
if cmp -s "$work/texel-scratch.actual-header.o" \
          "$work/texel-scratch-8m.actual-header.o"; then
    echo "8 MiB texel selection did not change the ARM object" >&2
    exit 3
fi

"$cc" $arm -I"$fake_include" \
    -DISAAC_VITA_TEXEL_SCRATCH_LINK_PROBE=1 \
    "$root/runtime/host_vita_texel_scratch.c" \
    "$root/runtime/host_vita_texel_scratch_oracle.c" \
    -Wl,--gc-sections -o "$work/texel-scratch-softfp-link.elf"
"$cc" $arm -I"$fake_include" \
    -DISAAC_VITA_TEXEL_SCRATCH_LINK_PROBE=1 \
    -DISAAC_VITA_TEXEL_SCRATCH_8M=1 \
    "$root/runtime/host_vita_texel_scratch.c" \
    "$root/runtime/host_vita_texel_scratch_oracle.c" \
    -Wl,--gc-sections -o "$work/texel-scratch-8m-softfp-link.elf"

"$readelf" -h "$work/texel-scratch-softfp-link.elf" > "$work/header"
"$readelf" -A "$work/texel-scratch-softfp-link.elf" > "$work/attributes"
"$readelf" -h "$work/texel-scratch-8m-softfp-link.elf" \
    > "$work/8m-header"
"$readelf" -A "$work/texel-scratch-8m-softfp-link.elf" \
    > "$work/8m-attributes"
"$nm" -u "$work/texel-scratch.actual-header.o" \
    > "$work/production.undefined"
"$nm" -u "$work/texel-scratch-8m.actual-header.o" \
    > "$work/8m-production.undefined"
"$nm" -u "$work/texel-scratch-softfp-link.elf" \
    > "$work/link.undefined"
"$nm" -u "$work/texel-scratch-8m-softfp-link.elf" \
    > "$work/8m-link.undefined"
"$nm" --defined-only "$work/texel-scratch.actual-header.o" \
    > "$work/production.defined"
cat "$work/header"
cat "$work/attributes"

if ! grep -q "Version5 EABI, soft-float ABI" "$work/header"; then
    echo "texel scratch link is not ARM EABI5 softfp" >&2
    exit 4
fi
if grep -q "Tag_ABI_VFP_args" "$work/attributes"; then
    echo "texel scratch link advertises hardfp arguments" >&2
    exit 5
fi
if ! grep -q "Version5 EABI, soft-float ABI" "$work/8m-header" || \
   grep -q "Tag_ABI_VFP_args" "$work/8m-attributes"; then
    echo "8 MiB texel scratch link is not ARM EABI5 softfp" >&2
    exit 5
fi
if [ -s "$work/link.undefined" ]; then
    cat "$work/link.undefined" >&2
    echo "fake-sysmem ARM texel link has unresolved symbols" >&2
    exit 6
fi
if [ -s "$work/8m-link.undefined" ]; then
    cat "$work/8m-link.undefined" >&2
    echo "8 MiB fake-sysmem ARM texel link has unresolved symbols" >&2
    exit 6
fi

for symbol in isaac_vita_log sceKernelAllocMemBlock \
              sceKernelFreeMemBlock sceKernelGetMemBlockBase
do
    if ! grep -Eq "[[:space:]]U[[:space:]]+$symbol$" \
            "$work/production.undefined"; then
        cat "$work/production.undefined" >&2
        echo "production texel object misses dependency: $symbol" >&2
        exit 7
    fi
    if ! grep -Eq "[[:space:]]U[[:space:]]+$symbol$" \
            "$work/8m-production.undefined"; then
        cat "$work/8m-production.undefined" >&2
        echo "8 MiB production texel object misses dependency: $symbol" >&2
        exit 7
    fi
done
if [ "$(wc -l < "$work/production.undefined")" -ne 4 ]; then
    cat "$work/production.undefined" >&2
    echo "production texel object gained an unexpected dependency" >&2
    exit 8
fi
if [ "$(wc -l < "$work/8m-production.undefined")" -ne 4 ]; then
    cat "$work/8m-production.undefined" >&2
    echo "8 MiB production texel object gained an unexpected dependency" >&2
    exit 8
fi

for symbol in isaac_vita_texel_scratch_malloc \
              isaac_vita_texel_scratch_free \
              isaac_vita_texel_scratch_realloc
do
    if ! grep -Eq "[[:space:]]T[[:space:]]+$symbol$" \
            "$work/production.defined"; then
        cat "$work/production.defined" >&2
        echo "production texel object misses API symbol: $symbol" >&2
        exit 9
    fi
done
if "$nm" "$work/texel-scratch.actual-header.o" \
        "$work/texel-scratch-8m.actual-header.o" \
        "$work/texel-scratch-softfp-link.elf" \
        "$work/texel-scratch-8m-softfp-link.elf" | \
        grep -Eq '(__atomic|libatomic|[[:space:]](malloc|calloc|realloc|free)$)'; then
    echo "texel scratch reached libatomic or a libc allocator" >&2
    exit 10
fi

if [ -n "${ISAAC_TEXEL_MAP_PATH:-}" ]; then
    python3 - \
        "$root/runtime/kage_vita_texture_memory.h" \
        "$root/runtime/host_vita_anm2_scratch.h" \
        "$ISAAC_TEXEL_MAP_PATH" <<'PY'
import pathlib
import re
import sys

texel_header = pathlib.Path(sys.argv[1])
anm2_header = pathlib.Path(sys.argv[2])
map_path = pathlib.Path(sys.argv[3])

def macro(path, name):
    text = path.read_text(encoding="utf-8")
    match = re.search(
        rf"^\s*#define\s+{re.escape(name)}\s+(0x[0-9a-fA-F]+|[0-9]+)U?\s*$",
        text,
        re.MULTILINE,
    )
    if not match:
        raise SystemExit(f"missing integer macro {name} in {path}")
    return int(match.group(1), 0)

map_text = map_path.read_text(encoding="utf-8", errors="replace")
ends = [
    int(value, 16)
    for value in re.findall(
        r"^\s*(0x[0-9a-fA-F]+)\s+_end\s*=\s*\.\s*$",
        map_text,
        re.MULTILINE,
    )
]
if len(ends) != 1:
    raise SystemExit(
        f"expected exactly one '_end = .' in {map_path}, found {len(ends)}"
    )

texel_link = macro(texel_header, "KAGE_VITA_TEXEL_LINK_END")
texel_retained = macro(texel_header, "KAGE_VITA_TEXEL_HEAP_RETAINED_END")
anm2_link = macro(anm2_header, "ISAAC_VITA_ANM2_LINK_END")
anm2_retained = macro(anm2_header, "ISAAC_VITA_ANM2_HEAP_RETAINED_END")
heap_bytes = macro(anm2_header, "ISAAC_VITA_ANM2_HEAP_BYTES")
heap_pad = macro(anm2_header, "ISAAC_VITA_ANM2_HEAP_ALIGNMENT_PAD")
page = macro(texel_header, "KAGE_VITA_TEXEL_MEMBLOCK_PAGE_BYTES")
expected_retained = (texel_link + heap_bytes + heap_pad + page - 1) & -page

if ends[0] != texel_link:
    raise SystemExit(
        f"map _end 0x{ends[0]:08x} != texel LINK_END 0x{texel_link:08x}"
    )
if texel_link != anm2_link or texel_retained != anm2_retained:
    raise SystemExit(
        "texel and ANM2 headers disagree on the shared linked/heap interval"
    )
if texel_retained != expected_retained:
    raise SystemExit(
        f"retained end 0x{texel_retained:08x} != formula 0x{expected_retained:08x}"
    )
print(
    "Vita texel scratch final-map contract: PASS "
    f"(_end=0x{texel_link:08x}, retained=0x{texel_retained:08x})"
)
PY
else
    echo "Vita texel scratch final-map contract: SKIP (set ISAAC_TEXEL_MAP_PATH)"
fi

sha256sum \
    "$root/runtime/host_vita_texel_scratch.h" \
    "$root/runtime/host_vita_texel_scratch.c" \
    "$root/runtime/host_vita_texel_scratch_oracle.c" \
    "$root/runtime/kage_vita_texture_memory.h" \
    "$root/test_texel_scratch_census.py" \
    "$root/vita/anm2_scratch_oracle_include/psp2/kernel/sysmem.h" \
    "$root/vita/test_texel_scratch.sh" \
    "$work/texel-scratch-host-oracle" \
    "$work/texel-scratch-8m-host-oracle" \
    "$work/texel-scratch-routing-oracle" \
    "$work/texel-scratch-8m-routing-oracle" \
    "$work/guest.texel-routing.o" \
    "$work/texel-scratch.actual-header.o" \
    "$work/texel-scratch-8m.actual-header.o" \
    "$work/texel-scratch-softfp-link.elf" \
    "$work/texel-scratch-8m-softfp-link.elf"
echo "Vita texel scratch default/8 MiB lifecycle/concurrency/canaries + fallback routing + memblock contract + ARM diff/EABI5 softfp/link: PASS (ARM ELF was not executed)"
