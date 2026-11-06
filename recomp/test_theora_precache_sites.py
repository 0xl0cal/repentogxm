#!/usr/bin/env python3
"""Frozen-PE proof for ISAAC_VITA_THEORA_PRECACHE_FRAMES.

Pins every guest fact host_vita_theora_precache.c relies on: the prologue of
TheoraFrameQueue::setSize (sub_005a4c60) and its read of the count argument
at [ebx+8] (the first stack argument of a frame with an aligned ESP), the
parent clip at [queue+8] handed to each new frame, the per-frame RGBA buffer
allocation w*h*bpp in sub_005bf690, the load-path call that pushes the clip's
mNumPrecachedFrames from [clip+0x44], the restart call that pushes the queue's
current size from [queue+4], and the runtime/CMake text that mirrors them.

usage: python test_theora_precache_sites.py --pe <isaac-ng.exe.unpacked.exe>
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
from pathlib import Path

import capstone
import pefile


HERE = Path(__file__).resolve().parent
PE_SIZE = 8_650_240
PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
IMAGE_BASE = 0x00400000

SET_SIZE = 0x005A4C60          # TheoraFrameQueue::setSize(int n)
FRAME_CTOR = 0x005BF690        # TheoraVideoFrame ctor: buffer = new byte[w*h*bpp]
LOAD_CALL = 0x005A9F24         # push [esi+0x44]; mov [esi+0x1c], edi; call setSize
RESTART_CALL = 0x005950C9      # push [ecx+4]; call setSize


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def decode(pe: pefile.PE, start: int, length: int) -> list:
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    instructions = list(decoder.disasm(pe.get_data(start, length),
                                       IMAGE_BASE + start))
    require(bool(instructions), f"empty decode at {start:08x}")
    return instructions


def text(instruction) -> str:
    return f"{instruction.mnemonic} {instruction.op_str}".strip()


def check_pe(path: Path) -> None:
    data = path.read_bytes()
    require(len(data) == PE_SIZE, f"PE size {len(data)} != {PE_SIZE}")
    require(hashlib.sha256(data).hexdigest() == PE_SHA256, "PE SHA-256 drifted")
    pe = pefile.PE(data=data, fast_load=True)

    # setSize: frame with ESP aligned to 8 (push ebx; mov ebx, esp; ...), the
    # count argument read once from [ebx+8], the parent clip from [edi+8].
    body = {i.address: text(i) for i in decode(pe, SET_SIZE, 0x100)}
    require(body[IMAGE_BASE + SET_SIZE] == "push ebx", "setSize prologue")
    require(body[IMAGE_BASE + SET_SIZE + 1] == "mov ebx, esp", "setSize frame base")
    require(body[IMAGE_BASE + 0x5A4CA6] == "mov eax, dword ptr [ebx + 8]",
            "setSize count argument is not [ebx+8]")
    require(body[IMAGE_BASE + 0x5A4CA1] == "mov edi, ecx", "setSize this")
    require(body[IMAGE_BASE + 0x5A4D20] == "mov esi, dword ptr [edi + 8]",
            "queue parent is not [this+8]")
    require(body[IMAGE_BASE + 0x5A4D3A] == f"call 0x{IMAGE_BASE + FRAME_CTOR:x}",
            "frame constructor call moved")
    require(body[IMAGE_BASE + 0x5A4D23] == "push 0x20", "frame object size")

    # frame ctor: edi = [ecx+0x68] * [ecx+0x64] * bpp; buffer = alloc(edi); memset
    ctor = {i.address: text(i) for i in decode(pe, FRAME_CTOR, 0xE0)}
    require(ctor[IMAGE_BASE + 0x5BF744] == "mov edi, dword ptr [ecx + 0x68]", "frame width")
    require(ctor[IMAGE_BASE + 0x5BF747] == "imul edi, dword ptr [ecx + 0x64]", "frame height")
    require(ctor[IMAGE_BASE + 0x5BF755] == "imul edi, eax", "frame bpp")
    require(ctor[IMAGE_BASE + 0x5BF758] == "push edi", "buffer size argument")
    require(ctor[IMAGE_BASE + 0x5BF759].startswith("call "), "buffer allocation call")
    require(ctor[IMAGE_BASE + 0x5BF761] == "mov dword ptr [esi + 0x18], eax",
            "buffer pointer field")

    # load path: the clip's mNumPrecachedFrames is the argument
    load = {i.address: text(i) for i in decode(pe, LOAD_CALL, 0x0B)}
    require(load[IMAGE_BASE + LOAD_CALL] == "push dword ptr [esi + 0x44]",
            "load path does not push [clip+0x44]")
    require(load[IMAGE_BASE + LOAD_CALL + 3] == "mov dword ptr [esi + 0x1c], edi",
            "load path does not store the queue at [clip+0x1c]")
    require(load[IMAGE_BASE + LOAD_CALL + 6] == f"call 0x{IMAGE_BASE + SET_SIZE:x}",
            "load path call target")

    # restart path: the queue's current size is the argument
    restart = {i.address: text(i) for i in decode(pe, RESTART_CALL, 0x08)}
    require(restart[IMAGE_BASE + RESTART_CALL] == "push dword ptr [ecx + 4]",
            "restart path does not push [queue+4]")
    require(restart[IMAGE_BASE + RESTART_CALL + 3] == f"call 0x{IMAGE_BASE + SET_SIZE:x}",
            "restart path call target")

    # Exactly these two direct call sites exist in .text.
    text_section = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
    code = pe.get_data(text_section.VirtualAddress, text_section.Misc_VirtualSize)
    sites = []
    for match in re.finditer(rb"\xe8", code):
        offset = match.start()
        rel = int.from_bytes(code[offset + 1:offset + 5], "little", signed=True)
        if text_section.VirtualAddress + offset + 5 + rel == SET_SIZE:
            sites.append(text_section.VirtualAddress + offset)
    require(sites == [RESTART_CALL + 3, LOAD_CALL + 6],
            f"setSize direct call sites drifted: {[hex(s) for s in sites]}")


def check_sources() -> None:
    runtime = (HERE / "runtime" / "host_vita_theora_precache.c").read_text(encoding="utf-8")
    require("__wrap_sub_005a4c60" in runtime and "__real_sub_005a4c60" in runtime,
            "runtime wrap symbols")
    require("VITA_THEORA_QUEUE_PARENT_OFFSET      0x8U" in runtime, "parent offset pin")
    require("VITA_THEORA_CLIP_PRECACHED_OFFSET    0x44U" in runtime, "precached offset pin")
    require("#error" in runtime, "runtime file must refuse a knob-less compile")
    cmake = (HERE / "vita" / "CMakeLists.txt").read_text(encoding="utf-8")
    require("set(ISAAC_VITA_THEORA_PRECACHE_FRAMES 0 CACHE STRING" in cmake,
            "knob default is not 0")
    require("-Wl,--wrap=sub_005a4c60" in cmake, "wrap link option")
    require("host_vita_theora_precache.c" in cmake, "runtime source wiring")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", type=Path,
                        default=Path(os.environ.get("ISAAC_AUDIO_TEST_PE", "")))
    arguments = parser.parse_args()
    check_sources()
    if not arguments.pe or not arguments.pe.is_file():
        print("theora precache sites: sources OK, no --pe given (PE pins not run)")
        return 0
    check_pe(arguments.pe)
    print("theora precache sites: PASS (setSize, frame ctor, 2 call sites, sources)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
