#!/usr/bin/env python3
"""Freeze the exact RoomConfig Entry allocation/free contract in one PE."""

from __future__ import annotations

import argparse
import hashlib
import json
import pathlib
import re
import struct

from gpr_locals import legacy_text

from test_ogg_emergency_contract import (
    PE32Image,
    expect_bytes,
    expect_call,
    expect_jump,
    fail,
)


EXPECTED_PE_BYTES = 8_650_240
EXPECTED_PE_SHA256 = (
    "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
)

ENTRY_COUNT_LOAD_RVA = 0x003E5AB0
ENTRY_ALLOC_SEQUENCE_RVA = 0x003E5AC5
ENTRY_ALLOC_CALL_RVA = 0x003E5AD9
ENTRY_OWNER_RETURN_RVA = 0x003E5ADE
ENTRY_POINTER_STORE_RVA = 0x003E5AE4
ENTRY_NORMAL_FREE_PUSH_RVA = 0x003E37FE
ENTRY_NORMAL_FREE_CALL_RVA = 0x003E3801
ENTRY_NORMAL_FREE_RETURN_RVA = 0x003E3806
ENTRY_STAGE_FREE_PUSH_RVA = 0x003E3B5E
ENTRY_STAGE_FREE_CALL_RVA = 0x003E3B61
ENTRY_STAGE_FREE_RETURN_RVA = 0x003E3B66

OPERATOR_NEW_RVA = 0x005EB09C
ALLOCATOR_RVA = 0x005EACF8
FREE_WRAPPER_RVA = 0x005EACE5
MALLOC_ARGUMENT_PUSH_RVA = 0x005EAD0A
MALLOC_WRAPPER_CALL_RVA = 0x005EAD0D
MALLOC_WRAPPER_RETURN_RVA = 0x005EAD12
MALLOC_IAT_THUNK_RVA = 0x005EC17C
FREE_IAT_THUNK_RVA = 0x005EC176
MALLOC_IAT_RVA = 0x00606504
FREE_IAT_RVA = 0x006064FC
REALLOC_IAT_RVA = 0x0060650C
REALLOC_CALL_RVA = 0x005C7C07
REALLOC_RETURN_RVA = 0x005C7C0D
ENTRY_BYTES = 12
GENERATED_IMAGE_BASE = 0x98000000


def macro_value(source: str, name: str) -> int:
    match = re.search(
        rf"^#define\s+{re.escape(name)}\s+(0x[0-9a-fA-F]+|[0-9]+)U\s*$",
        source,
        flags=re.MULTILINE,
    )
    if not match:
        fail(f"missing simple unsigned macro: {name}")
    return int(match.group(1), 0)


def expect_macros(source: str, expected: dict[str, int]) -> None:
    for name, value in expected.items():
        actual = macro_value(source, name)
        if actual != value:
            fail(f"header fact drifted: {name}=0x{actual:x}, expected 0x{value:x}")


def direct_iat_call_sites(image: PE32Image, iat_rva: int) -> list[int]:
    text = image.section(".text")
    text_bytes = image.section_data(text)
    pattern = b"\xff\x15" + struct.pack("<I", image.image_base + iat_rva)
    sites: list[int] = []
    offset = 0
    while True:
        offset = text_bytes.find(pattern, offset)
        if offset < 0:
            return sites
        sites.append(int(text["virtual_address"]) + offset)
        offset += 1


