#!/usr/bin/env python3
"""Frozen-PE/codegen contract for the Ogg Queue error-path cleanup."""

from __future__ import annotations

import argparse
import copy
import hashlib
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
ROOT_RVA = 0x005A2390
SITE_RVA = 0x005A242F
SUCCESS_RVA = 0x005A244C
OPERATOR_NEW_RVA = 0x005EB09C
DECODER_OPEN_RVA = 0x005BD740
FREE_RVA = 0x005EACE5
NODE_DELETE_RVA = 0x005EB08E

PE_WINDOWS = (
    (0x005A2390, 0x005A24EE,
     "f66ccfc60435063b0a2f4872be2120a2eeb38c35b3e802a61b939cafaab00060"),
    (0x005A23D1, 0x005A244C,
     "bfcacb94d39b9cbea7002f424754ffdd6f7f1ecf0182d4150c11c0698ba50b49"),
    (0x005BD740, 0x005BD8A8,
     "4d607a98d669cfba9029bf5501bef7009287f7ef960157447b1ad9359e325560"),
    (0x005A2216, 0x005A2236,
     "5ebfc8226af49470c1b4d61faddde7b0228fb2ca81fd3b8a7cb0739eb54848ae"),
    (0x005EB08E, 0x005EB09C,
     "038799471fdd0aea8ccfaf2d48b19c511fe90517c4bc4e5187608fea4c196969"),
    (0x005EACE5, 0x005EACEA,
     "b8c0ba3e4bb82a91bf9ab9c630d366705406e14b60187f3ccdc9ea253c7c5804"),
    (0x005EBD94, 0x005EBD99,
     "77523963367c4a260c534211fc449dcf24221c8be9ec1db82c6a2c0826564388"),
    (0x005EC176, 0x005EC17C,
     "a425d7c47d76a590f59d6c121446556633bec9f1f67314fcbd61b3448c8a0eaa"),
)

EXPECTED_BLOCK = """#if defined(__vita__)
    /* Queue decoder-open error: release the still-unowned backing before its node. */
    if (c->edi != 0U && ld32((uint32_t)(c->ebp - 4U)) != 0U) {
        uint32_t _ogg_decoder = ld32((uint32_t)c->ebx);
        uint32_t _ogg_backing = ld32((uint32_t)(c->ebx + 4U));
        uint32_t _ogg_bytes = ld32((uint32_t)(c->ebx + 8U));
        if (_ogg_decoder != 0U || _ogg_backing == 0U ||
                _ogg_bytes != 0x0004b000U) {
            guest_fault(c, 0x005a242fU,
                        "Ogg Queue cleanup ownership invariant");
            return;
        }
        gpush_generated(c, _ogg_backing);
        GUEST_STACK_CALLSITE_BARRIER();
        gpush_generated(c, 0x005a242fU);
        GUEST_STACK_CALLSITE_BARRIER();
        sub_005eace5(c);
        if (c->fault) return;
        if (!guest_stack_adjust_generated(c, 4U)) return;
        GUEST_STACK_CALLSITE_BARRIER();
    }
#endif"""


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


class PeImage:
    """Minimal stdlib-only RVA reader for the PE-aware release gate."""

    def __init__(self, path: Path) -> None:
        self.data = path.read_bytes()
        if self.data[:2] != b"MZ":
            raise AssertionError("frozen input has no MZ header")
        pe_offset = struct.unpack_from("<I", self.data, 0x3C)[0]
        if self.data[pe_offset:pe_offset + 4] != b"PE\0\0":
            raise AssertionError("frozen input has no PE signature")
        count = struct.unpack_from("<H", self.data, pe_offset + 6)[0]
        optional_size = struct.unpack_from("<H", self.data, pe_offset + 20)[0]
        section_offset = pe_offset + 24 + optional_size
        self.sections: list[tuple[int, int, int, int]] = []
        for index in range(count):
            offset = section_offset + index * 40
            virtual_size, virtual_address, raw_size, raw_pointer = (
                struct.unpack_from("<IIII", self.data, offset + 8)
            )
            characteristics = struct.unpack_from("<I", self.data, offset + 36)[0]
            self.sections.append(
                (virtual_address, max(virtual_size, raw_size),
                 raw_pointer, characteristics)
            )

    def bytes_at(self, rva: int, size: int) -> bytes:
        for virtual_address, extent, raw_pointer, _ in self.sections:
            if virtual_address <= rva < virtual_address + extent:
                offset = raw_pointer + rva - virtual_address
                result = self.data[offset:offset + size]
                if len(result) == size:
                    return result
                break
        raise AssertionError(f"short/unmapped PE read at RVA 0x{rva:08x}")

    def executable_ranges(self) -> list[tuple[int, bytes]]:
        result = []
        for virtual_address, extent, raw_pointer, characteristics in self.sections:
            if characteristics & 0x20000000:
                available = min(extent, len(self.data) - raw_pointer)
                result.append(
                    (virtual_address,
                     self.data[raw_pointer:raw_pointer + available])
                )
        return result


