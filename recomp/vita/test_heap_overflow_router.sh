#!/usr/bin/env bash
set -euo pipefail

export ASAN_OPTIONS='detect_leaks=1:halt_on_error=1'
export UBSAN_OPTIONS='halt_on_error=1:print_stacktrace=1'

if [ -z "${VITASDK:-}" ]; then
    echo "VITASDK must name the softfp VitaSDK" >&2
    exit 2
fi

root=${ISAAC_HEAP_OVERFLOW_ROUTER_TEST_ROOT:-$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)}
if [ -n "${ISAAC_HEAP_OVERFLOW_ROUTER_TEST_OUT:-}" ]; then
    work=$ISAAC_HEAP_OVERFLOW_ROUTER_TEST_OUT
    mkdir -p "$work"
else
    work=$(mktemp -d "${TMPDIR:-/tmp}/isaac-heap-overflow-router.XXXXXX")
fi

cc="$VITASDK/bin/arm-vita-eabi-gcc"
nm="$VITASDK/bin/arm-vita-eabi-nm"
readelf="$VITASDK/bin/arm-vita-eabi-readelf"
fake_include="$work/fake-include"
mkdir -p "$fake_include/psp2/kernel" "$fake_include/psp2"

python3 - \
    "$root/runtime/host_vita_heap.c" \
    "$root/runtime/entry_vita.c" \
    "$root/runtime/host_vita_room_entry_slab.c" \
    "$root/vita/CMakeLists.txt" <<'PY'
import pathlib
import sys

heap_source = pathlib.Path(sys.argv[1]).read_text(encoding="utf-8")
event_init_begin = heap_source.index(
    "static void vita_heap_telemetry_event_init(")
event_init_end = heap_source.index(
    "static void vita_heap_telemetry_claim_locked(", event_init_begin)
event_init_body = heap_source[event_init_begin:event_init_end]
if "memset" in event_init_body or "event->edges = 0U;" not in event_init_body:
    raise SystemExit("telemetry event init regained per-operation snapshot clear")
ready_begin = heap_source.index(
    "static int vita_heap_new_allocation_ready_locked(void)")
ready_end = heap_source.index(
    "static int vita_heap_pointer_is_overflow_locked(", ready_begin)
ready_body = heap_source[ready_begin:ready_end]
if ("vita_heap_overflow_state_locked" in ready_body or
        "mspace_snapshot" in ready_body):
    raise SystemExit("native allocation readiness regained a raw snapshot")
telemetry_begin = heap_source.index(
    "static void vita_heap_telemetry_validate_locked(void)")
telemetry_end = heap_source.index(
    "static void vita_heap_telemetry_snapshot_locked(", telemetry_begin)
telemetry_body = heap_source[telemetry_begin:telemetry_end]
if ("isaac_vita_room_entry_slab_raw_accounting_locked" not in telemetry_body or
        "isaac_vita_room_entry_slab_snapshot_locked" in telemetry_body):
    raise SystemExit("hot heap telemetry regained a cold slab snapshot")
slab_source = pathlib.Path(sys.argv[3]).read_text(encoding="utf-8")
accounting_begin = slab_source.index(
    "int isaac_vita_room_entry_slab_raw_accounting_locked(")
accounting_end = slab_source.index(
    "int isaac_vita_room_entry_slab_owns_exact_locked(", accounting_begin)
accounting_body = slab_source[accounting_begin:accounting_end]
if any(marker not in accounting_body for marker in (
        "room_slab_validate_fast()",
        "s_room_slab.raw_page_count",
        "ISAAC_VITA_ROOM_ENTRY_SLAB_PAGE_BYTES")) or any(
            marker in accounting_body for marker in (
                "room_slab_snapshot_fill", "room_slab_validate_cold",
                "room_slab_bitmap_count", "for (", "while (")):
    raise SystemExit("raw slab accounting observer is no longer bounded O(1)")
reserve_begin = heap_source.index(
    "static int vita_heap_ledger_reserve_target(")
reserve_end = heap_source.index(
    "static size_t vita_heap_find_slot(", reserve_begin)
reserve_body = heap_source[reserve_begin:reserve_end]
fixed_overflow_begin = reserve_body.index(
    "#ifdef ISAAC_VITA_HEAP_OVERFLOW_MSPACE")
fixed_overflow_end = reserve_body.index("#else", fixed_overflow_begin)
fixed_overflow_body = reserve_body[fixed_overflow_begin:fixed_overflow_end]
if ("ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_LIVE_LIMIT"
        not in fixed_overflow_body or
        "s_ledger_capacity / 2U" in fixed_overflow_body):
    raise SystemExit("fixed ledger reserve lost the named 3/4 ceiling")
begin = heap_source.index("static void vita_heap_malloc(")
end = heap_source.index("static void vita_heap_set_new_mode(", begin)
body = heap_source[begin:end]
markers = (
    "isaac_vita_anm2_scratch_malloc",
    "isaac_vita_texel_scratch_malloc",
    "isaac_vita_room_entry_slab_malloc_locked",
    "vita_heap_guest_malloc_with_reason",
    "isaac_vita_ogg_emergency_malloc",
)
positions = [body.index(marker) for marker in markers]
if positions != sorted(positions) or len(set(positions)) != len(positions):
    raise SystemExit("guest malloc route order contract failed")

handler_contracts = {
    "vita_heap_free": (
        "isaac_vita_guest_heap_terminal()",
        "isaac_vita_ogg_emergency_free",
        "isaac_vita_anm2_scratch_free",
        "isaac_vita_texel_scratch_free",
        "vita_heap_room_slab_free_route",
        "vita_heap_guest_free_impl",
    ),
    "vita_heap_calloc": (
        "isaac_vita_guest_heap_terminal()",
        "vita_heap_guest_calloc_impl",
    ),
    "vita_heap_malloc": (
        "isaac_vita_guest_heap_terminal()",
        "isaac_vita_anm2_scratch_malloc",
        "isaac_vita_texel_scratch_malloc",
        "isaac_vita_room_entry_slab_malloc_locked",
        "vita_heap_guest_malloc_with_reason",
        "isaac_vita_ogg_emergency_malloc",
    ),
    "vita_heap_realloc": (
        "isaac_vita_guest_heap_terminal()",
        "isaac_vita_ogg_emergency_realloc",
        "isaac_vita_texel_scratch_realloc",
        "vita_heap_room_slab_realloc_route",
        "vita_heap_guest_realloc_impl",
    ),
}
for handler, contract_markers in handler_contracts.items():
    handler_begin = heap_source.index(f"static void {handler}(")
    handler_end = heap_source.index("\nstatic ", handler_begin + 1)
    handler_body = heap_source[handler_begin:handler_end]
    handler_positions = [handler_body.index(marker)
                         for marker in contract_markers]
    if handler_positions != sorted(handler_positions):
        raise SystemExit(f"{handler} terminal/special route contract failed")

