#!/usr/bin/env python3
"""Frozen-PE proof for the OGG starvation/restart diagnostic boundary."""

from __future__ import annotations

import argparse
import hashlib
import re
from pathlib import Path

import capstone
import pefile


HERE = Path(__file__).resolve().parent
PE_SIZE = 8_650_240
PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
IMAGE_BASE = 0x00400000


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def decode_map(pe: pefile.PE, start: int, end: int) -> dict[int, str]:
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    instructions = list(decoder.disasm(
        pe.get_data(start, end - start), IMAGE_BASE + start
    ))
    require(instructions, f"empty decode at {start:08x}")
    require(instructions[-1].address + instructions[-1].size ==
            IMAGE_BASE + end,
            f"decode did not cover {start:08x}..{end:08x}")
    return {
        instruction.address - IMAGE_BASE:
            f"{instruction.mnemonic} {instruction.op_str}".rstrip()
        for instruction in instructions
    }


def expect(mapping: dict[int, str], expected: dict[int, str]) -> None:
    for rva, instruction in expected.items():
        require(mapping.get(rva) == instruction,
                f"instruction drift at {rva:08x}: "
                f"{mapping.get(rva)!r} != {instruction!r}")


def verify_runtime_contract() -> None:
    header = (HERE / "runtime" / "host_vita_audio.h").read_text(
        encoding="utf-8"
    )
    constants = {
        "ISAAC_VITA_AUDIO_MANAGER_ACTIVE_BEGIN_OFFSET": 0x0C,
        "ISAAC_VITA_AUDIO_MANAGER_ACTIVE_END_OFFSET": 0x10,
        "ISAAC_VITA_AUDIO_OGG_VTABLE_RVA": 0x76654C,
        "ISAAC_VITA_AUDIO_OGG_STOPPED_OFFSET": 0x08,
        "ISAAC_VITA_AUDIO_OGG_LOOP_OFFSET": 0x09,
        "ISAAC_VITA_AUDIO_OGG_SOURCE_OFFSET": 0x30,
        "ISAAC_VITA_AUDIO_OGG_QUEUE_OFFSET": 0x58,
        "ISAAC_VITA_AUDIO_OGG_QUEUE_STRIDE": 0x0C,
        "ISAAC_VITA_AUDIO_OGG_QUEUE_COUNT": 4,
        "ISAAC_VITA_AUDIO_OGG_EOF_OFFSET": 0x8C,
    }
    for name, expected in constants.items():
        match = re.search(
            rf"^#define\s+{name}\s+(0x[0-9a-fA-F]+|[0-9]+)U$",
            header,
            re.MULTILINE,
        )
        require(match is not None and int(match.group(1), 0) == expected,
                f"runtime constant {name} drifted")

    source = (HERE / "runtime" / "host_vita_audio_cooperative.c").read_text(
        encoding="utf-8"
    )
    for marker in (
        "ISAAC_VITA_AUDIO_STREAM_RECEIPT",
        "audio_stream_receipt_snapshot_load(manager, &receipt_before)",
        "audio_stream_receipt_snapshot_load(manager, &receipt_after)",
        'route, "guest-fault"',
        'route, "esp-fault"',
        'route, "jump-fault"',
        'route, "return"',
        "KAGE VITA AUDIO STREAM RECEIPT:",
    ):
        require(marker in source, f"runtime receipt lost marker: {marker}")

    cmake = (HERE / "vita" / "CMakeLists.txt").read_text(encoding="utf-8")
    require("option(ISAAC_VITA_AUDIO_STREAM_RECEIPT" in cmake,
            "CMake lost the opt-in stream receipt")
    require(
        "ISAAC_VITA_SCAFFOLD_ONLY OR NOT ISAAC_VITA_KAGE OR" in cmake,
        "CMake stream receipt no longer rejects scaffold-only builds",
    )
    require(cmake.count("ISAAC_VITA_AUDIO_STREAM_RECEIPT=1") == 1,
            "stream receipt compile definition is absent or duplicated")
    build_tool = (HERE.parent / "tools" / "build_vita.py").read_text(
        encoding="utf-8"
    )
    for marker in (
        '"--audio-stream-receipt"',
        'f"-DISAAC_VITA_AUDIO_STREAM_RECEIPT={on_off(audio_stream_receipt)}"',
        "audio_stream_receipt=args.audio_stream_receipt",
    ):
        require(marker in build_tool, f"build wrapper lost marker: {marker}")


