#!/usr/bin/env python3
"""Frozen-PE proof for ISAAC_VITA_NATIVE_THEORA.

Pins every guest fact host_vita_native_theora.c relies on: the thirteen
libtheora public entry points theoraplayer uses (their RVAs and the shape that
identifies each: th_decode_alloc's 0xdf60-byte context, th_decode_ctl's
seven-way request switch, th_decode_free's four frees, th_info_init's version
3.2.1 and keyframe shift 6, th_info_clear's 0x40-byte memset, the 16-byte
th_comment_init, th_decode_headerin's TH_EBADHEADER path, th_setup_free,
th_version_string's "Xiph.Org libtheora 1.2.0alpha"), the exact list of
direct call sites (all inside TheoraVideoClip_Theora, 0x5a9b60..0x5ac040,
plus libtheora's own th_comment_clear call inside oc_dec_headerin), the absence
of any address-taken reference to them (so --wrap sees every call), the frame
vtable slot that reaches TheoraVideoFrame::decode (the only data word naming
it), the guest structure sizes the native side must match (th_info 0x40 from
th_info_clear; ogg_packet's granulepos at +0x10 from decodeNextFrame's
argument pushes) and the two return conventions the wrappers reproduce
(th_granule_frame in edx:eax, th_granule_time in ST(0) consumed by fstp).  For
ISAAC_VITA_NATIVE_THEORA_RGB it also pins libyuv's I420ToRGB24Matrix: its two
direct callers (theoraplayer's RGB and BGR conversion entries) and their
argument pushes, the entry's argument checks and negative-height flip, the C
row function's coefficient offsets and integer math, the two constants tables,
and the absence of address-taken references; likewise vorbis_synthesis's two
callers for the timed pass-through.  The runtime/CMake text that mirrors them
is checked without a PE.

usage: python test_native_theora_sites.py --pe <isaac-ng.exe.unpacked.exe>
"""

from __future__ import annotations

import argparse
import hashlib
import os
import re
import struct
from pathlib import Path

import capstone
import pefile


HERE = Path(__file__).resolve().parent
PE_SIZE = 8_650_240
PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
IMAGE_BASE = 0x00400000

TH_DECODE_ALLOC = 0x005DA480
TH_DECODE_CTL = 0x005DA4F0
TH_DECODE_FREE = 0x005DA670
TH_DECODE_PACKETIN = 0x005DA6D0
TH_DECODE_YCBCR_OUT = 0x005DB3C0
TH_COMMENT_CLEAR = 0x005DB3F0
TH_COMMENT_INIT = 0x005DB440
TH_INFO_CLEAR = 0x005DB450
TH_INFO_INIT = 0x005DB470
TH_DECODE_HEADERIN = 0x005DBAC0
TH_SETUP_FREE = 0x005DBB10
TH_GRANULE_FRAME = 0x005DDB60
TH_GRANULE_TIME = 0x005DDC10
TH_VERSION_STRING = 0x005DDE10
OC_DEC_HEADERIN = 0x005DB6C0
FRAME_DECODE = 0x005BF7F0        # TheoraVideoFrame::decode (thiscall, ret 4)
FRAME_VTABLE = 0x00769280
FRAME_CTOR = 0x005BF690
CLIP_DTOR = 0x005A9B60
CLIP_LOAD = 0x005A9DB0
DECODE_NEXT_FRAME = 0x005AA6D0
DO_SEEK = 0x005AADA0
CLIP_RESTART = 0x005AB3B0
CLIP_FRAME_NUMBER = 0x005AB510
READ_HEADERS = 0x005AB8A0
CLIP_VERSION = 0x005AC030
THEORAPLAYER_RANGE = (0x005A9B60, 0x005AC040)
FREE_IAT = 0x006064FC
MEMSET_RVA = 0x005EC152
VERSION_STRING = b"Xiph.Org libtheora 1.2.0alpha"
VERSION_STRING_RVA = 0x00606FC8

WRAPPED = {
    TH_DECODE_ALLOC: "th_decode_alloc",
    TH_DECODE_CTL: "th_decode_ctl",
    TH_DECODE_FREE: "th_decode_free",
    TH_DECODE_PACKETIN: "th_decode_packetin",
    TH_DECODE_YCBCR_OUT: "th_decode_ycbcr_out",
    TH_COMMENT_CLEAR: "th_comment_clear",
    TH_COMMENT_INIT: "th_comment_init",
    TH_INFO_CLEAR: "th_info_clear",
    TH_INFO_INIT: "th_info_init",
    TH_DECODE_HEADERIN: "th_decode_headerin",
    TH_SETUP_FREE: "th_setup_free",
    TH_GRANULE_FRAME: "th_granule_frame",
    TH_GRANULE_TIME: "th_granule_time",
}

# Every direct call site of every wrapped function in the frozen .text.
CALL_SITES = {
    TH_DECODE_ALLOC: [0x005A9DF7, 0x005AAE55, 0x005AB40A],
    TH_DECODE_CTL: [0x005AAF3B, 0x005AAFA5, 0x005AB3ED, 0x005AB4E8],
    TH_DECODE_FREE: [0x005A9BA1, 0x005AAE43, 0x005AB3F8],
    TH_DECODE_PACKETIN: [0x005AA76D, 0x005AAFBF],
    TH_DECODE_YCBCR_OUT: [0x005AA882],
    TH_COMMENT_CLEAR: [0x005A9CB9, 0x005DB850],   # the second is libtheora's own
    TH_COMMENT_INIT: [0x005AB986],
    TH_INFO_CLEAR: [0x005A9CC5, 0x005DB894],      # the second is libtheora's own
    TH_INFO_INIT: [0x005AB992],
    TH_DECODE_HEADERIN: [0x005ABC16, 0x005ABDAB],
    TH_SETUP_FREE: [0x005A9BAC],
    TH_GRANULE_FRAME: [0x005AA102, 0x005AA79E, 0x005AAFE0, 0x005AB6A0],
    TH_GRANULE_TIME: [0x005AA78A],
}
# libtheora's own calls, inside oc_dec_headerin's error path; the translated
# oc_dec_headerin is never reached once th_decode_headerin is native.
INTERNAL_SITES = {0x005DB850: OC_DEC_HEADERIN, 0x005DB894: OC_DEC_HEADERIN}

# ISAAC_VITA_NATIVE_THEORA_RGB (host_vita_native_yuv.c): libyuv's
# I420ToRGB24Matrix, its C row function, its two callers (theoraplayer's RGB
# and BGR conversion-table entries) and the two constants tables they pass.
I420_TO_RGB24 = 0x005CE3F0
RGB24_ROW_C = 0x005D5270
CONVERT_RGB = 0x005C2AD0
CONVERT_BGR = 0x005C2C30
I420_SITES = [0x005C2AFD, 0x005C2C5D]
YVU_TABLE = 0x0073D260          # kYvuI601Constants: RGB entry, planes swapped
YUV_TABLE = 0x0073D340          # kYuvI601Constants: BGR entry, planes straight
CPU_FLAGS = 0x007FEC04          # libyuv cpu_info_ (0x40 = kCpuHasSSSE3)
CPU_INIT = 0x005D6410
# (UB, UG, VG, VR, BB, BG, BR, YG) at the offsets the C row function reads.
TABLE_VALUES = {
    YVU_TABLE: (-102, 52, 25, -128, -14216, 8696, -17544, 18997),
    YUV_TABLE: (-128, 25, 52, -102, -17544, 8696, -14216, 18997),
}
# The clip's audio decode, timed by a pass-through in the theora seam.
VORBIS_SYNTHESIS = 0x005C2910
VORBIS_SITES = [0x005AABFB, 0x005AB466]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def decode(pe: pefile.PE, start: int, length: int) -> dict[int, str]:
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    instructions = list(decoder.disasm(pe.get_data(start, length),
                                       IMAGE_BASE + start))
    require(bool(instructions), f"empty decode at {start:08x}")
    return {i.address - IMAGE_BASE: f"{i.mnemonic} {i.op_str}".strip()
            for i in instructions}


