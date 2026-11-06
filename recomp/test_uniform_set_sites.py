#!/usr/bin/env python3
"""Frozen-PE proof for ISAAC_VITA_UNIFORM_SET_RECEIPT (and for the decision
not to port the reference project's "ChampionColor uniform" fast path).

Pins every guest fact host_vita_uniform_set_receipt.c relies on and the
census behind the verdict "the KAGE Shader uniform loop is dead in 1.7.9b":

  * the Shader vtable at 0x75e0a8 (Init, Collapse, Bind, EnableAttribs,
    DisableAttribs, SetUniformInt, SetUniform slots);
  * Shader::Bind (sub_005671d0): glUseProgram slot, the NULL-list default
    [this+0x1c], the record vector base [this+0x10] with stride 0x18, the
    two vtable slot calls (+0x1c sampler, +0x20 value), the "Texture0" /
    "Texture1" literals, the projection call and `ret 4`;
  * Shader::ApplyUniforms (sub_00567b50): same loop shape, `ret 4`;
  * Shader::SetUniform (sub_00567490): glGetUniformLocation slot, the
    21-entry type switch, no scan and no allocator call, `ret 0x10`;
  * Shader::SetUniformInt (sub_00567430): glActiveTexture/glBindTexture/
    glGetUniformLocation/glUniform1i slots, `ret 0xc`;
  * Shader::Collapse (sub_005679e0, vtable slot 3, the fifth wrapped root):
    the record type word [record+8] against the sampler base 0x15, the
    values vector [record+0xc..0x10), the CRT free of every stale value and
    the two vector-grow calls, plain `ret`;
  * all five roots have no direct `call rel32` caller in .text (vtable
    only), and the record vector [this+0x10..0x14) is walked by exactly four
    functions in the Shader region, none of which appends;
  * the champion path: the Shader* table at 0x608910 (index 10 =
    "shaders/coloroffset_champion" at 0xbc5044, built by sub_0025a2c0 from
    the attribute table 0x7aa558 whose ninth entry is ChampionColorIn /
    format 4), and the per-layer draw sub_00003e50 swapping the
    current-shader pointer 0xbc7a44 for the champion/dogma/gold layers;
  * the runtime/CMake text that mirrors the offsets and link wraps, the
    option's OFF default, the knob-scoped source wiring and the ph120.us
    format;
  * with --cc, the host oracle host_vita_uniform_set_receipt_oracle.c is
    built against the real receipt TU and must print PASS (64-bit Linux
    host with a GNU-compatible compiler, e.g. --cc gcc on Linux).

usage: python test_uniform_set_sites.py --pe <isaac-ng.exe.unpacked.exe> [--cc gcc]
"""

from __future__ import annotations

import argparse
import hashlib
import re
import shlex
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path

import capstone
import pefile


HERE = Path(__file__).resolve().parent
PE_SIZE = 8_650_240
PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
IMAGE_BASE = 0x00400000

SHADER_VTABLE = 0x0075E0A8
SHADER_INIT = 0x00566F30
SHADER_COLLAPSE = 0x005679E0
SHADER_BIND = 0x005671D0
SHADER_ENABLE_ATTRIBS = 0x005672D0
SHADER_DISABLE_ATTRIBS = 0x005673F0
SHADER_SET_UNIFORM_INT = 0x00567430
SHADER_SET_UNIFORM = 0x00567490
SHADER_APPLY = 0x00567B50
SHADER_DTOR = 0x005676D0
SHADER_CLEAR = 0x005678C0
SHADER_RECORDS_DTOR = 0x00567BF0
SHADER_PROJECTION = 0x00565240
SHADER_REGION = (0x00566000, 0x00568900)

SLOT_GL_USE_PROGRAM = 0x007BF994
SLOT_GL_GET_UNIFORM_LOCATION = 0x007BF9BC
SLOT_GL_UNIFORM_1I = 0x007BF96C
SLOT_GL_UNIFORM_4FV = 0x007BF99C
SLOT_GL_UNIFORM_MATRIX_4FV = 0x007BF9D0
SLOT_GL_ACTIVE_TEXTURE = 0x007C293C
SLOT_GL_BIND_TEXTURE = 0x007C2978

STR_TEXTURE0 = 0x0075E098
STR_TEXTURE1 = 0x0075E03C
STR_TRANSFORM = 0x00744AC0
STR_UNKNOWN_UNIFORM = 0x0075E048
STR_CHAMPION_PATH = 0x00747124

