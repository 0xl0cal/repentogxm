#!/usr/bin/env python3
"""Frozen-PE proof for KAGE's canonical 6-index/4-vertex batch stream."""

from __future__ import annotations

import argparse
import hashlib
import json
import struct
from pathlib import Path

import capstone
import pefile
from capstone.x86 import X86_OP_MEM


EXPECTED_SIZE = 8_650_240
EXPECTED_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
EXPECTED_IMAGE_BASE = 0x00400000
EXPECTED_IMAGE_SIZE = 0x0085F000
ALLOCATOR_RVA = 0x0055EE80
PRODUCER_RANGE = (0x0055F990, 0x00560253)
FUNCTION_HASHES = {
    (0x0055EE80, 0x0055EF30):
        "e6b8e03847472f03b127189099820c5643f7705e240c3eaaea23da6fcf15dfb1",
    PRODUCER_RANGE:
        "7a8543c1188288ed850993822d0752038221e57fd929cbc274bb00b878d64d77",
    (0x00560260, 0x005603F0):
        "047af4a3d0b1f8f5bd60acdabc37743e334384d719aaa3a1e2d6750ffe21624a",
    (0x0056D500, 0x0056D8BD):
        "a53e303146f6d725d6b13e0d73ac3247cf7c1da047700e8c1481c25608e247e9",
    (0x005603F0, 0x00560700):
        "6c38516f5e35c8c1513e2456328f4cbd1cf160b0cb86739a71905cdb0672e9b1",
}
CALLSITE_WINDOWS = {
    0x00560397: bytes.fromhex(
        "578b7df051578bcaff5014ff75ec6803140000ff75f86a04ff15ecf9bb008b0d447abc00"
    ),
    0x0056D78E: bytes.fromhex(
        "f88b01578b7de857ff5014ff75fc6803140000ff75f06a04ff15ecf9bb008b0d447abc00"
    ),
}
GL_DRAW_ELEMENTS_IAT_CALL = bytes.fromhex("ff15ecf9bb00")
GL_DRAW_ELEMENTS_DIRECT_CALLS = (
    0x00560397,
    0x00560AA2,
    0x0056548B,
    0x0056D78E,
)
CANONICAL_CALL_RETURNS = {
    0x00560397: 0x0056039D,
    0x0056D78E: 0x0056D794,
}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def decode(pe: pefile.PE, start: int, end: int) -> list[capstone.CsInsn]:
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.detail = True
    result = list(md.disasm(
        pe.get_data(start, end - start),
        pe.OPTIONAL_HEADER.ImageBase + start,
    ))
    require(result and result[-1].address + result[-1].size ==
            pe.OPTIONAL_HEADER.ImageBase + end,
            f"decode did not cover 0x{start:x}..0x{end:x}")
    return result


def instruction_map(
    pe: pefile.PE, start: int, end: int,
) -> dict[int, str]:
    base = pe.OPTIONAL_HEADER.ImageBase
    return {
        ins.address - base: f"{ins.mnemonic} {ins.op_str}".rstrip()
        for ins in decode(pe, start, end)
    }


def expect(mapping: dict[int, str], expected: dict[int, str]) -> None:
    for rva, text in expected.items():
        require(mapping.get(rva) == text,
                f"instruction drift at 0x{rva:x}: {mapping.get(rva)!r} != {text!r}")


def prove_unique_allocator_calls(pe: pefile.PE, payload: bytes) -> None:
    text = next(section for section in pe.sections
                if section.Name.rstrip(b"\0") == b".text")
    data = text.get_data()
    calls: list[int] = []
    for offset in range(len(data) - 4):
        if data[offset] != 0xE8:
            continue
        call_rva = text.VirtualAddress + offset
        displacement = struct.unpack_from("<i", data, offset + 1)[0]
        if call_rva + 5 + displacement == ALLOCATOR_RVA:
            calls.append(call_rva)
    require(calls == [0x0055FD54, 0x0055FD67],
            f"allocator direct-call census drifted: {calls!r}")
    stored_va = struct.pack("<I", pe.OPTIONAL_HEADER.ImageBase + ALLOCATOR_RVA)
    require(payload.count(stored_va) == 0,
            "allocator gained an address-taken/indirect entry")


def prove_draw_call_census(pe: pefile.PE) -> None:
    text = next(section for section in pe.sections
                if section.Name.rstrip(b"\0") == b".text")
    data = text.get_data()
    calls = tuple(
        text.VirtualAddress + offset
        for offset in range(len(data) - len(GL_DRAW_ELEMENTS_IAT_CALL) + 1)
        if data.startswith(GL_DRAW_ELEMENTS_IAT_CALL, offset)
    )
    require(calls == GL_DRAW_ELEMENTS_DIRECT_CALLS,
            f"glDrawElements direct-call census drifted: {calls!r}")


