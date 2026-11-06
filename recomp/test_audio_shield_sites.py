#!/usr/bin/env python3
"""Frozen-PE proof for the starvation shield and the music drop receipt.

Pins every guest fact the two knobs rely on: the single AL_SOURCE_STATE query
of StreamSource::IsPlaying and its return marker, the MusicManager per-slot
update (active = IsPlaying || paused; drop = inactive && stream has a source
&& keep byte clear -> "music stopped playing" + sub_0056e9a0), the slot loop
frames the receipt walks for the slot index, Stop()'s list-erase-before-stop
order, the finished predicate Manager::Update stops streams with, and the
runtime/CMake constants that mirror them.
"""

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


def header_constant(text: str, name: str) -> int:
    match = re.search(
        rf"^#define\s+{name}\s+(0x[0-9a-fA-F]+|[0-9]+)U?$", text, re.MULTILINE
    )
    require(match is not None, f"runtime constant {name} is missing")
    return int(match.group(1), 0)


def verify_runtime_contract() -> None:
    refill = (HERE / "runtime" / "host_vita_audio_refill.h").read_text(
        encoding="utf-8"
    )
    require(header_constant(refill, "ISAAC_VITA_AUDIO_STREAM_IS_PLAYING_RETURN")
            == 0x005BE808, "IsPlaying return marker drifted")
    require(header_constant(refill, "ISAAC_VITA_AUDIO_AL_STOPPED") == 0x1014 and
            header_constant(refill, "ISAAC_VITA_AUDIO_AL_PLAYING") == 0x1012 and
            header_constant(refill, "ISAAC_VITA_AUDIO_AL_SOURCE_STATE") ==
            0x1010, "AL state constants drifted")
    for marker in (
        "ISAAC_VITA_AUDIO_REFILL_STOP_TRACKING",
        "isaac_vita_audio_shield_decide",
        "isaac_vita_audio_shield_note_queue",
        "isaac_vita_audio_shield_note_play",
        "isaac_vita_audio_refill_last_refill_age_ms",
    ):
        require(marker in refill, f"refill header lost {marker}")

    audio = (HERE / "runtime" / "host_vita_audio.h").read_text(encoding="utf-8")
    expected = {
        "ISAAC_VITA_AUDIO_MUSIC_SLOT_UPDATE_RVA": 0x003A18F0,
        "ISAAC_VITA_AUDIO_MUSIC_SLOT_RESET_RVA": 0x0056E9A0,
        "ISAAC_VITA_AUDIO_MUSIC_DROP_RETURN": 0x003A1A71,
        "ISAAC_VITA_AUDIO_MUSIC_SLOT0_RETURN": 0x0039FD3E,
        "ISAAC_VITA_AUDIO_MUSIC_SLOT_LOOP_RETURN": 0x0039FD4F,
        "ISAAC_VITA_AUDIO_MUSIC_SLOT7_RETURN": 0x003A1B56,
        "ISAAC_VITA_AUDIO_MUSIC_SLOT_STRIDE": 0x34,
        "ISAAC_VITA_AUDIO_MUSIC_SLOT_LOOP_COUNT": 7,
        "ISAAC_VITA_AUDIO_MUSIC_SLOT7_OFFSET": 0x32C,
        "ISAAC_VITA_AUDIO_OGG_VTABLE_RVA": 0x76654C,
        "ISAAC_VITA_AUDIO_OGG_STOPPED_OFFSET": 0x08,
        "ISAAC_VITA_AUDIO_OGG_LOOP_OFFSET": 0x09,
        "ISAAC_VITA_AUDIO_OGG_SOURCE_OFFSET": 0x30,
        "ISAAC_VITA_AUDIO_OGG_EOF_OFFSET": 0x8C,
        "ISAAC_VITA_AUDIO_MANAGER_ACTIVE_BEGIN_OFFSET": 0x0C,
        "ISAAC_VITA_AUDIO_MANAGER_ACTIVE_END_OFFSET": 0x10,
    }
    for name, value in expected.items():
        require(header_constant(audio, name) == value,
                f"runtime constant {name} drifted")

    adapter = (HERE / "runtime" / "host_vita_audio.c").read_text(
        encoding="utf-8"
    )
    for marker in (
        "ISAAC_VITA_AUDIO_STREAM_IS_PLAYING_RETURN",
        "vita_audio_live_stream_object",
        "isaac_vita_audio_shield_decide(",
        "void __wrap_sub_0056e9a0(CPU *__restrict c)",
        "void __real_sub_0056e9a0(CPU *__restrict c);",
        "ISAAC_VITA_AUDIO_MUSIC_DROP_RETURN",
        "KAGE VITA AUDIO MUSIC DROP: slot=%u stream=0x%08x src=%u ",
    ):
        require(marker in adapter, f"adapter lost marker: {marker}")

    cmake = (HERE / "vita" / "CMakeLists.txt").read_text(encoding="utf-8")
    for option in ("ISAAC_VITA_AUDIO_STARVATION_SHIELD",
                   "ISAAC_VITA_AUDIO_MUSIC_DROP_RECEIPT"):
        require(f"option({option}" in cmake, f"CMake lost option {option}")
        require(cmake.count(f"{option}=1") == 1,
                f"{option} compile definition is absent or duplicated")
        require(f"{option} requires the translated KAGE runtime" in cmake,
                f"{option} lost its KAGE/audio dependency rule")
        require(f"{option} requires the stream refill layer" in cmake,
                f"{option} lost its refill-layer dependency rule")
    require(cmake.count("-Wl,--wrap=sub_0056e9a0") == 1,
            "slot reset --wrap seam is absent or duplicated")