entry_source = pathlib.Path(sys.argv[2]).read_text(encoding="utf-8")
entry_begin = entry_source.index("int isaac_vita_run_first_fault(")
entry_body = entry_source[entry_begin:]
entry_markers = (
    "guest_image_load(pe_path)",
    "guest_stack_init(&cpu)",
    "isaac_vita_guest_heap_init(cpu.stack_floor, cpu.stack_ceiling)",
    "guest_register_all();",
    "guest_coverage_init_from_env()",
    "guest_run_until_stop(&cpu, attach_static_tls)",
    "guest_run_until_stop(&cpu, entry)",
)
entry_positions = [entry_body.index(marker) for marker in entry_markers]
if entry_positions != sorted(entry_positions):
    raise SystemExit("guest overflow heap startup-order contract failed")
if entry_body.count("isaac_vita_guest_heap_init(") != 1:
    raise SystemExit("guest overflow heap init is not called exactly once")
if entry_body.count("isaac_vita_guest_heap_telemetry_log_final();") != 1:
    raise SystemExit("guest overflow heap final telemetry call drifted")
done_position = entry_body.index("done:")
final_position = entry_body.index(
    "isaac_vita_guest_heap_telemetry_log_final();")
teardown_position = entry_body.index("kage_vita_backend_deactivate();")
if not done_position < final_position < teardown_position:
    raise SystemExit("guest overflow heap final telemetry teardown order drifted")
if (
    '"guest heap overflow PASS: ledger_capacity=%u pool_bytes=%u"'
    not in entry_body
):
    raise SystemExit("guest overflow heap startup breadcrumb drifted")

telemetry_format = (
    "heapovf: e=%s q=%u op=%u req=%u live=%u/%u peak=%u/%u "
    "a/f/r=%u/%u/%u n2p/p2n=%u/%u nf/pf=%u/%u cons=%u "
    "raw/str/int=%u/%u/%u/%u state=%u term=%u valid/sat=%u/%u"
)
if any(fragment not in heap_source for fragment in (
        '"heapovf: e=%s q=%u op=%u req=%u live=%u/%u peak=%u/%u "',
        '"a/f/r=%u/%u/%u n2p/p2n=%u/%u nf/pf=%u/%u cons=%u "',
        '"raw/str/int=%u/%u/%u/%u state=%u term=%u valid/sat=%u/%u"')):
    raise SystemExit("overflow telemetry bounded format drifted")
maximum = 2**32 - 1
worst_case = telemetry_format % (("native-to-pool",) + (maximum,) * 23)
if len(worst_case.encode("ascii")) >= 384:
    raise SystemExit(
        f"overflow telemetry record exceeds logger body: {len(worst_case)}")

cmake_source = pathlib.Path(sys.argv[4]).read_text(encoding="utf-8")
cmake_contracts = (
    'option(ISAAC_VITA_HEAP_OVERFLOW_MSPACE\n'
    '       "Add a bounded USER_RW SceClibMspace after newlib exhaustion" OFF)',
    '"${ISAAC_RUNTIME}/host_vita_heap_overflow_mspace.c"',
    '"${ISAAC_RUNTIME}/host_vita_heap.c"\n'
    '      "${ISAAC_RUNTIME}/entry_vita.c"\n'
    '      APPEND PROPERTY COMPILE_DEFINITIONS\n'
    '        ISAAC_VITA_HEAP_OVERFLOW_MSPACE=1',
    'if(ISAAC_VITA_FXLAYERS_NULL_ROLLBACK OR\n'
    '     ISAAC_VITA_HEAP_OVERFLOW_MSPACE)',
    'option(ISAAC_VITA_ROOM_ENTRY_SLAB\n'
    '       "Pack exact frozen RoomConfig Entry arrays into retained mspace pages" OFF)',
    '"${ISAAC_RUNTIME}/host_vita_room_entry_slab.c"',
    '"${ISAAC_RUNTIME}/host_vita_heap.c"\n'
    '      APPEND PROPERTY COMPILE_DEFINITIONS\n'
    '        ISAAC_VITA_ROOM_ENTRY_SLAB=1',
    'ISAAC_VITA_ROOM_ENTRY_SLAB requires the translated runtime and ',
    'option(ISAAC_VITA_ROOM_ENTRY_HYBRID\n'
    '       "Extend the RoomConfig Entry slab with bounded lazy USER_RW chunks" OFF)',
    '"${ISAAC_RUNTIME}/host_vita_room_entry_external.c"',
    '"${ISAAC_RUNTIME}/host_vita_heap.c"\n'
    '      "${ISAAC_RUNTIME}/host_vita_room_entry_slab.c"\n'
    '      APPEND PROPERTY COMPILE_DEFINITIONS\n'
    '        ISAAC_VITA_ROOM_ENTRY_HYBRID=1',
    'ISAAC_VITA_ROOM_ENTRY_HYBRID requires the translated runtime, ',
)
if any(contract not in cmake_source for contract in cmake_contracts):
    raise SystemExit("overflow CMake default/dependency/source contract failed")
if cmake_source.count("ISAAC_VITA_HEAP_OVERFLOW_MSPACE=1") != 1:
    raise SystemExit("overflow feature definition leaked outside heap/entry")
PY

cmake_cmd=${CMAKE_COMMAND:-cmake}
if ! command -v "$cmake_cmd" >/dev/null 2>&1; then
    echo "CMAKE_COMMAND must resolve to CMake" >&2
    exit 2
fi
cmake_fixture="$work/cmake-fixture"
mkdir -p "$cmake_fixture/generated"
python3 - "$cmake_fixture" <<'PY'
import hashlib
import json
import pathlib
import sys