SHADER_TABLE = 0x00608910           # Shader *[20], indexed by the game's shader id
CHAMPION_SHADER = 0x00BC5044        # "shaders/coloroffset_champion" object (VA)
DEFAULT_SHADER = 0x00BC4F3C         # "shaders/coloroffset" object (VA)
CHAMPION_ATTRIBS = 0x007AA558       # (name, format) pairs, terminator (0x6081d4, 0)
CURRENT_SHADER = 0x00BC7A44         # g_CurrentShader (VA)
SHADER_INITIALIZER = 0x0025A2C0
LAYER_DRAW = 0x00003E50

CRT_MALLOC = 0x005EB09C
CRT_FREE = 0x005EACE5
CRT_NEW = 0x005EACF8
CRT_DELETE = 0x005EB08E
CRT_MEMCPY = 0x005EC358
CRT_MEMCPY2 = 0x005EC14C
VECTOR_PTR_GROW = 0x00026FB0        # std::vector<void *> reallocating insert (1.5x, 4-byte elements)


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


def body(pe: pefile.PE, start: int, length: int) -> dict:
    return {i.address: text(i) for i in decode(pe, start, length)}


def slot_call(slot_rva: int) -> str:
    return f"call dword ptr [0x{IMAGE_BASE + slot_rva:x}]"


def direct_call_sites(pe: pefile.PE, target_rva: int) -> list:
    section = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
    code = pe.get_data(section.VirtualAddress, section.Misc_VirtualSize)
    sites = []
    for match in re.finditer(rb"\xe8", code):
        offset = match.start()
        rel = int.from_bytes(code[offset + 1:offset + 5], "little", signed=True)
        if section.VirtualAddress + offset + 5 + rel == target_rva:
            sites.append(section.VirtualAddress + offset)
    return sites


def check_vtable(pe: pefile.PE) -> None:
    slots = struct.unpack("<9I", pe.get_data(SHADER_VTABLE, 36))
    expected = {
        1: SHADER_INIT, 3: SHADER_COLLAPSE, 4: SHADER_BIND,
        5: SHADER_ENABLE_ATTRIBS, 6: SHADER_DISABLE_ATTRIBS,
        7: SHADER_SET_UNIFORM_INT, 8: SHADER_SET_UNIFORM,
    }
    for index, rva in expected.items():
        require(slots[index] == IMAGE_BASE + rva,
                f"Shader vtable slot {index} is {slots[index]:08x}, not {IMAGE_BASE + rva:08x}")


def check_bind(pe: pefile.PE) -> None:
    b = body(pe, SHADER_BIND, 0xF2)
    require(b[IMAGE_BASE + SHADER_BIND] == "push ebp", "Bind prologue")
    require(b[IMAGE_BASE + 0x5671D7] == "mov esi, ecx", "Bind this")
    require(b[IMAGE_BASE + 0x5671D9] == "push dword ptr [esi + 0x28]", "Bind program word")
    require(b[IMAGE_BASE + 0x5671DC] == slot_call(SLOT_GL_USE_PROGRAM), "Bind glUseProgram slot")
    require(b[IMAGE_BASE + 0x5671E2] == "mov eax, dword ptr [ebp + 8]", "Bind list argument")
    require(b[IMAGE_BASE + 0x5671E9] == "lea eax, [esi + 0x1c]", "Bind NULL-list default is not [this+0x1c]")
    require(b[IMAGE_BASE + 0x567205] == "mov ecx, dword ptr [esi + 0x10]", "record vector base is not [this+0x10]")
    require(b[IMAGE_BASE + 0x567210] == "mov eax, dword ptr [ecx + 0x10]", "record values end")
    require(b[IMAGE_BASE + 0x567213] == "mov edx, dword ptr [ecx + 0xc]", "record values begin")
    require(b[IMAGE_BASE + 0x567226] == "mov edx, dword ptr [ecx + 8]", "record type word")
    require(b[IMAGE_BASE + 0x56722C] == "lea eax, [edx - 0x15]", "sampler type window base")
    require(b[IMAGE_BASE + 0x56722F] == "cmp eax, 7", "sampler type window size")
    require(b[IMAGE_BASE + 0x567244] == "call dword ptr [edx + 0x1c]", "record sampler call is not slot 7")
    require(b[IMAGE_BASE + 0x567256] == "call dword ptr [eax + 0x20]", "record value call is not slot 8")
    require(b[IMAGE_BASE + 0x56725D] == "add dword ptr [ebp - 4], 0x18", "record stride is not 0x18")
    require(b[IMAGE_BASE + 0x567282] == f"push 0x{IMAGE_BASE + STR_TEXTURE0:x}", "Texture0 literal")
    require(b[IMAGE_BASE + 0x567289] == "call dword ptr [eax + 0x1c]", "Texture0 goes through slot 7")
    require(b[IMAGE_BASE + 0x5672A9] == f"push 0x{IMAGE_BASE + STR_TEXTURE1:x}", "Texture1 literal")
    require(b[IMAGE_BASE + 0x5672AE] == "call dword ptr [eax + 0x1c]", "Texture1 goes through slot 7")
    require(b[IMAGE_BASE + 0x5672B6] == f"call 0x{IMAGE_BASE + SHADER_PROJECTION:x}", "projection call")
    require(b[IMAGE_BASE + 0x5672BF] == "ret 4", "Bind epilogue")
    require(pe.get_data(STR_TEXTURE0, 9) == b"Texture0\0", "Texture0 bytes")
    require(pe.get_data(STR_TEXTURE1, 9) == b"Texture1\0", "Texture1 bytes")


