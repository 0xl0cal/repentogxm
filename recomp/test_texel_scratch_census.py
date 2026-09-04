#!/usr/bin/env python3
"""Pin the procedural-texel scratch owners to the frozen PE/codegen corpus."""

from __future__ import annotations

import argparse
import hashlib
import re
import struct
import sys
from pathlib import Path


HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

import build_contract as BC  # noqa: E402


PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
GENERATOR_RECIPE_INPUT = "source/recomp/gen_all.py"
PE_RECIPE_INPUT = "input/isaac-ng.exe.unpacked.exe"
VITA_IMAGE_BASE = 0x98000000

OWNER_4_RETURN = 0x005B5FCF
OWNER_5_RETURN = 0x005B618C
UPLOAD_RVA = 0x005B0920
ALLOC_IMPL_RVA = 0x0059A090
FREE_IMPL_RVA = 0x0059A100
ALLOC_WRAPPER_RVA = 0x0059A590
FREE_WRAPPER_RVA = 0x0059A5B0
ALLOC_ADAPTER_RVA = 0x0059A5E0
ALLOC_IMPORT_RETURN = 0x0059A0AB
WRAPPER_RETURN = 0x0059A5A1
ADAPTER_RETURN = 0x0059A5F3
ALLOCATOR_OBJECT_RVA = 0x007E8170
ALLOCATOR_VTABLE_RVA = 0x00764AE4
PROCEDURAL_VTABLE_RVA = 0x007682AC
PROCEDURAL_RELOAD_SLOT = 0x7C
GL_TEX_IMAGE_RESOLVER_RVA = 0x0058D0E0
GL_TEX_IMAGE_NAME_RVA = 0x0062077D
GL_TEX_IMAGE_SLOT_RVA = 0x007C1F04