fixture = pathlib.Path(sys.argv[1])
generated = fixture / "generated"
pe = fixture / "isaac-ng.exe.unpacked.exe"
with pe.open("wb") as stream:
    stream.truncate(8_650_240)
pe_sha = hashlib.sha256(pe.read_bytes()).hexdigest()

outputs = []
for index in range(3):
    path = generated / f"guest_{index}.c"
    data = f"void guest_{index}(void) {{}}\n".encode("ascii")
    path.write_bytes(data)
    outputs.append({
        "path": path.name,
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
    })

manifest = {
    "recipe": {
        "inputs": {
            "input/isaac-ng.exe.unpacked.exe": pe_sha,
        },
        "params": {"image_base": 0x98000000},
    },
    "recipe_id": "1" * 64,
    "output_set_id": "2" * 64,
    "outputs": outputs,
}
(generated / "manifest.json").write_text(
    json.dumps(manifest, sort_keys=True), encoding="utf-8")
PY

cmake_fixture_args=(
    -S "$root/vita" -G Ninja
    -DISAAC_GENERATED_DIR="$cmake_fixture/generated"
    -DISAAC_PE_PATH="$cmake_fixture/isaac-ng.exe.unpacked.exe"
    -DSOURCE_DATE_EPOCH=1700000000
    -DISAAC_VITA_SCAFFOLD_ONLY=OFF
    -DISAAC_VITA_KAGE=OFF
    -DISAAC_VITA_AUDIO=OFF
    -DISAAC_VITA_FXLAYERS_NULL_ROLLBACK=OFF
    -DISAAC_VITA_HEAP_MB=81
    -DISAAC_VITA_HEAP_LEDGER_MEMBLOCK=ON
    -DISAAC_VITA_HEAP_LEDGER_BACKSHIFT=ON
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
)

"$cmake_cmd" "${cmake_fixture_args[@]}" \
    -B "$work/cmake-hybrid-on" \
    -DISAAC_VITA_HEAP_OVERFLOW_MSPACE=ON \
    -DISAAC_VITA_ROOM_ENTRY_SLAB=ON \
    -DISAAC_VITA_ROOM_ENTRY_HYBRID=ON \
    > "$work/cmake-hybrid-on.log" 2>&1
"$cmake_cmd" "${cmake_fixture_args[@]}" \
    -B "$work/cmake-room-on" \
    -DISAAC_VITA_HEAP_OVERFLOW_MSPACE=ON \
    -DISAAC_VITA_ROOM_ENTRY_SLAB=ON \
    > "$work/cmake-room-on.log" 2>&1
"$cmake_cmd" "${cmake_fixture_args[@]}" \
    -B "$work/cmake-overflow-on" \
    -DISAAC_VITA_HEAP_OVERFLOW_MSPACE=ON \
    > "$work/cmake-overflow-on.log" 2>&1
"$cmake_cmd" "${cmake_fixture_args[@]}" \
    -B "$work/cmake-overflow-off" \
    > "$work/cmake-overflow-off.log" 2>&1
if ! grep -Fxq \
        'ISAAC_VITA_HEAP_OVERFLOW_MSPACE:BOOL=OFF' \
        "$work/cmake-overflow-off/CMakeCache.txt"; then
    echo "raw CMake overflow option did not default to OFF" >&2
    exit 1
fi
if ! grep -Fxq 'ISAAC_VITA_ROOM_ENTRY_SLAB:BOOL=OFF' \
        "$work/cmake-overflow-on/CMakeCache.txt" || \
   ! grep -Fxq 'ISAAC_VITA_ROOM_ENTRY_SLAB:BOOL=OFF' \
        "$work/cmake-overflow-off/CMakeCache.txt" || \
   ! grep -Fxq 'ISAAC_VITA_ROOM_ENTRY_SLAB:BOOL=ON' \
        "$work/cmake-room-on/CMakeCache.txt"; then
    echo "raw CMake RoomConfig slab option/default drifted" >&2
    exit 1
fi
if ! grep -Fxq 'ISAAC_VITA_ROOM_ENTRY_HYBRID:BOOL=OFF' \
        "$work/cmake-room-on/CMakeCache.txt" || \
   ! grep -Fxq 'ISAAC_VITA_ROOM_ENTRY_HYBRID:BOOL=OFF' \
        "$work/cmake-overflow-on/CMakeCache.txt" || \
   ! grep -Fxq 'ISAAC_VITA_ROOM_ENTRY_HYBRID:BOOL=OFF' \
        "$work/cmake-overflow-off/CMakeCache.txt" || \
   ! grep -Fxq 'ISAAC_VITA_ROOM_ENTRY_HYBRID:BOOL=ON' \
        "$work/cmake-hybrid-on/CMakeCache.txt"; then
    echo "raw CMake RoomConfig hybrid option/default drifted" >&2
    exit 1
fi

python3 - \
    "$root" \
    "$work/cmake-hybrid-on/compile_commands.json" \
    "$work/cmake-room-on/compile_commands.json" \
    "$work/cmake-overflow-on/compile_commands.json" \
    "$work/cmake-overflow-off/compile_commands.json" <<'PY'
import json
import pathlib
import shlex
import sys

root = pathlib.Path(sys.argv[1]).resolve()
heap = (root / "runtime/host_vita_heap.c").resolve()
entry = (root / "runtime/entry_vita.c").resolve()
raw = (root / "runtime/host_vita_heap_overflow_mspace.c").resolve()
room = (root / "runtime/host_vita_room_entry_slab.c").resolve()
external = (root / "runtime/host_vita_room_entry_external.c").resolve()

def load(path):
    records = json.loads(pathlib.Path(path).read_text(encoding="utf-8"))
    normalized = []
    for record in records:
        source = pathlib.Path(record["file"])
        if not source.is_absolute():
            source = pathlib.Path(record["directory"]) / source
        arguments = record.get("arguments")
        if arguments is None:
            arguments = shlex.split(record["command"])
        normalized.append((source.resolve(), tuple(arguments)))
    return normalized

def definition_uses(records, definition):
    bare = f"-D{definition}"
    prefix = bare + "="
    return [
        (source, argument)
        for source, arguments in records
        for argument in arguments
        if argument == bare or argument.startswith(prefix)
    ]

