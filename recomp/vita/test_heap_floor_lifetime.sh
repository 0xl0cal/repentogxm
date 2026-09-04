#!/usr/bin/env bash
set -euo pipefail

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=${ISAAC_FLOOR_LIFETIME_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
if [ -n "${ISAAC_FLOOR_LIFETIME_TEST_OUT:-}" ]; then
    work=$ISAAC_FLOOR_LIFETIME_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-floor-lifetime.XXXXXX")
fi
host_gcc=${HOST_GCC:-gcc}
host_clang=${HOST_CLANG:-clang}
arm_cc="$VITASDK/bin/arm-vita-eabi-gcc"
arm_nm="$VITASDK/bin/arm-vita-eabi-nm"
arm_readelf="$VITASDK/bin/arm-vita-eabi-readelf"

for command in python3 "$host_gcc" "$host_clang" \
        "$arm_cc" "$arm_nm" "$arm_readelf"; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "required floor-lifetime gate tool is absent: $command" >&2
        exit 2
    fi
done
if [ "$(realpath "$(command -v "$host_gcc")")" = \
     "$(realpath "$(command -v "$host_clang")")" ]; then
    echo "HOST_GCC and HOST_CLANG must be distinct compilers" >&2
    exit 2
fi
"$host_gcc" --version > "$work/gcc.version"
"$host_clang" --version > "$work/clang.version"
"$host_gcc" -dM -E -x c /dev/null > "$work/gcc.macros"
"$host_clang" -dM -E -x c /dev/null > "$work/clang.macros"
if ! grep -q '^#define __GNUC__ ' "$work/gcc.macros" ||
   grep -q '^#define __clang__ ' "$work/gcc.macros"; then
    echo "HOST_GCC is not a genuine GCC preprocessor backend" >&2
    exit 2
fi
if ! grep -q '^#define __clang__ ' "$work/clang.macros"; then
    echo "HOST_CLANG is not a genuine Clang preprocessor backend" >&2
    exit 2
fi
printf 'Host compiler backends: GCC=%s; Clang=%s\n' \
    "$(sed -n '1p' "$work/gcc.version")" \
    "$(sed -n '1p' "$work/clang.version")"

python3 - "$root/runtime/host_vita_heap.c" \
    "$root/runtime/host_vita_heap.h" <<'PY'
import ast
import pathlib
import re
import sys

heap = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
header = pathlib.Path(sys.argv[2]).read_text(encoding="utf-8")

def bodies(name):
    matches = list(re.finditer(
        r"\b%s\s*\([^;]*?\)\s*\{" % re.escape(name),
        heap, flags=re.S))
    if not matches:
        raise SystemExit("missing function: %s" % name)
    results = []
    for match in matches:
        depth = 1
        cursor = match.end()
        while cursor < len(heap) and depth:
            if heap[cursor] == "{":
                depth += 1
            elif heap[cursor] == "}":
                depth -= 1
            cursor += 1
        if depth:
            raise SystemExit("unterminated function: %s" % name)
        results.append(heap[match.end():cursor - 1])
    return results

def body(name):
    return bodies(name)[0]

constants = {
    "ISAAC_VITA_GUEST_HEAP_LEDGER_ENTRY_TABLE_BYTES": "0x00400000U",
    "ISAAC_VITA_GUEST_HEAP_FLOOR_PHASE_BYTES": "0x00080000U",
    "ISAAC_VITA_GUEST_HEAP_FLOOR_CURRENT_BYTES": "0x00010000U",
    "ISAAC_VITA_GUEST_HEAP_LEDGER_USABLE_BYTES": "0x00490000U",
    "ISAAC_VITA_GUEST_HEAP_LEDGER_REQUEST_BYTES": "0x00491000U",
    "ISAAC_VITA_GUEST_HEAP_LEDGER_RETAINED_BYTES": "0x00492000U",
    "ISAAC_VITA_GUEST_HEAP_TOTAL_REQUEST_BYTES": "0x00c66000U",
    "ISAAC_VITA_GUEST_HEAP_TOTAL_RETAINED_BYTES": "0x00c68000U",
}
for name, value in constants.items():
    match = re.search(r"(?m)^#define\s+%s\s+(\S+)\s*$" %
                      re.escape(name), header)
    if not match or match.group(1) != value:
        raise SystemExit("fixed floor-lifetime budget drifted: %s" % name)

layout = body("vita_heap_ledger_combined_layout")
for marker in ("capacity * sizeof(vita_heap_ledger_entry)",
               "current_bytes = capacity / 8U",
               "layout->phase_bytes = capacity",
               "layout->combined_bytes"):
    if marker not in layout:
        raise SystemExit("combined layout lost marker: %s" % marker)

rehash = next((item for item in bodies("vita_heap_ledger_rehash")
               if "vita_heap_ledger_combined_layout" in item), "")
if "vita_heap_ledger_combined_layout" not in rehash or \
        "vita_heap_floor_lifetime_slot_store" not in rehash:
    raise SystemExit("rehash no longer moves both sidecars")
remove = next((item for item in bodies("vita_heap_ledger_remove_commit_table")
               if "phases" in item), "")
if "phases && current_bits" not in remove or \
        remove.count("vita_heap_floor_lifetime_slot_store") != 2:
    raise SystemExit("backshift no longer moves+clears exact sidecars")
replace = body("vita_heap_ledger_replace_commit")
for marker in ("vita_heap_floor_lifetime_requested_size_replace_locked",
               "floor_token = vita_heap_floor_lifetime_slot_token",
               "vita_heap_floor_lifetime_slot_insert_locked"):
    if marker not in replace:
        raise SystemExit("replacement lost inherited metadata: %s" % marker)
if "s_ledger[new_slot].base == VITA_HEAP_LEDGER_TOMBSTONE" not in replace or \
        "--s_ledger_tombstones" not in replace:
    raise SystemExit("legacy range replacement lost tombstone reuse accounting")
failure_snapshot = body("vita_heap_failure_ledger_snapshot")
if failure_snapshot.count(
        "vita_heap_ledger_rehash_target_bytes_locked") != 1:
    raise SystemExit("failure snapshot bypasses combined rehash byte helper")
for name in ("vita_heap_migrate_native_to_overflow_locked",
             "vita_heap_migrate_overflow_to_native_locked"):
    migration = body(name)
    if migration.count("vita_heap_ledger_replace_commit") != 1:
        raise SystemExit("%s bypasses exact replacement commit" % name)

slab_move = body("vita_heap_room_slab_realloc_route")
first_retag = slab_move.find("vita_heap_floor_lifetime_retag_locked")
commit = slab_move.find("isaac_vita_room_entry_slab_realloc_commit_locked")
if first_retag < 0 or commit < 0 or first_retag > commit or \
        slab_move.count("vita_heap_floor_lifetime_retag_locked") != 2:
    raise SystemExit("slab MOVE retag/rollback order drifted")

rollover = body("vita_heap_floor_lifetime_rollover_locked")
if re.search(r"\b(?:for|while)\s*\(", rollover):
    raise SystemExit("rollover gained a ledger scan")
for marker in ("memset(s_ledger_floor_current, 0, layout.current_bytes)",
               "s_floor_lifetime.current_count = 0U",
               "sizeof s_floor_lifetime.phase_count"):
    if marker not in rollover:
        raise SystemExit("rollover lost bounded clear: %s" % marker)

note = body("isaac_vita_stage_memory_note")
ordered = ("vita_heap_lock();",
           "vita_floor_lifetime_stage_event_locked(",
           "vita_stage_memory_snapshot_locked(",
           "vita_floor_lifetime_record_locked(",
           "vita_heap_unlock();",
           "VITA_STAGE_MEMORY_MALLINFO()",
           '"stagemem: q=%x',
           '"floorlife: q=%x')
positions = [note.find(marker) for marker in ordered]
if any(position < 0 for position in positions) or \
        positions != sorted(positions) or \
        note.count("vita_heap_lock();") != 1 or \
        note.count("vita_heap_unlock();") != 1:
    raise SystemExit("boundary/snapshot/mallinfo/logger order drifted")

expected_format = (
    "floorlife: q=%x e=%s epoch=%x boot/roll=%x/%x phase=%s "
    "heap=%x/%x/%x/%x,%x/%x/%x/%x "
    "hphase=%x/%x/%x,%x/%x/%x slab=%x/%x/%x/%x "
    "sphase=%x/%x/%x valid/term/sat=%x/%x/%x"
)
floor_start = note.index('isaac_vita_log(\n        "floorlife:')
floor_tail = note[floor_start:]
call_end = floor_tail.index(");")
format_region = floor_tail[:call_end]
literals = re.findall(r'"(?:\\.|[^"\\])*"', format_region)
format_string = "".join(ast.literal_eval(item) for item in literals[:4])
if format_string != expected_format:
    raise SystemExit("floorlife exact format drifted: %r" % format_string)
conversions = re.findall(r"%[xs]", format_string)
if conversions.count("%x") != 28 or conversions.count("%s") != 2:
    raise SystemExit("floorlife conversion census drifted: %r" % conversions)
values = []
string_index = 0
for conversion in conversions:
    if conversion == "%x":
        values.append(2**32 - 1)
    else:
        values.append(("room-after-unload", "level-init")[string_index])
        string_index += 1
worst = format_string % tuple(values)
if len(worst.encode("ascii")) != 356 or len(worst.encode("ascii")) >= 384:
    raise SystemExit("floorlife logger bound drifted: %d" % len(worst))
print("floor-lifetime source/format gate: PASS")
PY

export ASAN_OPTIONS='detect_leaks=1:halt_on_error=1:allocator_may_return_null=1'
export UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1'
host_common=(
    -std=gnu11 -O1 -g -Wall -Wextra -Werror
    -fno-omit-frame-pointer -ffunction-sections -fdata-sections
    -fsanitize=address,undefined
    -DISAAC_VITA_STAGE_MEMORY_ORACLE=1
    -DISAAC_VITA_HEAP_TESTING=1
    -DISAAC_VITA_HEAP_RANGE_LEASE=1
    -DISAAC_VITA_TEXEL_OOM_DIAGNOSTIC=1
    -I"$root/runtime"
)
for compiler in "$host_gcc" "$host_clang"; do
    warning_flags=()
    if [ "$compiler" = "$host_gcc" ]; then
        warning_flags=(-Wno-maybe-uninitialized)
    fi
    name=$(basename "$compiler")
    for ledger_mode in legacy backshift; do
        ledger_defines=()
        if [ "$ledger_mode" = backshift ]; then
            ledger_defines=(-DISAAC_VITA_HEAP_LEDGER_BACKSHIFT=1)
        fi
        "$compiler" "${host_common[@]}" "${warning_flags[@]}" \
            "${ledger_defines[@]}" \
            "$root/runtime/host_vita_floor_lifetime_oracle.c" \
            -Wl,--gc-sections \
            -o "$work/floor-lifetime-$name-$ledger_mode"
        "$work/floor-lifetime-$name-$ledger_mode"
    done
done

host_object_flags=(
    -std=gnu11 -O1 -Wall -Wextra -Werror -Wno-maybe-uninitialized
    -ffunction-sections -fdata-sections -I"$root/runtime"
    -DISAAC_VITA_STAGE_MEMORY_ORACLE=1 -DISAAC_VITA_HEAP_TESTING=1
)
"$host_gcc" "${host_object_flags[@]}" \
    -c "$root/runtime/host_vita_heap.c" -o "$work/floor-off.o"
"$host_gcc" "${host_object_flags[@]}" \
    -DISAAC_VITA_HEAP_RANGE_LEASE=1 \
    -DISAAC_VITA_HEAP_LEDGER_BACKSHIFT=1 \
    -c "$root/runtime/host_vita_heap.c" -o "$work/floor-range.o"
if LC_ALL=C grep -aFq 'floorlife: q=%x' "$work/floor-off.o"; then
    echo "no-range feature OFF object unexpectedly contains floorlife" >&2
    exit 1
fi
if ! LC_ALL=C grep -aFq 'floorlife: q=%x' "$work/floor-range.o"; then
    echo "range-only object lost floorlife" >&2
    exit 1
fi

hybrid_defs=(
    -DISAAC_VITA_HEAP_RANGE_LEASE=1
    -DISAAC_VITA_HEAP_LEDGER_MEMBLOCK=1
    -DISAAC_VITA_HEAP_LEDGER_BACKSHIFT=1
    -DISAAC_VITA_HEAP_OVERFLOW_MSPACE=1
    -DISAAC_VITA_ROOM_ENTRY_SLAB=1
    -DISAAC_VITA_ROOM_ENTRY_HYBRID=1
    -DGUEST_IMAGE_BASE=0x98000000U
)
"$host_gcc" "${host_object_flags[@]}" "${hybrid_defs[@]}" \
    -c "$root/runtime/host_vita_heap.c" -o "$work/floor-hybrid.o"

fake_include="$work/fake-include"
mkdir -p "$fake_include/psp2/kernel"
cat > "$fake_include/psp2/kernel/sysmem.h" <<'EOF'
#ifndef ISAAC_FLOOR_FAKE_SYSMEM_H
#define ISAAC_FLOOR_FAKE_SYSMEM_H
#include <stdint.h>
typedef int32_t SceUID;
typedef uint32_t SceSize;
typedef uint32_t SceKernelMemBlockType;
typedef struct SceKernelAllocMemBlockOpt { uint32_t size; }
    SceKernelAllocMemBlockOpt;
#define SCE_KERNEL_MEMBLOCK_TYPE_USER_RW UINT32_C(0x0c20d060)
SceUID sceKernelAllocMemBlock(const char *, SceKernelMemBlockType,
                              SceSize, SceKernelAllocMemBlockOpt *);
int sceKernelGetMemBlockBase(SceUID, void **);
int sceKernelFreeMemBlock(SceUID);
#endif
EOF
cat > "$work/layout-main.c" <<'EOF'
#include <stddef.h>
#include "host_vita_heap_ledger_memblock.h"
int main(void)
{
    size_t block = 0U;
    return !isaac_vita_heap_ledger_memblock_layout(0x490000U, &block) ||
        block != 0x491000U;
}
EOF
"$host_gcc" -std=gnu11 -O2 -Wall -Wextra -Werror \
    -ffunction-sections -fdata-sections -I"$fake_include" \
    -I"$root/runtime" "$root/runtime/host_vita_heap_ledger_memblock.c" \
    "$work/layout-main.c" -Wl,--gc-sections -o "$work/layout-main"
"$work/layout-main"

cat > "$work/arm-main.c" <<'EOF'
#include <stdint.h>
#include "host_vita_heap.h"
void isaac_vita_log(const char *format, ...) { (void)format; }
void guest_fault(CPU *__restrict c, uint32_t address, const char *what)
{ (void)c; (void)address; (void)what; }
int guest_stack_violation(CPU *__restrict c, uint32_t pc, uint32_t kind,
                          uint32_t address, uint32_t size)
{ (void)c; (void)pc; (void)kind; (void)address; (void)size; return 0; }
int guest_stack_owner_violation(CPU *__restrict c, uint32_t pc)
{ (void)c; (void)pc; return 0; }
int main(void)
{
    isaac_vita_stage_memory_note(
        ISAAC_VITA_STAGE_MEMORY_LEVEL_BEGIN, 1U, 0U, UINT32_MAX);
    return 0;
}
EOF
arm_flags=(
    -std=gnu11 -O2 -mfloat-abi=soft -mcpu=cortex-a9
    -fno-builtin -fno-common -fno-short-enums -mword-relocations
    -fno-strict-aliasing -ffunction-sections -fdata-sections
    -Wall -Wextra -Werror -Wno-maybe-uninitialized -I"$root/runtime"
)
"$arm_cc" "${arm_flags[@]}" -D__vita__=1 \
    -DISAAC_VITA_HEAP_RANGE_LEASE=1 \
    -DISAAC_VITA_HEAP_LEDGER_BACKSHIFT=1 \
    -c "$root/runtime/host_vita_heap.c" -o "$work/floor-range.arm.o"
"$arm_cc" "${arm_flags[@]}" -D__vita__=1 \
    -c "$work/arm-main.c" -o "$work/floor-main.arm.o"
"$arm_cc" "${arm_flags[@]}" "$work/floor-range.arm.o" \
    "$work/floor-main.arm.o" -Wl,--gc-sections \
    -o "$work/floor-range.arm.elf"
"$arm_cc" "${arm_flags[@]}" -D__vita__=1 \
    "${hybrid_defs[@]}" -c "$root/runtime/host_vita_heap.c" \
    -o "$work/floor-hybrid.arm.o"

for object in "$work/floor-range.arm.o" "$work/floor-hybrid.arm.o"; do
    "$arm_nm" -u "$object" > "$object.undefined"
    if grep -Eq \
            '[[:space:]]U[[:space:]]+(__atomic[^[:space:]]*|__sync[^[:space:]]*|__aeabi_(ll|ul|l)[^[:space:]]*)$' \
            "$object.undefined"; then
        echo "ARM floor-lifetime object gained atomic/64-bit helper: $object" >&2
        exit 1
    fi
done
if ! LC_ALL=C grep -aFq 'floorlife: q=%x' \
        "$work/floor-range.arm.o"; then
    echo "ARM range object lost floorlife format" >&2
    exit 1
fi
"$arm_readelf" -h "$work/floor-range.arm.elf" > "$work/arm.header"
"$arm_readelf" -A "$work/floor-range.arm.elf" > "$work/arm.attributes"
grep -q 'Machine:[[:space:]]*ARM$' "$work/arm.header"
grep -q 'Version5 EABI, soft-float ABI' "$work/arm.header"
if grep -q 'Tag_ABI_VFP_args' "$work/arm.attributes"; then
    echo "floor-lifetime ARM ELF advertises hardfp arguments" >&2
    exit 1
fi

echo "Vita heap floor-lifetime gate: PASS (combined failure layout; legacy tombstone/backshift/rehash cold census; failed/same/moved realloc; Continue/rollover/malformed/saturation; GCC+Clang ASan/UBSan; no-range/range/Hybrid; ARM EABI5 softfp; ARM ELF not executed)"
