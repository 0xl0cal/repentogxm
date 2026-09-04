#!/usr/bin/env python3
"""Pin the narrow ANM2 scratch seam to the frozen PE and generated corpus."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
import struct
import tempfile
from dataclasses import dataclass
from pathlib import Path, PurePosixPath

import build_contract as BC
import linker_map_contract


PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
CANONICAL_MAP_SCHEMA = linker_map_contract.SCHEMA
CANONICAL_MAP_ALGORITHM = linker_map_contract.ALGORITHM
CANONICAL_MAP_SHA256 = (
    "21bc2db8bbdecc4d58d8abd2bedad5471b6c61e8a97773d6e72ec0924ccfa66f"
)
# Canonical Bundle3 generated manifest SHA-256
# 827b5239c6d99242f8adad5b3a3e16177fc1a02db0756893346a6de23bc8063b
# (recipe 1974a4c67420e9ec3f98206bc97fa0c9cd1ca7e147e3ce41338ea8b6fae9f809,
# output set c8551f6e596cce0c8e290d631e85124bff6ee93ac5219bbbc395510127a2f9fc)
# pins the LF-generated literal owner and both wrapper-boundary units; the map
# separately binds the final link and this generation recipe.  sub_005eacf8
# owns its public body in guest_0183, while sub_005eb09c has an independent
# merged copy of the shared allocation tail in guest_0184.
OWNER_SHA256 = "8e6af1fa9ba606e1a6f525d5815a92c3ad32a8cd1243ca005abcf53d17bd639e"
WRAPPER_0_SHA256 = "570b8e97a0007d3abddc6044d32f3d328613ad226f2b6f9a3187a95d69977b49"
WRAPPER_1_SHA256 = "f99b82112f2fdb73943e79426436ac15b0dc0aa0d58a4c1191daec03e4bbfb86"
OPERATOR_NEW_RVA = 0x005EB09C
MALLOC_IMPORT_RETURN_RVA = 0x005EAD12
LINK_END = 0x82AD1560
GUEST_IMAGE_BASE = 0x98000000
PE_RECIPE_INPUT = "input/isaac-ng.exe.unpacked.exe"


@dataclass(frozen=True)
class Site:
    push_rva: int
    owner_return_rva: int
    size: int
    pe_bytes: bytes


SITES = (
    Site(0x0000AA4D, 0x0000AA5D, 420000,
         bytes.fromhex("68 a0 68 06 00 89 bd 64 99 fd ff e8 3f 06 5e 00")),
    Site(0x0000AA9F, 0x0000AAA9, 420000,
         bytes.fromhex("68 a0 68 06 00 e8 f3 05 5e 00")),
    Site(0x0000AAEF, 0x0000AAF9, 420000,
         bytes.fromhex("68 a0 68 06 00 e8 a3 05 5e 00")),
    Site(0x0000AB4F, 0x0000AB59, 420000,
         bytes.fromhex("68 a0 68 06 00 e8 43 05 5e 00")),
    Site(0x0000AB9F, 0x0000ABA9, 540000,
         bytes.fromhex("68 60 3d 08 00 e8 f3 04 5e 00")),
    Site(0x0000AC1B, 0x0000AC25, 540000,
         bytes.fromhex("68 60 3d 08 00 e8 77 04 5e 00")),
)


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


class PeImage:
    def __init__(self, path: Path) -> None:
        self.path = path
        self.data = path.read_bytes()
        if self.data[:2] != b"MZ":
            raise AssertionError("frozen input has no MZ header")
        pe_offset = struct.unpack_from("<I", self.data, 0x3C)[0]
        if self.data[pe_offset:pe_offset + 4] != b"PE\0\0":
            raise AssertionError("frozen input has no PE signature")
        section_count = struct.unpack_from("<H", self.data, pe_offset + 6)[0]
        optional_size = struct.unpack_from("<H", self.data, pe_offset + 20)[0]
        section_offset = pe_offset + 24 + optional_size
        self.sections: list[tuple[int, int, int, int]] = []
        for index in range(section_count):
            offset = section_offset + index * 40
            virtual_size, virtual_address, raw_size, raw_pointer = (
                struct.unpack_from("<IIII", self.data, offset + 8)
            )
            characteristics = struct.unpack_from("<I", self.data, offset + 36)[0]
            self.sections.append(
                (virtual_address, max(virtual_size, raw_size),
                 raw_pointer, characteristics)
            )

    def offset(self, rva: int) -> int:
        for virtual_address, extent, raw_pointer, _ in self.sections:
            if virtual_address <= rva < virtual_address + extent:
                offset = raw_pointer + rva - virtual_address
                if offset >= len(self.data):
                    break
                return offset
        raise AssertionError(f"RVA 0x{rva:08x} is not file-backed")

    def bytes_at(self, rva: int, size: int) -> bytes:
        offset = self.offset(rva)
        result = self.data[offset:offset + size]
        if len(result) != size:
            raise AssertionError(f"short PE read at RVA 0x{rva:08x}")
        return result

    def executable_ranges(self) -> list[tuple[int, bytes]]:
        result = []
        for virtual_address, extent, raw_pointer, characteristics in self.sections:
            if not characteristics & 0x20000000:
                continue
            available = min(extent, len(self.data) - raw_pointer)
            result.append(
                (virtual_address, self.data[raw_pointer:raw_pointer + available])
            )
        return result


def direct_call_target(image: PeImage, call_rva: int) -> int:
    instruction = image.bytes_at(call_rva, 5)
    if instruction[0] != 0xE8:
        raise AssertionError(f"RVA 0x{call_rva:08x} is not CALL rel32")
    displacement = struct.unpack_from("<i", instruction, 1)[0]
    return (call_rva + 5 + displacement) & 0xFFFFFFFF


def verify_pe(path: Path) -> None:
    actual_hash = sha256(path)
    if actual_hash != PE_SHA256:
        raise AssertionError(
            f"frozen PE SHA-256 mismatch: {actual_hash} != {PE_SHA256}"
        )
    image = PeImage(path)
    for site in SITES:
        actual = image.bytes_at(site.push_rva, len(site.pe_bytes))
        if actual != site.pe_bytes:
            raise AssertionError(
                f"PE allocation bytes drifted at 0x{site.push_rva:08x}: "
                f"{actual.hex()} != {site.pe_bytes.hex()}"
            )
        call_rva = site.owner_return_rva - 5
        if direct_call_target(image, call_rva) != OPERATOR_NEW_RVA:
            raise AssertionError(
                f"owner 0x{site.owner_return_rva:08x} no longer calls "
                f"operator new 0x{OPERATOR_NEW_RVA:08x}"
            )

    wrapper_windows = {
        0x005EB09C: bytes.fromhex("55 8b ec 5d e9 53 fc ff ff"),
        0x005EACF8: bytes.fromhex(
            "55 8b ec eb 0d ff 75 08 e8 89 14 00 00 59 85 c0 "
            "74 0f ff 75 08 e8 6a 14 00 00"
        ),
        0x005EC17C: bytes.fromhex("ff 25 04 65 a0 00"),
    }
    for rva, expected in wrapper_windows.items():
        actual = image.bytes_at(rva, len(expected))
        if actual != expected:
            raise AssertionError(
                f"operator-new/malloc wrapper drifted at 0x{rva:08x}"
            )

    # Global binary census: only the six pinned push-immediate sites may call
    # this operator-new target within the following 16 bytes.
    matches: list[tuple[int, int, int]] = []
    literals = {420000, 540000}
    for section_rva, data in image.executable_ranges():
        for offset in range(max(0, len(data) - 5)):
            if data[offset] != 0x68:
                continue
            size = struct.unpack_from("<I", data, offset + 1)[0]
            if size not in literals:
                continue
            push_rva = section_rva + offset
            for delta in range(5, 17):
                if offset + delta + 5 > len(data) or data[offset + delta] != 0xE8:
                    continue
                call_rva = push_rva + delta
                displacement = struct.unpack_from("<i", data, offset + delta + 1)[0]
                target = (call_rva + 5 + displacement) & 0xFFFFFFFF
                if target == OPERATOR_NEW_RVA:
                    matches.append((push_rva, call_rva + 5, size))
    expected_matches = [
        (site.push_rva, site.owner_return_rva, site.size) for site in SITES
    ]
    if matches != expected_matches:
        raise AssertionError(
            f"global PE literal/operator-new census drifted: {matches!r}"
        )


def _canonical_json_bytes(value: object) -> bytes:
    return json.dumps(
        value, sort_keys=True, separators=(",", ":"), ensure_ascii=False,
        allow_nan=False,
    ).encode("utf-8")


def generation_recipe_id(
    directory: Path,
    pe_path: Path,
    *,
    expected_pe_sha256: str = PE_SHA256,
    expected_guest_base: int = GUEST_IMAGE_BASE,
) -> str:
    if directory.is_symlink() or not directory.is_dir():
        raise AssertionError(f"generation output root is not a directory: {directory}")
    directory = directory.resolve()
    manifest_path = directory / "manifest.json"
    if manifest_path.is_symlink():
        raise AssertionError("generation manifest must not be a symlink")
    try:
        candidate = BC.load_manifest(manifest_path)
        required_keys = {
            "schema", "stage", "recipe_id", "recipe", "outputs",
            "output_set_id",
        }
        if set(candidate) != required_keys:
            raise AssertionError("generation manifest fields differ from schema 1")
        recipe_id = candidate.get("recipe_id")
        output_set_id = candidate.get("output_set_id")
        if (
            re.fullmatch(r"[0-9a-f]{64}", str(recipe_id)) is None
            or re.fullmatch(r"[0-9a-f]{64}", str(output_set_id)) is None
        ):
            raise AssertionError("generation manifest identities are not lowercase SHA-256")
        outputs = candidate.get("outputs")
        if not isinstance(outputs, list):
            raise AssertionError("generation manifest outputs are not a list")
        for index, entry in enumerate(outputs):
            if (
                not isinstance(entry, dict)
                or set(entry) != {"path", "size", "sha256"}
                or re.fullmatch(r"[0-9a-f]{64}", str(entry.get("sha256")))
                is None
            ):
                raise AssertionError(
                    f"generation manifest output {index} is malformed"
                )

        output_paths: dict[str, Path] = {}
        for path in sorted(directory.rglob("*")):
            if path == manifest_path:
                continue
            if path.is_symlink():
                raise AssertionError(f"symlink in generation output set: {path}")
            if path.is_dir():
                continue
            if not path.is_file():
                raise AssertionError(f"non-file generation output: {path}")
            relative = path.relative_to(directory).as_posix()
            pure = PurePosixPath(relative)
            if (
                not relative
                or "\\" in relative
                or pure.is_absolute()
                or any(part in ("", ".", "..") for part in pure.parts)
                or pure.as_posix() != relative
                or relative in output_paths
            ):
                raise AssertionError(
                    f"unsafe current generation output path: {relative!r}"
                )
            output_paths[relative] = path

        manifest = BC.require_manifest(
            manifest_path, "generate", str(recipe_id), output_paths
        )
    except BC.BuildContractError as exc:
        raise AssertionError(f"generation manifest contract failed: {exc}") from exc

    recipe = manifest["recipe"]
    inputs = recipe.get("inputs") if isinstance(recipe, dict) else None
    params = recipe.get("params") if isinstance(recipe, dict) else None
    if not isinstance(inputs, dict) or not isinstance(params, dict):
        raise AssertionError("generation manifest recipe shape changed")
    actual_pe_sha256 = sha256(pe_path)
    if actual_pe_sha256 != expected_pe_sha256:
        raise AssertionError(
            f"frozen PE SHA-256 mismatch: {actual_pe_sha256} != "
            f"{expected_pe_sha256}"
        )
    if inputs.get(PE_RECIPE_INPUT) != actual_pe_sha256:
        raise AssertionError("generation manifest records a different frozen PE")
    recorded_base = params.get("image_base")
    if isinstance(recorded_base, bool) or recorded_base != expected_guest_base:
        raise AssertionError(
            "generation manifest has the wrong guest image base: "
            f"{recorded_base!r} != 0x{expected_guest_base:08x}"
        )
    return recipe_id


def verify_generation_manifest_validator() -> None:
    with tempfile.TemporaryDirectory(
        prefix="isaac-anm2-generation-contract."
    ) as temporary:
        fixture_root = Path(temporary)
        pe_path = fixture_root / "input.bin"
        pe_path.write_bytes(b"generation-contract-pe\x00")
        pe_digest = sha256(pe_path)
        case_index = 0

        def fresh_case() -> tuple[Path, dict[str, object]]:
            nonlocal case_index
            case_index += 1
            directory = fixture_root / f"generated-{case_index}"
            directory.mkdir()
            for name, payload in (
                ("guest_0000.c", b"zero\n"),
                ("guest_table.c", b"table\n"),
            ):
                (directory / name).write_bytes(payload)
            recipe = {
                "stage": "generate",
                "inputs": {PE_RECIPE_INPUT: pe_digest},
                "params": {"image_base": GUEST_IMAGE_BASE},
                "tools": {"python": "fixture"},
            }
            recipe_id = hashlib.sha256(_canonical_json_bytes(recipe)).hexdigest()
            outputs = BC.snapshot(
                directory, ("guest_0000.c", "guest_table.c")
            )
            manifest = BC.make_manifest(
                "generate", recipe_id, recipe, outputs
            )
            (directory / "manifest.json").write_bytes(
                _canonical_json_bytes(manifest) + b"\n"
            )
            return directory, manifest

        def write_manifest(directory: Path, manifest: dict[str, object]) -> None:
            (directory / "manifest.json").write_bytes(
                _canonical_json_bytes(manifest) + b"\n"
            )

        def reject(
            label: str, expected: str, directory: Path,
        ) -> None:
            try:
                generation_recipe_id(
                    directory, pe_path, expected_pe_sha256=pe_digest
                )
            except AssertionError as exc:
                if expected not in str(exc):
                    raise AssertionError(
                        f"{label}: wrong rejection: {exc}"
                    ) from exc
            else:
                raise AssertionError(f"{label}: hostile manifest was accepted")

        valid_dir, valid_manifest = fresh_case()
        if generation_recipe_id(
            valid_dir, pe_path, expected_pe_sha256=pe_digest
        ) != valid_manifest["recipe_id"]:
            raise AssertionError("valid generation-manifest fixture changed")

        schema_dir, schema_manifest = fresh_case()
        schema_manifest["extra"] = "forbidden"
        write_manifest(schema_dir, schema_manifest)
        reject("schema", "fields differ", schema_dir)

        path_dir, path_manifest = fresh_case()
        path_manifest["outputs"][0]["path"] = "../escape.c"
        write_manifest(path_dir, path_manifest)
        reject("unsafe path", "escapes its root", path_dir)

        extra_dir, unused_manifest = fresh_case()
        (extra_dir / "unlisted.c").write_bytes(b"extra\n")
        reject("extra output", "output set differs", extra_dir)

        missing_dir, unused_manifest = fresh_case()
        (missing_dir / "guest_table.c").unlink()
        reject("missing output", "output set differs", missing_dir)

        hash_dir, unused_manifest = fresh_case()
        (hash_dir / "guest_0000.c").write_bytes(b"mutated\n")
        reject("output hash", "output content differs", hash_dir)

        set_dir, set_manifest = fresh_case()
        set_manifest["output_set_id"] = "0" * 64
        write_manifest(set_dir, set_manifest)
        reject("output set identity", "output_set_id", set_dir)

        sorted_dir, sorted_manifest = fresh_case()
        sorted_manifest["outputs"].reverse()
        write_manifest(sorted_dir, sorted_manifest)
        reject("output ordering", "outputs must be sorted", sorted_dir)

        recipe_dir, recipe_manifest = fresh_case()
        recipe_manifest["recipe"]["tools"]["python"] = "mutated"
        write_manifest(recipe_dir, recipe_manifest)
        reject("recipe identity", "embedded recipe hashes", recipe_dir)

        pe_dir, pe_manifest = fresh_case()
        pe_manifest["recipe"]["inputs"][PE_RECIPE_INPUT] = "9" * 64
        pe_manifest["recipe_id"] = hashlib.sha256(
            _canonical_json_bytes(pe_manifest["recipe"])
        ).hexdigest()
        write_manifest(pe_dir, pe_manifest)
        reject("PE identity", "different frozen PE", pe_dir)

        base_dir, base_manifest = fresh_case()
        base_manifest["recipe"]["params"]["image_base"] = 0x30000000
        base_manifest["recipe_id"] = hashlib.sha256(
            _canonical_json_bytes(base_manifest["recipe"])
        ).hexdigest()
        write_manifest(base_dir, base_manifest)
        reject("guest base", "wrong guest image base", base_dir)


def verify_map(
    path: Path, source_root: Path, generated: Path, vitasdk: Path,
    recipe_id: str,
) -> None:
    try:
        canonical = linker_map_contract.canonicalize(
            path.read_bytes(),
            source_root=source_root,
            generated_root=generated,
            vitasdk_root=vitasdk,
            generation_recipe_id=recipe_id,
        )
    except (OSError, linker_map_contract.LinkerMapContractError) as exc:
        raise AssertionError(f"frozen map contract failed: {exc}") from exc
    actual_hash = canonical.record["sha256"]
    if actual_hash != CANONICAL_MAP_SHA256:
        raise AssertionError(
            "frozen canonical map SHA-256 mismatch: "
            f"{actual_hash} != {CANONICAL_MAP_SHA256}"
        )
    if canonical.record["end"] != f"0x{LINK_END:08x}":
        raise AssertionError(
            f"frozen map _end drifted: {canonical.record['end']!r}"
        )


def verify_generated(directory: Path) -> None:
    owner = directory / "guest_0001.c"
    wrapper_0 = directory / "guest_0183.c"
    wrapper_1 = directory / "guest_0184.c"
    if sha256(owner) != OWNER_SHA256:
        raise AssertionError("guest_0001.c SHA-256 drifted")
    if sha256(wrapper_0) != WRAPPER_0_SHA256:
        raise AssertionError("guest_0183.c SHA-256 drifted")
    if sha256(wrapper_1) != WRAPPER_1_SHA256:
        raise AssertionError("guest_0184.c SHA-256 drifted")

    all_text = "\n".join(
        path.read_text(encoding="utf-8")
        for path in sorted(directory.glob("guest_*.c"))
    )
    literal_counts = {
        420000: all_text.count(
            "gpush_generated(c, 0x668a0U); GUEST_STACK_CALLSITE_BARRIER();"
        ),
        540000: all_text.count(
            "gpush_generated(c, 0x83d60U); GUEST_STACK_CALLSITE_BARRIER();"
        ),
    }
    if literal_counts != {420000: 4, 540000: 2}:
        raise AssertionError(f"generated literal census drifted: {literal_counts}")

    owner_text = owner.read_text(encoding="utf-8")
    for site in SITES:
        size_hex = "668a0" if site.size == 420000 else "83d60"
        push_marker = f"/* {site.push_rva:08x}  push 0x{size_hex} */"
        call_marker = (
            f"gpush_generated(c, 0x{site.owner_return_rva:x}U); "
            "GUEST_STACK_CALLSITE_BARRIER(); sub_005eb09c(c);"
        )
        start = owner_text.find(push_marker)
        if start < 0:
            raise AssertionError(f"generated push marker absent: {push_marker}")
        end = owner_text.find(call_marker, start, start + 700)
        if end < 0:
            raise AssertionError(
                f"generated owner/callee edge absent for "
                f"0x{site.owner_return_rva:08x}"
            )

    wrapper_0_text = wrapper_0.read_text(encoding="utf-8")
    wrapper_1_text = wrapper_1.read_text(encoding="utf-8")
    wrapper_0_markers = (
        "void sub_005eacf8(CPU *__restrict c)",
        "/* 005eacf8  push ebp */",
        "/* 005ead0a  push dword ptr [ebp + 8] */",
        "gpush_generated(c, 0x5ead12U); GUEST_STACK_CALLSITE_BARRIER(); "
        "sub_005ec17c(c);",
    )
    wrapper_1_markers = (
        "void sub_005eb09c(CPU *__restrict c)",
        "/* 005eb09c  push ebp */",
        "/* 005eacf8  push ebp */",
        "/* 005ead0a  push dword ptr [ebp + 8] */",
        "gpush_generated(c, 0x5ead12U); GUEST_STACK_CALLSITE_BARRIER(); "
        "sub_005ec17c(c);",
        "void sub_005ec17c(CPU *__restrict c)",
    )
    for filename, text, markers in (
        ("guest_0183.c", wrapper_0_text, wrapper_0_markers),
        ("guest_0184.c", wrapper_1_text, wrapper_1_markers),
    ):
        for marker in markers:
            if marker not in text:
                raise AssertionError(
                    f"generated malloc-stack marker absent in {filename}: {marker}"
                )

    ownership = {
        "sub_005eacf8": all_text.count("void sub_005eacf8(CPU *__restrict c)"),
        "sub_005eb09c": all_text.count("void sub_005eb09c(CPU *__restrict c)"),
        "sub_005ec17c": all_text.count("void sub_005ec17c(CPU *__restrict c)"),
    }
    if ownership != {
        "sub_005eacf8": 1,
        "sub_005eb09c": 1,
        "sub_005ec17c": 1,
    }:
        raise AssertionError(f"generated wrapper ownership drifted: {ownership!r}")


def header_macro(text: str, name: str) -> int:
    matches = re.findall(
        rf"^#define\s+{re.escape(name)}\s+(0x[0-9a-fA-F]+|[0-9]+)U?\s*$",
        text,
        re.MULTILINE,
    )
    if len(matches) != 1:
        raise AssertionError(
            f"header macro absent or duplicated: {name}: {len(matches)}"
        )
    return int(matches[0], 0)


def header_string_macro(text: str, name: str) -> str:
    matches = re.findall(
        rf'^#define[ \t]+{re.escape(name)}[ \t]*'
        rf'(?:\\[ \t]*\r?\n[ \t]*)?"([^"\r\n]+)"[ \t]*$',
        text,
        re.MULTILINE,
    )
    if len(matches) != 1:
        raise AssertionError(
            f"header string macro absent or duplicated: {name}: {len(matches)}"
        )
    return matches[0]


def verify_header_string_parser() -> None:
    fixture = '#define EXPECTED \\\n    "exact"\n/* exact */\n'
    if header_string_macro(fixture, "EXPECTED") != "exact":
        raise AssertionError("header string macro parser changed")
    for hostile in (
        fixture.replace("EXPECTED", "RENAMED"),
        '/* #define EXPECTED "exact" */\n',
        fixture + fixture,
    ):
        try:
            header_string_macro(hostile, "EXPECTED")
        except AssertionError:
            pass
        else:
            raise AssertionError("header string macro parser accepted a false pin")


def verify_header(path: Path) -> None:
    text = path.read_text(encoding="utf-8")
    expected = {
        "ISAAC_VITA_ANM2_CANONICAL_MAP_SCHEMA": CANONICAL_MAP_SCHEMA,
        "ISAAC_VITA_ANM2_MALLOC_IMPORT_RETURN_RVA": MALLOC_IMPORT_RETURN_RVA,
        "ISAAC_VITA_ANM2_OWNER_STACK_OFFSET": 12,
        "ISAAC_VITA_ANM2_OPERATOR_NEW_RVA": OPERATOR_NEW_RVA,
        "ISAAC_VITA_ANM2_SMALL_SEGMENT_BYTES": 420000,
        "ISAAC_VITA_ANM2_LARGE_SEGMENT_BYTES": 540000,
        "ISAAC_VITA_ANM2_PAYLOAD_BYTES": 2760000,
        "ISAAC_VITA_ANM2_MEMBLOCK_BYTES": 2760704,
        "ISAAC_VITA_ANM2_VITA3K_ALIGNMENT": 8192,
        "ISAAC_VITA_ANM2_VITA3K_RETAINED_MAX": 2768896,
        "ISAAC_VITA_ANM2_LINK_END": 0x82AD1560,
        "ISAAC_VITA_ANM2_HEAP_BYTES": 0x05100000,
        "ISAAC_VITA_ANM2_HEAP_ALIGNMENT_PAD": 0x00100000,
        "ISAAC_VITA_ANM2_HEAP_RETAINED_END": 0x87CD2000,
        "ISAAC_VITA_ANM2_GUEST_IMAGE_BASE": 0x98000000,
        "ISAAC_VITA_ANM2_GUEST_IMAGE_BYTES": 0x0085F000,
        "ISAAC_VITA_ANM2_GUEST_IMAGE_END": 0x9885F000,
        "ISAAC_VITA_ANM2_PRE_IMAGE_GAP_BYTES": 0x1032E000,
        "ISAAC_VITA_ANM2_GAP_REMAINING_BYTES": 0x1008A000,
    }
    expected.update({
        f"ISAAC_VITA_ANM2_OWNER_RETURN_{index}": site.owner_return_rva
        for index, site in enumerate(SITES)
    })
    actual = {name: header_macro(text, name) for name in expected}
    if actual != expected:
        raise AssertionError(f"header constants drifted: {actual!r}")
    expected_strings = {
        "ISAAC_VITA_ANM2_FROZEN_PE_SHA256": PE_SHA256,
        "ISAAC_VITA_ANM2_CANONICAL_MAP_ALGORITHM": CANONICAL_MAP_ALGORITHM,
        "ISAAC_VITA_ANM2_FROZEN_CANONICAL_MAP_SHA256": CANONICAL_MAP_SHA256,
        "ISAAC_VITA_ANM2_GENERATED_OWNER_SHA256": OWNER_SHA256,
        "ISAAC_VITA_ANM2_GENERATED_WRAPPER_0_SHA256": WRAPPER_0_SHA256,
        "ISAAC_VITA_ANM2_GENERATED_WRAPPER_1_SHA256": WRAPPER_1_SHA256,
    }
    actual_strings = {
        name: header_string_macro(text, name) for name in expected_strings
    }
    if actual_strings != expected_strings:
        raise AssertionError(f"header string identities drifted: {actual_strings!r}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", type=Path, required=True)
    parser.add_argument("--generated", type=Path, required=True)
    parser.add_argument("--header", type=Path, required=True)
    parser.add_argument("--map", type=Path)
    parser.add_argument("--source-root", type=Path)
    parser.add_argument("--vitasdk", type=Path)
    arguments = parser.parse_args()

    verify_header_string_parser()
    verify_generation_manifest_validator()
    verify_header(arguments.header)
    verify_pe(arguments.pe)
    recipe_id = generation_recipe_id(arguments.generated, arguments.pe)
    if arguments.map:
        if arguments.source_root is None or arguments.vitasdk is None:
            parser.error("--map requires --source-root and --vitasdk")
        verify_map(
            arguments.map, arguments.source_root,
            arguments.generated, arguments.vitasdk, recipe_id,
        )
    verify_generated(arguments.generated)
    print(
        "ANM2 scratch frozen census: PASS; "
        f"PE={PE_SHA256}; map="
        + (CANONICAL_MAP_SHA256 if arguments.map else "SKIP")
        + "; sites="
        + ",".join(f"{site.owner_return_rva:08x}/{site.size}"
                   for site in SITES)
        + "; malloc-stack=ESP+12"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