def verify_frozen_semantics(pe: pefile.PE) -> None:
    vtable = {
        offset: int.from_bytes(pe.get_data(0x0076654C + offset, 4), "little")
        for offset in (0x04, 0x28, 0x38, 0x3C, 0x74, 0x7C, 0x80, 0x84)
    }
    require(vtable == {
        0x04: IMAGE_BASE + 0x005BE080,
        0x28: IMAGE_BASE + 0x003FA860,
        0x38: IMAGE_BASE + 0x005BE7E0,
        0x3C: IMAGE_BASE + 0x005BF0A0,
        0x74: IMAGE_BASE + 0x005A20E0,
        0x7C: IMAGE_BASE + 0x005BF100,
        0x80: IMAGE_BASE + 0x005BF230,
        0x84: IMAGE_BASE + 0x005BF260,
    }, "StreamSourceOgg vtable contract changed")

    # Sound::Manager::Update calls every active source's +0x7c Update.  A true
    # return is collected and later sent to +0x3c Stop/removal.
    manager = decode_map(pe, 0x0056F040, 0x0056F180)
    expect(manager, {
        0x0056F0C4: "mov eax, dword ptr [eax + 0x7c]",
        0x0056F0C7: "call eax",
        0x0056F0C9: "test al, al",
        0x0056F0CB: "je 0x96f0ed",
        0x0056F117: "call dword ptr [eax + 0x3c]",
    })

    # StreamSourceOgg::Update unqueues processed PCM, decodes only state-zero
    # slots, leaves a negative decode as state zero/non-EOF, turns ordinary
    # non-looping zero into EOF, and queues state two through virtual +4.
    update = decode_map(pe, 0x005BF100, 0x005BF230)
    expect(update, {
        0x005BF150: "call dword ptr [0xa06320]",
        0x005BF1C7: "call dword ptr [eax + 0x74]",
        0x005BF1CA: "test eax, eax",
        0x005BF1CC: "jg 0x9bf1e6",
        0x005BF1CE: "mov dword ptr [edi + 0x58], 0",
        0x005BF1D5: "jne 0x9bf1f0",
        0x005BF1D7: "cmp byte ptr [esi + 9], 0",
        0x005BF1DD: "mov byte ptr [esi + 0x8c], 1",
        0x005BF204: "call dword ptr [eax + 4]",
        0x005BF207: "mov dword ptr [ebx], 3",
    })

    # The +4 queue method is already the recovery policy: after BufferData and
    # QueueBuffers it calls +0x38 IsPlaying; false falls through to SourcePlay.
    queue = decode_map(pe, 0x005BE080, 0x005BE160)
    expect(queue, {
        0x005BE111: "call dword ptr [0xa06364]",
        0x005BE120: "call dword ptr [0xa06318]",
        0x005BE135: "call eax",
        0x005BE137: "test al, al",
        0x005BE139: "jne 0x9be147",
        0x005BE13E: "call dword ptr [0xa06308]",
    })

    # Decoder absence and the stream error predicate return -1.  The Update
    # branch above deliberately does not convert that result to EOF, allowing
    # a later cooperative Update to retry rather than destroying the actor.
    decode = decode_map(pe, 0x005A20E0, 0x005A21D8)
    expect(decode, {
        0x005A20FA: "cmp dword ptr [edi + 0x94], 0",
        0x005A210B: "or eax, 0xffffffff",
        0x005A2147: "call 0x9bdf80",
        0x005A2152: "test eax, eax",
        0x005A2154: "jg 0x9a21b9",
        0x005A2163: "call eax",
        0x005A2165: "test al, al",
        0x005A2169: "mov dword ptr [ebp + 8], 0xffffffff",
    })

    # The game's "music stopped" record comes from the source object's +0x28
    # flag query, not from an OpenAL error return.  That distinction is why the
    # host receipt observes both queue state and pump outcome without forcing
    # alSourcePlay itself.
    music = decode_map(pe, 0x003A18F0, 0x003A1A80)
    expect(music, {
        0x003A1A47: "mov eax, dword ptr [eax + 0x28]",
        0x003A1A4A: "call eax",
        0x003A1A4C: "test al, al",
        0x003A1A4E: "je 0x7a1a71",
        0x003A1A5B: "push 0xb4d618",
        0x003A1A6C: "call 0x96e9a0",
    })
    require(pe.get_data(0x0074D618, 22).startswith(b"music stopped playing"),
            "MusicManager stop record string moved")
    getter = decode_map(pe, 0x003FA860, 0x003FA870)
    expect(getter, {
        0x003FA860: "mov al, byte ptr [ecx + 8]",
        0x003FA863: "ret",
    })


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", required=True, type=Path)
    arguments = parser.parse_args()
    payload = arguments.pe.read_bytes()
    require(len(payload) == PE_SIZE, "frozen PE size changed")
    require(hashlib.sha256(payload).hexdigest() == PE_SHA256,
            "frozen PE SHA-256 changed")
    pe = pefile.PE(data=payload, fast_load=True)
    require(pe.OPTIONAL_HEADER.ImageBase == IMAGE_BASE,
            "frozen PE image base changed")
    verify_runtime_contract()
    verify_frozen_semantics(pe)
    print(
        "Vita OGG starvation contract: PASS; "
        "Update=retry; queue=restart-if-stopped; receipt=read-only/fault-closed"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