def require_scope(records, definition, expected):
    expected_token = f"-D{definition}=1"
    uses = definition_uses(records, definition)
    owners = {source for source, _ in uses}
    invalid = [(source, argument) for source, argument in uses
               if argument != expected_token]
    if owners != expected or invalid:
        raise SystemExit(
            f"CMake scope/value for {definition}: owners={owners}, "
            f"invalid={invalid}, expected={expected}")

hybrid_on = load(sys.argv[2])
room_on = load(sys.argv[3])
overflow_only = load(sys.argv[4])
off = load(sys.argv[5])
contracts = {
    "ISAAC_VITA_HEAP_OVERFLOW_MSPACE": {heap, entry},
    "ISAAC_VITA_HEAP_RANGE_LEASE": {heap},
    "ISAAC_VITA_HEAP_LEDGER_MEMBLOCK": {heap},
    "ISAAC_VITA_HEAP_LEDGER_BACKSHIFT": {heap},
}
for definition, expected in contracts.items():
    require_scope(hybrid_on, definition, expected)
require_scope(hybrid_on, "ISAAC_VITA_ROOM_ENTRY_SLAB", {heap})
require_scope(hybrid_on, "ISAAC_VITA_ROOM_ENTRY_HYBRID", {heap, room})
hybrid_sources = {source for source, _ in hybrid_on}
if not {raw, room, external}.issubset(hybrid_sources):
    raise SystemExit("CMake hybrid ON graph omitted a backing/slab source")
if definition_uses(hybrid_on, "ISAAC_VITA_ROOM_ENTRY_HYBRID") and any(
        source not in {heap, room}
        for source, _ in definition_uses(
            hybrid_on, "ISAAC_VITA_ROOM_ENTRY_HYBRID")):
    raise SystemExit("hybrid feature macro leaked beyond heap/slab")
for definition, expected in contracts.items():
    require_scope(room_on, definition, expected)
require_scope(room_on, "ISAAC_VITA_ROOM_ENTRY_SLAB", {heap})
require_scope(room_on, "ISAAC_VITA_ROOM_ENTRY_HYBRID", set())
if raw not in {source for source, _ in room_on}:
    raise SystemExit("CMake ON graph omitted raw overflow mspace source")
if room not in {source for source, _ in room_on}:
    raise SystemExit("CMake room ON graph omitted RoomConfig slab source")
if external in {source for source, _ in room_on}:
    raise SystemExit("CMake hybrid OFF graph retained external backing source")
if any(source in {raw, room}
       for definition in contracts
       for source, _ in definition_uses(room_on, definition)) or any(
           source == room
           for source, _ in definition_uses(
               room_on, "ISAAC_VITA_ROOM_ENTRY_SLAB")):
    raise SystemExit("policy-free allocator module gained router macros")
for definition, expected in contracts.items():
    require_scope(overflow_only, definition, expected)
require_scope(overflow_only, "ISAAC_VITA_ROOM_ENTRY_SLAB", set())
require_scope(overflow_only, "ISAAC_VITA_ROOM_ENTRY_HYBRID", set())
if room in {source for source, _ in overflow_only}:
    raise SystemExit("CMake room OFF graph retained RoomConfig slab source")
if external in {source for source, _ in overflow_only}:
    raise SystemExit("CMake overflow-only graph retained external backing source")
if raw in {source for source, _ in off}:
    raise SystemExit("CMake OFF graph retained raw overflow mspace source")
if room in {source for source, _ in off}:
    raise SystemExit("CMake OFF graph retained RoomConfig slab source")
if external in {source for source, _ in off}:
    raise SystemExit("CMake OFF graph retained external backing source")
require_scope(off, "ISAAC_VITA_HEAP_OVERFLOW_MSPACE", set())
require_scope(off, "ISAAC_VITA_HEAP_RANGE_LEASE", set())
require_scope(off, "ISAAC_VITA_ROOM_ENTRY_SLAB", set())
require_scope(off, "ISAAC_VITA_ROOM_ENTRY_HYBRID", set())
for definition in (
        "ISAAC_VITA_HEAP_LEDGER_MEMBLOCK",
        "ISAAC_VITA_HEAP_LEDGER_BACKSHIFT"):
    require_scope(off, definition, {heap})
PY

expect_cmake_reject() {
    tag=$1
    shift
    build_dir="$work/cmake-reject-$tag"
    if "$cmake_cmd" "${cmake_fixture_args[@]}" \
            -B "$build_dir" \
            -DISAAC_VITA_HEAP_OVERFLOW_MSPACE=ON "$@" \
            > "$build_dir.log" 2>&1; then
        echo "CMake accepted invalid overflow configuration: $tag" >&2
        exit 1
    fi
    if ! grep -Fq \
            "ISAAC_VITA_HEAP_OVERFLOW_MSPACE requires translated runtime" \
            "$build_dir.log"; then
        cat "$build_dir.log" >&2
        echo "CMake rejected $tag for an unexpected reason" >&2
        exit 1
    fi
}
expect_cmake_reject heap79 -DISAAC_VITA_HEAP_MB=79
expect_cmake_reject ledger-off -DISAAC_VITA_HEAP_LEDGER_MEMBLOCK=OFF
expect_cmake_reject backshift-off -DISAAC_VITA_HEAP_LEDGER_BACKSHIFT=OFF
expect_cmake_reject scaffold -DISAAC_VITA_SCAFFOLD_ONLY=ON

if "$cmake_cmd" "${cmake_fixture_args[@]}" \
        -B "$work/cmake-reject-room-without-overflow" \
        -DISAAC_VITA_HEAP_OVERFLOW_MSPACE=OFF \
        -DISAAC_VITA_ROOM_ENTRY_SLAB=ON \
        > "$work/cmake-reject-room-without-overflow.log" 2>&1; then
    echo "CMake accepted RoomConfig slab without overflow mspace" >&2
    exit 1
fi
if ! grep -Fq \
        "ISAAC_VITA_ROOM_ENTRY_SLAB requires the translated runtime" \
        "$work/cmake-reject-room-without-overflow.log"; then
    cat "$work/cmake-reject-room-without-overflow.log" >&2
    exit 1
fi