def check_apply(pe: pefile.PE) -> None:
    b = body(pe, SHADER_APPLY, 0x9B)
    require(b[IMAGE_BASE + SHADER_APPLY] == "push ebp", "ApplyUniforms prologue")
    require(b[IMAGE_BASE + 0x567B5A] == "mov ebx, ecx", "ApplyUniforms this")
    require(b[IMAGE_BASE + 0x567B60] == "lea edi, [ebx + 0x1c]", "ApplyUniforms NULL-list default")
    require(b[IMAGE_BASE + 0x567B78] == "mov ecx, dword ptr [ebx + 0x10]", "ApplyUniforms record base")
    require(b[IMAGE_BASE + 0x567BB7] == "call dword ptr [edx + 0x1c]", "ApplyUniforms sampler slot")
    require(b[IMAGE_BASE + 0x567BC9] == "call dword ptr [eax + 0x20]", "ApplyUniforms value slot")
    require(b[IMAGE_BASE + 0x567BD4] == "add dword ptr [ebp + 8], 0x18", "ApplyUniforms record stride")
    require(b[IMAGE_BASE + 0x567BE8] == "ret 4", "ApplyUniforms epilogue")


def check_set_uniform(pe: pefile.PE) -> None:
    b = body(pe, SHADER_SET_UNIFORM, 0x12E)
    require(b[IMAGE_BASE + SHADER_SET_UNIFORM] == "push ebp", "SetUniform prologue")
    require(b[IMAGE_BASE + 0x567495] == "mov edi, dword ptr [ebp + 8]", "SetUniform name argument")
    require(b[IMAGE_BASE + 0x567499] == "push dword ptr [ecx + 0x28]", "SetUniform program word")
    require(b[IMAGE_BASE + 0x56749C] == slot_call(SLOT_GL_GET_UNIFORM_LOCATION), "SetUniform location slot")
    require(b[IMAGE_BASE + 0x5674A9] == f"push 0x{IMAGE_BASE + STR_UNKNOWN_UNIFORM:x}", "unknown-uniform message")
    require(b[IMAGE_BASE + 0x5674BB] == "ret 0x10", "SetUniform early epilogue")
    require(b[IMAGE_BASE + 0x5674BE] == "mov eax, dword ptr [ebp + 0xc]", "SetUniform type argument")
    require(b[IMAGE_BASE + 0x5674C1] == "cmp eax, 0x14", "SetUniform type bound")
    require(b[IMAGE_BASE + 0x5674CA] == "mov edi, dword ptr [ebp + 0x10]", "SetUniform value argument")
    require(b[IMAGE_BASE + 0x5674CD] == f"jmp dword ptr [eax*4 + 0x{IMAGE_BASE + 0x5675C0:x}]", "type switch")
    require(b[IMAGE_BASE + 0x5674F6] == slot_call(SLOT_GL_UNIFORM_4FV), "vec4 case slot")
    require(b[IMAGE_BASE + 0x5675B2] == slot_call(SLOT_GL_UNIFORM_MATRIX_4FV), "matrix4 case slot")
    require(b[IMAGE_BASE + 0x5675BB] == "ret 0x10", "SetUniform epilogue")
    require(pe.get_data(STR_UNKNOWN_UNIFORM, 31) == b"Unknown uniform '%s' specified\n", "message bytes")
    targets = struct.unpack("<21I", pe.get_data(0x5675C0, 84))
    require(all(IMAGE_BASE + 0x5674D4 <= t <= IMAGE_BASE + 0x5675AC for t in targets),
            "type switch targets left the body")
    # The body's only rel32 call is the log helper; no allocator, no compare loop.
    rel32 = [t for t in b.values() if t.startswith("call 0x")]
    require(rel32 == [f"call 0x{IMAGE_BASE + 0x55E330:x}"], f"SetUniform rel32 calls drifted: {rel32}")