def sha256_file(path: pathlib.Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while True:
            block = stream.read(1024 * 1024)
            if not block:
                return digest.hexdigest()
            digest.update(block)


def verify_heap_source(source: str) -> None:
    owner_begin = source.index(
        "static uint32_t vita_heap_operator_new_owner_return_rva("
    )
    owner_end = source.index("\n}\n", owner_begin) + 3
    owner = source[owner_begin:owner_end]
    owner_markers = (
        "vita_heap_read_stack_word(c, c->esp, &allocator_return_rva)",
        "ISAAC_VITA_HEAP_WRAPPER_MALLOC_RETURN_RVA",
        "c, c->esp + 12U, &owner_return_rva",
    )
    positions = [owner.index(marker) for marker in owner_markers]
    if positions != sorted(positions):
        fail("operator-new owner stack-chain source contract drifted")

    malloc_begin = source.index("static void vita_heap_malloc(")
    malloc_end = source.index("static void vita_heap_set_new_mode(", malloc_begin)
    malloc_body = source[malloc_begin:malloc_end]
    route_markers = (
        "request == ISAAC_VITA_ROOM_ENTRY_REQUEST_BYTES",
        "vita_heap_operator_new_owner_return_rva(c)",
        "ISAAC_VITA_ROOM_ENTRY_OWNER_RETURN_RVA",
        "isaac_vita_room_entry_slab_malloc_locked",
    )
    positions = [malloc_body.index(marker) for marker in route_markers]
    if positions != sorted(positions) or len(set(positions)) != len(positions):
        fail("RoomConfig owner-gated malloc source contract drifted")


def canonical_bytes(value: object) -> bytes:
    return json.dumps(
        value,
        sort_keys=True,
        separators=(",", ":"),
        ensure_ascii=False,
        allow_nan=False,
    ).encode("utf-8")


def verify_generated(
    generated_dir: pathlib.Path, source_root: pathlib.Path,
) -> tuple[str, str, str, dict[str, str]]:
    manifest_path = generated_dir / "manifest.json"
    manifest_raw = manifest_path.read_bytes()
    try:
        manifest = json.loads(manifest_raw)
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        fail(f"generated manifest is invalid: {error}")
    expected_keys = {
        "schema", "stage", "recipe_id", "recipe", "outputs", "output_set_id"
    }
    if set(manifest) != expected_keys or manifest.get("schema") != 1 or \
            manifest.get("stage") != "generate":
        fail("generated manifest is not a schema-1 generate stage")
    try:
        recipe = manifest["recipe"]
        pe_hash = recipe["inputs"]["input/isaac-ng.exe.unpacked.exe"]
        image_base = recipe["params"]["image_base"]
        outputs = manifest["outputs"]
    except (KeyError, TypeError) as error:
        fail(f"generated manifest contract is incomplete: {error}")
    if pe_hash != EXPECTED_PE_SHA256:
        fail(f"generated corpus PE identity drifted: {pe_hash}")
    if image_base != GENERATED_IMAGE_BASE:
        fail(f"generated image base drifted: {image_base!r}")
    recipe_inputs = recipe.get("inputs")
    if not isinstance(recipe_inputs, dict):
        fail("generated recipe input closure is missing")
    generator_key = "source/recomp/gen_all.py"
    if generator_key not in recipe_inputs:
        fail("generated recipe omits gen_all.py")
    compared_inputs = 0
    for logical_name, recorded_digest in recipe_inputs.items():
        local_path: pathlib.Path | None = None
        if logical_name.startswith("source/recomp/"):
            local_path = source_root / logical_name.removeprefix("source/recomp/")
        elif logical_name.startswith("runtime/"):
            local_path = source_root / logical_name
        if local_path is None:
            continue
        if not local_path.is_file():
            fail(f"generated recipe dependency is missing locally: {logical_name}")
        actual_digest = sha256_file(local_path)
        if actual_digest != recorded_digest:
            fail(
                f"generated recipe dependency drifted: {logical_name} "
                f"{recorded_digest} != {actual_digest}"
            )
        compared_inputs += 1
    if compared_inputs < 8:
        fail("generated recipe did not expose the expected source closure")
    if hashlib.sha256(canonical_bytes(recipe)).hexdigest() != \
            manifest.get("recipe_id"):
        fail("generated recipe_id does not match its canonical recipe")
    if not isinstance(outputs, list) or not outputs:
        fail("generated output inventory is empty")
    if hashlib.sha256(canonical_bytes(outputs)).hexdigest() != \
            manifest.get("output_set_id"):
        fail("generated output_set_id does not match its inventory")

    output_names: set[str] = set()
    guest_units: dict[str, str] = {}
    verified_outputs: list[dict[str, object]] = []
    for index, record in enumerate(outputs):
        if not isinstance(record, dict) or set(record) != {
                "path", "size", "sha256"}:
            fail(f"generated output record {index} is malformed")
        name = record["path"]
        size = record["size"]
        digest = record["sha256"]
        if not isinstance(name, str) or not re.fullmatch(
                r"[A-Za-z0-9_.-]+", name) or name in output_names:
            fail(f"generated output name is unsafe or duplicate: {name!r}")
        if not isinstance(size, int) or isinstance(size, bool) or size < 0 or \
                not isinstance(digest, str) or not re.fullmatch(
                    r"[0-9a-f]{64}", digest):
            fail(f"generated output metadata is invalid: {name}")
        raw = (generated_dir / name).read_bytes()
        actual_digest = hashlib.sha256(raw).hexdigest()
        if len(raw) != size or actual_digest != digest:
            fail(f"generated output changed after its manifest: {name}")
        output_names.add(name)
        verified_outputs.append({"path": name, "size": size, "sha256": digest})
        if re.fullmatch(r"guest_[0-9]{4}\.c", name):
            normalized = raw.replace(b"\r\n", b"\n")
            if b"\r" in normalized:
                fail(f"{name} contains a non-CRLF carriage return")
            try:
                # Register/stack spelling and the GPR sync lines are the
                # contract of test_flags_local_corpus_contract.py; the frozen
                # statements here are pinned in the pre-token spelling.
                guest_units[name] = legacy_text(normalized.decode("ascii"))
            except UnicodeDecodeError as error:
                fail(f"{name} is not ASCII: {error}")
    if verified_outputs != sorted(
            verified_outputs, key=lambda record: str(record["path"])):
        fail("generated output inventory is not canonically sorted")
    actual_names = {item.name for item in generated_dir.iterdir()}
    if actual_names != output_names | {"manifest.json"}:
        fail("generated directory differs from its manifest inventory")

    def function_region_containing(source: str, marker: str) -> str:
        marker_position = source.index(marker)
        begin = source.rfind("\n/* sub_", 0, marker_position)
        if begin < 0:
            begin = 0
        else:
            begin += 1
        end = source.find("\n/* sub_", marker_position)
        if end < 0:
            end = len(source)
        return source[begin:end]

    # An indirect JMP hands the destination the flags: a unit that keeps the
    # flag state in locals publishes it first.
    malloc_import_body = (
        "GUEST_FLAGS_FLUSH(c); guest_call(c, ld32((uint32_t)((uint32_t)(int32_t)"
        "(-1738513148)))); return;"
    )
    free_import_body = (
        "GUEST_FLAGS_FLUSH(c); guest_call(c, ld32((uint32_t)((uint32_t)(int32_t)"
        "(-1738513156)))); return;"
    )

    # Locate semantic edges rather than pinning unit numbers: generator target
    # size and partitioning have changed before, while the instruction RVAs and
    # call-return stack contract are the frozen facts.
    edge_groups = {
        "entry-owner": (
            "/* 003e5ab0  mov cl, byte ptr [ebx + 8] */\n"
            "    c->ecx = (c->ecx & 0xFFFFFF00U) | "
            "((uint32_t)(ld8((uint32_t)(c->ebx + 0x8U))) & 0xFFU);",
            # The byte TEST pairs with its JE: the producer stores nothing
            # and the branch is the direct comparison.
            "/* 003e5ac1  test cl, cl */\n"
            "    /* 003e5ac3  je 0x3e5aea */\n"
            "    if ((((((uint8_t)((uint8_t)c->ecx)) & "
            "((uint8_t)((uint8_t)c->ecx)))) == (0U))) goto L_003e5aea;",
            "/* 003e5ac5  movzx eax, cl */\n"
            "    c->eax = (uint32_t)((uint32_t)(uint8_t)c->ecx);",
            "/* 003e5ac8  mov edx, 0xc */\n"
            "    c->edx = (uint32_t)(0xcU);",
            "/* 003e5acd  xor ecx, ecx */\n"
            "    { uint32_t _a = c->ecx, _b = c->ecx, "
            "_r = (uint32_t)(_a ^ _b);\n"
            "      c->ecx = (uint32_t)(_r);\n"
            "    }",
            "/* 003e5acf  mul edx */\n"
            "    { uint64_t _p = (uint64_t)c->eax * (uint64_t)c->edx;\n"
            "      c->eax = (uint32_t)_p; c->edx = (uint32_t)(_p >> 32);\n"
            "      GUEST_FL->f_cf = GUEST_FL->f_of = "
            "(uint8_t)((_p >> 32) != 0U);\n"
            "      SET_FLAGS(GUEST_FL, FLAG_MUL, 0U, 0U, (uint32_t)_p, 4);\n"
            "    }",
            "/* 003e5ad1  seto cl */\n"
            "    c->ecx = (c->ecx & 0xFFFFFF00U) | "
            "((uint32_t)((cc_o(GUEST_FL)) ? 1U : 0U) & 0xFFU);",
            "/* 003e5ad4  neg ecx */\n"
            "    { uint32_t _a = c->ecx, _r = (uint32_t)(0U - _a);\n"
            "      c->ecx = (uint32_t)(_r);\n"
            "    }",
            "/* 003e5ad6  or ecx, eax */\n"
            "    { uint32_t _a = c->ecx, _b = c->eax, "
            "_r = (uint32_t)(_a | _b);\n"
            "      c->ecx = (uint32_t)(_r);\n"
            "    }",
            "/* 003e5ad8  push ecx */\n"
            "    gpush_generated(c, c->ecx); GUEST_STACK_CALLSITE_BARRIER();",
            "/* 003e5ad9  call 0x5eb09c */\n"
            "    gpush_generated(c, 0x3e5adeU); "
            "GUEST_STACK_CALLSITE_BARRIER(); sub_005eb09c(c);",
            "/* 003e5ae4  mov dword ptr [ebx + 4], eax */\n"
            "    st32((uint32_t)(c->ebx + 0x4U), "
            "(uint32_t)(c->eax));",
        ),
        "normal-free": (
            "/* 003e37fe  push dword ptr [esi + 4] */\n"
            "    gpush_generated(c, ld32((uint32_t)(c->esi + 0x4U))); "
            "GUEST_STACK_CALLSITE_BARRIER();",
            "/* 003e3801  call 0x5eace5 */\n"
            "    gpush_generated(c, 0x3e3806U); "
            "GUEST_STACK_CALLSITE_BARRIER(); sub_005eace5(c);",
        ),
        "stage-free": (
            "/* 003e3b5e  push dword ptr [esi + 4] */\n"
            "    gpush_generated(c, ld32((uint32_t)(c->esi + 0x4U))); "
            "GUEST_STACK_CALLSITE_BARRIER();",
            "/* 003e3b61  call 0x5eace5 */\n"
            "    gpush_generated(c, 0x3e3b66U); "
            "GUEST_STACK_CALLSITE_BARRIER(); sub_005eace5(c);",
        ),
    }
    locations: dict[str, str] = {}
    for subject, markers in edge_groups.items():
        matches = [
            name for name, source in guest_units.items()
            if all(marker in source for marker in markers)
        ]
        if len(matches) != 1:
            fail(f"generated {subject} maps to {matches}, expected one unit")
        locations[subject] = matches[0]
        body = function_region_containing(guest_units[matches[0]], markers[0])
        positions = []
        for marker in markers:
            if marker not in body:
                fail(f"generated {subject} lost its frozen edge: {marker!r}")
            positions.append(body.index(marker))
        if positions != sorted(positions) or len(set(positions)) != len(positions):
            fail(f"generated {subject} bounded function order drifted")
        if sum(source.count(markers[0]) for source in guest_units.values()) != 1:
            fail(f"generated edge is not unique: {markers[0]!r}")

    function_contracts = {
        "operator-new-wrapper": ("005eb09c", (
            "/* 005eb09c  push ebp */\n"
            "    gpush_generated(c, c->ebp); GUEST_STACK_CALLSITE_BARRIER();",
            "/* 005eb09d  mov ebp, esp */\n"
            "    c->ebp = (uint32_t)(c->esp);",
            "/* 005eb09f  pop ebp */\n"
            "    c->ebp = (uint32_t)(gpop_generated(c));",
            "/* 005eb0a0  jmp 0x5eacf8 */\n"
            "    goto L_005eacf8;",
        )),
        "malloc-wrapper": ("005eacf8", (
            "/* 005eacf8  push ebp */\n"
            "    gpush_generated(c, c->ebp); GUEST_STACK_CALLSITE_BARRIER();",
            "/* 005eacf9  mov ebp, esp */\n"
            "    c->ebp = (uint32_t)(c->esp);",
            "/* 005eacfb  jmp 0x5ead0a */\n"
            "    goto L_005ead0a;",
            "/* 005ead0a  push dword ptr [ebp + 8] */\n"
            "    gpush_generated(c, ld32((uint32_t)(c->ebp + 0x8U))); "
            "GUEST_STACK_CALLSITE_BARRIER();",
            "/* 005ead0d  call 0x5ec17c */\n"
            "    gpush_generated(c, 0x5ead12U); "
            "GUEST_STACK_CALLSITE_BARRIER(); sub_005ec17c(c);",
            "/* 005ead12  pop ecx */\n"
            "    c->ecx = (uint32_t)(gpop_generated(c));",
        )),
        "malloc-import": ("005ec17c", (
            "/* 005ec17c  jmp dword ptr [0x98606504] */\n"
            f"    {malloc_import_body}",
        )),
        "free-wrapper": ("005eace5", (
            "/* 005eace5  jmp 0x5ebd94 */\n"
            "    goto L_005ebd94;",
            "/* 005ebd94  jmp 0x5ec176 */\n"
            "    goto L_005ec176;",
            "/* 005ec176  jmp dword ptr [0x986064fc] */\n"
            f"    {free_import_body}",
        )),
        "free-tail": ("005ebd94", (
            "/* 005ebd94  jmp 0x5ec176 */\n"
            "    goto L_005ec176;",
            "/* 005ec176  jmp dword ptr [0x986064fc] */\n"
            f"    {free_import_body}",
        )),
    }
    for subject, (rva, markers) in function_contracts.items():
        header = f"/* sub_{rva}  RVA {rva} "
        owners = [name for name, source in guest_units.items() if header in source]
        if len(owners) != 1:
            fail(f"generated function sub_{rva} maps to {owners}")
        owner = owners[0]
        source = guest_units[owner]
        begin = source.index(header)
        end = source.find("\n/* sub_", begin + len(header))
        if end < 0:
            end = len(source)
        body = source[begin:end]
        positions = []
        for marker in markers:
            if marker not in body:
                fail(f"generated {subject} lost its frozen edge: {marker!r}")
            positions.append(body.index(marker))
        if positions != sorted(positions) or len(set(positions)) != len(positions):
            fail(f"generated {subject} bounded function order drifted")
        locations[subject] = owner

    expected_closure_counts = {
        "/* 005ead0d  call 0x5ec17c */": 2,
        "/* 005ead12  pop ecx */": 2,
        "/* 005ebd94  jmp 0x5ec176 */": 2,
        "/* 005ec176  jmp dword ptr [0x986064fc] */": 2,
    }
    for marker, expected_count in expected_closure_counts.items():
        actual_count = sum(source.count(marker) for source in guest_units.values())
        if actual_count != expected_count:
            fail(
                f"generated closure count drifted for {marker!r}: "
                f"{actual_count} != {expected_count}"
            )
    # MUL 85 -> 86, IMUL 52 -> 53: the push/mov-imm supplemental roots (gen_all.py)
    # carries an explicit-flag IMUL.
    flag_snapshot_census = {"MUL": 86, "IMUL": 53}
    for operation, expected_count in flag_snapshot_census.items():
        flag_marker = f"SET_FLAGS(GUEST_FL, FLAG_{operation}"
        actual_count = sum(
            source.count(flag_marker) for source in guest_units.values()
        )
        snapshot_pattern = re.compile(
            r"GUEST_FL->f_cf = GUEST_FL->f_of = \(uint8_t\)[^;\n]+;\n"
            rf"\s+SET_FLAGS\(GUEST_FL, FLAG_{operation}"
        )
        protected_count = sum(
            len(snapshot_pattern.findall(source))
            for source in guest_units.values()
        )
        if actual_count != expected_count or protected_count != expected_count:
            fail(
                f"generated {operation} explicit-flag census drifted: "
                f"sites={actual_count} snapshots={protected_count} "
                f"expected={expected_count}"
            )
    return (
        hashlib.sha256(manifest_raw).hexdigest(),
        manifest["recipe_id"],
        manifest["output_set_id"],
        locations,
    )


def main() -> int:
    here = pathlib.Path(__file__).resolve().parent
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", required=True, type=pathlib.Path)
    parser.add_argument("--generated-dir", required=True, type=pathlib.Path)
    parser.add_argument(
        "--room-header",
        type=pathlib.Path,
        default=here / "runtime" / "host_vita_room_entry_slab.h",
    )
    parser.add_argument(
        "--heap-header",
        type=pathlib.Path,
        default=here / "runtime" / "host_vita_heap.h",
    )
    parser.add_argument(
        "--heap-source",
        type=pathlib.Path,
        default=here / "runtime" / "host_vita_heap.c",
    )
    arguments = parser.parse_args()

    raw = arguments.pe.read_bytes()
    digest = hashlib.sha256(raw).hexdigest()
    if len(raw) != EXPECTED_PE_BYTES or digest != EXPECTED_PE_SHA256:
        fail(
            f"RoomConfig PE identity drifted: size={len(raw)} sha256={digest}"
        )

    room_header = arguments.room_header.read_text(encoding="utf-8")
    heap_header = arguments.heap_header.read_text(encoding="utf-8")
    heap_source = arguments.heap_source.read_text(encoding="utf-8")
    manifest_sha, recipe_id, output_set_id, generated_locations = verify_generated(
        arguments.generated_dir.resolve(), here
    )
    expect_macros(
        room_header,
        {
            "ISAAC_VITA_ROOM_ENTRY_OWNER_RETURN_RVA": ENTRY_OWNER_RETURN_RVA,
            "ISAAC_VITA_ROOM_ENTRY_NORMAL_FREE_RETURN_RVA":
                ENTRY_NORMAL_FREE_RETURN_RVA,
            "ISAAC_VITA_ROOM_ENTRY_STAGE_FREE_RETURN_RVA":
                ENTRY_STAGE_FREE_RETURN_RVA,
            "ISAAC_VITA_ROOM_ENTRY_REQUEST_BYTES": ENTRY_BYTES,
        },
    )
    expect_macros(
        heap_header,
        {
            "ISAAC_VITA_HEAP_WRAPPER_MALLOC_CALL_RVA": MALLOC_WRAPPER_CALL_RVA,
            "ISAAC_VITA_HEAP_WRAPPER_MALLOC_RETURN_RVA":
                MALLOC_WRAPPER_RETURN_RVA,
            "ISAAC_VITA_HEAP_MALLOC_IAT_RVA": MALLOC_IAT_RVA,
            "ISAAC_VITA_HEAP_FREE_IAT_RVA": FREE_IAT_RVA,
            "ISAAC_VITA_HEAP_REALLOC_IAT_RVA": REALLOC_IAT_RVA,
            "ISAAC_VITA_HEAP_REALLOC_CALL_RVA": REALLOC_CALL_RVA,
            "ISAAC_VITA_HEAP_REALLOC_RETURN_RVA": REALLOC_RETURN_RVA,
        },
    )
    verify_heap_source(heap_source)

    pe = PE32Image(raw)
    if pe.image_base != 0x00400000:
        fail(f"unexpected PE image base: 0x{pe.image_base:08x}")

    # entityCount is loaded from spawn+8, widened, multiplied by exactly 12
    # with the frozen overflow idiom, passed to operator-new[], and the result
    # is stored at spawn+4.  The call return is the allocator owner identity.
    expect_bytes(
        pe,
        ENTRY_COUNT_LOAD_RVA,
        bytes.fromhex(
            "8a4b08 668903 0fb745f4 66894302 8d4308 84c9 7425"
        ),
        "RoomConfig Entry count load and non-empty branch",
    )
    expect_bytes(
        pe,
        ENTRY_ALLOC_SEQUENCE_RVA,
        bytes.fromhex(
            "0fb6c1 ba0c000000 33c9 f7e2 0f90c1 f7d9 0bc8 51"
        ),
        "RoomConfig entityCount*12 allocation argument",
    )
    expect_call(
        pe, ENTRY_ALLOC_CALL_RVA, OPERATOR_NEW_RVA,
        "RoomConfig Entry operator-new[] call",
    )
    if ENTRY_ALLOC_CALL_RVA + 5 != ENTRY_OWNER_RETURN_RVA:
        fail("RoomConfig allocation owner is not the exact call return")
    expect_bytes(
        pe,
        ENTRY_OWNER_RETURN_RVA,
        bytes.fromhex("8a4b08 83c404"),
        "RoomConfig allocation return and cdecl cleanup",
    )
    expect_bytes(
        pe, ENTRY_POINTER_STORE_RVA, b"\x89\x43\x04",
        "RoomConfig stores the Entry pointer at spawn+4",
    )

    expect_bytes(
        pe, ENTRY_NORMAL_FREE_PUSH_RVA, b"\xff\x76\x04",
        "normal teardown pushes spawn+4",
    )
    expect_call(
        pe, ENTRY_NORMAL_FREE_CALL_RVA, FREE_WRAPPER_RVA,
        "normal teardown Entry free",
    )
    expect_bytes(
        pe,
        ENTRY_NORMAL_FREE_RETURN_RVA,
        bytes.fromhex("8b4df0 83c404 c7460400000000"),
        "normal teardown free return and pointer clear",
    )
    expect_bytes(
        pe, ENTRY_STAGE_FREE_PUSH_RVA, b"\xff\x76\x04",
        "Stage unload pushes spawn+4",
    )
    expect_call(
        pe, ENTRY_STAGE_FREE_CALL_RVA, FREE_WRAPPER_RVA,
        "Stage unload inline Entry free",
    )
    expect_bytes(
        pe,
        ENTRY_STAGE_FREE_RETURN_RVA,
        bytes.fromhex("8b4dec 83c404 c7460400000000"),
        "Stage unload free return and pointer clear",
    )

    # Freeze the stack-neutral operator-new wrapper and allocator frame that
    # make the original caller return observable at malloc-import [ESP+12].
    expect_bytes(
        pe, OPERATOR_NEW_RVA, b"\x55\x8b\xec\x5d",
        "operator-new[] stack-neutral wrapper",
    )
    expect_jump(pe, 0x005EB0A0, ALLOCATOR_RVA, "operator-new allocator jump")
    expect_bytes(
        pe, ALLOCATOR_RVA, bytes.fromhex("558bec eb0d"),
        "allocator prologue and direct size-push jump",
    )
    expect_bytes(
        pe, MALLOC_ARGUMENT_PUSH_RVA, b"\xff\x75\x08",
        "allocator pushes exact size",
    )
    expect_call(
        pe, MALLOC_WRAPPER_CALL_RVA, MALLOC_IAT_THUNK_RVA,
        "allocator malloc-IAT thunk call",
    )
    expect_bytes(
        pe, MALLOC_WRAPPER_RETURN_RVA, b"\x59",
        "malloc wrapper return at import [ESP]",
    )
    expect_bytes(
        pe,
        MALLOC_IAT_THUNK_RVA,
        b"\xff\x25" + struct.pack("<I", pe.image_base + MALLOC_IAT_RVA),
        "malloc IAT jump",
    )
    expect_jump(pe, FREE_WRAPPER_RVA, 0x005EBD94, "free-wrapper jump")
    expect_jump(pe, 0x005EBD94, FREE_IAT_THUNK_RVA, "free-IAT thunk jump")
    expect_bytes(
        pe,
        FREE_IAT_THUNK_RVA,
        b"\xff\x25" + struct.pack("<I", pe.image_base + FREE_IAT_RVA),
        "free IAT jump",
    )

    imports = pe.imports()
    expected_imports = {
        MALLOC_IAT_RVA: "api-ms-win-crt-heap-l1-1-0.dll!malloc",
        FREE_IAT_RVA: "api-ms-win-crt-heap-l1-1-0.dll!free",
        REALLOC_IAT_RVA: "api-ms-win-crt-heap-l1-1-0.dll!realloc",
    }
    for iat_rva, expected in expected_imports.items():
        if imports.get(iat_rva) != expected:
            fail(f"heap IAT mapping drifted at 0x{iat_rva:08x}")

    # The representative direct realloc import belongs to an unrelated
    # byte-vector growth path.  There is no direct realloc import edge anywhere
    # in the broad frozen RoomConfig code neighborhood containing all three
    # exact allocation/free blocks above.
    realloc_sites = direct_iat_call_sites(pe, REALLOC_IAT_RVA)
    if REALLOC_CALL_RVA not in realloc_sites:
        fail("representative direct realloc call disappeared from the PE")
    room_realloc_sites = [
        site for site in realloc_sites if 0x003E0000 <= site < 0x003E6000
    ]
    if room_realloc_sites:
        fail(f"RoomConfig neighborhood gained realloc calls: {room_realloc_sites}")
    expect_bytes(
        pe,
        REALLOC_CALL_RVA,
        b"\xff\x15" + struct.pack("<I", pe.image_base + REALLOC_IAT_RVA),
        "representative unrelated direct realloc call",
    )
    expect_bytes(
        pe, REALLOC_RETURN_RVA, b"\x83\xc4\x08",
        "unrelated realloc return",
    )

    dependency_paths = (
        pathlib.Path(__file__).resolve(),
        here / "test_ogg_emergency_contract.py",
        here / "gen_all.py",
        here / "emit.py",
        arguments.room_header.resolve(),
        arguments.heap_header.resolve(),
        arguments.heap_source.resolve(),
    )
    print(
        "RoomConfig frozen gate dependency SHA256: "
        + " ".join(
            f"{path.name}={sha256_file(path)}" for path in dependency_paths
        )
    )
    print(
        "RoomConfig frozen generated receipt: "
        f"manifest={manifest_sha} recipe={recipe_id} outputs={output_set_id} "
        "mul/imul-explicit=85/52 "
        + " ".join(
            f"{subject}={unit}"
            for subject, unit in sorted(generated_locations.items())
        )
    )
    print(
        "RoomConfig Entry frozen PE contract: PASS; "
        "size=8650240 sha256=31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404 "
        "alloc=003e5ad9->005eb09c owner=003e5ade size=12 "
        "free=003e3801/003e3806 stage=003e3b61/003e3b66 "
        "room-direct-realloc-sites=0 representative=005c7c07(outside-room)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