def direct_call_target(image: PeImage, site: int) -> int:
    machine = image.bytes_at(site, 5)
    if machine[0] != 0xE8:
        raise AssertionError(f"RVA 0x{site:08x} is not CALL rel32")
    displacement = struct.unpack_from("<i", machine, 1)[0]
    return (site + 5 + displacement) & 0xFFFFFFFF


def direct_call_xrefs(image: PeImage, target: int) -> tuple[int, ...]:
    result = []
    for section_rva, data in image.executable_ranges():
        for offset in range(len(data) - 4):
            if data[offset] != 0xE8:
                continue
            displacement = struct.unpack_from("<i", data, offset + 1)[0]
            site = section_rva + offset
            if ((site + 5 + displacement) & 0xFFFFFFFF) == target:
                result.append(site)
    return tuple(result)


def verify_pe(path: Path) -> None:
    actual_hash = sha256(path)
    if actual_hash != PE_SHA256:
        raise AssertionError(
            f"frozen PE SHA-256 mismatch: {actual_hash} != {PE_SHA256}"
        )
    image = PeImage(path)
    for start, end, expected in PE_WINDOWS:
        actual = hashlib.sha256(image.bytes_at(start, end - start)).hexdigest()
        if actual != expected:
            raise AssertionError(
                f"frozen ownership window changed at 0x{start:08x}: {actual}"
            )
    expected_calls = {
        0x005A23F6: OPERATOR_NEW_RVA,
        0x005A240E: DECODER_OPEN_RVA,
        0x005A2427: 0x0055E330,
        0x005A2432: NODE_DELETE_RVA,
        0x005A2224: FREE_RVA,
    }
    actual_calls = {
        site: direct_call_target(image, site) for site in expected_calls
    }
    if actual_calls != expected_calls:
        raise AssertionError(f"Queue ownership call graph changed: {actual_calls!r}")
    if direct_call_xrefs(image, DECODER_OPEN_RVA) != (0x005A2346, 0x005A240E):
        raise AssertionError("decoder-open caller census changed")


def verify_text(text: str) -> None:
    if text.count(EXPECTED_BLOCK) != 1:
        raise AssertionError("exact Vita Ogg cleanup block changed")
    if text.count("sub_005eace5(c);") != 1:
        raise AssertionError("Queue backing free call is absent or duplicated")
    if text.count("gpush_generated(c, _ogg_backing);") != 1:
        raise AssertionError("Queue backing argument push is absent or duplicated")

    error_cleanup = text.index("/* 005a242c  add esp, 0xc */")
    site_label = text.index("L_005a242f:")
    seam = text.index(EXPECTED_BLOCK)
    node_delete = text.index("/* 005a242f  push 0x10 */")
    node_delete_call = text.index(
        "GUEST_STACK_CALLSITE_BARRIER(); sub_005eb08e(c);",
        node_delete,
    )
    success = text.index("L_005a244c:")
    if not (error_cleanup < site_label < seam < node_delete <
            node_delete_call < success):
        raise AssertionError("cleanup no longer precedes only the error node delete")

    # The site's other inbound edge is the pre-allocation EDI==0 branch.  Its
    # local error word is not initialized, so the short-circuit EDI guard is
    # load-bearing and must stay ahead of the EBP-4 read.
    guard = "c->edi != 0U && ld32((uint32_t)(c->ebp - 4U)) != 0U"
    if text.count(guard) != 1:
        raise AssertionError("pre-allocation inbound guard changed")
    if text.index("if (((((c->eax) & (c->eax))) == (0U))) goto L_005a244c;") > seam:
        raise AssertionError("success branch no longer bypasses cleanup")