def prove_generated_return_words(directory: Path) -> None:
    paths = sorted(directory.glob("guest_*.c"))
    require(bool(paths), "generated directory has no guest_*.c corpus")
    matches: dict[int, list[tuple[Path, str, int]]] = {
        site: [] for site in CANONICAL_CALL_RETURNS
    }
    for path in paths:
        source = path.read_text(encoding="utf-8")
        for site in CANONICAL_CALL_RETURNS:
            marker = f"/* {site:08x}  call dword ptr "
            start = source.find(marker)
            while start >= 0:
                matches[site].append((path, source, start))
                start = source.find(marker, start + len(marker))

    for site, return_rva in CANONICAL_CALL_RETURNS.items():
        found = matches[site]
        require(len(found) == 1,
                f"generated callsite 0x{site:08x} maps to "
                f"{[str(item[0]) for item in found]!r}")
        path, source, start = found[0]
        end = source.find("    /* ", start + 4)
        require(end >= 0, f"generated call block has no successor: {path}")
        block = source[start:end]
        push = f"gpush_generated(c, 0x{return_rva:x}U);"
        require(block.count(push) == 1 and
                block.count("guest_call(c, _target);") == 1 and
                block.index(push) < block.index("guest_call(c, _target);") and
                "GUEST_IMAGE_BASE" not in block,
                f"generated canonical return convention drifted at "
                f"0x{site:08x} in {path.name}")