def expect(body: dict[int, str], rva: int, text: str) -> None:
    require(body.get(rva) == text,
            f"{rva:08x}: expected `{text}`, found `{body.get(rva)}`")


def va(rva: int) -> str:
    return f"0x{IMAGE_BASE + rva:x}"


def direct_call_sites(code: bytes, text_rva: int, target: int) -> list[int]:
    sites = []
    for match in re.finditer(rb"\xe8", code):
        offset = match.start()
        rel = int.from_bytes(code[offset + 1:offset + 5], "little", signed=True)
        if text_rva + offset + 5 + rel == target:
            sites.append(text_rva + offset)
    return sites


def word_references(pe: pefile.PE, target: int) -> list[tuple[str, int]]:
    """Every 4-byte word in any section equal to the target's VA."""
    needle = struct.pack("<I", IMAGE_BASE + target)
    found = []
    for section in pe.sections:
        name = section.Name.rstrip(b"\0").decode()
        data = bytes(pe.get_data(section.VirtualAddress, section.Misc_VirtualSize))
        index = data.find(needle)
        while index != -1:
            found.append((name, section.VirtualAddress + index))
            index = data.find(needle, index + 1)
    return found


def check_pe(path: Path) -> None:
    data = path.read_bytes()
    require(len(data) == PE_SIZE, f"PE size {len(data)} != {PE_SIZE}")
    require(hashlib.sha256(data).hexdigest() == PE_SHA256, "PE SHA-256 drifted")
    pe = pefile.PE(data=data, fast_load=True)
    text_section = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
    code = bytes(pe.get_data(text_section.VirtualAddress, text_section.Misc_VirtualSize))
    text_rva = text_section.VirtualAddress

    # --- the thirteen entry points, by shape -------------------------------
    alloc = decode(pe, TH_DECODE_ALLOC, 0x63)
    expect(alloc, 0x5DA483, "cmp dword ptr [ebp + 8], 0")     # info NULL check
    expect(alloc, 0x5DA489, "cmp dword ptr [ebp + 0xc], 0")   # setup NULL check
    expect(alloc, 0x5DA490, "push 0x10")
    expect(alloc, 0x5DA492, "push 0xdf60")                    # sizeof(th_dec_ctx) 1.2.0alpha
    expect(alloc, 0x5DA4B8, "mov dword ptr [esi + 0x380], 0")
    expect(alloc, 0x5DA4D0, "ret")

    ctl = decode(pe, TH_DECODE_CTL, 0x20)
    expect(ctl, 0x5DA4F4, "mov eax, dword ptr [ebp + 0xc]")   # req
    expect(ctl, 0x5DA4F7, "dec eax")
    expect(ctl, 0x5DA4F9, "cmp eax, 6")                        # TH_DECCTL 1..7
    expect(ctl, 0x5DA502, f"jmp dword ptr [eax*4 + {va(0x5DA650)}]")

    free = decode(pe, TH_DECODE_FREE, 0x51)
    expect(free, 0x5DA674, "mov edi, dword ptr [ebp + 8]")
    expect(free, 0x5DA679, f"je {va(0x5DA6BE)}")               # NULL-tolerant
    expect(free, 0x5DA67C, "push dword ptr [edi + 0xd350]")
    expect(free, 0x5DA682, f"mov esi, dword ptr [{va(FREE_IAT)}]")
    expect(free, 0x5DA6C0, "ret")

    packetin = decode(pe, TH_DECODE_PACKETIN, 0x10)
    require(TH_DECODE_PACKETIN in packetin, "th_decode_packetin decodes")
    ycbcr = decode(pe, TH_DECODE_YCBCR_OUT, 0x29)
    expect(ycbcr, 0x5DB3E8, "ret")

    comment_clear = decode(pe, TH_COMMENT_CLEAR, 0x4c)
    expect(comment_clear, 0x5DB3F4, "mov edi, dword ptr [ebp + 8]")
    expect(comment_clear, 0x5DB3F9, f"je {va(0x5DB439)}")      # NULL-tolerant
    expect(comment_clear, 0x5DB3FC, f"mov ebx, dword ptr [{va(FREE_IAT)}]")
    expect(comment_clear, 0x5DB405, "cmp dword ptr [edi + 8], esi")   # comments
    expect(comment_clear, 0x5DB420, "push dword ptr [edi]")     # user_comments
    expect(comment_clear, 0x5DB424, "push dword ptr [edi + 4]") # comment_lengths
    expect(comment_clear, 0x5DB429, "push dword ptr [edi + 0xc]")  # vendor
    expect(comment_clear, 0x5DB434, "movups xmmword ptr [edi], xmm0")  # 16 bytes

    comment_init = decode(pe, TH_COMMENT_INIT, 0xe)
    expect(comment_init, 0x5DB443, "mov eax, dword ptr [ebp + 8]")
    expect(comment_init, 0x5DB446, "xorps xmm0, xmm0")
    expect(comment_init, 0x5DB449, "movups xmmword ptr [eax], xmm0")  # sizeof(th_comment) == 16
    expect(comment_init, 0x5DB44D, "ret")

    info_clear = decode(pe, TH_INFO_CLEAR, 0x14)
    expect(info_clear, 0x5DB453, "push 0x40")                  # sizeof(th_info) == 0x40
    expect(info_clear, 0x5DB455, "push 0")
    expect(info_clear, 0x5DB457, "push dword ptr [ebp + 8]")
    expect(info_clear, 0x5DB45A, f"call {va(MEMSET_RVA)}")
    expect(info_clear, 0x5DB463, "ret")

    info_init = decode(pe, TH_INFO_INIT, 0x27)
    expect(info_init, 0x5DB477, "push 0x40")
    expect(info_init, 0x5DB47C, f"call {va(MEMSET_RVA)}")
    expect(info_init, 0x5DB484, "mov word ptr [esi], 0x203")   # version 3.2
    expect(info_init, 0x5DB489, "mov byte ptr [esi + 2], 1")   # .1
    expect(info_init, 0x5DB48D, "mov dword ptr [esi + 0x3c], 6")  # keyframe_granule_shift at 0x3c
    expect(info_init, 0x5DB496, "ret")

    headerin = decode(pe, TH_DECODE_HEADERIN, 0x4d)
    expect(headerin, 0x5DBAC7, "mov esi, dword ptr [ebp + 0x14]")  # op
    expect(headerin, 0x5DBACE, "lea eax, [esi - 0x14]")            # TH_EBADHEADER (-20)
    expect(headerin, 0x5DBAD6, "cmp dword ptr [ebp + 8], 0")       # info
    expect(headerin, 0x5DBADC, "or eax, 0xffffffff")               # TH_EFAULT
    expect(headerin, 0x5DBAE4, "push dword ptr [esi + 4]")         # op->bytes
    expect(headerin, 0x5DBAEA, "push dword ptr [esi]")             # op->packet
    expect(headerin, 0x5DBB00, f"call {va(OC_DEC_HEADERIN)}")
    expect(headerin, 0x5DBB0C, "ret")

    setup_free = decode(pe, TH_SETUP_FREE, 0x2a)
    expect(setup_free, 0x5DBB14, "mov esi, dword ptr [ebp + 8]")
    expect(setup_free, 0x5DBB19, f"je {va(0x5DBB37)}")             # NULL-tolerant
    expect(setup_free, 0x5DBB1B, "lea eax, [esi + 0x140]")         # qinfo
    expect(setup_free, 0x5DBB2E, f"call dword ptr [{va(FREE_IAT)}]")
    expect(setup_free, 0x5DBB39, "ret")

    version = decode(pe, TH_VERSION_STRING, 0x6)
    expect(version, TH_VERSION_STRING, f"mov eax, {va(VERSION_STRING_RVA)}")
    require(bytes(pe.get_data(VERSION_STRING_RVA, len(VERSION_STRING))) == VERSION_STRING,
            "libtheora version string")

    granule_frame = decode(pe, TH_GRANULE_FRAME, 0x10)
    require(TH_GRANULE_FRAME in granule_frame, "th_granule_frame decodes")
    granule_time = decode(pe, TH_GRANULE_TIME, 0x10)
    require(TH_GRANULE_TIME in granule_time, "th_granule_time decodes")

    # --- call sites: complete, direct, inside theoraplayer ------------------
    for target, sites in CALL_SITES.items():
        found = direct_call_sites(code, text_rva, target)
        require(sorted(found) == sorted(sites),
                f"{WRAPPED[target]} call sites {sorted(map(hex, found))} != {sorted(map(hex, sites))}")
        for site in sites:
            if site in INTERNAL_SITES:
                continue
            require(THEORAPLAYER_RANGE[0] <= site < THEORAPLAYER_RANGE[1],
                    f"{WRAPPED[target]} caller {site:08x} outside theoraplayer")
        require(not word_references(pe, target),
                f"{WRAPPED[target]} is address-taken; --wrap would miss an indirect call")
    internal = decode(pe, 0x5DB84C, 0x50)
    expect(internal, 0x5DB850, f"call {va(TH_COMMENT_CLEAR)}")  # oc_dec_headerin clears on error
    expect(internal, 0x5DB894, f"call {va(TH_INFO_CLEAR)}")

    # --- the arguments the wrappers read -----------------------------------
    decode_next = decode(pe, 0x5AA75C, 0x54)
    expect(decode_next, 0x5AA75C, "lea eax, [ebp - 0x58]")          # &granpos (int64 out)
    expect(decode_next, 0x5AA760, "lea eax, [ebp - 0xa8]")          # &ogg_packet (local)
    expect(decode_next, 0x5AA767, "push dword ptr [esi + 0x404]")   # dec
    expect(decode_next, 0x5AA76D, f"call {va(TH_DECODE_PACKETIN)}")
    expect(decode_next, 0x5AA772, "add esp, 0xc")                   # cdecl, three words
    expect(decode_next, 0x5AA779, "cmp eax, 1")                     # TH_DUPFRAME accepted
    expect(decode_next, 0x5AA77E, "push dword ptr [ebp - 0x54]")    # granpos hi
    expect(decode_next, 0x5AA781, "push dword ptr [ebp - 0x58]")    # granpos lo
    expect(decode_next, 0x5AA784, "push dword ptr [esi + 0x404]")
    expect(decode_next, 0x5AA78A, f"call {va(TH_GRANULE_TIME)}")
    expect(decode_next, 0x5AA792, "fstp dword ptr [ebp - 0x5c]")    # double result in ST(0)
    expect(decode_next, 0x5AA79E, f"call {va(TH_GRANULE_FRAME)}")
    expect(decode_next, 0x5AA7A6, "add esp, 0x18")                  # cdecl: caller pops
    expect(decode_next, 0x5AA7A9, "mov ebx, eax")                   # int64 low word
    ycbcr_site = decode(pe, 0x5AA87C, 0x20)
    expect(ycbcr_site, 0x5AA87C, "mov dword ptr [esi + 0x524], ebx")
    expect(ycbcr_site, 0x5AA882, f"call {va(TH_DECODE_YCBCR_OUT)}")
    expect(ycbcr_site, 0x5AA896, "add esp, 8")
    ctl_site = decode(pe, 0x5AAF30, 0x10)
    expect(ctl_site, 0x5AAF30, "push 5")                            # TH_DECCTL_SET_GRANPOS
    expect(ctl_site, 0x5AAF32, "push dword ptr [edi + 0x404]")
    expect(ctl_site, 0x5AAF3B, f"call {va(TH_DECODE_CTL)}")

    # --- TheoraVideoFrame::decode: the timed pass-through -------------------
    frame = decode(pe, FRAME_DECODE, 0x60)
    expect(frame, 0x5BF7F9, "mov edi, dword ptr [ebp + 8]")         # TheoraPixelTransform *
    expect(frame, 0x5BF7FC, "mov ebx, ecx")                         # this
    expect(frame, 0x5BF801, "mov esi, dword ptr [edi]")             # t->raw
    expect(frame, 0x5BF843, "mov byte ptr [ebx + 8], 1")            # mReady
    expect(frame, 0x5BF84D, "ret 4")
    vtable = struct.unpack_from("<II", bytes(pe.get_data(FRAME_VTABLE, 8)))
    require(vtable[1] == IMAGE_BASE + FRAME_DECODE, "frame vtable slot +4 is decode")
    refs = word_references(pe, FRAME_DECODE)
    require(refs == [(".rdata", FRAME_VTABLE + 4)],
            f"TheoraVideoFrame::decode references {refs}")
    ctor = decode(pe, FRAME_CTOR, 0x60)
    expect(ctor, 0x5BF6E0, f"mov dword ptr [esi], {va(FRAME_VTABLE)}")
    require(not direct_call_sites(code, text_rva, FRAME_DECODE),
            "TheoraVideoFrame::decode is reached only through its vtable")

    # --- ISAAC_VITA_NATIVE_THEORA_RGB: libyuv I420ToRGB24Matrix -------------
    require(sorted(direct_call_sites(code, text_rva, I420_TO_RGB24)) == I420_SITES,
            "I420ToRGB24Matrix call sites drifted")
    require(not word_references(pe, I420_TO_RGB24),
            "I420ToRGB24Matrix is address-taken; --wrap would miss an indirect call")
    require(sorted(direct_call_sites(code, text_rva, VORBIS_SYNTHESIS)) == VORBIS_SITES,
            "vorbis_synthesis call sites drifted")
    require(not word_references(pe, VORBIS_SYNTHESIS),
            "vorbis_synthesis is address-taken; --wrap would miss an indirect call")
    # The RGB entry: ecx = src_y, edx = src_stride_y, nine stack words pushed
    # last-first (src_u, src_stride_u, src_v, src_stride_v, dst, dst_stride,
    # yuvconstants, width, height), caller-cleaned.  It hands the YVU table and
    # the u/v planes swapped (t+0xc as src_u, t+8 as src_v).
    rgb = decode(pe, CONVERT_RGB, 0x37)
    expect(rgb, 0x5C2AD3, "mov ecx, dword ptr [ebp + 8]")          # TheoraPixelTransform *t
    expect(rgb, 0x5C2AD6, "push dword ptr [ecx + 0x18]")           # height
    expect(rgb, 0x5C2ADC, "push dword ptr [ecx + 0x14]")           # width
    expect(rgb, 0x5C2ADF, "mov edx, dword ptr [ecx + 0x20]")       # src_stride_y
    expect(rgb, 0x5C2AE2, f"push {va(YVU_TABLE)}")                 # yuvconstants
    expect(rgb, 0x5C2AE7, "lea eax, [eax + eax*2]")                # dst_stride = 3 * t->w
    expect(rgb, 0x5C2AEB, "push dword ptr [ecx + 0x10]")           # dst
    expect(rgb, 0x5C2AEE, "push dword ptr [ecx + 0x24]")           # src_stride_v
    expect(rgb, 0x5C2AF1, "push dword ptr [ecx + 8]")              # src_v (t->u)
    expect(rgb, 0x5C2AF4, "push dword ptr [ecx + 0x28]")           # src_stride_u
    expect(rgb, 0x5C2AF7, "push dword ptr [ecx + 0xc]")            # src_u (t->v)
    expect(rgb, 0x5C2AFA, "mov ecx, dword ptr [ecx + 4]")          # src_y
    expect(rgb, 0x5C2AFD, f"call {va(I420_TO_RGB24)}")
    expect(rgb, 0x5C2B02, "add esp, 0x24")                         # nine words, caller pops
    bgr = decode(pe, CONVERT_BGR, 0x37)
    expect(bgr, 0x5C2C42, f"push {va(YUV_TABLE)}")
    expect(bgr, 0x5C2C4E, "push dword ptr [ecx + 0x28]")
    expect(bgr, 0x5C2C51, "push dword ptr [ecx + 0xc]")            # src_v (t->v)
    expect(bgr, 0x5C2C54, "push dword ptr [ecx + 0x24]")
    expect(bgr, 0x5C2C57, "push dword ptr [ecx + 8]")              # src_u (t->u)
    expect(bgr, 0x5C2C5D, f"call {va(I420_TO_RGB24)}")
    expect(bgr, 0x5C2C62, "add esp, 0x24")
    # The entry: NULL planes / NULL dst / width <= 0 / height == 0 answer -1,
    # a negative height flips the destination, the row function is the C one
    # unless cpu_info_ has kCpuHasSSSE3.
    entry = decode(pe, I420_TO_RGB24, 0x8e)
    expect(entry, 0x5CE3F7, "mov eax, ecx")                         # src_y
    expect(entry, 0x5CE3F9, "mov dword ptr [ebp - 0x10], edx")      # src_stride_y
    expect(entry, 0x5CE3FF, f"mov ecx, {va(RGB24_ROW_C)}")          # C row by default
    expect(entry, 0x5CE411, "cmp dword ptr [ebp + 8], 0")           # src_u
    expect(entry, 0x5CE41B, "cmp dword ptr [ebp + 0x10], 0")        # src_v
    expect(entry, 0x5CE425, "mov edi, dword ptr [ebp + 0x18]")      # dst
    expect(entry, 0x5CE430, "mov ebx, dword ptr [ebp + 0x24]")      # width
    expect(entry, 0x5CE435, f"jle {va(0x5CE51B)}")                   # width <= 0
    expect(entry, 0x5CE43B, "mov esi, dword ptr [ebp + 0x28]")      # height
    expect(entry, 0x5CE440, f"je {va(0x5CE51B)}")                    # height == 0
    expect(entry, 0x5CE448, "mov edx, dword ptr [ebp + 0x1c]")      # dst_stride
    expect(entry, 0x5CE44B, "neg esi")                              # height = -height
    expect(entry, 0x5CE44D, "lea eax, [esi - 1]")
    expect(entry, 0x5CE450, "imul eax, edx")                        # (height-1) * dst_stride
    expect(entry, 0x5CE453, "add edi, eax")
    expect(entry, 0x5CE455, "neg edx")                              # dst_stride = -dst_stride
    expect(entry, 0x5CE462, f"mov eax, dword ptr [{va(CPU_FLAGS)}]")
    expect(entry, 0x5CE46B, f"call {va(CPU_INIT)}")
    expect(entry, 0x5CE47A, "test al, 0x40")                        # kCpuHasSSSE3
    # The C row: y1 = (y * YG * 0x0101) >> 16 unsigned; b/g/r = clamp((y1 -
    # u*UB + BB) >> 6) etc. with the coefficients at the offsets the native
    # converter reads (int8 +0x00/+0x20/+0x21/+0x41, int16 +0x60/+0x80/+0xa0/
    # +0xc0), stored b, g, r.
    row = decode(pe, RGB24_ROW_C, 0x110)
    expect(row, 0x5D5273, "mov eax, dword ptr [ebp + 0x1c]")        # width
    expect(row, 0x5D5279, "mov ecx, dword ptr [ebp + 0x10]")        # src_v
    expect(row, 0x5D527E, "mov ebx, dword ptr [ebp + 0xc]")         # src_u
    expect(row, 0x5D5283, "mov edi, dword ptr [ebp + 8]")           # src_y
    expect(row, 0x5D529B, "mov eax, dword ptr [ebp + 0x14]")        # dst
    expect(row, 0x5D52A3, "mov esi, dword ptr [ebp + 0x18]")        # yuvconstants
    expect(row, 0x5D52AF, "movsx ecx, byte ptr [esi + 0x20]")       # UG
    expect(row, 0x5D52B6, "movsx ecx, byte ptr [esi + 0x21]")       # VG
    expect(row, 0x5D52BD, "movsx ecx, byte ptr [esi + 0x41]")       # VR
    expect(row, 0x5D52C4, "movsx ecx, word ptr [esi + 0x80]")       # BG
    expect(row, 0x5D52CE, "movsx ecx, word ptr [esi + 0xa0]")       # BR
    expect(row, 0x5D52D8, "movsx ecx, word ptr [esi + 0xc0]")       # YG
    expect(row, 0x5D52DF, "imul edx, ecx")                          # y * YG
    expect(row, 0x5D52E2, "movsx ecx, byte ptr [esi]")              # UB
    expect(row, 0x5D52E5, "movsx esi, word ptr [esi + 0x60]")       # BB
    expect(row, 0x5D52E9, "imul ecx, ebx")                          # u * UB
    expect(row, 0x5D52F0, "imul edx, edx, 0x101")                   # * 0x0101
    expect(row, 0x5D52FA, "sub esi, ecx")                           # BB - u*UB
    expect(row, 0x5D5301, "shr edx, 0x10")                          # >> 16, unsigned
    expect(row, 0x5D5304, "add esi, edx")                           # + y1
    expect(row, 0x5D5309, "sar esi, 6")                             # >> 6, arithmetic
    expect(row, 0x5D531F, "mov byte ptr [eax], cl")                 # b
    expect(row, 0x5D534D, "mov byte ptr [eax + 1], cl")             # g
    expect(row, 0x5D5370, "mov byte ptr [eax + 2], cl")             # r
    for table, values in TABLE_VALUES.items():
        data = bytes(pe.get_data(table, 0xe0))
        got = (struct.unpack_from("<b", data, 0x00)[0], struct.unpack_from("<b", data, 0x20)[0],
               struct.unpack_from("<b", data, 0x21)[0], struct.unpack_from("<b", data, 0x41)[0],
               struct.unpack_from("<h", data, 0x60)[0], struct.unpack_from("<h", data, 0x80)[0],
               struct.unpack_from("<h", data, 0xa0)[0], struct.unpack_from("<h", data, 0xc0)[0])
        require(got == values, f"yuv constants table {table:#x} holds {got}, expected {values}")