expect_hybrid_reject() {
    tag=$1
    shift
    build_dir="$work/cmake-reject-hybrid-$tag"
    if "$cmake_cmd" "${cmake_fixture_args[@]}" \
            -B "$build_dir" \
            -DISAAC_VITA_HEAP_OVERFLOW_MSPACE=ON \
            -DISAAC_VITA_ROOM_ENTRY_SLAB=ON \
            -DISAAC_VITA_ROOM_ENTRY_HYBRID=ON "$@" \
            > "$build_dir.log" 2>&1; then
        echo "CMake accepted invalid RoomConfig hybrid configuration: $tag" >&2
        exit 1
    fi
    if ! grep -Eq \
            'ISAAC_VITA_(ROOM_ENTRY_HYBRID|ROOM_ENTRY_SLAB|HEAP_OVERFLOW_MSPACE) requires' \
            "$build_dir.log"; then
        cat "$build_dir.log" >&2
        echo "CMake rejected hybrid $tag for an unexpected reason" >&2
        exit 1
    fi
}
expect_hybrid_reject scaffold -DISAAC_VITA_SCAFFOLD_ONLY=ON
expect_hybrid_reject slab-off -DISAAC_VITA_ROOM_ENTRY_SLAB=OFF
expect_hybrid_reject overflow-off \
    -DISAAC_VITA_HEAP_OVERFLOW_MSPACE=OFF \
    -DISAAC_VITA_ROOM_ENTRY_SLAB=OFF
expect_hybrid_reject ledger-off -DISAAC_VITA_HEAP_LEDGER_MEMBLOCK=OFF
expect_hybrid_reject backshift-off -DISAAC_VITA_HEAP_LEDGER_BACKSHIFT=OFF

cat > "$fake_include/psp2/types.h" <<'EOF'
#ifndef ISAAC_OVERFLOW_ROUTER_PSP2_TYPES_H
#define ISAAC_OVERFLOW_ROUTER_PSP2_TYPES_H
#include <stdint.h>
typedef int32_t SceUID;
typedef uint32_t SceSize;
#endif
EOF

cat > "$fake_include/psp2/kernel/sysmem.h" <<'EOF'
#ifndef ISAAC_OVERFLOW_ROUTER_PSP2_SYSMEM_H
#define ISAAC_OVERFLOW_ROUTER_PSP2_SYSMEM_H
#include <psp2/types.h>
typedef int SceKernelMemBlockType;
typedef struct SceKernelAllocMemBlockOpt {
    SceSize size;
    uint32_t opaque[15];
} SceKernelAllocMemBlockOpt;
enum { SCE_KERNEL_MEMBLOCK_TYPE_USER_RW = 0x0c20d060 };
SceUID sceKernelAllocMemBlock(const char *, SceKernelMemBlockType,
                              SceSize, SceKernelAllocMemBlockOpt *);
int sceKernelGetMemBlockBase(SceUID, void **);
int sceKernelFreeMemBlock(SceUID);
#endif
EOF

cat > "$fake_include/psp2/kernel/clib.h" <<'EOF'
#ifndef ISAAC_OVERFLOW_ROUTER_PSP2_CLIB_H
#define ISAAC_OVERFLOW_ROUTER_PSP2_CLIB_H
#include <psp2/types.h>
typedef void *SceClibMspace;
SceClibMspace sceClibMspaceCreate(void *, SceSize);
void sceClibMspaceDestroy(SceClibMspace);
void *sceClibMspaceMalloc(SceClibMspace, SceSize);
void *sceClibMspaceCalloc(SceClibMspace, SceSize, SceSize);
void *sceClibMspaceRealloc(SceClibMspace, void *, SceSize);
void sceClibMspaceFree(SceClibMspace, void *);
#endif
EOF

host_defines=(
    -DGUEST_IMAGE_BASE=0x98000000u
    -DISAAC_VITA_HEAP_TESTING=1
    -DISAAC_VITA_HEAP_RANGE_LEASE=1
    -DISAAC_VITA_HEAP_LEDGER_MEMBLOCK=1
    -DISAAC_VITA_HEAP_LEDGER_BACKSHIFT=1
    -DISAAC_VITA_HEAP_OVERFLOW_MSPACE=1
    -DISAAC_VITA_HEAP_OVERFLOW_MSPACE_TESTING=1
    -DISAAC_VITA_ROOM_ENTRY_SLAB=1
    -DISAAC_VITA_ROOM_ENTRY_SLAB_TESTING=1
    -DISAAC_VITA_ROOM_ENTRY_HYBRID=1
    -DISAAC_VITA_ROOM_ENTRY_EXTERNAL_TESTING=1
)
host_interpose=(
    -Dmalloc=oracle_native_malloc
    -Dcalloc=oracle_native_calloc
    -Drealloc=oracle_native_realloc
    -Dfree=oracle_native_free
)

if ! host_gcc=$(command -v "${HOST_CC:-gcc}"); then
    echo "HOST_CC must resolve to GCC" >&2
    exit 2
fi
if ! host_clang=$(command -v "${HOST_CLANG:-clang}"); then
    echo "HOST_CLANG must resolve to Clang" >&2
    exit 2
fi
host_gcc=$(python3 -c 'import pathlib,sys; print(pathlib.Path(sys.argv[1]).resolve())' "$host_gcc")
host_clang=$(python3 -c 'import pathlib,sys; print(pathlib.Path(sys.argv[1]).resolve())' "$host_clang")
if [ "$host_gcc" = "$host_clang" ]; then
    echo "HOST_CC and HOST_CLANG must be distinct compilers" >&2
    exit 2
fi
host_gcc_macros=$("$host_gcc" -dM -E -x c /dev/null)
host_clang_macros=$("$host_clang" -dM -E -x c /dev/null)
if [[ "$host_gcc_macros" == *"#define __clang__"* ]]; then
    echo "HOST_CC must be GCC, not Clang" >&2
    exit 2
fi
if [[ "$host_gcc_macros" != *"#define __GNUC__"* ]]; then
    echo "HOST_CC does not identify as GCC" >&2
    exit 2
fi
if [[ "$host_clang_macros" != *"#define __clang__"* ]]; then
    echo "HOST_CLANG does not identify as Clang" >&2
    exit 2
fi