def prove_producer(pe: pefile.PE) -> None:
    producer = instruction_map(pe, *PRODUCER_RANGE)
    expect(producer, {
        # q=(vertex.used-vertex.draw_start); the low U16 base is 4*q by
        # induction because the sole allocator request below is four vertices.
        0x0055FD38: "add ecx, ecx",
        0x0055FD49: "mov esi, dword ptr [eax + ecx*8 + 4]",
        0x0055FD4D: "sub esi, dword ptr [eax + ecx*8 + 0xc]",
        0x0055FD41: "push 6",
        0x0055FD51: "lea ecx, [edx + 0x2c]",
        0x0055FD54: "call 0x95ee80",
        0x0055FD5F: "push 4",
        0x0055FD61: "mov ecx, dword ptr [ecx + 0x60]",
        0x0055FD64: "add ecx, 0x20",
        0x0055FD67: "call 0x95ee80",
        0x0055FD7F: "movzx eax, si",
        0x0055FD86: "mov word ptr [edi], ax",
        0x0055FD89: "lea edx, [eax + 2]",
        0x0055FD8C: "lea ecx, [eax + 1]",
        0x0055FD8F: "mov word ptr [edi + 2], dx",
        0x0055FD93: "mov word ptr [edi + 8], dx",
        0x0055FD97: "add eax, 3",
        0x0055FD9C: "mov word ptr [edi + 4], cx",
        0x0055FDA0: "mov word ptr [edi + 6], cx",
        0x0055FDA4: "mov word ptr [edi + 0xa], ax",
    })
    word_stores = [
        ins.address - pe.OPTIONAL_HEADER.ImageBase
        for ins in decode(pe, *PRODUCER_RANGE)
        if ins.mnemonic == "mov" and ins.operands and
        ins.operands[0].type == X86_OP_MEM and ins.operands[0].size == 2
    ]
    require(word_stores == [
        0x0055FD86, 0x0055FD8F, 0x0055FD93,
        0x0055FD9C, 0x0055FDA0, 0x0055FDA4,
    ], f"producer U16-store census drifted: {word_stores!r}")

    allocator = instruction_map(pe, 0x0055EE80, 0x0055EF30)
    expect(allocator, {
        # Active 16-byte descriptor; return base+stride*used, then used += n.
        0x0055EE9D: "mov esi, dword ptr [ebx + 4]",
        0x0055EEA3: "shl esi, 4",
        0x0055EEA6: "add esi, dword ptr [ebx]",
        0x0055EF17: "mov ecx, dword ptr [esi + 4]",
        0x0055EF1A: "mov eax, dword ptr [ebx + 8]",
        0x0055EF1D: "imul eax, ecx",
        0x0055EF20: "add eax, dword ptr [esi]",
        0x0055EF22: "add ecx, edi",
        0x0055EF25: "mov dword ptr [esi + 4], ecx",
    })
    factory = instruction_map(pe, 0x00560670, 0x005606EC)
    expect(factory, {
        0x00560697: "cmp dword ptr [esi + 0x34], 2",
        0x005606A0: "mov dword ptr [esi + 0x34], 2",
    })

    # Exhaust the exact recurrence that follows from sole +6/+4 allocations.
    for quad in range(0xC000 // 6):
        base = quad * 4
        require((base, base + 2, base + 1, base + 1, base + 2, base + 3) ==
                tuple(base + x for x in (0, 2, 1, 1, 2, 3)),
                "canonical induction failed")
    require((0xC000 // 6) * 4 == 32768,
            "maximum canonical top index drifted")


def prove_consumers(pe: pefile.PE) -> None:
    image = instruction_map(pe, 0x00560260, 0x005603F0)
    expect(image, {
        # indices=index.base+stride*draw_start; count=used-draw_start.
        0x00560324: "mov edi, dword ptr [esi + 0x30]",
        0x00560327: "mov eax, dword ptr [esi + 0x34]",
        0x0056032D: "shl edi, 4",
        0x00560332: "add edi, dword ptr [esi + 0x2c]",
        0x00560338: "imul eax, dword ptr [edi + 0xc]",
        0x0056033C: "add eax, dword ptr [edi]",
        0x00560364: "mov edx, dword ptr [edi + 4]",
        0x00560367: "sub edx, dword ptr [edi + 0xc]",
        0x0056038A: "push dword ptr [ebp - 0x14]",
        0x0056038D: "push 0x1403",
        0x00560392: "push dword ptr [ebp - 8]",
        0x00560395: "push 4",
        0x00560397: "call dword ptr [0xbbf9ec]",
        # Both vertex and index draw_start advance to used after the draw.
        0x005603BB: "mov eax, dword ptr [ecx + 4]",
        0x005603BE: "mov dword ptr [ecx + 0xc], eax",
        0x005603CA: "mov eax, dword ptr [ecx + 4]",
        0x005603CD: "mov dword ptr [ecx + 0xc], eax",
    })
    queued = instruction_map(pe, 0x0056D500, 0x0056D8BD)
    expect(queued, {
        0x0056D700: "mov ecx, dword ptr [edx + 0x30]",
        0x0056D703: "mov eax, dword ptr [edx + 0x2c]",
        0x0056D706: "shl ecx, 4",
        0x0056D70C: "mov edi, dword ptr [ecx + eax + 0xc]",
        0x0056D710: "mov ecx, dword ptr [edx + 0x34]",
        0x0056D716: "imul ecx, edi",
        0x0056D72B: "add edi, dword ptr [ecx + eax]",
        0x0056D74C: "mov eax, dword ptr [edi + eax + 4]",
        0x0056D750: "sub eax, dword ptr [ebp - 0x10]",
        0x0056D781: "push dword ptr [ebp - 4]",
        0x0056D784: "push 0x1403",
        0x0056D789: "push dword ptr [ebp - 0x10]",
        0x0056D78C: "push 4",
        0x0056D78E: "call dword ptr [0xbbf9ec]",
        0x0056D7B3: "mov eax, dword ptr [ecx + edx*8 + 4]",
        0x0056D7B7: "mov dword ptr [ecx + edx*8 + 0xc], eax",
        0x0056D7C5: "mov eax, dword ptr [ecx + edx*8 + 4]",
        0x0056D7C9: "mov dword ptr [ecx + edx*8 + 0xc], eax",
    })


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", required=True, type=Path)
    parser.add_argument("--generated", type=Path)
    args = parser.parse_args()
    payload = args.pe.read_bytes()
    require(len(payload) == EXPECTED_SIZE, "frozen PE size changed")
    require(hashlib.sha256(payload).hexdigest() == EXPECTED_SHA256,
            "frozen PE SHA-256 changed")
    pe = pefile.PE(data=payload, fast_load=True)
    require(pe.OPTIONAL_HEADER.ImageBase == EXPECTED_IMAGE_BASE,
            "frozen ImageBase changed")
    require(pe.OPTIONAL_HEADER.SizeOfImage == EXPECTED_IMAGE_SIZE,
            "frozen SizeOfImage changed")
    for (start, end), digest in FUNCTION_HASHES.items():
        require(hashlib.sha256(pe.get_data(start, end - start)).hexdigest() == digest,
                f"function bytes drifted: 0x{start:x}..0x{end:x}")
    for call_rva, window in CALLSITE_WINDOWS.items():
        require(pe.get_data(call_rva - 24, len(window)) == window,
                f"glDrawElements callsite window drifted: 0x{call_rva:x}")
    prove_unique_allocator_calls(pe, payload)
    prove_draw_call_census(pe)
    prove_producer(pe)
    prove_consumers(pe)
    if args.generated:
        prove_generated_return_words(args.generated.resolve())

    manifest_path = Path(__file__).with_name("gl_surface_manifest.json")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    command = next(item for item in manifest["commands"]
                   if item["name"] == "glDrawElements")
    require(command["x86_api_entry"] == "stdcall" and
            command["x86_stack_bytes"] == 16 and
            [p["abi_kind"] for p in command["params"]] ==
            ["u32", "i32", "u32", "guest_addr"],
            "typed glDrawElements ABI drifted")
    generated_claim = (
        "canonical generated return words"
        if args.generated else "generated corpus not supplied"
    )
    print(
        "KAGE canonical quad producer/callsite oracle: PASS "
        "(unique +6/+4 producer; exact 0,2,1/1,2,3; two proven consumers "
        f"of four direct draw callsites; {generated_claim})"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