# Whole frozen functions, not selected opcodes.  The structural checks below
# explain why these bytes establish a transient lifetime.
PE_WINDOWS = (
    (0x005B5E40, 0x005B611E,
     "a9e94243f54cebe51340458fb9ca0e4af10e9062ad30e1e8c1c7e06dc8a4627a"),
    (0x005B6160, 0x005B6268,
     "db758a59ef5363e86e613705363232c2aa5f5ba62cfa218856d1d3c25e1539c4"),
    (0x005B0920, 0x005B0A41,
     "ab71dd81e373c8e614fdeeec9f8a3cf9c077770b0b61521db3d8a88733414cf4"),
    (0x0059A090, 0x0059A0F2,
     "36d95566874c4b3dfe47434843253c233ca1ae961927fa5d9d85c00db5c6393c"),
    (0x0059A100, 0x0059A1C0,
     "8adbe23b32cbbc8397944c582a1a7d015b5780d1aa4e985199ccf7ea95550d23"),
    (0x0059A590, 0x0059A5A5,
     "4d4937b17b516ea6565f8d1b07c0b8227751f75fec1155ae7eee926c3ab531bb"),
    (0x0059A5B0, 0x0059A5B8,
     "9bcd46433fd2fe4e3c1c0a99bbcc052a6572e35ac0141afc0fd2d6a4170079cf"),
    (0x0059A5E0, 0x0059A5F9,
     "12d373033ceaf0a744c8979c9d42ee688374e5c6526c22d4cbe5a635824a6b6c"),
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


class PeImage:
    """Small file-backed RVA/VA reader for the frozen ownership proof."""

    def __init__(self, path: Path) -> None:
        self.data = path.read_bytes()
        if self.data[:2] != b"MZ":
            raise AssertionError("frozen input has no MZ header")
        pe_offset = struct.unpack_from("<I", self.data, 0x3C)[0]
        if self.data[pe_offset:pe_offset + 4] != b"PE\0\0":
            raise AssertionError("frozen input has no PE signature")
        section_count = struct.unpack_from("<H", self.data, pe_offset + 6)[0]
        optional_size = struct.unpack_from("<H", self.data, pe_offset + 20)[0]
        optional_offset = pe_offset + 24
        if struct.unpack_from("<H", self.data, optional_offset)[0] != 0x10B:
            raise AssertionError("frozen input is not PE32")
        self.image_base = struct.unpack_from(
            "<I", self.data, optional_offset + 28
        )[0]
        self.import_directory_rva, self.import_directory_size = (
            struct.unpack_from("<II", self.data, optional_offset + 104)
        )
        section_offset = optional_offset + optional_size
        self.sections: list[tuple[int, int, int]] = []
        for index in range(section_count):
            offset = section_offset + index * 40
            virtual_size, virtual_address, raw_size, raw_pointer = (
                struct.unpack_from("<IIII", self.data, offset + 8)
            )
            self.sections.append(
                (virtual_address, max(virtual_size, raw_size), raw_pointer)
            )

    def bytes_at(self, rva: int, size: int) -> bytes:
        for virtual_address, extent, raw_pointer in self.sections:
            if virtual_address <= rva < virtual_address + extent:
                offset = raw_pointer + rva - virtual_address
                result = self.data[offset:offset + size]
                if len(result) == size:
                    return result
                break
        raise AssertionError(f"short/unmapped PE read at RVA 0x{rva:08x}")

    def pointer_rva(self, rva: int) -> int:
        value = struct.unpack("<I", self.bytes_at(rva, 4))[0]
        if value < self.image_base:
            raise AssertionError(
                f"PE pointer at 0x{rva:08x} is below ImageBase: 0x{value:08x}"
            )
        return value - self.image_base

    def c_string(self, rva: int) -> str:
        value = bytearray()
        for offset in range(512):
            byte = self.bytes_at(rva + offset, 1)
            if byte == b"\0":
                return value.decode("ascii")
            value += byte
        raise AssertionError(f"unterminated PE string at RVA 0x{rva:08x}")

    def import_names(self) -> dict[int, str]:
        """Return PE32 first-thunk RVAs keyed to their imported names."""
        if not self.import_directory_rva or not self.import_directory_size:
            raise AssertionError("frozen PE has no import directory")
        result = {}
        descriptor_rva = self.import_directory_rva
        descriptor_limit = descriptor_rva + self.import_directory_size
        while descriptor_rva + 20 <= descriptor_limit:
            descriptor = struct.unpack(
                "<IIIII", self.bytes_at(descriptor_rva, 20)
            )
            if descriptor == (0, 0, 0, 0, 0):
                return result
            original_thunk, _, _, name_rva, first_thunk = descriptor
            if not name_rva or not first_thunk:
                raise AssertionError(
                    f"malformed import descriptor at RVA 0x{descriptor_rva:08x}"
                )
            lookup_thunk = original_thunk or first_thunk
            for index in range(65536):
                name_entry = struct.unpack(
                    "<I", self.bytes_at(lookup_thunk + index * 4, 4)
                )[0]
                if name_entry == 0:
                    break
                if not name_entry & 0x80000000:
                    result[first_thunk + index * 4] = self.c_string(
                        name_entry + 2
                    )
            else:
                raise AssertionError("unterminated PE import thunk table")
            descriptor_rva += 20
        raise AssertionError("unterminated PE import descriptor table")


def direct_call_target(image: PeImage, site: int) -> int:
    instruction = image.bytes_at(site, 5)
    if instruction[0] != 0xE8:
        raise AssertionError(f"RVA 0x{site:08x} is not CALL rel32")
    displacement = struct.unpack_from("<i", instruction, 1)[0]
    return (site + 5 + displacement) & 0xFFFFFFFF


def verify_pe(path: Path) -> None:
    actual_hash = sha256(path)
    if actual_hash != PE_SHA256:
        raise AssertionError(
            f"frozen PE SHA-256 mismatch: {actual_hash} != {PE_SHA256}"
        )
    image = PeImage(path)
    if image.image_base != 0x00400000:
        raise AssertionError(
            f"frozen PE ImageBase changed: 0x{image.image_base:08x}"
        )
    for start, end, expected in PE_WINDOWS:
        actual = hashlib.sha256(image.bytes_at(start, end - start)).hexdigest()
        if actual != expected:
            raise AssertionError(
                f"procedural ownership window changed at 0x{start:08x}: {actual}"
            )

    expected_calls = {
        OWNER_4_RETURN - 5: ALLOC_ADAPTER_RVA,
        0x005B6069: UPLOAD_RVA,
        0x005B61B6: UPLOAD_RVA,
    }
    actual_calls = {
        site: direct_call_target(image, site) for site in expected_calls
    }
    if actual_calls != expected_calls:
        raise AssertionError(
            f"procedural direct-call graph changed: {actual_calls!r}"
        )

    # Owner 5 calls the same allocation wrapper directly through the global
    # allocator vtable; owner 4 reaches it through ALLOC_ADAPTER_RVA.  Their
    # real imported-malloc stack depths are therefore two and three.
    if image.pointer_rva(ALLOCATOR_OBJECT_RVA) != ALLOCATOR_VTABLE_RVA:
        raise AssertionError("global image allocator vtable pointer changed")
    expected_vtable = {
        0x18: ALLOC_IMPL_RVA,
        0x1C: FREE_IMPL_RVA,
        0x20: ALLOC_WRAPPER_RVA,
        0x24: FREE_WRAPPER_RVA,
    }
    actual_vtable = {
        offset: image.pointer_rva(ALLOCATOR_VTABLE_RVA + offset)
        for offset in expected_vtable
    }
    if actual_vtable != expected_vtable:
        raise AssertionError(
            f"global image allocator vtable changed: {actual_vtable!r}"
        )
    if image.bytes_at(ALLOCATOR_OBJECT_RVA + 0x0C, 1) != b"\0":
        raise AssertionError("global image allocator does not start in libc mode")

    exact_indirect_calls = {
        0x005B6189: bytes.fromhex("ff 50 20"),
        0x005B0A1F: bytes.fromhex("ff 50 24"),
        0x0059A59E: bytes.fromhex("ff 50 18"),
        0x0059A5B6: bytes.fromhex("ff 60 1c"),
        0x0059A5F0: bytes.fromhex("ff 50 20"),
    }
    for site, expected in exact_indirect_calls.items():
        actual = image.bytes_at(site, len(expected))
        if actual != expected:
            raise AssertionError(
                f"allocator indirect edge changed at 0x{site:08x}: {actual.hex()}"
            )

    imports = image.import_names()
    if imports.get(0x00606504) != "malloc":
        raise AssertionError("allocation implementation no longer calls malloc")
    if imports.get(0x006064FC) != "free":
        raise AssertionError("deallocation implementation no longer calls free")
    if image.bytes_at(0x0059A0A5, 6) != bytes.fromhex("ff 15 04 65 a0 00"):
        raise AssertionError("malloc IAT callsite changed")
    if image.bytes_at(0x0059A110, 6) != bytes.fromhex("ff 15 fc 64 a0 00"):
        raise AssertionError("free IAT callsite changed")

    # The constructor installs this vtable, whose RTTI names the class.  The
    # second owner is the same class's frozen virtual Reload slot.
    if image.bytes_at(0x005B5E8B, 6) != bytes.fromhex(
            "c7 03 ac 82 b6 00"):
        raise AssertionError("procedural constructor vtable write changed")
    object_locator_rva = image.pointer_rva(PROCEDURAL_VTABLE_RVA - 4)
    type_descriptor_rva = image.pointer_rva(object_locator_rva + 12)
    if image.c_string(type_descriptor_rva + 8) != (
            ".?AVProceduralImageBase@Graphics@KAGE@@"):
        raise AssertionError("procedural vtable RTTI name changed")
    if image.pointer_rva(
            PROCEDURAL_VTABLE_RVA + PROCEDURAL_RELOAD_SLOT
    ) != 0x005B6160:
        raise AssertionError("procedural Reload vtable slot changed")

    # The common upload's indirect slot is populated by the lazy resolver
    # using the literal API name glTexImage2D.
    expected_name_load = b"\xb9" + struct.pack(
        "<I", image.image_base + GL_TEX_IMAGE_NAME_RVA
    )
    expected_slot_store = b"\xa3" + struct.pack(
        "<I", image.image_base + GL_TEX_IMAGE_SLOT_RVA
    )
    if image.c_string(GL_TEX_IMAGE_NAME_RVA) != "glTexImage2D":
        raise AssertionError("procedural upload API name changed")
    if image.bytes_at(0x0058D0ED, 5) != expected_name_load or (
            direct_call_target(image, 0x0058D0F2) != 0x005700E0
    ) or image.bytes_at(0x0058D0FA, 5) != expected_slot_store:
        raise AssertionError("glTexImage2D lazy-resolver edge changed")


def header_macro(text: str, name: str) -> int:
    match = re.search(
        rf"^\s*#define\s+{re.escape(name)}\s+"
        rf"(0x[0-9a-fA-F]+|[0-9]+)U?\s*$",
        text,
        re.MULTILINE,
    )
    if not match:
        raise AssertionError(f"header macro absent: {name}")
    return int(match.group(1), 0)


def verify_header(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    expected = {
        "KAGE_VITA_TEXEL_MALLOC_IAT_RETURN_RVA": ALLOC_IMPORT_RETURN,
        "KAGE_VITA_TEXEL_WRAPPER_RETURN_RVA": WRAPPER_RETURN,
        "KAGE_VITA_TEXEL_ADAPTER_RETURN_RVA": ADAPTER_RETURN,
        "KAGE_VITA_TEXEL_LOADER_RETURN_4": OWNER_4_RETURN,
        "KAGE_VITA_TEXEL_LOADER_RETURN_5": OWNER_5_RETURN,
    }
    actual = {name: header_macro(text, name) for name in expected}
    if actual != expected:
        raise AssertionError(f"texel ownership constants drifted: {actual!r}")


def verify_source(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    match = re.search(
        r"static int texel_scratch_owner_is_transient\([^)]*\)\s*"
        r"\{(?P<body>.*?)\n\}",
        text,
        re.DOTALL,
    )
    if not match:
        raise AssertionError("transient texel owner predicate is absent")
    cases = re.findall(r"\bcase\s+([A-Z0-9_]+)\s*:", match.group("body"))
    expected = [
        "KAGE_VITA_TEXEL_PNG_LOADER_RETURN",
        "KAGE_VITA_TEXEL_LOADER_RETURN_4",
        "KAGE_VITA_TEXEL_LOADER_RETURN_5",
    ]
    if cases != expected:
        raise AssertionError(f"transient texel owner allowlist changed: {cases!r}")


def verify_oracle(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    checks = {
        (1480192, "KAGE_VITA_TEXEL_LOADER_RETURN_4"): 1,
        (2088960, "KAGE_VITA_TEXEL_LOADER_RETURN_5"): 0,
    }
    for (size, owner), expected_adapter in checks.items():
        pattern = re.compile(
            rf"routing_texel_malloc\(\s*&cpu,\s*stack,\s*{size}U,\s*"
            rf"{owner},\s*([01]),\s*&result\)",
            re.DOTALL,
        )
        matches = pattern.findall(text)
        if matches != [str(expected_adapter)]:
            raise AssertionError(
                f"routing oracle has wrong {owner} stack: {matches!r}; "
                f"expected has_adapter={expected_adapter}"
            )


def verify_generated_contract(directory: Path) -> None:
    manifest_path = directory / "manifest.json"
    candidate = BC.load_manifest(manifest_path)
    recipe_id = candidate.get("recipe_id")
    if not isinstance(recipe_id, str):
        raise AssertionError("generation manifest has no recipe_id")
    output_paths = {}
    for path in sorted(directory.iterdir()):
        if path.name == manifest_path.name:
            continue
        if not path.is_file():
            raise AssertionError(f"non-file generation output: {path}")
        output_paths[path.name] = path
    manifest = BC.require_manifest(
        manifest_path, "generate", recipe_id, output_paths
    )
    recipe = manifest.get("recipe")
    inputs = recipe.get("inputs") if isinstance(recipe, dict) else None
    params = recipe.get("params") if isinstance(recipe, dict) else None
    if not isinstance(inputs, dict) or not isinstance(params, dict):
        raise AssertionError("generation manifest recipe shape changed")
    current_generator = sha256(HERE / "gen_all.py")
    if inputs.get(GENERATOR_RECIPE_INPUT) != current_generator:
        raise AssertionError("generated corpus is stale for current gen_all.py")
    if inputs.get(PE_RECIPE_INPUT) != PE_SHA256:
        raise AssertionError("generated corpus records a different frozen PE")
    if params.get("image_base") != VITA_IMAGE_BASE:
        raise AssertionError("generated corpus is not for Vita image base")


def ordered(text: str, markers: tuple[str, ...], label: str) -> tuple[int, ...]:
    positions = []
    cursor = 0
    for marker in markers:
        position = text.find(marker, cursor)
        if position < 0:
            raise AssertionError(f"{label} marker absent/out of order: {marker}")
        positions.append(position)
        cursor = position + len(marker)
    return tuple(positions)


def guest_control_transfers(text: str) -> list[str]:
    """Return guest jump/return mnemonics, ignoring translator fault guards."""
    return re.findall(
        r"/\*\s+[0-9a-f]{8}\s+((?:j[a-z]+)|ret)\b", text
    )


def generated_function(directory: Path, rva: int) -> str:
    marker = f"/* sub_{rva:08x}  RVA {rva:08x}"
    owners = []
    for path in sorted(directory.glob("guest_*.c")):
        text = path.read_text(encoding="utf-8", errors="strict")
        if marker in text:
            owners.append((path, text))
    if len(owners) != 1:
        raise AssertionError(
            f"generated function {rva:08x} owner count changed: "
            f"{[str(path) for path, _ in owners]}"
        )
    _, text = owners[0]
    start = text.index(marker)
    end = text.find("\n/* sub_", start + len(marker))
    return text[start:] if end < 0 else text[start:end]


def verify_generated(directory: Path) -> None:
    verify_generated_contract(directory)
    owner_4 = generated_function(directory, 0x005B5E40)
    owner_5 = generated_function(directory, 0x005B6160)
    upload = generated_function(directory, UPLOAD_RVA)
    adapter = generated_function(directory, ALLOC_ADAPTER_RVA)
    wrapper = generated_function(directory, ALLOC_WRAPPER_RVA)
    free_wrapper = generated_function(directory, FREE_WRAPPER_RVA)
    free_impl = generated_function(directory, FREE_IMPL_RVA)
    gl_tex_image_resolver = generated_function(
        directory, GL_TEX_IMAGE_RESOLVER_RVA
    )

    owner_4_positions = ordered(owner_4, (
        "/* 005b5fca  call 0x59a5e0 */",
        "gpush_generated(c, 0x5b5fcfU); GUEST_STACK_CALLSITE_BARRIER(); "
        "sub_0059a5e0(c);",
        "/* 005b5fcf  mov dword ptr [ebp + 0x10], eax */",
        "/* 005b5fd4  je 0x5b6070 */",
        "/* 005b6058  je 0x5b6063 */",
        "L_005b6063:",
        "/* 005b6063  lea eax, [ebp + 0x10] */",
        "/* 005b6069  call 0x5b0920 */",
        "gpush_generated(c, 0x5b606eU); GUEST_STACK_CALLSITE_BARRIER(); "
        "sub_005b0920(c);",
        "L_005b6070:",
        "/* 005b611d  ret 0x24 */",
    ), "procedural owner 4")
    owner_4_controls = guest_control_transfers(
        owner_4[owner_4_positions[1]:owner_4_positions[7]]
    )
    # The first branch skips upload only when allocation returned NULL; the
    # second skips a zero-length fill and converges immediately on upload.
    if owner_4_controls != ["je", "je"]:
        raise AssertionError(
            "owner 4 allocation/upload control flow changed: "
            f"{owner_4_controls!r}"
        )

    owner_5_positions = ordered(owner_5, (
        "/* 005b6189  call dword ptr [eax + 0x20] */",
        "gpush_generated(c, 0x5b618cU);",
        "/* 005b6199  mov dword ptr [esp + 4], eax */",
        "/* 005b61a7  call 0x5ec152 */",
        "/* 005b61af  lea eax, [esp + 4] */",
        "/* 005b61b6  call 0x5b0920 */",
        "gpush_generated(c, 0x5b61bbU); GUEST_STACK_CALLSITE_BARRIER(); "
        "sub_005b0920(c);",
        "/* 005b6267  ret  */",
    ), "procedural owner 5")
    owner_5_controls = guest_control_transfers(
        owner_5[owner_5_positions[1]:owner_5_positions[5]]
    )
    if owner_5_controls:
        raise AssertionError(
            "owner 5 allocation/upload control flow changed: "
            f"{owner_5_controls!r}"
        )

    upload_positions = ordered(upload, (
        "/* 005b09ed  push dword ptr [esi] */",
        "/* 005b0a0d  call dword ptr [0x987c1f04] */",
        "/* 005b0a13  mov eax, dword ptr [0x987e8170] */",
        "/* 005b0a1d  push dword ptr [esi] */",
        "/* 005b0a1f  call dword ptr [eax + 0x24] */",
        "/* 005b0a40  ret 4 */",
    ), "common upload/free helper")
    upload_controls = guest_control_transfers(
        upload[upload_positions[1]:upload_positions[4]]
    )
    if upload_controls:
        raise AssertionError(
            "upload/free control flow changed: " f"{upload_controls!r}"
        )

    ordered(adapter, (
        "/* 0059a5f0  call dword ptr [eax + 0x20] */",
        "gpush_generated(c, 0x59a5f3U);",
    ), "allocation adapter")
    ordered(wrapper, (
        "/* 0059a59e  call dword ptr [eax + 0x18] */",
        "gpush_generated(c, 0x59a5a1U);",
    ), "allocation wrapper")
    ordered(free_wrapper, (
        "/* 0059a5b6  jmp dword ptr [eax + 0x1c] */",
        "guest_call(c, ld32((uint32_t)(c->eax + 0x1cU))); return;",
    ), "free wrapper")
    ordered(free_impl, (
        "/* 0059a107  cmp byte ptr [esi + 0xc], 0 */",
        "/* 0059a10d  push dword ptr [ebp + 8] */",
        "/* 0059a110  call dword ptr [0x986064fc] */",
        "gpush_generated(c, 0x59a116U);",
    ), "free implementation")
    ordered(gl_tex_image_resolver, (
        "/* 0058d0ed  mov ecx, 0x9862077d */",
        "/* 0058d0f2  call 0x5700e0 */",
        "gpush_generated(c, 0x58d0f7U); GUEST_STACK_CALLSITE_BARRIER(); "
        "sub_005700e0(c);",
        "/* 0058d0fa  mov dword ptr [0x987c1f04], eax */",
    ), "glTexImage2D lazy resolver")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", type=Path, required=True)
    parser.add_argument("--generated", type=Path, required=True)
    parser.add_argument("--header", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--oracle", type=Path, required=True)
    arguments = parser.parse_args()

    verify_pe(arguments.pe.resolve())
    verify_header(arguments.header.resolve())
    verify_source(arguments.source.resolve())
    verify_oracle(arguments.oracle.resolve())
    verify_generated(arguments.generated.resolve())
    print(
        "Texel scratch procedural frozen census: PASS; "
        f"PE={PE_SHA256}; owners={OWNER_4_RETURN:08x}/depth3,"
        f"{OWNER_5_RETURN:08x}/depth2; upload/free=005b0920"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