def check_set_uniform_int(pe: pefile.PE) -> None:
    b = body(pe, SHADER_SET_UNIFORM_INT, 0x56)
    require(b[IMAGE_BASE + SHADER_SET_UNIFORM_INT] == "push ebp", "SetUniformInt prologue")
    require(b[IMAGE_BASE + 0x567434] == "mov esi, dword ptr [ebp + 0xc]", "SetUniformInt unit argument")
    require(b[IMAGE_BASE + 0x56743A] == "lea eax, [esi + 0x84c0]", "GL_TEXTURE0 + unit")
    require(b[IMAGE_BASE + 0x567441] == slot_call(SLOT_GL_ACTIVE_TEXTURE), "glActiveTexture slot")
    require(b[IMAGE_BASE + 0x567466] == slot_call(SLOT_GL_BIND_TEXTURE), "glBindTexture slot")
    require(b[IMAGE_BASE + 0x56746D] == "push dword ptr [ebp + 8]", "SetUniformInt name argument")
    require(b[IMAGE_BASE + 0x567473] == slot_call(SLOT_GL_GET_UNIFORM_LOCATION), "SetUniformInt location slot")
    require(b[IMAGE_BASE + 0x56747A] == slot_call(SLOT_GL_UNIFORM_1I), "glUniform1i slot")
    require(b[IMAGE_BASE + 0x567483] == "ret 0xc", "SetUniformInt epilogue")
    require(not any(t.startswith("call 0x") for t in b.values()), "SetUniformInt has a rel32 call")


def check_no_churn(pe: pefile.PE) -> None:
    """None of the four roots (nor the projection helper) reaches the CRT
    allocator or memcpy thunks by direct call."""
    thunks = {f"call 0x{IMAGE_BASE + t:x}" for t in
              (CRT_MALLOC, CRT_FREE, CRT_NEW, CRT_DELETE, CRT_MEMCPY, CRT_MEMCPY2)}
    for start, length in ((SHADER_BIND, 0xF2), (SHADER_APPLY, 0x9B),
                          (SHADER_SET_UNIFORM, 0x12E), (SHADER_SET_UNIFORM_INT, 0x56),
                          (SHADER_PROJECTION, 0x16F)):
        b = body(pe, start, length)
        hits = sorted(t for t in b.values() if t in thunks)
        require(not hits, f"{start:08x} reaches the CRT heap/memcpy: {hits}")
    # The five roots are vtable-only: no direct caller anywhere in .text.
    for root in (SHADER_BIND, SHADER_APPLY, SHADER_SET_UNIFORM, SHADER_SET_UNIFORM_INT,
                 SHADER_COLLAPSE):
        sites = direct_call_sites(pe, root)
        require(sites == [], f"{root:08x} gained direct callers: {[hex(s) for s in sites]}")
    # Exactly the two projection callers inside Bind's family that matter here.
    require(0x5672B6 in direct_call_sites(pe, SHADER_PROJECTION), "Bind no longer calls the projection helper")