def verify_frozen_semantics(pe: pefile.PE) -> None:
    vtable = {
        offset: int.from_bytes(pe.get_data(0x0076654C + offset, 4), "little")
        for offset in (0x04, 0x28, 0x38, 0x3C, 0x7C, 0x80)
    }
    require(vtable == {
        0x04: IMAGE_BASE + 0x005BE080,
        0x28: IMAGE_BASE + 0x003FA860,
        0x38: IMAGE_BASE + 0x005BE7E0,
        0x3C: IMAGE_BASE + 0x005BF0A0,
        0x7C: IMAGE_BASE + 0x005BF100,
        0x80: IMAGE_BASE + 0x005BF230,
    }, "StreamSourceOgg vtable contract changed")

    # StreamSource::IsPlaying: the one AL_SOURCE_STATE query in the PE, its
    # return marker 0x5be808, alGetError right after, result = (state ==
    # AL_PLAYING).
    is_playing = decode_map(pe, 0x005BE7E0, 0x005BE833)
    expect(is_playing, {
        0x005BE7E4: "mov eax, dword ptr [ecx + 0x30]",
        0x005BE7FC: "push 0x1010",
        0x005BE802: "call dword ptr [0xa0630c]",
        0x005BE808: "add esp, 0xc",
        0x005BE80B: "call dword ptr [0xa0634c]",
        0x005BE825: "cmp dword ptr [ebp - 4], 0x1012",
        0x005BE82C: "sete al",
    })

    # The +0x28 predicate the drop tests is the "owns an AL source" byte,
    # set by the create path right after alGenSources succeeded.
    getter = decode_map(pe, 0x003FA860, 0x003FA864)
    expect(getter, {
        0x003FA860: "mov al, byte ptr [ecx + 8]",
        0x003FA863: "ret",
    })
    created = decode_map(pe, 0x005BE1C4, 0x005BE1CC)
    expect(created, {
        0x005BE1C4: "mov byte ptr [esi + 8], 1",
        0x005BE1C8: "mov al, 1",
    })

    # MusicManager per-slot update: aligned prologue (caller's ebp at [ebp],
    # return address copied to [ebp+4]), active = IsPlaying() || paused(+9)
    # after ten updates, drop = inactive && +0x28 && keep(+0xa) == 0 ->
    # "music stopped playing" + sub_0056e9a0 returning to 0x3a1a71.
    music = decode_map(pe, 0x003A18F0, 0x003A1A80)
    expect(music, {
        0x003A18F0: "push ebx",
        0x003A18F1: "mov ebx, esp",
        0x003A18FC: "push ebp",
        0x003A18FD: "mov ebp, dword ptr [ebx + 4]",
        0x003A1900: "mov dword ptr [esp + 4], ebp",
        0x003A1904: "mov ebp, esp",
        0x003A192D: "cmp dword ptr [esi + 0x2c], 0xa",
        0x003A1940: "cmp byte ptr [esi + 9], 0",
        0x003A1948: "mov eax, dword ptr [eax + 0x38]",
        0x003A194B: "call eax",
        0x003A1959: "mov eax, dword ptr [eax + 0x38]",
        0x003A195C: "call eax",
        0x003A1A47: "mov eax, dword ptr [eax + 0x28]",
        0x003A1A4A: "call eax",
        0x003A1A55: "cmp byte ptr [esi + 0xa], 0",
        0x003A1A5B: "push 0xb4d618",
        0x003A1A62: "call 0x95e330",
        0x003A1A6A: "mov ecx, esi",
        0x003A1A6C: "call 0x96e9a0",
        0x003A1A71: "pop esi",
    })
    require(pe.get_data(0x0074D618, 22).startswith(b"music stopped playing"),
            "MusicManager stop record string moved")

    # MusicManager::Update: this in edi and at [ebp-4]; slot 0 at this+0
    # (return 0x39fd3e), slots 1..6 at this+0x34*i (return 0x39fd4f).
    update = decode_map(pe, 0x0039FBF0, 0x0039FD5A)
    expect(update, {
        0x0039FBFC: "push ebp",
        0x0039FBFD: "mov ebp, dword ptr [ebx + 4]",
        0x0039FC04: "mov ebp, esp",
        0x0039FC0B: "mov edi, ecx",
        0x0039FC0D: "mov dword ptr [ebp - 4], edi",
        0x0039FD39: "call 0x7a18f0",
        0x0039FD3E: "lea esi, [edi + 0x34]",
        0x0039FD41: "mov edi, 6",
        0x0039FD48: "mov ecx, esi",
        0x0039FD4A: "call 0x7a18f0",
        0x0039FD4F: "add esi, 0x34",
        0x0039FD52: "sub edi, 1",
        0x0039FD55: "jne 0x79fd46",
    })
    # The eighth slot at this+0x32c (return 0x3a1b56).
    eighth = decode_map(pe, 0x003A1B41, 0x003A1B56)
    expect(eighth, {
        0x003A1B41: "lea ecx, [esi + 0x32c]",
        0x003A1B4F: "push 1",
        0x003A1B51: "call 0x7a18f0",
    })

    # The slot reset: releases the shared_ptr, clears +8/+0xa, volume 1.0.
    reset = decode_map(pe, 0x0056E9A0, 0x0056E9CB)
    expect(reset, {
        0x0056E9A3: "mov esi, ecx",
        0x0056E9A5: "call 0x96ecf0",
        0x0056E9AA: "mov word ptr [esi + 8], 0",
        0x0056E9B0: "mov byte ptr [esi + 0xa], 0",
        0x0056E9CA: "ret",
    })

    # Stop(): erase from the manager's active list (sub_0056f850 reads
    # manager+0xc / +0x10) BEFORE IsPlaying and alSourceStop, so a stopped
    # stream is never found live by the shield.
    stop = decode_map(pe, 0x005BF0A0, 0x005BF0F1)
    expect(stop, {
        0x005BF0A4: "call 0x96f850",
        0x005BF0AD: "mov eax, dword ptr [eax + 0x38]",
        0x005BF0B0: "call eax",
        0x005BF0CC: "call dword ptr [0xa06310]",
    })
    erase = decode_map(pe, 0x0056F850, 0x0056F89A)
    expect(erase, {
        0x0056F864: "mov eax, dword ptr [0xbe7d4c]",
        0x0056F869: "mov esi, dword ptr [0xbe7d50]",
        0x0056F87B: "cmp dword ptr [eax], edi",
    })

    # Manager::Update stops a stream only when its +0x80 predicate is true:
    # eof (+0x8c) set and all four queue slots drained.  A merely starved
    # looping stream is therefore never stopped by the manager; only the
    # MusicManager's IsPlaying decision drops it.
    finished = decode_map(pe, 0x005BF230, 0x005BF254)
    expect(finished, {
        0x005BF230: "cmp byte ptr [ecx + 0x8c], 0",
        0x005BF240: "cmp dword ptr [eax], 0",
        0x005BF24E: "mov al, 1",
        0x005BF251: "xor al, al",
    })
    manager = decode_map(pe, 0x0056F040, 0x0056F180)
    expect(manager, {
        0x0056F0C4: "mov eax, dword ptr [eax + 0x7c]",
        0x0056F0C7: "call eax",
        0x0056F117: "call dword ptr [eax + 0x3c]",
    })

    # QueueData: alSourceQueueBuffers (return 0x5be126), IsPlaying, and
    # alSourcePlay only when IsPlaying is false: the game's own restart path
    # the shield replays for a stopped source with fresh PCM.
    queue = decode_map(pe, 0x005BE080, 0x005BE160)
    expect(queue, {
        0x005BE120: "call dword ptr [0xa06318]",
        0x005BE132: "mov eax, dword ptr [eax + 0x38]",
        0x005BE135: "call eax",
        0x005BE137: "test al, al",
        0x005BE139: "jne 0x9be147",
        0x005BE13E: "call dword ptr [0xa06308]",
    })


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--pe", required=True, type=Path)
    args = parser.parse_args()

    data = args.pe.read_bytes()
    require(len(data) == PE_SIZE, "unexpected PE size")
    require(hashlib.sha256(data).hexdigest() == PE_SHA256,
            "unexpected PE hash")
    pe = pefile.PE(data=data, fast_load=True)

    verify_runtime_contract()
    verify_frozen_semantics(pe)
    print("audio starvation shield / music drop sites: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
