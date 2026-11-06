#!/usr/bin/env python3
"""Frozen-PE census: the vertex-quad copy constructor does not exist here.

The reference project (the-browsing-of-isaac, Repentance+ 1.9.7.17, round
90d) replaced a 112-byte vertex-quad copy constructor - eight dwords zeroed,
four vec2 from a constant, four Color(0xffffffff) of five floats each, then
28 dwords copied over all of it - with a memcpy and saved ~0.9 ms/frame.
This test pins the facts that show our PE (Repentance 1.7.9b J835) has no
equivalent, so the search is not repeated:

* Color(uint32) is sub_00560b80 and writes exactly four floats (16 bytes);
  the five-float colour of the newer build does not exist.
* No `mov ecx, 0x1c` / `mov ecx, 0x18` feeds a `rep movsd` anywhere in
  .text, and no `push -1; call sub_00560b80` site is followed by a string
  copy: nothing builds a colour quad and then copies 24 or 28 dwords over it.
* MSVC also spells struct copies as movups pairs, which a `rep movsd` scan
  misses.  A boundary-free linear sweep of .text (regions split at every
  `ret`/`int3`) finds exactly 42 regions that end in `ret 4` and copy >= 96
  bytes by SSE pairs or a counted `rep movsd`; the table below freezes them.
  None overlaps the sprite chain, the only three that call Color(uint32) are
  game renderers of 3.5-11.8 KB (not constructor bodies), and the eight
  constructor-sized ones (<= 0x400 bytes copying exactly 24 or 28 dwords:
  the 0x114-byte Sprite copy at sub_00002680, the 0x8c-byte copy at
  sub_004eb1e0 with two callers, and six position/string helpers) build no
  colour and zero nothing they overwrite.
* The per-layer sprite draw is sub_00003e50 (SEH frame, saves the five blend
  globals at 0xbc7a34..0xbc7a44).  It builds the 36-byte UV quad through
  sub_00004760 and the 32-byte position quad through sub_00004b60 into its
  own frame, writes one 16-byte colour, and calls Image vtable slot +0xc
  (sub_0055f610), which pushes that colour four times into slot +8
  (sub_0055f630); that wrapper copies the UV quad by value (two movups and
  one dword - nine dwords, no dead prologue), normalises it through
  sub_0055ebd0 when the flag at +0x20 is not 1 and calls Image::PushQuad
  (sub_0055f990) with six pointer arguments.  Every quad travels by pointer;
  there is no by-value 112-byte aggregate and therefore no copy constructor
  to wrap.
* The only stored-then-overwritten residue in the chain is three dead dword
  zero stores in each quad builder (MSVC's default init before the flip
  logic); they are pinned here so a later dead-store pass can find them.

usage: python test_quad_ctor_sites.py --pe <isaac-ng.exe.unpacked.exe>
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

LAYER_DRAW = 0x00003E50        # per-layer sprite draw (AnimationState layer render)
LAYER_DRAW_END = 0x00004751
UV_QUAD_BUILDER = 0x00004760   # 36-byte UV quad (four vec2 + normalised flag)
POS_QUAD_BUILDER = 0x00004B60  # 32-byte position quad (rotated twice via sub_0055e770)
QUAD_ROTATE = 0x0055E770
DRAW_THUNK = 0x0055F610        # Image vtable +0xc: one colour -> four, into slot +8
DRAW_WRAPPER = 0x0055F630      # Image vtable +8: by-value UV copy, normalise, PushQuad
UV_NORMALISE = 0x0055EBD0
PUSH_QUAD = 0x0055F990         # KAGE::Graphics::Image::PushQuad (ISAAC_VITA_KAGE_QUAD_FASTPATH root)
COLOR_FROM_U32 = 0x00560B80    # Color(uint32 packed) -> four floats
IMAGE_VTABLE = 0x0075D2B4
PURE_VIRTUAL = 0x005EC13A
BLEND_GLOBALS = (0xBC7A34, 0xBC7A38, 0xBC7A3C, 0xBC7A40, 0xBC7A44)

PUSH_QUAD_CALLERS = (0x0055F425, 0x0055F486, 0x0055F5F9, 0x0055F675)
# Direct `call rel32` sites in .text (byte-level E8 scan, like the other site tests).
LAYER_DRAW_CALL_SITES = 142
COLOR_CALL_SITES = 218         # every direct caller of Color(uint32)
COLOR_PUSH_M1_SITES = 9        # ... of which `push -1; call` (white), max 8 in one function

# Linear sweep of .text, regions split at every `ret`/`int3`: (region start,
# `ret 4` rva, bytes copied = max(16 * min(movups loads, movups stores),
# 4 * the last `mov ecx, imm` before a `rep movsd`), direct Color(uint32)
# calls in the region).  Every region ending in `ret 4` that copies >= 96
# bytes (24 dwords: four vec2 + four 16-byte colours) is listed; a copy
# constructor of a colour quad would have to be one of these.
RET4_COPY_REGIONS = (
    (0x00002680, 0x00002A7B, 96, 0),      # Sprite copy ctor: 0x114 bytes + 0x9c-stride layer array
    (0x000F3E00, 0x000F3E9E, 96, 0),
    (0x0015D9B0, 0x0015DBA3, 96, 0),
    (0x00170780, 0x00171579, 96, 0),
    (0x001E9810, 0x001ECFB1, 96, 0),
    (0x0027A110, 0x0027A581, 112, 0),
    (0x0027A590, 0x0027A94A, 160, 0),
    (0x00289400, 0x0028962D, 96, 0),
    (0x00292080, 0x00292951, 96, 0),
    (0x002E96A0, 0x002E9802, 96, 0),
    (0x002E996F, 0x002E9BCA, 128, 0),
    (0x00314730, 0x00314F8F, 2028, 0),
    (0x00314FB0, 0x0031532E, 2028, 0),
    (0x00356CA0, 0x00356EC4, 1200, 0),
    (0x00372FC0, 0x0037464D, 128, 0),
    (0x00375460, 0x00376B4F, 208, 0),
    (0x00391490, 0x00392A4A, 320, 4),     # 5.5 KB game renderer, one caller
    (0x003D2882, 0x003D83A9, 976, 0),
    (0x003EFAB0, 0x003EFCE9, 220, 0),
    (0x003F1800, 0x003F1DD4, 96, 0),
    (0x003F4540, 0x003F4C57, 128, 0),
    (0x00454A80, 0x00454B9C, 96, 0),
    (0x0045F3C0, 0x0045F503, 1792, 0),
    (0x004865A0, 0x00486C04, 344, 0),
    (0x00486C10, 0x00487000, 440, 0),
    (0x0048D52B, 0x0048F096, 176, 0),
    (0x004A2390, 0x004A3151, 256, 1),     # 3.5 KB game renderer
    (0x004B6612, 0x004B7883, 96, 0),
    (0x004BE8B0, 0x004BF144, 144, 0),
    (0x004C7070, 0x004C729E, 144, 0),
    (0x004D1330, 0x004D4150, 144, 6),     # 11.8 KB game renderer, vtable-only
    (0x004D5090, 0x004D65CB, 96, 0),
    (0x004EB1E0, 0x004EB24F, 112, 0),     # 0x8c-byte copy ctor, two callers, no prologue
    (0x004F7CD0, 0x004F8331, 96, 0),
    (0x004FE8E0, 0x004FFD3C, 128, 0),
    (0x004FFE30, 0x00500560, 96, 0),
    (0x00500570, 0x00500CBD, 192, 0),
    (0x00500E04, 0x0050120A, 128, 0),
    (0x0052F090, 0x0052F43A, 128, 0),
    (0x0055F910, 0x0055F963, 1024, 0),    # vtable-only 1 KB copy next to PushQuad, never a direct callee
    (0x0059F240, 0x0059F337, 200, 0),
    (0x005AD0C0, 0x005AD486, 112, 0),
)
# The sprite chain no copy region may overlap.
SPRITE_CHAIN = ((LAYER_DRAW, LAYER_DRAW_END), (UV_QUAD_BUILDER, 0x00004B54), (POS_QUAD_BUILDER, 0x00005181),
                (DRAW_THUNK, 0x0055F629), (DRAW_WRAPPER, 0x0055F681), (PUSH_QUAD, 0x00560253),
                (COLOR_FROM_U32, 0x00560C2D))
# Regions of at most 0x400 bytes that copy exactly 24 or 28 dwords - the size a colour-quad
# copy constructor would have.  Each was read: none zeroes what it then overwrites and none
# builds a colour.  (Sprite copy ctor; string-returning position helpers; a Lua tuple reader;
# an SEH string routine; the 0x8c-byte copy ctor with two callers; a vtable-only position routine.)
CONSTRUCTOR_SIZED_COPIERS = frozenset((0x00002680, 0x000F3E00, 0x0015D9B0, 0x00289400, 0x002E96A0,
                                       0x00454A80, 0x004EB1E0, 0x005AD0C0))


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def decode(pe: pefile.PE, start: int, length: int) -> dict:
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    instructions = list(decoder.disasm(pe.get_data(start, length), IMAGE_BASE + start))
    require(bool(instructions), f"empty decode at {start:08x}")
    return {i.address: f"{i.mnemonic} {i.op_str}".strip() for i in instructions}


def text_section(pe: pefile.PE):
    section = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
    return section.VirtualAddress, pe.get_data(section.VirtualAddress, section.Misc_VirtualSize)


def direct_call_sites(text_rva: int, code: bytes, target: int) -> list:
    sites = []
    for match in re.finditer(rb"\xe8", code):
        offset = match.start()
        rel = int.from_bytes(code[offset + 1:offset + 5], "little", signed=True)
        if text_rva + offset + 5 + rel == target:
            sites.append(text_rva + offset)
    return sites


def check_layer_draw(pe: pefile.PE) -> None:
    body = decode(pe, LAYER_DRAW, LAYER_DRAW_END - LAYER_DRAW)
    va = lambda rva: IMAGE_BASE + rva  # noqa: E731
    require(body[va(0x3E50)] == "push ebx" and body[va(0x3E51)] == "mov ebx, esp",
            "layer draw prologue")
    require(body[va(0x3E6D)] == "mov eax, dword ptr fs:[0]", "layer draw SEH frame")
    require(body[va(0x3E75)] == "sub esp, 0x190", "layer draw frame size")
    # the five blend globals are saved as one xmmword + one dword and restored at the end
    require(body[va(0x3F3C)] == "movups xmm0, xmmword ptr [0xbc7a34]", "blend globals load")
    require(body[va(0x3F47)] == "movups xmmword ptr [ebp - 0x180], xmm0", "blend globals save")
    for site, glob in zip((0x470B, 0x4716, 0x4721, 0x472C), BLEND_GLOBALS[:4]):
        require(body[va(site)] == f"mov dword ptr [0x{glob:x}], eax", f"blend global restore {glob:x}")
    # the two quad builders write into the layer draw's own frame
    require(body[va(0x407C)] == "lea eax, [ebp - 0x150]", "UV quad frame slot")
    require(body[va(0x4096)] == f"call 0x{va(UV_QUAD_BUILDER):x}", "UV quad builder call")
    require(body[va(0x40A9)] == "lea eax, [ebp - 0x1a0]", "position quad frame slot")
    require(body[va(0x40BF)] == f"call 0x{va(POS_QUAD_BUILDER):x}", "position quad builder call")
    # Image::Draw through vtable slot +0xc with (uv, positions, colour) pointers: four sites
    require(body[va(0x4284)] == "mov eax, dword ptr [eax + 0xc]" and body[va(0x4287)] == "call eax",
            "Draw slot +0xc site 1")
    require(body[va(0x454F)] == "mov eax, dword ptr [eax + 0xc]" and body[va(0x4552)] == "call eax",
            "Draw slot +0xc site 2")
    require(body[va(0x4648)] == "call dword ptr [eax + 0xc]", "Draw slot +0xc site 3")
    require(body[va(0x46D1)] == "mov eax, dword ptr [eax + 0xc]" and body[va(0x46D4)] == "call eax",
            "Draw slot +0xc site 4")
    require(all(body[va(a)] == "push edx" for a in (0x426B, 0x427C, 0x4283)),
            "Draw arguments are three pushed pointers")
    require(body[va(0x4274)] == "lea edx, [ebp - 0x1a0]" and body[va(0x427D)] == "lea edx, [ebp - 0x150]",
            "Draw receives the two frame quads by pointer")
    require(body[va(0x422B)] == "lea edx, [ebp - 0xe8]", "Draw receives the colour by pointer")
    require(body[va(0x474E)] == "ret 0x14", "layer draw epilogue")
    # no string copy and no 112-byte aggregate anywhere in the body
    require(not any(t.startswith("rep ") for t in body.values()), "layer draw has a string copy")
    require(not any(t.startswith("call 0x") and t.endswith(f"{va(COLOR_FROM_U32):x}") for t in body.values()),
            "layer draw builds a colour from a packed dword")


def check_quad_builders(pe: pefile.PE) -> None:
    va = lambda rva: IMAGE_BASE + rva  # noqa: E731
    uv = decode(pe, UV_QUAD_BUILDER, 0x4B54 - UV_QUAD_BUILDER)
    require(uv[va(0x481F)] == "movss dword ptr [esi], xmm0", "UV quad first vertex")
    # three dead zero stores (default init), each overwritten before any read; a GSTACK
    # load sits between them (0x483d) so the translated C keeps them.
    for site, field in ((0x4823, 0xC), (0x482A, 0x1C), (0x4831, 0x14)):
        require(uv[va(site)] == f"mov dword ptr [esi + 0x{field:x}], 0", f"UV dead zero store {field:x}")
    require(uv[va(0x483D)] == "movss xmm0, dword ptr [esp + 0x10]", "UV intervening frame load")
    for site, field, reg in ((0x484D, 0xC, "xmm3"), (0x4857, 0x1C, "xmm4"), (0x485C, 0x14, "xmm4")):
        require(uv[va(site)] == f"movss dword ptr [esi + 0x{field:x}], {reg}", f"UV overwrite {field:x}")
    require(uv[va(0x4861)] == "mov dword ptr [esi + 0x20], 0", "UV quad normalised flag = 0")
    require(uv[va(0x4B51)] == "ret 0x1c", "UV quad builder epilogue")

    pos = decode(pe, POS_QUAD_BUILDER, 0x5181 - POS_QUAD_BUILDER)
    for site, field in ((0x4E49, 0xC), (0x4E50, 0x14), (0x4E57, 0x1C)):
        require(pos[va(site)] == f"mov dword ptr [esi + 0x{field:x}], 0", f"position dead zero store {field:x}")
    for site, field, reg in ((0x4E86, 0x1C, "xmm2"), (0x4E8B, 0x14, "xmm2"), (0x4EC5, 0xC, "xmm0")):
        require(pos[va(site)] == f"movss dword ptr [esi + 0x{field:x}], {reg}", f"position overwrite {field:x}")
    require(pos[va(0x5159)] == f"call 0x{va(QUAD_ROTATE):x}" and pos[va(0x5173)] == f"call 0x{va(QUAD_ROTATE):x}",
            "position quad rotations")
    require(pos[va(0x517E)] == "ret 0x1c", "position quad builder epilogue")
    require(not any(t.startswith("rep ") for t in list(uv.values()) + list(pos.values())),
            "quad builders have a string copy")


def check_draw_wrappers(pe: pefile.PE) -> None:
    va = lambda rva: IMAGE_BASE + rva  # noqa: E731
    thunk = decode(pe, DRAW_THUNK, 0x19)
    require(thunk[va(0x55F613)] == "mov eax, dword ptr [ebp + 0x10]", "thunk colour argument")
    require(all(thunk[va(a)] == "push eax" for a in (0x55F618, 0x55F619, 0x55F61A, 0x55F61B)),
            "thunk pushes the colour four times")
    require(thunk[va(0x55F622)] == "call dword ptr [edx + 8]" and thunk[va(0x55F626)] == "ret 0xc",
            "thunk enters slot +8")

    wrapper = decode(pe, DRAW_WRAPPER, 0x51)
    require(wrapper[va(0x55F63C)] == "movups xmm0, xmmword ptr [eax]", "UV copy load 0")
    require(wrapper[va(0x55F63F)] == "movups xmmword ptr [ebp - 0x24], xmm0", "UV copy store 0")
    require(wrapper[va(0x55F643)] == "movups xmm0, xmmword ptr [eax + 0x10]", "UV copy load 1")
    require(wrapper[va(0x55F647)] == "mov eax, dword ptr [eax + 0x20]", "UV flag load")
    require(wrapper[va(0x55F64A)] == "mov dword ptr [ebp - 4], eax", "UV flag store")
    require(wrapper[va(0x55F64D)] == "movups xmmword ptr [ebp - 0x14], xmm0", "UV copy store 1")
    require(wrapper[va(0x55F651)] == "cmp eax, 1", "UV normalised test")
    require(wrapper[va(0x55F65B)] == f"call 0x{va(UV_NORMALISE):x}", "UV normalise call")
    require(wrapper[va(0x55F675)] == f"call 0x{va(PUSH_QUAD):x}", "PushQuad call")
    require(wrapper[va(0x55F67E)] == "ret 0x18", "Draw wrapper epilogue")
    # the copied aggregate is 36 bytes: two xmmwords and one dword, nothing zeroed first
    stores = [t for t in wrapper.values() if "ptr [ebp - " in t and t.split(" ")[0] in ("mov", "movups")
              and t.split(",")[0].endswith("]")]
    require(len(stores) == 3, f"Draw wrapper frame stores {stores}")

    # Image vtable: Draw overloads at +8..+0x24, GetWidth/GetHeight pure at +0x28/+0x2c
    slots = struct.unpack("<12I", pe.get_data(IMAGE_VTABLE, 48))
    expected = (0x55F300, 0x55F320, DRAW_WRAPPER, DRAW_THUNK, 0x55F4C0, 0x55F4A0,
                0x55F460, 0x55F440, 0x55F360, 0x55F340, PURE_VIRTUAL, PURE_VIRTUAL)
    require(slots == tuple(va(r) for r in expected), f"Image vtable drifted: {[hex(s) for s in slots]}")


def check_color(pe: pefile.PE) -> None:
    va = lambda rva: IMAGE_BASE + rva  # noqa: E731
    body = decode(pe, COLOR_FROM_U32, 0xAD)
    require(body[va(0x560B83)] == "mov edx, dword ptr [ebp + 8]", "Color(uint32) argument")
    stores = sorted(t for t in body.values() if t.startswith("movss dword ptr ["))
    require(stores == ["movss dword ptr [ecx + 0xc], xmm0", "movss dword ptr [ecx + 4], xmm0",
                       "movss dword ptr [ecx + 8], xmm0", "movss dword ptr [ecx], xmm0"],
            f"Color(uint32) is not four floats: {stores}")
    require(not any(t.startswith("mov dword ptr [ecx") for t in body.values()), "Color(uint32) extra field")
    require(body[va(0x560C2A)] == "ret 4", "Color(uint32) epilogue")


def check_census(pe: pefile.PE) -> None:
    text_rva, code = text_section(pe)
    # 1. `mov ecx, 0x1c` immediately feeding `rep movsd` (the reference constructor's copy)
    hits = [m.start() for m in re.finditer(rb"\xb9\x1c\x00\x00\x00", code)
            if code.find(b"\xf3\xa5", m.start() + 5, m.start() + 5 + 12) >= 0]
    require(hits == [], f"28-dword rep movsd sites: {[hex(text_rva + h) for h in hits]}")
    # 2. every rep movsd: the nearest preceding `mov ecx, imm32` is never 24 or 28 dwords
    counts = set()
    for match in re.finditer(rb"\xf3\xa5", code):
        window = code[max(0, match.start() - 24):match.start()]
        idx = window.rfind(b"\xb9")
        if idx >= 0 and idx + 5 <= len(window):
            counts.add(int.from_bytes(window[idx + 1:idx + 5], "little"))
    require(0x1C not in counts and 0x18 not in counts,
            f"a 24/28-dword string copy appeared: {sorted(hex(c) for c in counts)}")
    # 3. push -1; call Color(uint32): counted, and never followed by a string copy
    sites = []
    for match in re.finditer(rb"\x6a\xff\xe8", code):
        offset = match.start() + 2
        rel = int.from_bytes(code[offset + 1:offset + 5], "little", signed=True)
        if text_rva + offset + 5 + rel == COLOR_FROM_U32:
            sites.append(offset)
            require(code.find(b"\xf3\xa5", offset, offset + 0x80) < 0,
                    f"Color(-1) followed by a string copy at {text_rva + offset:08x}")
    require(len(sites) == COLOR_PUSH_M1_SITES,
            f"push -1; call Color(uint32) sites: {len(sites)} != {COLOR_PUSH_M1_SITES}")
    color_sites = direct_call_sites(text_rva, code, COLOR_FROM_U32)
    require(len(color_sites) == COLOR_CALL_SITES,
            f"Color(uint32) direct call sites: {len(color_sites)} != {COLOR_CALL_SITES}")
    # 4. call-site census of the chain
    require(direct_call_sites(text_rva, code, PUSH_QUAD) == list(PUSH_QUAD_CALLERS), "PushQuad callers drifted")
    layer_sites = direct_call_sites(text_rva, code, LAYER_DRAW)
    require(len(layer_sites) == LAYER_DRAW_CALL_SITES,
            f"layer draw call sites: {len(layer_sites)} != {LAYER_DRAW_CALL_SITES}")
    require(direct_call_sites(text_rva, code, UV_QUAD_BUILDER) == [0x4096, 0x65AD], "UV builder callers")
    require(direct_call_sites(text_rva, code, POS_QUAD_BUILDER) == [0x40BF, 0x65D1, 0x350EFA, 0x354036, 0x3C7843],
            "position builder callers")
    # the Draw wrappers are reached only through the vtables
    for wrapper in (DRAW_WRAPPER, DRAW_THUNK, 0x55F360, 0x55F460, 0x55F4C0):
        require(direct_call_sites(text_rva, code, wrapper) == [], f"direct call to Draw wrapper {wrapper:08x}")


def check_copy_regions(pe: pefile.PE) -> None:
    """Linear sweep of .text for SSE / rep-movsd copies that end in `ret 4`.

    Regions are split at every `ret` and `int3`, so a function with one
    return is one region and a constructor is never merged with anything but
    a preceding tail-jump.  Boundary-free on purpose: the pin must run from
    the PE alone, without the translator's function table."""
    text_rva, code = text_section(pe)
    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    decoder.skipdata = True
    sse = ("movups", "movaps", "movdqu", "movdqa")
    found = []
    start, loads, stores, rep_bytes, colours, last_ecx = text_rva, 0, 0, 0, 0, None
    for insn in decoder.disasm(code, IMAGE_BASE + text_rva):
        mnemonic, operands = insn.mnemonic, insn.op_str
        if mnemonic in sse:
            if operands.startswith("xmmword ptr ["):
                stores += 1
            elif operands.startswith("xmm"):
                loads += 1
        elif mnemonic == "mov" and operands.startswith("ecx, 0x"):
            last_ecx = int(operands[5:], 16)
        elif mnemonic == "rep movsd":
            if last_ecx is not None and last_ecx < 0x10000:
                rep_bytes = max(rep_bytes, last_ecx * 4)
        elif mnemonic == "call" and operands.startswith("0x") and int(operands, 16) - IMAGE_BASE == COLOR_FROM_U32:
            colours += 1
        if mnemonic in ("ret", "int3"):
            if mnemonic == "ret" and operands == "4":
                copied = max(min(loads, stores) * 16, rep_bytes)
                if copied >= 96:
                    found.append((start, insn.address - IMAGE_BASE, copied, colours))
            start = insn.address - IMAGE_BASE + insn.size
            loads, stores, rep_bytes, colours, last_ecx = 0, 0, 0, 0, None
    require(tuple(found) == RET4_COPY_REGIONS,
            "ret-4 copy regions drifted:\n" + "\n".join("    (0x%08X, 0x%08X, %d, %d)," % r for r in found))
    for start, end, copied, colours in RET4_COPY_REGIONS:
        for lo, hi in SPRITE_CHAIN:
            require(end < lo or start >= hi, f"copy region {start:08x} overlaps the sprite chain {lo:08x}")
        # a colour-quad constructor would call Color(uint32) four times in a body of a few
        # hundred bytes; the only regions that call it at all are large game renderers
        require(colours == 0 or end - start > 0x800, f"constructor-sized colour builder at {start:08x}")
        # the constructor-sized copiers (24/28 dwords in <= 0x400 bytes) are known and colour-free
        if copied in (96, 112) and end - start <= 0x400:
            require(start in CONSTRUCTOR_SIZED_COPIERS and colours == 0,
                    f"new constructor-sized 24/28-dword copier at {start:08x}")
    # the layer draw's direct callees are the quad builders, colour/blend helpers and the CRT;
    # none of them is a copy region
    body = decode(pe, LAYER_DRAW, LAYER_DRAW_END - LAYER_DRAW)
    callees = frozenset(int(t.split(" ")[1], 16) - IMAGE_BASE for t in body.values()
                        if t.startswith("call 0x"))
    require(callees == frozenset((0x3540, 0x7790, 0x2170, 0x2290, 0x7BB0, UV_QUAD_BUILDER, POS_QUAD_BUILDER,
                                  0x3A7D50, QUAD_ROTATE, 0x25AF60, 0x5EACD7)),
            f"layer draw callees drifted: {sorted(hex(c) for c in callees)}")
    require(not any(any(s <= c < e for s, e, _, _ in RET4_COPY_REGIONS) for c in callees),
            "the layer draw calls a copy region")