def check_sources() -> None:
    runtime = (HERE / "runtime" / "host_vita_native_theora.c").read_text(encoding="utf-8")
    for rva in list(WRAPPED) + [FRAME_DECODE]:
        require(f"void __wrap_sub_{rva:08x}(CPU *__restrict c)" in runtime,
                f"runtime wraps sub_{rva:08x}")
    require("void __real_sub_005bf7f0(CPU *__restrict c);" in runtime and
            "__real_sub_005bf7f0(c);" in runtime,
            "frame decode is a pass-through")
    for rva in WRAPPED:
        require(f"__real_sub_{rva:08x}" not in runtime,
                f"native seam must not call the translated sub_{rva:08x}")
    pins = {
        "VITA_NT_TH_DECODE_ALLOC_RVA": TH_DECODE_ALLOC,
        "VITA_NT_TH_DECODE_CTL_RVA": TH_DECODE_CTL,
        "VITA_NT_TH_DECODE_FREE_RVA": TH_DECODE_FREE,
        "VITA_NT_TH_DECODE_PACKETIN_RVA": TH_DECODE_PACKETIN,
        "VITA_NT_TH_DECODE_YCBCR_OUT_RVA": TH_DECODE_YCBCR_OUT,
        "VITA_NT_TH_COMMENT_CLEAR_RVA": TH_COMMENT_CLEAR,
        "VITA_NT_TH_COMMENT_INIT_RVA": TH_COMMENT_INIT,
        "VITA_NT_TH_INFO_CLEAR_RVA": TH_INFO_CLEAR,
        "VITA_NT_TH_INFO_INIT_RVA": TH_INFO_INIT,
        "VITA_NT_TH_DECODE_HEADERIN_RVA": TH_DECODE_HEADERIN,
        "VITA_NT_TH_SETUP_FREE_RVA": TH_SETUP_FREE,
        "VITA_NT_TH_GRANULE_FRAME_RVA": TH_GRANULE_FRAME,
        "VITA_NT_TH_GRANULE_TIME_RVA": TH_GRANULE_TIME,
        "VITA_NT_FRAME_DECODE_RVA": FRAME_DECODE,
    }
    for name, value in pins.items():
        match = re.search(rf"#define {name}\s+(0x[0-9a-fA-F]+)U", runtime)
        require(match is not None and int(match.group(1), 16) == value,
                f"runtime pin {name} is {match and match.group(1)}, expected {value:#010x}")
    for text in ("sizeof(th_comment) == 0x10", "sizeof(ogg_packet) == 0x20",
                 "offsetof(ogg_packet, granulepos) == 0x10",
                 "sizeof(th_ycbcr_buffer) == 0x30",
                 "offsetof(th_img_plane, data) == 0xc",
                 "sizeof(void *) == 4", "sizeof(long) == 4",
                 "sizeof(th_info) <= VITA_NT_GUEST_INFO_SIZE"):
        require(text in runtime, f"layout assert `{text}`")
    # th_info is converted field by field: the VitaSDK toolchain uses one-byte
    # enums, so the native structure is 0x3c bytes against the guest's 0x40.
    guest_info = {
        "VITA_NT_GUEST_INFO_FRAME_WIDTH": 0x04, "VITA_NT_GUEST_INFO_PIC_X": 0x14,
        "VITA_NT_GUEST_INFO_FPS_NUMERATOR": 0x1c, "VITA_NT_GUEST_INFO_COLORSPACE": 0x2c,
        "VITA_NT_GUEST_INFO_PIXEL_FMT": 0x30, "VITA_NT_GUEST_INFO_KEYFRAME_SHIFT": 0x3c,
        "VITA_NT_GUEST_INFO_SIZE": 0x40,
    }
    for name, value in guest_info.items():
        match = re.search(rf"#define {name}\s+(0x[0-9a-fA-F]+)U", runtime)
        require(match is not None and int(match.group(1), 16) == value,
                f"guest th_info pin {name} is {match and match.group(1)}, expected {value:#x}")
    require("(th_info *)(uintptr_t)" not in runtime and
            "(const th_info *)(uintptr_t)" not in runtime,
            "a guest th_info pointer is handed to libtheora without conversion")
    require("#define _OS_TYPES_H" in runtime and "typedef int64_t ogg_int64_t;" in runtime,
            "the ogg allocator macros must stay out of a poisoned unit")
    require("ISAAC_VITA_RAW_ALLOCATOR_EXEMPT" not in runtime,
            "the seam must not ask for an allocator exemption")
    require("fpush(c, time);" in runtime, "th_granule_time returns in ST(0)")
    require("c->edx = (uint32_t)((uint64_t)frame >> 32);" in runtime,
            "th_granule_frame returns in edx:eax")
    require("#error" in runtime, "runtime file must refuse a knob-less compile")
    require("#include <theora/theoradec.h>" in runtime, "native header")
    # libtheoradec's allocator imports are renamed to these four entry points
    # (see the CMake objcopy step); the seam serves them from a memblock
    # mspace with the guest heap's public API as fallback and never spells a
    # raw allocator call.
    for signature in ("void *isaac_vita_theora_malloc(size_t size)",
                      "void *isaac_vita_theora_calloc(size_t count, size_t size)",
                      "void *isaac_vita_theora_realloc(void *pointer, size_t size)",
                      "void isaac_vita_theora_free(void *pointer)"):
        require(signature + "\n{" in runtime, f"pool allocator entry `{signature}`")
    for text in ("sceClibMspaceCreate(", "sceClibMspaceDestroy(",
                 "sceKernelAllocMemBlock(", "sceKernelFreeMemBlock(",
                 "isaac_vita_guest_malloc(", "isaac_vita_guest_calloc(",
                 "isaac_vita_guest_realloc(", "isaac_vita_guest_free(",
                 '#include "host_vita_heap.h"',
                 "VITA_NT_POOL_BYTES_FIRST  0x00800000U",
                 "VITA_NT_POOL_BYTES_SECOND 0x00600000U"):
        require(text in runtime, f"pool allocator uses `{text}`")
    require(re.search(r"(?<![_a-zA-Z0-9])(malloc|calloc|realloc|free)\(", runtime) is None,
            "the seam spells a raw allocator call")
    # Every seam that receives the decoder pointer refuses a NULL one; the nt1
    # device run died in th_granule_frame(NULL) after th_decode_alloc failed.
    for what in ("th_decode_ctl", "th_decode_packetin", "th_decode_ycbcr_out",
                 "th_granule_frame", "th_granule_time"):
        require(f'vita_nt_null_ctx("{what}")' in runtime, f"NULL decoder guard for {what}")
    cmake = (HERE / "vita" / "CMakeLists.txt").read_text(encoding="utf-8")
    require('option(ISAAC_VITA_NATIVE_THEORA\n' in cmake, "knob option")
    option = cmake[cmake.index("option(ISAAC_VITA_NATIVE_THEORA\n"):]
    option = option[:option.index("\n", option.index('" O'))]
    require(option.rstrip().endswith('" OFF)'), "knob default is not OFF")
    wraps = ("-Wl,--wrap=sub_005da480 -Wl,--wrap=sub_005da4f0 -Wl,--wrap=sub_005da670\n"
             "      -Wl,--wrap=sub_005da6d0 -Wl,--wrap=sub_005db3c0 -Wl,--wrap=sub_005db3f0\n"
             "      -Wl,--wrap=sub_005db440 -Wl,--wrap=sub_005db450 -Wl,--wrap=sub_005db470\n"
             "      -Wl,--wrap=sub_005dbac0 -Wl,--wrap=sub_005dbb10 -Wl,--wrap=sub_005ddb60\n"
             "      -Wl,--wrap=sub_005ddc10 -Wl,--wrap=sub_005bf7f0 -Wl,--wrap=sub_005c2910)")
    require(wraps in cmake, "wrap link options")
    block_start = cmake.index("  if(ISAAC_VITA_NATIVE_THEORA)\n")
    block_end = cmake.index("\n  endif()\n", block_start)
    block = cmake[block_start:block_end]
    pooled = ('target_link_libraries(isaac_first_arm_fault PRIVATE\n'
              '      "${ISAAC_VITA_THEORADEC_POOLED}")')
    require("host_vita_native_theora.c" in block and wraps in block and pooled in block,
            "source, wraps and the pooled libtheoradec are wired inside the knob block")
    require(cmake.count("host_vita_native_theora.c") == block.count("host_vita_native_theora.c"),
            "the source is referenced outside the knob block")
    require("PRIVATE theoradec)" not in cmake, "the raw libtheoradec archive must never be linked")
    require(cmake.count("ISAAC_VITA_THEORADEC_POOLED") == block.count("ISAAC_VITA_THEORADEC_POOLED"),
            "the pooled archive is referenced outside the knob block")
    for name in ("malloc", "calloc", "realloc", "free"):
        require(f"--redefine-sym {name}=isaac_vita_theora_{name}" in block,
                f"objcopy renames libtheoradec's {name}")
    require('"${ISAAC_VITA_THEORADEC_LIBRARY}" "${ISAAC_VITA_THEORADEC_POOLED}"' in block and
            'libtheoradec.a")' in block and "add_dependencies(isaac_first_arm_fault" in block,
            "the objcopy step reads the VitaSDK archive and feeds the link")

    # --- ISAAC_VITA_THEORADEC_SOURCE (vitasdk | master), vendor receipt, audit --
    # The archive behind the seams is a cache choice: the sysroot's 1.1.1 or a
    # replacement 1.2.0 with the ARM/NEON assembly under ISAAC_VITA_THEORADEC_PREFIX.
    # The variable may appear only at its cache
    # declaration, inside the knob block, and in the check after the block that
    # refuses a non-vitasdk source while the decoder knob is OFF (message()
    # only), so OFF sees no new input.
    declaration_start = cmake.index('set(ISAAC_VITA_THEORADEC_SOURCE "vitasdk" CACHE STRING\n')
    prefix_line = 'set(ISAAC_VITA_THEORADEC_PREFIX "" CACHE PATH\n'
    declaration_end = cmake.index('")\n', cmake.index(prefix_line, declaration_start)) + 3
    declaration = cmake[declaration_start:declaration_end]
    require(declaration.count("set_property(CACHE ISAAC_VITA_THEORADEC_SOURCE PROPERTY STRINGS vitasdk master)\n") == 1 and
            declaration.count("\nset(") == 1 and declaration.count("\noption(") == 0,
            "the cache declarations are the two set()s and the STRINGS property, nothing else")
    require(("/dev/" + "shm/") not in cmake, "a build-host scratch path is baked into CMakeLists.txt")
    crosscheck_start = cmake.index('  if(NOT ISAAC_VITA_THEORADEC_SOURCE MATCHES "^(vitasdk|master)$")\n')
    require(crosscheck_start > block_end, "the source cross-check must follow the knob block")
    crosscheck = cmake[crosscheck_start:cmake.index("  endif()\n", cmake.index(
        'if(NOT ISAAC_VITA_NATIVE_THEORA AND NOT ISAAC_VITA_THEORADEC_SOURCE STREQUAL "vitasdk")\n',
        crosscheck_start)) + len("  endif()\n")]
    require(crosscheck.count("message(FATAL_ERROR") == 2 and
            len(re.findall(r"(?m)^  if\(", crosscheck)) == 2 and
            len(re.findall(r"(?m)^  endif\(\)", crosscheck)) == 2 and
            "ISAAC_VITA_THEORADEC_SOURCE=${ISAAC_VITA_THEORADEC_SOURCE} requires ISAAC_VITA_NATIVE_THEORA=ON" in crosscheck and
            "ISAAC_VITA_THEORADEC_SOURCE must be vitasdk or master, got '${ISAAC_VITA_THEORADEC_SOURCE}'" in crosscheck and
            re.search(r"\b(set|add_|target_|list\(|include|file\()", crosscheck.replace("message(FATAL_ERROR", "")) is None,
            "the cross-check is two if()/FATAL_ERROR pairs and nothing else")
    for name in ("ISAAC_VITA_THEORADEC_SOURCE", "ISAAC_VITA_THEORADEC_PREFIX"):
        require(cmake.count(name) == declaration.count(name) + block.count(name) + crosscheck.count(name),
                f"{name} is referenced outside the cache declarations, the knob block and the cross-check")
    require("ISAAC_VITA_THEORADEC_PREFIX" not in crosscheck, "the cross-check reads the source only")
    require('if(ISAAC_VITA_THEORADEC_SOURCE STREQUAL "vitasdk")\n'
            '      set(ISAAC_VITA_THEORADEC_LIBRARY "${VITASDK}/arm-vita-eabi/lib/libtheoradec.a")\n' in block,
            "the vitasdk branch selects the sysroot archive (unchanged path)")
    master_start = block.index('elseif(ISAAC_VITA_THEORADEC_SOURCE STREQUAL "master")\n')
    master = block[master_start:block.index("\n    else()\n", master_start)]
    require('set(ISAAC_VITA_THEORADEC_LIBRARY "${ISAAC_VITA_THEORADEC_PREFIX}/lib/libtheoradec.a")' in master,
            "the master branch selects the prefix archive")
    require(master.index("if(NOT ISAAC_VITA_THEORADEC_PREFIX)\n") <
            master.index('set(ISAAC_VITA_THEORADEC_LIBRARY "${ISAAC_VITA_THEORADEC_PREFIX}') and
            "ISAAC_VITA_THEORADEC_SOURCE=master needs ISAAC_VITA_THEORADEC_PREFIX" in master,
            "the master branch refuses an empty prefix before using it")
    require('theora/theoradec.h")' in master and master.count("FATAL_ERROR") == 2,
            "the master branch refuses a prefix without the archive's header")
    require('set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS\n'
            '      "${ISAAC_VITA_THEORADEC_LIBRARY}")' in block,
            "the archive is a configure dependency so the vendor receipt tracks a rebuilt archive")
    include_append = 'APPEND PROPERTY INCLUDE_DIRECTORIES "${ISAAC_VITA_THEORADEC_PREFIX}/include")'
    require(include_append in master and cmake.count(include_append) == 1 and
            master.count("host_vita_native_theora.c") == 1 and
            cmake.count("PROPERTY INCLUDE_DIRECTORIES") == 2 and cmake.count('APPEND PROPERTY INCLUDE_DIRECTORIES "${ISAAC_LBR_GENERATED_INCLUDE}")') == 1,
            "the archive's include directory is appended to the seam source only, inside the master branch only")
    require("ISAAC_VITA_THEORADEC_SOURCE must be vitasdk or master, got '${ISAAC_VITA_THEORADEC_SOURCE}'" in block,
            "FATAL_ERROR text for an unknown source")
    require('file(STRINGS "${ISAAC_VITA_THEORADEC_LIBRARY}" ISAAC_VITA_THEORADEC_VENDOR\n'
            '      REGEX "Xiph\\\\.Org libtheora " LIMIT_COUNT 1)' in block,
            "the vendor string is read from the selected archive at configure time")
    # The 1.2.0 ARM archive keeps the literal in one string run with the
    # printable tail of the preceding constant, so the read is unanchored and
    # the vendor is the suffix starting at the literal.
    require('string(REGEX REPLACE "^.*(Xiph\\\\.Org libtheora )" "\\\\1"\n'
            '      ISAAC_VITA_THEORADEC_VENDOR "${ISAAC_VITA_THEORADEC_VENDOR}")' in block,
            "the vendor is the suffix of the string run (unanchored read)")
    require("source=${ISAAC_VITA_THEORADEC_SOURCE} archive=${ISAAC_VITA_THEORADEC_LIBRARY} "
            "vendor='${ISAAC_VITA_THEORADEC_VENDOR}'" in block,
            "the status line names source, archive and vendor")
    # The pooled copy's imports are audited right after the rename; the audit
    # is the only check of a LINK_LIBRARIES archive (the raw allocator gate
    # scans objects and the Lua archive).
    command_start = block.index('add_custom_command(OUTPUT "${ISAAC_VITA_THEORADEC_POOLED}"\n')
    command = block[command_start:block.index("VERBATIM)", command_start)]
    require(command.count("COMMAND ") == 2, "the pooled archive command has exactly two steps")
    require(command.index('COMMAND "${CMAKE_OBJCOPY}"') <
            command.index('COMMAND "${Python3_EXECUTABLE}" "${ISAAC_VITA_THEORADEC_IMPORT_AUDIT}"'),
            "the import audit runs after objcopy")
    require('--nm "${ISAAC_VITA_THEORADEC_NM}" --delete-on-failure\n'
            '        "${ISAAC_VITA_THEORADEC_POOLED}"' in command,
            "the audit reads the pooled copy through the toolchain nm and deletes a failing copy")
    require(command.count("--redefine-sym") == 4, "four allocator renames, no more, no fewer")
    # The pooled archive must hash the same across builds (objcopy 2.34 stamps
    # member headers with the current time unless told otherwise).
    require(command.count("--enable-deterministic-archives") == 1 and
            command.index("--enable-deterministic-archives") < command.index("--redefine-sym"),
            "objcopy writes a deterministic archive")
    require('DEPENDS "${ISAAC_VITA_THEORADEC_LIBRARY}" "${ISAAC_VITA_THEORADEC_IMPORT_AUDIT}"' in command,
            "the copy depends on the archive and the audit script")
    require('"${CMAKE_CURRENT_SOURCE_DIR}/vita_theoradec_import_audit.py")' in block and
            "find_package(Python3 COMPONENTS Interpreter REQUIRED)" in block and
            "arm-vita-eabi-nm" in block,
            "python and nm are located inside the knob block")
    audit = (HERE / "vita" / "vita_theoradec_import_audit.py").read_text(encoding="utf-8")
    for name in ("isaac_vita_theora_malloc", "isaac_vita_theora_calloc",
                 "isaac_vita_theora_realloc", "isaac_vita_theora_free"):
        require(f'"{name}"' in audit, f"the audit requires the import {name}")
    for name in ("malloc", "calloc", "realloc", "free", "memalign", "posix_memalign",
                 "aligned_alloc", "strdup", "valloc", "pvalloc", "_malloc_r"):
        require(f'"{name}"' in audit, f"the audit forbids the import {name}")
    require('re.compile(r"^ogg")' in audit, "the audit forbids libogg imports")

    # --- ISAAC_VITA_NATIVE_THEORA_PLANE_DIGEST (diagnostic, default OFF) ------
    require('option(ISAAC_VITA_NATIVE_THEORA_PLANE_DIGEST\n' in cmake, "digest knob option")
    option = cmake[cmake.index("option(ISAAC_VITA_NATIVE_THEORA_PLANE_DIGEST\n"):]
    option = option[:option.index("\n", option.index('" O'))]
    require(option.rstrip().endswith('" OFF)'), "digest knob default is not OFF")
    require("if(ISAAC_VITA_NATIVE_THEORA_PLANE_DIGEST AND NOT ISAAC_VITA_NATIVE_THEORA)" in cmake and
            "ISAAC_VITA_NATIVE_THEORA_PLANE_DIGEST requires ISAAC_VITA_NATIVE_THEORA=ON" in cmake,
            "the digest knob refuses to run without the decoder knob")
    digest_define = ('APPEND PROPERTY COMPILE_DEFINITIONS\n'
                     '          ISAAC_VITA_NATIVE_THEORA_PLANE_DIGEST=1)')
    digest_block = block[block.index("    if(ISAAC_VITA_NATIVE_THEORA_PLANE_DIGEST)\n"):]
    digest_block = digest_block[:digest_block.index("    endif()\n")]
    require(digest_define in digest_block and "host_vita_native_theora.c" in digest_block and
            cmake.count("ISAAC_VITA_NATIVE_THEORA_PLANE_DIGEST=1") == 1,
            "the digest define reaches the seam source only, inside the knob block")
    # Runtime: everything digest-related sits under #if ISAAC_VITA_NATIVE_THEORA_PLANE_DIGEST
    # (three regions: helpers, the alloc reset, the packetin call), and
    # th_decode_ycbcr_out is called only by its own wrapper and inside them.
    guard = "#if ISAAC_VITA_NATIVE_THEORA_PLANE_DIGEST\n"
    require("#if !defined(ISAAC_VITA_NATIVE_THEORA_PLANE_DIGEST)\n"
            "#define ISAAC_VITA_NATIVE_THEORA_PLANE_DIGEST 0\n#endif\n" in runtime,
            "the digest knob defaults to 0 inside the unit")
    require(runtime.count(guard) == 3, f"three digest regions expected, {runtime.count(guard)} found")
    regions: list[tuple[int, int]] = []
    at = 0
    while True:
        start = runtime.find(guard, at)
        if start < 0:
            break
        end = runtime.index("\n#endif\n", start)
        require("#if" not in runtime[start + len(guard):end], "no nested #if inside a digest region")
        regions.append((start, end))
        at = end

    def in_digest(position: int) -> bool:
        return any(start <= position < end for start, end in regions)

    for needle in ("plane_digest_crc32", "s_nt_digest_", "vita_nt_digest(", "NATIVE THEORA DIGEST:"):
        positions = [m.start() for m in re.finditer(re.escape(needle), runtime)]
        require(bool(positions) and all(in_digest(p) for p in positions),
                f"`{needle}` appears outside the digest regions")
    ycbcr_calls = [m.start() for m in re.finditer(r"= th_decode_ycbcr_out\(", runtime)]
    ycbcr_wrapper = runtime.index("void __wrap_sub_005db3c0(CPU *__restrict c)")
    ycbcr_wrapper_end = runtime.index("\n}\n", ycbcr_wrapper)
    require(len(ycbcr_calls) == 2 and
            sum(1 for p in ycbcr_calls if ycbcr_wrapper <= p < ycbcr_wrapper_end) == 1 and
            sum(1 for p in ycbcr_calls if in_digest(p)) == 1,
            "th_decode_ycbcr_out is called by its wrapper and by the digest only")
    require(runtime.count("th_decode_ycbcr_out(") == 3,
            "an unexpected th_decode_ycbcr_out( spelling appeared in the seam")
    alloc_start = runtime.index("void __wrap_sub_005da480(CPU *__restrict c)")
    alloc = runtime[alloc_start:runtime.index("\n}\n", alloc_start)]
    require("th_version_string()" in alloc and
            '"KAGE VITA NATIVE THEORA: archive=%s native_th_info=%u"' in alloc,
            "the alloc receipt names the linked archive")
    require(guard + "    s_nt_digest_packet = 0U;\n#endif\n" in alloc,
            "the digest ordinal restarts at every alloc")
    packetin_start = runtime.index("void __wrap_sub_005da6d0(CPU *__restrict c)")
    packetin = runtime[packetin_start:runtime.index("\n}\n", packetin_start)]
    require(guard + "    vita_nt_digest(dec, result, granpos);\n#endif\n" in packetin,
            "the packetin wrapper digests under the guard only")
    require("th_decode_ycbcr_out" not in packetin, "the packetin wrapper itself must not read planes")
    require("result != 0 && result != TH_DUPFRAME" in runtime and
            "th_decode_ycbcr_out((th_dec_ctx *)(uintptr_t)dec, planes)" in runtime,
            "the digest follows a 0/TH_DUPFRAME packetin only")
    require('"KAGE VITA NATIVE THEORA DIGEST: packet=%u ret=%d gp=%lld y=%08x cb=%08x "\n'
            '        "cr=%08x"' in runtime, "device digest line format")
    # The host oracle prints the same line with the same crc functions.
    oracle = (HERE / "vita" / "theora_plane_digest.c").read_text(encoding="utf-8")

    def function_text(source: str, signature: str) -> str:
        start = source.index(signature)
        return source[start:source.index("\n}\n", start) + 3]

    for signature in ("static void plane_digest_crc32_init(uint32_t *table)\n",
                      "static uint32_t plane_digest_crc32(const uint32_t *table,\n"):
        require(function_text(runtime, signature) == function_text(oracle, signature),
                f"`{signature.strip()}` differs between the seam and the host oracle")
    require("0xEDB88320U" in oracle and "0xCBF43926U" in oracle and
            "THEORA_PLANE_DIGEST_SELFTEST" in oracle,
            "oracle crc polynomial, IEEE check value and header-free selftest")
    require('printf("packet=%u ret=%d gp=%lld y=%08x cb=%08x cr=%08x\\n"' in oracle,
            "oracle line format equals the device receipt minus the prefix")
    require("ret == 0 || ret == TH_DUPFRAME" in oracle and
            "th_decode_ycbcr_out(td, planes)" in oracle and
            "TH_DECCTL_SET_STRIPE_CB" not in oracle,
            "the oracle always reads the planes back after 0/TH_DUPFRAME and takes no stripe callback")
    require(re.search(r"(?<![_a-zA-Z0-9])(malloc|calloc|realloc|free)\(", oracle) is None,
            "the oracle spells a raw allocator call")
    require((HERE / "vita" / "theora_plane_digest_compare.py").is_file() and
            (HERE / "vita" / "test_theora_plane_digest_compare.py").is_file() and
            (HERE / "vita" / "test_vita_theoradec_import_audit.py").is_file(),
            "digest compare tool, its fixtures and the audit fixtures exist")
    # The vorbis_synthesis pass-through lives in the theora seam.
    require("void __wrap_sub_005c2910(CPU *__restrict c)" in runtime and
            "__real_sub_005c2910(c);" in runtime,
            "vorbis_synthesis is a timed pass-through")
    match = re.search(r"#define VITA_NT_VORBIS_SYNTHESIS_RVA\s+(0x[0-9a-fA-F]+)U", runtime)
    require(match is not None and int(match.group(1), 16) == VORBIS_SYNTHESIS,
            "runtime pin VITA_NT_VORBIS_SYNTHESIS_RVA")

    # --- ISAAC_VITA_NATIVE_THEORA_RGB: host_vita_native_yuv.c ---------------
    yuv = (HERE / "runtime" / "host_vita_native_yuv.c").read_text(encoding="utf-8")
    require("void __wrap_sub_005ce3f0(CPU *__restrict c)" in yuv, "yuv wraps I420ToRGB24Matrix")
    require("__real_sub_005ce3f0" not in yuv,
            "the native conversion must not call the translated libyuv")
    match = re.search(r"#define VITA_NY_I420_TO_RGB24_MATRIX_RVA\s+(0x[0-9a-fA-F]+)U", yuv)
    require(match is not None and int(match.group(1), 16) == I420_TO_RGB24,
            "yuv pin VITA_NY_I420_TO_RGB24_MATRIX_RVA")
    offsets = {"VITA_NY_CONST_UB": 0x00, "VITA_NY_CONST_UG": 0x20, "VITA_NY_CONST_VG": 0x21,
               "VITA_NY_CONST_VR": 0x41, "VITA_NY_CONST_BB": 0x60, "VITA_NY_CONST_BG": 0x80,
               "VITA_NY_CONST_BR": 0xa0, "VITA_NY_CONST_YG": 0xc0}
    for name, value in offsets.items():
        match = re.search(rf"#define {name}\s+(0x[0-9a-fA-F]+)U", yuv)
        require(match is not None and int(match.group(1), 16) == value,
                f"yuv table offset {name} is {match and match.group(1)}, expected {value:#x}")
    for index in range(9):
        require(f"vita_ny_arg(c, {index}U)" in yuv, f"yuv reads stack argument {index}")
    for text in ("uint32_t src_y = c->ecx;", "int32_t stride_y = (int32_t)c->edx;",
                 "#include <arm_neon.h>", "#error", "vst3_u8(", "vqmovun_s32(", "vqmovn_u16(",
                 "vmlsl_s16(", "* 0x0101U * k->yg) >> 16", "vmull_n_u16(", "0x0101U)",
                 "vshrq_n_u32(vmulq_u32(", "vshrq_n_s32(", ", 6)", "if (y & 1) {",
                 "static int vita_ny_consts_fit_s16(", "vita_ny_consts_fit_s16(&k)",
                 "vshrn_n_u32(vmull_lane_u16(", "vqsubq_s16(", "vmlsq_lane_s16(", "vmulq_lane_s16(",
                 "vqmovun_s16(vshrq_n_s16(", "vld1q_u8(sy + x)", "x + 16 <= width",
                 "vita_ny_row16(", "vita_ny_row32(", "s16_rows=%u",
                 "stride_dst = -stride_dst;", "width <= 0 || height == 0)", "return -1;",
                 "c->eax = (uint32_t)result;", "(void)gpop(c);", "vita_ny_load_consts(table, &k);",
                 "KAGE VITA NATIVE YUV:"):
        require(text in yuv, f"yuv converter `{text}`")
    require(re.search(r"(?<![_a-zA-Z0-9])(malloc|calloc|realloc|free)\(", yuv) is None,
            "the yuv seam spells a raw allocator call")
    require("ISAAC_VITA_RAW_ALLOCATOR_EXEMPT" not in yuv, "the yuv seam must not ask for an exemption")
    require('option(ISAAC_VITA_NATIVE_THEORA_RGB\n' in cmake, "rgb knob option")
    option = cmake[cmake.index("option(ISAAC_VITA_NATIVE_THEORA_RGB\n"):]
    option = option[:option.index("\n", option.index('" O'))]
    require(option.rstrip().endswith('" OFF)'), "rgb knob default is not OFF")
    rgb = cmake[cmake.index("  if(ISAAC_VITA_NATIVE_THEORA_RGB)\n"):]
    rgb = rgb[:rgb.index("\n  endif()\n")]
    require("host_vita_native_yuv.c" in rgb and "-Wl,--wrap=sub_005ce3f0" in rgb and
            "if(NOT ISAAC_VITA_NATIVE_THEORA)" in rgb and "FATAL_ERROR" in rgb,
            "the yuv source and wrap are wired inside the rgb knob block, which requires the decoder")
    require(cmake.count("host_vita_native_yuv.c") == rgb.count("host_vita_native_yuv.c"),
            "the yuv source is referenced outside the rgb knob block")
    require(cmake.count("--wrap=sub_005ce3f0") == 1, "the libyuv wrap appears once")
    # libyuv selects its SSSE3/AVX2 rows from cpuid leaf 1 ecx; the Vita cpuid
    # seam leaves ecx at 0 for that leaf, so the translated build ran the C row
    # the NEON converter reproduces bit for bit.
    cpuid = (HERE / "runtime" / "host_vita_first_fault.c").read_text(encoding="utf-8")
    start = cpuid.index("void guest_cpuid(CPU *__restrict c)")
    body = cpuid[start:cpuid.index("\n}\n", start)]
    require("c->eax = c->ebx = c->ecx = c->edx = 0U;" in body, "cpuid seam zeroes every register first")
    leaf1 = body[body.index("leaf == 1U"):]
    leaf1 = leaf1[:leaf1.index("} else if")]
    require("c->ecx" not in leaf1, "cpuid leaf 1 must report no ecx feature (SSSE3/AVX2) bits")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", type=Path,
                        default=Path(os.environ.get("ISAAC_AUDIO_TEST_PE", "")))
    arguments = parser.parse_args()
    check_sources()
    if not arguments.pe or not arguments.pe.is_file():
        print("native theora sites: sources OK, no --pe given (PE pins not run)")
        return 0
    check_pe(arguments.pe)
    print("native theora sites: PASS (thirteen th_* entry points, call sites, "
          "no address-taken references, argument/return conventions, frame "
          "decode vtable, libyuv I420ToRGB24Matrix callers/row math/tables, "
          "vorbis_synthesis sites, sources)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