host_compilers=("$host_gcc" "$host_clang")
host_tags=(gcc clang)
for compiler_index in 0 1; do
    resolved=${host_compilers[$compiler_index]}
    tag=${host_tags[$compiler_index]}
    host_flags=(
        -std=gnu11 -O1 -g -Wall -Wextra -Werror
        -fno-omit-frame-pointer -fno-pie
        -ffunction-sections -fdata-sections
        -fsanitize=address,undefined
    )
    "$resolved" "${host_flags[@]}" \
        -DISAAC_VITA_ROOM_ENTRY_EXTERNAL_TESTING=1 \
        -I"$fake_include" -I"$root/runtime" \
        "$root/runtime/host_vita_room_entry_external.c" \
        "$root/runtime/host_vita_room_entry_external_oracle.c" \
        -no-pie -o "$work/room-entry-external-oracle-$tag"
    "$work/room-entry-external-oracle-$tag"
    "$work/room-entry-external-oracle-$tag" --corrupt-count
    "$work/room-entry-external-oracle-$tag" --corrupt-receipts
    "$resolved" "${host_flags[@]}" \
        -DISAAC_VITA_ROOM_ENTRY_SLAB_TESTING=1 \
        -DISAAC_VITA_ROOM_ENTRY_HYBRID=1 \
        -DISAAC_VITA_ROOM_ENTRY_EXTERNAL_TESTING=1 \
        -I"$fake_include" -I"$root/runtime" \
        "$root/runtime/host_vita_room_entry_external.c" \
        "$root/runtime/host_vita_room_entry_slab.c" \
        "$root/runtime/host_vita_room_entry_hybrid_oracle.c" \
        -no-pie -o "$work/room-entry-hybrid-oracle-$tag"
    "$work/room-entry-hybrid-oracle-$tag"
    "$resolved" "${host_flags[@]}" "${host_defines[@]}" \
        "${host_interpose[@]}" -I"$fake_include" -I"$root/runtime" \
        -c "$root/runtime/host_vita_heap.c" \
        -o "$work/heap-router-$tag.o"
    "$resolved" "${host_flags[@]}" \
        -DISAAC_VITA_HEAP_OVERFLOW_MSPACE_TESTING=1 \
        -I"$fake_include" -I"$root/runtime" \
        -c "$root/runtime/host_vita_heap_overflow_mspace.c" \
        -o "$work/overflow-mspace-$tag.o"
    "$resolved" "${host_flags[@]}" \
        -DISAAC_VITA_ROOM_ENTRY_SLAB_TESTING=1 \
        -DISAAC_VITA_ROOM_ENTRY_HYBRID=1 \
        -DISAAC_VITA_ROOM_ENTRY_EXTERNAL_TESTING=1 \
        -I"$fake_include" -I"$root/runtime" \
        -c "$root/runtime/host_vita_room_entry_slab.c" \
        -o "$work/room-entry-slab-$tag.o"
    "$resolved" "${host_flags[@]}" \
        -DISAAC_VITA_ROOM_ENTRY_EXTERNAL_TESTING=1 \
        -I"$fake_include" -I"$root/runtime" \
        -c "$root/runtime/host_vita_room_entry_external.c" \
        -o "$work/room-entry-external-$tag.o"
    "$resolved" "${host_flags[@]}" \
        -I"$fake_include" -I"$root/runtime" \
        -c "$root/runtime/host_vita_heap_ledger_memblock.c" \
        -o "$work/heap-ledger-$tag.o"
    "$resolved" "${host_flags[@]}" "${host_defines[@]}" \
        -I"$fake_include" -I"$root/runtime" \
        -c "$root/runtime/host_vita_heap_overflow_router_oracle.c" \
        -o "$work/router-oracle-$tag.o"
    "$resolved" -fsanitize=address,undefined -fno-omit-frame-pointer \
        -no-pie \
        "$work/heap-router-$tag.o" "$work/overflow-mspace-$tag.o" \
        "$work/room-entry-slab-$tag.o" \
        "$work/room-entry-external-$tag.o" \
        "$work/heap-ledger-$tag.o" \
        "$work/router-oracle-$tag.o" \
        -Wl,--wrap=isaac_vita_heap_overflow_mspace_snapshot_get \
        -Wl,--gc-sections -o "$work/router-oracle-$tag"
    "$work/router-oracle-$tag" > "$work/router-oracle-$tag.log"
    "$work/router-oracle-$tag" --terminal-observer
    # Same real router/state-machine fixture, with only the terminal observer
    # changed. The oracle checks false/latched/reset states, lock acquisitions
    # and a concurrent observer while the normal init path publishes terminal.
    "$resolved" "${host_flags[@]}" "${host_defines[@]}" \
        -DISAAC_VITA_HEAP_TERMINAL_FASTPATH=1 "${host_interpose[@]}" \
        -I"$fake_include" -I"$root/runtime" \
        -c "$root/runtime/host_vita_heap.c" \
        -o "$work/heap-router-terminal-$tag.o"
    "$resolved" "${host_flags[@]}" "${host_defines[@]}" \
        -DISAAC_VITA_HEAP_TERMINAL_FASTPATH=1 -pthread \
        -I"$fake_include" -I"$root/runtime" \
        -c "$root/runtime/host_vita_heap_overflow_router_oracle.c" \
        -o "$work/router-terminal-oracle-$tag.o"
    "$resolved" -fsanitize=address,undefined -fno-omit-frame-pointer \
        -no-pie -pthread \
        "$work/heap-router-terminal-$tag.o" "$work/overflow-mspace-$tag.o" \
        "$work/room-entry-slab-$tag.o" "$work/room-entry-external-$tag.o" \
        "$work/heap-ledger-$tag.o" "$work/router-terminal-oracle-$tag.o" \
        -Wl,--wrap=isaac_vita_heap_overflow_mspace_snapshot_get \
        -Wl,--gc-sections -o "$work/router-terminal-oracle-$tag"
    "$work/router-terminal-oracle-$tag" --terminal-observer
    "$work/router-oracle-$tag" --room-commit-terminal \
        > "$work/router-room-terminal-$tag.log"
    "$work/router-oracle-$tag" --room-invariant-terminal \
        > "$work/router-room-invariant-$tag.log"
    "$work/router-oracle-$tag" --room-cold-invariant-terminal \
        > "$work/router-room-cold-invariant-$tag.log"
    cat "$work/router-oracle-$tag.log"
    cat "$work/router-room-terminal-$tag.log"
    cat "$work/router-room-invariant-$tag.log"
    cat "$work/router-room-cold-invariant-$tag.log"
    if ! grep -Fq \
            'phase=fill live=393216 capacity=524288 load=3/4' \
            "$work/router-oracle-$tag.log" || \
       ! grep -Fq \
            'phase=cleanup live=0 capacity=524288 load=3/4' \
            "$work/router-oracle-$tag.log"; then
        echo "fixed ledger probe/lock receipt missing for $tag" >&2
        exit 1
    fi