def verify_generated_contract(directory: Path) -> None:
    """Bind an external corpus to this generator and every manifest output."""
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
    recorded_generator = inputs.get(GENERATOR_RECIPE_INPUT)
    if recorded_generator != current_generator:
        raise AssertionError(
            "generated corpus is stale for current gen_all.py: "
            f"{recorded_generator} != {current_generator}"
        )
    if inputs.get(PE_RECIPE_INPUT) != PE_SHA256:
        raise AssertionError("generated corpus records a different frozen PE")
    if params.get("image_base") != VITA_IMAGE_BASE:
        raise AssertionError(
            "generated corpus is not for Vita image base 0x98000000"
        )


def verify_generated(directory: Path) -> None:
    verify_generated_contract(directory)
    owners = []
    free_owners = []
    marker = "/* sub_005a2390  RVA 005a2390"
    free_marker = "void sub_005eace5(CPU *__restrict c)"
    for path in sorted(directory.glob("guest_*.c")):
        text = path.read_text(encoding="utf-8", errors="strict")
        if marker in text:
            owners.append((path, text))
        if free_marker in text:
            free_owners.append(path)
    if len(owners) != 1:
        raise AssertionError(
            f"generated Queue owner count changed: {[str(item[0]) for item in owners]}"
        )
    if len(free_owners) != 1:
        raise AssertionError(
            f"generated free-wrapper owner count changed: {list(map(str, free_owners))}"
        )
    declarations = (directory / "guest_funcs.h").read_text(
        encoding="utf-8", errors="strict"
    )
    if declarations.count(free_marker + ";") != 1:
        raise AssertionError("generated free-wrapper declaration changed")
    path, text = owners[0]
    start = text.index(marker)
    end = text.find("\n/* sub_", start + len(marker))
    owner = text[start:] if end < 0 else text[start:end]
    verify_text(owner)
    if "void sub_005a2390(CPU *__restrict c)" not in owner:
        raise AssertionError(f"Queue definition absent from {path}")


def exercise_generator(pe: Path) -> None:
    import gen_all as G  # noqa: E402
    from image import Image  # noqa: E402

    def render(image: Image) -> str:
        result = G._translate_function(
            image,
            {"rva": ROOT_RVA},
            None,
            {},
            pin_img=image,
        )
        if result.get("stub") is not None:
            raise AssertionError(f"Queue unexpectedly became a stub: {result!r}")
        if result.get("insns") != G.VITA_OGG_QUEUE_ERROR_CLEANUP_INSNS:
            raise AssertionError("Queue instruction census drifted")
        if result.get("vita_ogg_queue_error_cleanup") is not True:
            raise AssertionError("Queue cleanup seam was not selected")
        return result["text"]

    G.EXE = str(pe)
    default_image = Image(str(pe), 0x30000000)
    vita_image = Image(str(pe), 0x98000000)
    default_text = render(default_image)
    vita_text = render(vita_image)
    verify_text(default_text)
    verify_text(vita_text)

    mutated = copy.copy(vita_image)
    memory = bytearray(vita_image.mem)
    # Keep the instruction valid while changing 0x4b000 -> 0x4b001.  This
    # exercises the ownership validator rather than a decoder failure.
    memory[0x005A23EB] ^= 0x01
    mutated.mem = bytes(memory)
    try:
        render(mutated)
    except RuntimeError as exc:
        if "Ogg Queue allocation/error/backing/node/free ownership changed" \
                not in str(exc):
            raise
    else:
        raise AssertionError("mutated Queue allocation was accepted")

    default_start = default_text.index(EXPECTED_BLOCK)
    vita_start = vita_text.index(EXPECTED_BLOCK)
    if (default_text[default_start:default_start + len(EXPECTED_BLOCK)] !=
            vita_text[vita_start:vita_start + len(EXPECTED_BLOCK)]):
        raise AssertionError("cleanup block differs by guest image base")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", type=Path, required=True)
    parser.add_argument("--generated", type=Path)
    parser.add_argument("--exercise-generator", action="store_true")
    arguments = parser.parse_args()
    pe = arguments.pe.resolve()

    verify_pe(pe)
    if arguments.generated:
        verify_generated(arguments.generated.resolve())
    if arguments.exercise_generator:
        exercise_generator(pe)

    print(
        "Ogg Queue error cleanup: PASS; root=005a2390; "
        "backing=005a23fb/0x4b000; error-site=005a242f; "
        "free=005eace5; node-delete=005a2432; generated="
        + ("PASS" if arguments.generated else "SKIP")
        + "; generator="
        + ("2 bases/mutation closed" if arguments.exercise_generator else "SKIP")
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