def check_record_walkers(pe: pefile.PE) -> None:
    """The uniform-record vector (24-byte elements) is walked by exactly four
    functions in the Shader region: destructor, Clear, per-frame Collapse and
    the vector destructor.  None appends (they only free/collapse), so
    Bind's loop cannot see a record in this PE."""
    start, end = SHADER_REGION
    region = pe.get_data(start, end - start)
    # `mov eax, 0x2aaaaaab` (b8 ab aa aa 2a): the reciprocal MSVC uses for
    # (last - first) / 24 on a std::vector of 24-byte elements.
    walkers = {start + m.start() for m in re.finditer(rb"\xb8\xab\xaa\xaa\x2a", region)}
    require(walkers == {0x567738, 0x5677C9, 0x567919, 0x5679AA, 0x5679EB, 0x567A8F,
                        0x567AC6, 0x567B2C, 0x567C16},
            f"24-byte reciprocal sites in the Shader region drifted: {sorted(hex(w) for w in walkers)}")
    expected_owners = {SHADER_DTOR: (0x5676D0, 0x567808), SHADER_CLEAR: (0x5678C0, 0x5679D4),
                       SHADER_COLLAPSE: (0x5679E0, 0x567B49), SHADER_RECORDS_DTOR: (0x567BF0, 0x567C70)}
    owners = set()
    for site in walkers:
        owner = next((o for o, (lo, hi) in expected_owners.items() if lo <= site < hi), None)
        require(owner is not None, f"24-byte vector walker outside the four known owners: {site:08x}")
        owners.add(owner)
    require(owners == set(expected_owners), f"record walkers drifted: {sorted(hex(o) for o in owners)}")
    # Collapse keeps only the last value of each record (frees the others).
    # host_vita_uniform_set_receipt.c reads the same words at entry.
    c = body(pe, SHADER_COLLAPSE, 0x169)
    require(c[IMAGE_BASE + SHADER_COLLAPSE] == "push ebp", "Collapse prologue")
    require(c[IMAGE_BASE + 0x5679E9] == "mov edi, ecx", "Collapse this")
    require(c[IMAGE_BASE + 0x5679F8] == "mov edx, dword ptr [edi + 0x14]", "Collapse records end")
    require(c[IMAGE_BASE + 0x5679FB] == "mov esi, dword ptr [edi + 0x10]", "Collapse records begin")
    require(c[IMAGE_BASE + 0x567A1A] == "mov eax, dword ptr [edx + 0xc]", "record values begin is not +0xc")
    require(c[IMAGE_BASE + 0x567A1D] == "mov ecx, dword ptr [edx + 0x10]", "record values end is not +0x10")
    require(c[IMAGE_BASE + 0x567A2C] == "cmp dword ptr [edx + 8], 0x15", "record type word / sampler base")
    require(c[IMAGE_BASE + 0x567A3A] == f"jge 0x{IMAGE_BASE + 0x567A6D:x}", "sampler records skip the frees")
    require(c[IMAGE_BASE + 0x567A4F] == f"call 0x{IMAGE_BASE + CRT_FREE:x}", "Collapse frees stale values")
    require(c[IMAGE_BASE + 0x567A84] == f"call 0x{IMAGE_BASE + VECTOR_PTR_GROW:x}", "Collapse value push_back grow")
    require(c[IMAGE_BASE + 0x567A98] == "add dword ptr [ebp - 0xc], 0x18", "Collapse record stride")
    require(c[IMAGE_BASE + 0x567B16] == f"call 0x{IMAGE_BASE + VECTOR_PTR_GROW:x}", "Collapse index push_back grow")
    require(c[IMAGE_BASE + 0x567B48] == "ret", "Collapse epilogue (no stack arguments)")
    rel32 = sorted(set(t for t in c.values() if t.startswith("call 0x")))
    require(rel32 == sorted({f"call 0x{IMAGE_BASE + CRT_FREE:x}", f"call 0x{IMAGE_BASE + VECTOR_PTR_GROW:x}"}),
            f"Collapse rel32 calls drifted: {rel32}")
    # The grow helper is the 4-byte-element vector insert (1.5x growth), i.e.
    # the record's values are pointers, matching the free above.
    g = body(pe, VECTOR_PTR_GROW, 0x30)
    require(g[IMAGE_BASE + 0x26FC7] == "sar ebx, 2", "grow helper element size is not 4")
    require(g[IMAGE_BASE + 0x26FCD] == "cmp eax, 0x3fffffff", "grow helper max_size is not 2^30 pointers")