done

arm_flags=(
    -std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb
    -fno-strict-aliasing -ffunction-sections -fdata-sections
    -Wall -Wextra -Werror -Wno-maybe-uninitialized
)
arm_defines=(
    -DGUEST_IMAGE_BASE=0x98000000u
    -DISAAC_VITA_HEAP_RANGE_LEASE=1
    -DISAAC_VITA_HEAP_LEDGER_MEMBLOCK=1
    -DISAAC_VITA_HEAP_LEDGER_BACKSHIFT=1
    -DISAAC_VITA_HEAP_OVERFLOW_MSPACE=1
    -DISAAC_VITA_ROOM_ENTRY_SLAB=1
    -DISAAC_VITA_ROOM_ENTRY_HYBRID=1
)

"$cc" "${arm_flags[@]}" "${arm_defines[@]}" -I"$root/runtime" \
    -c "$root/runtime/host_vita_heap.c" -o "$work/heap-router.arm.o"
"$cc" "${arm_flags[@]}" -I"$root/runtime" \
    -c "$root/runtime/host_vita_heap_ledger_memblock.c" \
    -o "$work/heap-ledger.arm.o"
"$cc" "${arm_flags[@]}" -I"$root/runtime" \
    -c "$root/runtime/host_vita_heap_overflow_mspace.c" \
    -o "$work/overflow-mspace.arm.o"
"$cc" "${arm_flags[@]}" -DISAAC_VITA_ROOM_ENTRY_HYBRID=1 \
    -I"$root/runtime" \
    -c "$root/runtime/host_vita_room_entry_slab.c" \
    -o "$work/room-entry-slab.arm.o"
"$cc" "${arm_flags[@]}" -I"$root/runtime" \
    -c "$root/runtime/host_vita_room_entry_external.c" \
    -o "$work/room-entry-external.arm.o"

cat > "$work/arm-contract.c" <<'EOF'
#include <stddef.h>
#include <stdint.h>
#include "host_vita_heap.h"
#include "host_vita_room_entry_external.h"

typedef struct arm_range_ledger_receipt {
    uint32_t base;
    uint32_t requested_size;
} arm_range_ledger_receipt;

_Static_assert(sizeof(arm_range_ledger_receipt) == 8U,
               "ARM range ledger entry is not eight bytes");
_Static_assert(ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_CAPACITY == 524288U,
               "ARM fixed ledger capacity drifted");
_Static_assert(ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_LOAD_NUMERATOR == 3U &&
                   ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_LOAD_DENOMINATOR == 4U,
               "ARM fixed ledger load ratio drifted");
_Static_assert(ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_LIVE_LIMIT == 393216U &&
                   ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_LIVE_LIMIT ==
                       (ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_CAPACITY /
                        ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_LOAD_DENOMINATOR) *
                           ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_LOAD_NUMERATOR,
               "ARM fixed ledger live ceiling drifted");
_Static_assert(ISAAC_VITA_GUEST_HEAP_FIXED_LEDGER_CAPACITY *
                   sizeof(arm_range_ledger_receipt) == 0x00400000U,
               "ARM ledger entry-table budget drifted");
_Static_assert(ISAAC_VITA_GUEST_HEAP_LEDGER_ENTRY_TABLE_BYTES ==
                   0x00400000U &&
                   ISAAC_VITA_GUEST_HEAP_FLOOR_PHASE_BYTES == 0x00080000U &&
                   ISAAC_VITA_GUEST_HEAP_FLOOR_CURRENT_BYTES == 0x00010000U &&
                   ISAAC_VITA_GUEST_HEAP_LEDGER_USABLE_BYTES == 0x00490000U,
               "ARM ledger floor-lifetime sidecar budget drifted");
_Static_assert(ISAAC_VITA_GUEST_HEAP_LEDGER_REQUEST_BYTES == 0x00491000U &&
                   ISAAC_VITA_GUEST_HEAP_LEDGER_RETAINED_BYTES == 0x00492000U,
               "ARM ledger USER_RW receipt drifted");
_Static_assert(ISAAC_VITA_HEAP_OVERFLOW_MSPACE_BYTES == 0x007d5000U &&
                   ISAAC_VITA_HEAP_OVERFLOW_MSPACE_RETAINED_BYTES ==
                       0x007d6000U,
               "ARM overflow mspace receipt drifted");
_Static_assert(ISAAC_VITA_GUEST_HEAP_TOTAL_REQUEST_BYTES == 0x00c66000U &&
                   ISAAC_VITA_GUEST_HEAP_TOTAL_RETAINED_BYTES == 0x00c68000U,
               "ARM combined USER_RW receipt drifted");
_Static_assert(ISAAC_VITA_ROOM_ENTRY_EXTERNAL_REQUEST_BYTES == 0x00101000U &&
                   ISAAC_VITA_ROOM_ENTRY_EXTERNAL_USABLE_BYTES == 0x00100000U &&
                   ISAAC_VITA_ROOM_ENTRY_EXTERNAL_RETAINED_BYTES == 0x00102000U &&
                   ISAAC_VITA_ROOM_ENTRY_EXTERNAL_MAX_CHUNKS == 12U &&
                   ISAAC_VITA_ROOM_ENTRY_EXTERNAL_MAX_ISSUED_PAGES == 192U,
               "ARM RoomConfig hybrid receipt drifted");

unsigned int _newlib_heap_size_user = 0x05100000U;

void isaac_vita_log(const char *format, ...)
{
    (void)format;
}

int main(void)
{
    isaac_vita_guest_heap_telemetry_snapshot telemetry;
    void *pointer;

    (void)isaac_vita_guest_heap_init(0x85000000U, 0x85400000U);
    pointer = isaac_vita_guest_malloc(32U);
    pointer = isaac_vita_guest_realloc(pointer, 64U, NULL);
    (void)isaac_vita_guest_free(pointer);
    (void)isaac_vita_guest_heap_telemetry_snapshot_get(&telemetry);
    isaac_vita_guest_heap_telemetry_log_final();
    return 0;
}
EOF