def check_pe(path: Path) -> None:
    data = path.read_bytes()
    require(len(data) == PE_SIZE, f"PE size {len(data)} != {PE_SIZE}")
    require(hashlib.sha256(data).hexdigest() == PE_SHA256, "PE SHA-256 drifted")
    pe = pefile.PE(data=data, fast_load=True)
    check_layer_draw(pe)
    check_quad_builders(pe)
    check_draw_wrappers(pe)
    check_color(pe)
    check_census(pe)
    check_copy_regions(pe)


def check_sources() -> None:
    gen_all = (HERE / "gen_all.py").read_text(encoding="utf-8")
    require("VITA_KAGE_QUAD_FASTPATH_ROOT_RVA = 0x0055F990" in gen_all, "PushQuad root pin")
    callers = "VITA_KAGE_QUAD_FASTPATH_CALLERS = (\n    0x0055F425, 0x0055F486, 0x0055F5F9, 0x0055F675)"
    require(callers in gen_all, "PushQuad caller pin")
    cmake = (HERE / "vita" / "CMakeLists.txt").read_text(encoding="utf-8")
    require("ISAAC_VITA_QUAD_CTOR_FASTPATH" not in cmake,
            "a quad constructor knob appeared; re-run this census before trusting it")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", type=Path,
                        default=Path(os.environ.get("ISAAC_AUDIO_TEST_PE", "")))
    arguments = parser.parse_args()
    check_sources()
    if not arguments.pe or not arguments.pe.is_file():
        print("quad ctor sites: sources OK, no --pe given (PE pins not run)")
        return 0
    check_pe(arguments.pe)
    print("quad ctor sites: PASS (no 24/28-dword copy constructor by rep movsd or movups; "
          f"{len(RET4_COPY_REGIONS)} ret-4 copy regions frozen, none in the sprite chain; "
          "Color(uint32) = 4 floats; layer draw sub_00003e50 -> builders -> Image::Draw slot +0xc "
          "-> +8 -> PushQuad by pointer)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