def check_champion_path(pe: pefile.PE) -> None:
    table = struct.unpack("<20I", pe.get_data(SHADER_TABLE, 80))
    require(table[0] == DEFAULT_SHADER, f"Shader* table index 0 is {table[0]:08x}")
    require(table[10] == CHAMPION_SHADER, f"Shader* table index 10 is {table[10]:08x}")
    # Static initializer: this = 0xbc5044, edx = attribute table, path literal.
    init = body(pe, 0x25A453, 0x14)
    require(init[IMAGE_BASE + 0x25A453] == f"push 0x{IMAGE_BASE + STR_CHAMPION_PATH:x}", "champion path literal")
    require(init[IMAGE_BASE + 0x25A458] == f"mov edx, 0x{IMAGE_BASE + CHAMPION_ATTRIBS:x}", "champion attribute table")
    require(init[IMAGE_BASE + 0x25A45D] == f"mov ecx, 0x{CHAMPION_SHADER:x}", "champion shader object")
    require(init[IMAGE_BASE + 0x25A462] == f"call 0x{IMAGE_BASE + 0x259F30:x}", "shader wrapper constructor")
    require(pe.get_data(STR_CHAMPION_PATH, 29) == b"shaders/coloroffset_champion\0", "champion path bytes")
    # Attribute table: (name VA, format) pairs; ChampionColorIn is a vec4 attribute.
    pairs = struct.unpack("<20I", pe.get_data(CHAMPION_ATTRIBS, 80))
    names = []
    for k in range(0, 20, 2):
        name_va, fmt = pairs[k], pairs[k + 1]
        if fmt == 0:
            break
        raw = pe.get_data(name_va - IMAGE_BASE, 24).split(b"\0")[0].decode()
        names.append((raw, fmt))
    require(names == [("Position", 5), ("Color", 6), ("TexCoord", 7), ("ColorizeIn", 4),
                      ("ColorOffsetIn", 3), ("TextureSize", 2), ("PixelationAmount", 1),
                      ("ClipPlane", 3), ("ChampionColorIn", 4)],
            f"champion attribute layout drifted: {names}")
    # Shader::Init copies the (name, format) pairs into [this+8]/[this+0xc]
    # and EnableAttribs binds every entry: attributes, not uniforms.
    i = body(pe, 0x567117, 0x24)
    require(i[IMAGE_BASE + 0x56711C] == "mov dword ptr [edi + 0xc], 0", "Init attribute count")
    require(i[IMAGE_BASE + 0x567134] == "cmp dword ptr [ebx + eax*8 + 4], 0", "Init pair terminator")
    e = body(pe, SHADER_ENABLE_ATTRIBS, 0x30)
    require(any("[edi + 0xc]" in t for t in e.values()), "EnableAttribs count word")
    # Per-layer draw: champion (index 10), dogma (15), gold (16) layers swap
    # the current-shader pointer and restore index 0; nothing sets a uniform.
    d = {a - IMAGE_BASE: t for a, t in body(pe, LAYER_DRAW, 0x900).items()}
    require(d[0x420C] == f"mov eax, dword ptr [0x{IMAGE_BASE + SHADER_TABLE + 40:x}]", "champion table read")
    require(d[0x4213] == f"mov dword ptr [0x{CURRENT_SHADER:x}], eax", "champion current-shader store")
    require(d[0x4478] == f"mov eax, dword ptr [0x{IMAGE_BASE + SHADER_TABLE:x}]", "default table read")
    require(d[0x447D] == f"mov dword ptr [0x{CURRENT_SHADER:x}], eax", "default restore")
    require(d[0x4490] == f"mov eax, dword ptr [0x{IMAGE_BASE + SHADER_TABLE + 60:x}]", "dogma table read")
    require(d[0x4563] == f"mov eax, dword ptr [0x{IMAGE_BASE + SHADER_TABLE + 64:x}]", "gold table read")
    require(d[0x465B] == f"mov eax, dword ptr [0x{IMAGE_BASE + SHADER_TABLE:x}]", "plain layer table read")
    # The game's own SetShader(id) helper is the same table + global.
    s = body(pe, 0x25A570, 0xD)
    require(s[IMAGE_BASE + 0x25A570] == f"mov eax, dword ptr [ecx*4 + 0x{IMAGE_BASE + SHADER_TABLE:x}]", "SetShader table")
    require(s[IMAGE_BASE + 0x25A577] == f"mov dword ptr [0x{CURRENT_SHADER:x}], eax", "SetShader store")
    # ChampionColorIn has no code reference at all: only the attribute tables.
    section = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
    code = pe.get_data(section.VirtualAddress, section.Misc_VirtualSize)
    require(struct.pack("<I", IMAGE_BASE + 0x746E14) not in code,
            "ChampionColorIn literal gained a code reference")
    require(pe.get_data(0x746E14, 16) == b"ChampionColorIn\0", "ChampionColorIn bytes")