"$cc" "${arm_flags[@]}" "${arm_defines[@]}" -I"$root/runtime" \
    "$work/arm-contract.c" "$work/heap-router.arm.o" \
    "$work/heap-ledger.arm.o" "$work/overflow-mspace.arm.o" \
    "$work/room-entry-slab.arm.o" "$work/room-entry-external.arm.o" \
    -Wl,--gc-sections -lSceLibKernel_stub -lSceSysmem_stub \
    -o "$work/vita-heap-overflow-router.elf"

"$readelf" -h "$work/vita-heap-overflow-router.elf" \
    > "$work/router.elf.header"
"$readelf" -A "$work/vita-heap-overflow-router.elf" \
    > "$work/router.elf.attributes"
grep -q 'Machine:[[:space:]]*ARM$' "$work/router.elf.header"
grep -q 'Version5 EABI, soft-float ABI' "$work/router.elf.header"
if grep -q 'Tag_ABI_VFP_args' "$work/router.elf.attributes"; then
    echo "overflow router ARM link gained a hardfp VFP-args tag" >&2
    exit 1
fi

"$nm" "$work/vita-heap-overflow-router.elf" > "$work/router.elf.nm"
if grep -Eq ' U (__atomic|__sync|mspace_|sceLibc|malloc$|calloc$|realloc$|free$)' \
       "$work/router.elf.nm"; then
    echo "overflow router ARM link gained a forbidden allocator/helper" >&2
    exit 1
fi

"$nm" -u "$work/overflow-mspace.arm.o" | \
    awk '$1 == "U" { print $2 }' | LC_ALL=C sort \
    > "$work/overflow.undefined.names"
printf '%s\n' \
    sceClibMspaceCalloc sceClibMspaceCreate sceClibMspaceFree \
    sceClibMspaceMalloc sceClibMspaceRealloc \
    sceKernelAllocMemBlock sceKernelFreeMemBlock sceKernelGetMemBlockBase | \
    LC_ALL=C sort > "$work/overflow.expected.names"
cmp -s "$work/overflow.expected.names" "$work/overflow.undefined.names"

"$nm" -u "$work/heap-ledger.arm.o" | \
    awk '$1 == "U" { print $2 }' | LC_ALL=C sort \
    > "$work/ledger.undefined.names"
printf '%s\n' memset sceKernelAllocMemBlock sceKernelFreeMemBlock \
    sceKernelGetMemBlockBase | LC_ALL=C sort \
    > "$work/ledger.expected.names"
cmp -s "$work/ledger.expected.names" "$work/ledger.undefined.names"

"$nm" -u "$work/room-entry-slab.arm.o" | \
    awk '$1 == "U" { print $2 }' | LC_ALL=C sort \
    > "$work/room.undefined.names"
printf '%s\n' \
    isaac_vita_heap_overflow_mspace_internal_page_free \
    isaac_vita_heap_overflow_mspace_internal_page_malloc \
    isaac_vita_heap_overflow_mspace_snapshot_get \
    isaac_vita_room_entry_external_cancel_page \
    isaac_vita_room_entry_external_commit_page \
    isaac_vita_room_entry_external_page_matches \
    isaac_vita_room_entry_external_reserve_page \
    isaac_vita_room_entry_external_snapshot_get \
    isaac_vita_room_entry_external_unissue_page \
    isaac_vita_log memmove memset | LC_ALL=C sort \
    > "$work/room.expected.names"
if ! cmp -s "$work/room.expected.names" "$work/room.undefined.names"; then
    diff -u "$work/room.expected.names" "$work/room.undefined.names" >&2 || true
    exit 1
fi

"$nm" -u "$work/room-entry-external.arm.o" | \
    awk '$1 == "U" { print $2 }' | LC_ALL=C sort \
    > "$work/external.undefined.names"
printf '%s\n' memset sceKernelAllocMemBlock sceKernelFreeMemBlock \
    sceKernelGetMemBlockBase | LC_ALL=C sort \
    > "$work/external.expected.names"
if ! cmp -s "$work/external.expected.names" \
        "$work/external.undefined.names"; then
    diff -u "$work/external.expected.names" \
        "$work/external.undefined.names" >&2 || true
    exit 1
fi

if "$nm" -u "$work/heap-router.arm.o" | \
     awk '$1 == "U" { print $2 }' | \
     grep -Eq '^(__atomic|__sync|mspace_|sceClibMspace|sceLibc)'; then
    echo "heap router object gained a forbidden raw allocator/helper" >&2
    exit 1
fi

wrapper_python=${ISAAC_RECOMP_PYTHON:-python3}
if ! "$wrapper_python" -c 'import capstone' >/dev/null 2>&1; then
    echo "ISAAC_RECOMP_PYTHON must provide the generator's capstone module" >&2
    exit 2
fi
"$wrapper_python" "$root/../tools/build_vita.py" --self-test

sha256sum \
    "$root/runtime/host_vita_heap.h" \
    "$root/runtime/host_vita_heap.c" \
    "$root/runtime/entry_vita.c" \
    "$root/runtime/host_vita_heap_overflow_router_oracle.c" \
    "$root/runtime/host_vita_room_entry_slab.h" \
    "$root/runtime/host_vita_room_entry_slab.c" \
    "$root/runtime/host_vita_room_entry_external.h" \
    "$root/runtime/host_vita_room_entry_external.c" \
    "$root/runtime/host_vita_room_entry_external_oracle.c" \
    "$root/runtime/host_vita_room_entry_hybrid_oracle.c" \
    "$root/vita/CMakeLists.txt" \
    "$root/../tools/build_vita.py" \
    "$root/vita/test_heap_overflow_router.sh" \
    "$work/heap-router.arm.o" \
    "$work/heap-ledger.arm.o" \
    "$work/overflow-mspace.arm.o" \
    "$work/room-entry-slab.arm.o" \
    "$work/room-entry-external.arm.o" \
    "$work/vita-heap-overflow-router.elf"
echo "Vita heap overflow integrated host/ARM gate: PASS (ARM linked, not executed)"