def check_pe(path: Path) -> None:
    data = path.read_bytes()
    require(len(data) == PE_SIZE, f"PE size {len(data)} != {PE_SIZE}")
    require(hashlib.sha256(data).hexdigest() == PE_SHA256, "PE SHA-256 drifted")
    pe = pefile.PE(data=data, fast_load=True)
    check_vtable(pe)
    check_bind(pe)
    check_apply(pe)
    check_set_uniform(pe)
    check_set_uniform_int(pe)
    check_no_churn(pe)
    check_record_walkers(pe)
    check_champion_path(pe)


def check_sources() -> None:
    runtime = (HERE / "runtime" / "host_vita_uniform_set_receipt.c").read_text(encoding="utf-8")
    for symbol in ("sub_005671d0", "sub_00567b50", "sub_00567490", "sub_00567430",
                   "sub_005679e0"):
        require(f"__wrap_{symbol}" in runtime and f"__real_{symbol}" in runtime,
                f"runtime wrap symbols for {symbol}")
    require("VITA_SHADER_RECORD_TYPE_OFFSET     0x08U" in runtime, "record type pin")
    require("VITA_SHADER_RECORD_VALUES_BEGIN    0x0cU" in runtime, "record values begin pin")
    require("VITA_SHADER_RECORD_VALUES_END      0x10U" in runtime, "record values end pin")
    require("VITA_SHADER_SAMPLER_TYPE_BASE      0x15U" in runtime, "sampler type base pin")
    require("VITA_UNIFORM_RECEIPT_MAX_RECORDS   64U" in runtime, "Collapse walk bound")
    require("VITA_SHADER_RECORDS_BEGIN_OFFSET   0x10U" in runtime, "records begin pin")
    require("VITA_SHADER_RECORDS_END_OFFSET     0x14U" in runtime, "records end pin")
    require("VITA_SHADER_RECORD_STRIDE          0x18U" in runtime, "record stride pin")
    require("VITA_SHADER_DEFAULT_LIST_OFFSET    0x1cU" in runtime, "default list pin")
    require(f"VITA_UNIFORM_NAME_TRANSFORM_RVA    0x{STR_TRANSFORM:x}U" in runtime, "Transform literal pin")
    require(f"VITA_UNIFORM_NAME_TEXTURE0_RVA     0x{STR_TEXTURE0:x}U" in runtime, "Texture0 literal pin")
    require(f"VITA_UNIFORM_NAME_TEXTURE1_RVA     0x{STR_TEXTURE1:x}U" in runtime, "Texture1 literal pin")
    require("#error" in runtime, "runtime file must refuse a knob-less compile")
    header = (HERE / "runtime" / "host_vita_uniform_set_receipt.h").read_text(encoding="utf-8")
    require("isaac_vita_uniform_set_receipt_take" in header, "take prototype")
    for field in ("collapses", "collapse_values", "frees"):
        require(f"uint32_t {field};" in header, f"counter field {field}")
    oracle = (HERE / "runtime" / "host_vita_uniform_set_receipt_oracle.c").read_text(encoding="utf-8")
    for symbol in ("sub_005671d0", "sub_00567b50", "sub_00567490", "sub_00567430",
                   "sub_005679e0"):
        require(f"__wrap_{symbol}(&c);" in oracle and f"void __real_{symbol}(" in oracle,
                f"oracle drives and stubs {symbol}")
    require("Vita uniform set receipt oracle: PASS" in oracle, "oracle pass line")
    require("host_vita_uniform_set_receipt_oracle" not in (HERE / "vita" / "CMakeLists.txt").read_text(encoding="utf-8"),
            "the host oracle must never be wired into the Vita build")


ORACLE_DEFINES = (
    "-DGUEST_IMAGE_BASE=0x98000000u", "-DGUEST_STACK_REQUIRED=1",
    "-DGUEST_FLAGS_LOCAL=1", "-DGUEST_COVERAGE_HOOKS=0",
    "-DISAAC_VITA_UNIFORM_SET_RECEIPT=1",
)


def run_oracle(cc: str) -> None:
    """Builds host_vita_uniform_set_receipt_oracle.c against the real receipt
    TU with the production defines and requires the PASS line."""
    require(shutil.which(shlex.split(cc)[0]) is not None, f"compiler not found: {cc}")
    runtime = HERE / "runtime"
    with tempfile.TemporaryDirectory(prefix="uniform-receipt-oracle-") as tmp:
        exe = Path(tmp) / "uniform_receipt_oracle"
        command = [*shlex.split(cc), "-std=gnu11", "-O1", "-Wall", "-Wextra", "-Werror",
                   "-I", str(runtime), *ORACLE_DEFINES,
                   str(runtime / "host_vita_uniform_set_receipt_oracle.c"),
                   str(runtime / "host_vita_uniform_set_receipt.c"), "-o", str(exe)]
        build = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                               text=True, errors="replace", check=False)
        require(build.returncode == 0, f"oracle build failed:\n{build.stdout}")
        result = subprocess.run([str(exe)], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, errors="replace", check=False)
        require(result.returncode == 0 and "Vita uniform set receipt oracle: PASS" in result.stdout,
                f"oracle run failed ({result.returncode}):\n{result.stdout}")
    cmake = (HERE / "vita" / "CMakeLists.txt").read_text(encoding="utf-8")
    require("option(ISAAC_VITA_UNIFORM_SET_RECEIPT" in cmake, "CMake option")
    require("ISAAC_VITA_UNIFORM_SET_RECEIPT requires the translated KAGE runtime" in cmake, "CMake guard")
    require('"${ISAAC_RUNTIME}/host_vita_uniform_set_receipt.c")' in cmake, "CMake source wiring")
    require("-Wl,--wrap=sub_005671d0 -Wl,--wrap=sub_00567b50" in cmake, "CMake wraps (Bind/Apply)")
    require("-Wl,--wrap=sub_00567490 -Wl,--wrap=sub_00567430" in cmake, "CMake wraps (setters)")
    require("-Wl,--wrap=sub_005679e0)" in cmake, "CMake wrap (Collapse)")
    require(cmake.count("--wrap=sub_005679e0") == 1, "Collapse wrapped exactly once")
    # Default OFF, and the only source wiring sits inside if(<knob>).
    require(re.search(r'option\(ISAAC_VITA_UNIFORM_SET_RECEIPT\s+"[^"]*"\s+OFF\)', cmake),
            "ISAAC_VITA_UNIFORM_SET_RECEIPT must default to OFF")
    wiring = cmake.index('"${ISAAC_RUNTIME}/host_vita_uniform_set_receipt.c")')
    opening = cmake.rfind("\n  if(", 0, wiring)
    require(opening >= 0 and cmake[opening:].startswith("\n  if(ISAAC_VITA_UNIFORM_SET_RECEIPT)\n"),
            "receipt source wiring is not scoped by if(ISAAC_VITA_UNIFORM_SET_RECEIPT)")
    require(cmake.count("host_vita_uniform_set_receipt.c") == 4,
            "receipt TU referenced outside its knob block")
    release = (HERE.parent / "release" / "v0.1.1-alpha.cmake").read_text(encoding="utf-8")
    require("ISAAC_VITA_UNIFORM_SET_RECEIPT" not in release, "release knob set must not enable the receipt")
    profile = (HERE / "runtime" / "kage_vita_phase_profile.c").read_text(encoding="utf-8")
    require("ph120.us bid=%.32s" in profile, "ph120.us emitter")
    require("c(n,v,f)=%u,%u,%u bad=%u" in profile, "ph120.us Collapse fields")
    require(profile.count("#if defined(ISAAC_VITA_UNIFORM_SET_RECEIPT)") == 5,
            "ph120.us hooks (include, emitter, window, max, discard)")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--pe", required=True, type=Path,
                        help="frozen isaac-ng.exe.unpacked.exe")
    parser.add_argument("--cc", default=None,
                        help="host C compiler command line; builds and runs the receipt oracle")
    args = parser.parse_args()
    check_pe(args.pe)
    check_sources()
    if args.cc:
        run_oracle(args.cc)
    print("uniform-set sites: OK (Shader vtable 0x75e0a8; Bind 005671d0, "
          "ApplyUniforms 00567b50, SetUniform 00567490, SetUniformInt 00567430, "
          "Collapse 005679e0 vtable-only, setters allocate nothing; record "
          "vector walked by 4 owners, none appends; ChampionColorIn = "
          "attribute 9 of table 0x7aa558; knob default OFF, wiring knob-scoped"
          + ("; host oracle PASS" if args.cc else "") + ")")


if __name__ == "__main__":
    main()
