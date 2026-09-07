#!/usr/bin/env python3
"""Proofs for the ImageManager sprite-sheet retention seam
(recomp/runtime/host_vita_image_retain.c, ISAAC_VITA_IMAGE_RETAIN).

  1. PE pins: the frozen release observer sub_0056d8c0 (0x39 bytes, its three
     return words, its `call 0x56d220`, the ctrl lock/strong offsets it uses),
     ImageManager::Unregister's `ret 4`, the two observer-global installs at
     0x5bfb78/0x5bfb84, the Image vtables' loader slots, the control-block
     vtable, the Image field offsets the loader / base ctor / Unregister use.
  2. Header cross-check: every ISAAC_IR_* constant in host_vita_image_retain.h
     equals the value decoded from the PE.
  3. Corpus pins (--generated): the observer is defined exactly once and has
     no direct `sub_0056d8c0(c)` caller anywhere (so --wrap catches every
     edge), the four guest_0000.c release sites reach it through the observer
     global + guest_call, the only direct sub_0056d220 callers are the
     observer itself and the two compiler-devirtualised observer copies in
     the installing function, and the field-offset instructions are present.
  4. Host oracle (ILP32): vita_image_retain_oracle.c drives the unmodified
     module over a fake identity-mapped ImageManager in both stack variants
     (inline / GUEST_STACK_REQUIRED) at -O2 and -O0.
  5. ARM cross compile (--arm-cc): the module with the eboot flags, -Werror,
     nm contract (T __wrap_sub_0056d8c0, U __real_sub_0056d8c0, no libc
     allocator or printf references).

Usage: test_vita_image_retain.py --pe <frozen.exe> [--generated DIR]
         [--cc "clang -m32"] [--arm-cc arm-vita-eabi-gcc] [--keep DIR]
"""

from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import re
import shlex
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
RUNTIME = HERE / "runtime"
VITA = HERE / "vita"
MODULE = RUNTIME / "host_vita_image_retain.c"
HEADER = RUNTIME / "host_vita_image_retain.h"
ORACLE = RUNTIME / "vita_image_retain_oracle.c"

PE_SIZE = 8_650_240
PE_SHA256 = "31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404"
PE_IMAGE_BASE = 0x400000

HOOK_RVA = 0x56D8C0
HOOK_BYTES = bytes.fromhex(
    "558bec538b5d08568b730485f674268b4608578d7e086aff8bcfff500c8b07"
    "8bcf0fb77604ff50105f83fe017507ff33e82bf9ffff5e5b5dc3")
HOOK_SHA256 = "fa9908587e394213909049acf8ba5f22a40c349c112b6c4c744586a92abb438f"
UNREGISTER_RVA = 0x56D220
UNREGISTER_RET4_RVA = 0x56D497
INSTALL_RVA = (0x5BFB78, 0x5BFB84)
GLOBAL_RVA = (0x7FD62C, 0x803D00)
BUCKETS_PTR_RVA = 0x7E7D00
VT_RVA = {"PNG": 0x766028, "PCX": 0x765E90, "PIC": 0x7662F0}
VT_LOADER_RVA = {"PNG": 0x5A0C50, "PCX": 0x5A0630, "PIC": 0x5A0630}
CTRL_VT_RVA = 0x608360
CTRL_VT_SLOTS = {4: 0x7B70, 8: 0x7B50, 0xC: 0x7AF0}   # TryAddRef, AddRef, Release
ORACLE_PASS = "IMAGE RETAIN ORACLE PASS: checks=644 scenarios=66 records=1024"
# The game's Mutex class: vtable (+0xc Enter sub_00562e00, +0x10 Leave
# sub_00562ec0), {vt, u8 init @+4, CRITICAL_SECTION* @+8}, held byte at
# CS+0x18.  Enter = EnterCriticalSection; while (held) Sleep(1000); held = 1.
MUTEX_VT_RVA = 0x75D648
MUTEX_ENTER_RVA = 0x562E00
MUTEX_LEAVE_RVA = 0x562EC0
# Unregister's erase loop / tail observer sites: mov eax,[0x7fd62c]; call eax;
# add esp,4 at 0x56d41a/0x56d427/0x56d429 and 0x56d45d/0x56d467/0x56d469.
UNREGISTER_SHIFT_OBSERVER_RVA = 0x56D41A
UNREGISTER_TAIL_OBSERVER_RVA = 0x56D45D

EBOOT_FLAGS = [
    "-std=gnu11", "-O2", "-mcpu=cortex-a9", "-mfpu=neon",
    "-mfloat-abi=softfp", "-mthumb", "-fno-strict-aliasing",
    "-ffunction-sections", "-fdata-sections", "-Wall", "-Wextra", "-Werror",
    "-DGUEST_IMAGE_BASE=0x98000000u", "-DGUEST_STACK_REQUIRED=1",
    "-DISAAC_VITA_HAS_RUNTIME=1", "-DISAAC_VITA_HEAP_MB=64",
    "-DISAAC_VITA_RAW_ALLOCATOR_GATE=1",
]
RETAIN_DEFS = [
    "-DISAAC_VITA_IMAGE_RETAIN=1", "-DISAAC_VITA_IMAGE_RETAIN_WRAP=1",
    "-DISAAC_VITA_IMAGE_RETAIN_BUDGET_MB=48",
    "-DISAAC_VITA_IMAGE_RETAIN_HEADROOM_MB=64",
    '-DISAAC_VITA_IMAGE_RETAIN_BUILD_ID="image-retain:test"',
]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def sha256_bytes(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def run(command: list[str], cwd: Path | None = None) -> str:
    result = subprocess.run(command, check=False, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, cwd=cwd)
    if result.returncode != 0:
        raise AssertionError(
            f"command failed ({result.returncode}): {' '.join(command)}\n"
            f"{result.stdout}")
    return result.stdout


# --------------------------------------------------------------------- PE ---

class PE:
    def __init__(self, path: Path) -> None:
        self.data = path.read_bytes()
        require(len(self.data) == PE_SIZE, "frozen PE size changed")
        require(sha256_bytes(self.data) == PE_SHA256, "frozen PE hash changed")
        pe = struct.unpack_from("<I", self.data, 0x3C)[0]
        nsec = struct.unpack_from("<H", self.data, pe + 6)[0]
        optsz = struct.unpack_from("<H", self.data, pe + 20)[0]
        self.image_base = struct.unpack_from("<I", self.data, pe + 24 + 28)[0]
        require(self.image_base == PE_IMAGE_BASE, "PE ImageBase changed")
        self.sections = []
        first = pe + 24 + optsz
        for index in range(nsec):
            entry = first + index * 40
            name = self.data[entry:entry + 8].rstrip(b"\0").decode()
            vsize, va, rsize, roff = struct.unpack_from(
                "<IIII", self.data, entry + 8)
            self.sections.append((name, va, vsize, roff, rsize))
        self.text = next(s for s in self.sections if s[0] == ".text")
        self.rdata = next(s for s in self.sections if s[0] == ".rdata")
        self.dat = next(s for s in self.sections if s[0] == ".data")

    def offset(self, rva: int) -> int:
        for _name, va, _vsize, roff, rsize in self.sections:
            if va <= rva < va + rsize:
                return roff + rva - va
        raise AssertionError(f"rva {rva:#x} has no file bytes")

    def read(self, rva: int, size: int) -> bytes:
        off = self.offset(rva)
        return self.data[off:off + size]

    def u32(self, rva: int) -> int:
        return struct.unpack_from("<I", self.data, self.offset(rva))[0]

    def in_text(self, va: int) -> bool:
        _name, base, vsize, _roff, _rsize = self.text
        return self.image_base + base <= va < self.image_base + base + vsize

    def in_bss(self, rva: int) -> bool:
        _name, va, vsize, _roff, rsize = self.dat
        return va + rsize <= rva < va + vsize


def verify_pe(pe: PE) -> dict[str, int]:
    facts: dict[str, int] = {}
    hook = pe.read(HOOK_RVA, len(HOOK_BYTES))
    require(hook == HOOK_BYTES, "observer body bytes changed")
    require(sha256_bytes(hook) == HOOK_SHA256, "observer body hash changed")
    # push ebp / mov ebp,esp / push ebx / mov ebx,[ebp+8] / push esi /
    # mov esi,[ebx+4] / test esi,esi / je +0x26
    require(hook[0x00:0x0F] == bytes.fromhex("558bec538b5d08568b730485f67426"),
            "observer prologue changed")
    require(hook[0x0F:0x12] == bytes.fromhex("8b4608"), "ctrl lock vtable load (esi+8)")
    require(hook[0x13:0x16] == bytes.fromhex("8d7e08"), "ctrl lock this (esi+8)")
    require(hook[0x16:0x18] == bytes.fromhex("6aff"), "push -1 before Enter")
    require(hook[0x1A:0x1D] == bytes.fromhex("ff500c"), "call [vt+0xc] Enter")
    require(hook[0x21:0x25] == bytes.fromhex("0fb77604"), "movzx esi, word [ctrl+4] strong")
    require(hook[0x25:0x28] == bytes.fromhex("ff5010"), "call [vt+0x10] Leave")
    require(hook[0x29:0x2C] == bytes.fromhex("83fe01"), "cmp esi, 1")
    require(hook[0x2E:0x30] == bytes.fromhex("ff33"), "push [pair] (Image*)")
    require(hook[0x30] == 0xE8, "call rel32 Unregister")
    rel = struct.unpack_from("<i", hook, 0x31)[0]
    facts["UNREGISTER_RVA"] = (HOOK_RVA + 0x30 + 5 + rel) & 0xFFFFFFFF
    require(facts["UNREGISTER_RVA"] == UNREGISTER_RVA, "observer's Unregister target changed")
    facts["HOOK_RVA"] = HOOK_RVA
    facts["HOOK_BYTES"] = len(HOOK_BYTES)
    facts["ENTER_SITE_RVA"] = HOOK_RVA + 0x1D
    facts["LEAVE_SITE_RVA"] = HOOK_RVA + 0x28
    facts["UNREGISTER_SITE_RVA"] = HOOK_RVA + 0x35
    require(hook[0x35:0x39] == bytes.fromhex("5e5b5dc3"), "observer epilogue changed")
    facts["CTRL_STRONG"] = hook[0x24]          # disp8 of movzx esi, word [esi+4]
    facts["CTRL_LOCK"] = hook[0x11]            # disp8 of mov eax, [esi+8]
    facts["LOCK_VT_ENTER"] = hook[0x1C]        # disp8 of call [eax+0xc]
    facts["LOCK_VT_LEAVE"] = hook[0x27]        # disp8 of call [eax+0x10]

    require(pe.read(UNREGISTER_RVA, 8) == bytes.fromhex("538bdc83ec0883e4"),
            "Unregister prologue changed")
    require(pe.read(UNREGISTER_RET4_RVA, 3) == bytes.fromhex("c20400"),
            "Unregister `ret 4` moved")
    # Unregister's render-target list tests: mov eax,[ecx+0x10]; and eax,4 /
    # and eax,0x10 (0x56d275 / 0x56d2b6)
    require(pe.read(0x56D275, 6) == bytes.fromhex("8b411083e004"), "Unregister flags&4 test")
    require(pe.read(0x56D2B6, 6) == bytes.fromhex("8b411083e010"), "Unregister flags&0x10 test")
    facts["IMG_FLAGS"] = pe.read(0x56D275, 3)[2]

    for install, expected_global in zip(INSTALL_RVA, GLOBAL_RVA):
        code = pe.read(install, 10)
        require(code[:2] == bytes.fromhex("c705"), f"install at {install:#x} is not mov [abs], imm32")
        target = struct.unpack_from("<I", code, 2)[0] - pe.image_base
        value = struct.unpack_from("<I", code, 6)[0] - pe.image_base
        require(target == expected_global, f"observer global at {install:#x} moved")
        require(value == HOOK_RVA, f"install at {install:#x} stores another observer")
        require(pe.in_bss(target), f"observer global {target:#x} is not zero-initialised")
    facts["HOOK_GLOBAL_RVA"], facts["HOOK_GLOBAL2_RVA"] = GLOBAL_RVA
    require(pe.u32(BUCKETS_PTR_RVA) == 0, "bucket table pointer is not zero in .data")
    facts["BUCKETS_PTR_RVA"] = BUCKETS_PTR_RVA
    facts["MANAGER_LOCK_RVA"] = BUCKETS_PTR_RVA + 4

    for name, vt in VT_RVA.items():
        loader = pe.u32(vt + 0x68) - pe.image_base
        require(loader == VT_LOADER_RVA[name], f"{name} vtable +0x68 (LoadFromFile) changed")
        for slot in range(0, 0x70, 4):
            require(pe.in_text(pe.u32(vt + slot)), f"{name} vtable slot {slot:#x} outside .text")
        facts[f"VT_{name}_RVA"] = vt
    require(pe.u32(VT_RVA["PNG"] + 0x64) - pe.image_base == 0x5A0CD0,
            "PNG vtable +0x64 (LoadFromMemory) changed")
    for slot, target in CTRL_VT_SLOTS.items():
        require(pe.u32(CTRL_VT_RVA + slot) - pe.image_base == target,
                f"control-block vtable slot {slot:#x} changed")
    facts["CTRL_VT_RVA"] = CTRL_VT_RVA

    # Loader stores: mov word [esi+0x84], ax (0x5a11c7); mov dword
    # [esi+0x88], eax (0x5a1143); base ctor: mov dword [esi+0x78], -1
    # (0x5b080b) then mov dword [esi+0x7c], 0 (0x5b0812).
    require(pe.read(0x5A11C7, 7) == bytes.fromhex("66898684000000"), "padded width store")
    require(pe.read(0x5A1143, 6) == bytes.fromhex("898688000000"), "bpp store")
    require(pe.read(0x5B080B, 7) == bytes.fromhex("c74678ffffffff"), "base ctor tex0 = -1")
    require(pe.read(0x5B0812, 7) == bytes.fromhex("c7467c00000000"), "base ctor tex1 = 0")
    facts["IMG_PW"] = struct.unpack_from("<I", pe.read(0x5A11C7, 7), 3)[0]
    facts["IMG_PH"] = facts["IMG_PW"] + 2
    facts["IMG_BPP"] = struct.unpack_from("<I", pe.read(0x5A1143, 6), 2)[0]
    facts["IMG_TEX0"] = pe.read(0x5B080B, 3)[2]
    facts["IMG_TEX1"] = pe.read(0x5B0812, 3)[2]
    # Load's strcmp key: mov eax,[eax+0x3c] (0x56c8b6: 8b 40 3c)
    require(pe.read(0x56C8B6, 3) == bytes.fromhex("8b403c"), "Load name field load")
    facts["IMG_NAME"] = pe.read(0x56C8B6, 3)[2]

    # The Mutex class the observer replays and the manager lock uses.  Exactly
    # one vtable in the PE has Enter at +0xc and Leave at +0x10.
    enter_word = struct.pack("<I", pe.image_base + MUTEX_ENTER_RVA)
    leave_word = struct.pack("<I", pe.image_base + MUTEX_LEAVE_RVA)
    _rn, rva0, _rvs, roff0, rsize0 = pe.rdata
    rdata = pe.data[roff0:roff0 + rsize0]
    vtables = [rva0 + i - 0xC for i in range(0, len(rdata) - 8, 4)
               if rdata[i:i + 4] == enter_word and rdata[i + 4:i + 8] == leave_word]
    require(vtables == [MUTEX_VT_RVA], f"Mutex vtable candidates changed: {list(map(hex, vtables))}")
    for slot in range(0, 0x14, 4):
        require(pe.in_text(pe.u32(MUTEX_VT_RVA + slot)), f"Mutex vtable slot {slot:#x} outside .text")
    facts["MUTEX_VT_RVA"] = MUTEX_VT_RVA
    facts["MUTEX_ENTER_RVA"] = MUTEX_ENTER_RVA
    facts["MUTEX_LEAVE_RVA"] = MUTEX_LEAVE_RVA
    # Enter: cmp byte [ebx+4],0 (init) / mov esi,[ebx+8] (CS) / push esi;
    # call [EnterCriticalSection] / cmp byte [esi+0x18],0 ; je / mov edi,
    # [Sleep] ... push 0x3e8; call edi ; loop / mov byte [esi+0x18],1.
    require(pe.read(0x562E08, 4) == bytes.fromhex("807b0400"), "Mutex init byte test")
    require(pe.read(0x562E25, 3) == bytes.fromhex("8b7308"), "Mutex CS pointer load")
    require(pe.read(0x562E28, 7) == bytes.fromhex("56ff15fc60a000"), "EnterCriticalSection call")
    require(pe.read(0x562E2F, 6) == bytes.fromhex("807e18007418"), "held byte test + je")
    require(pe.read(0x562E35, 6) == bytes.fromhex("8b3d3861a000"), "Sleep import load")
    require(pe.read(0x562E40, 7) == bytes.fromhex("68e8030000ffd7"), "push 1000; call Sleep")
    require(pe.read(0x562E47, 6) == bytes.fromhex("807e180075f3"), "held spin loop")
    require(pe.read(0x562E4D, 4) == bytes.fromhex("c6461801"), "held = 1")
    # Leave: mov eax,[esi+8]; push eax; mov byte [eax+0x18],0; call [Leave..]
    require(pe.read(0x562ED8, 11) == bytes.fromhex("8b460850c6401800ff15f8"), "Mutex Leave body")
    facts["MUTEX_INIT"] = pe.read(0x562E08, 4)[2]
    facts["MUTEX_CS"] = pe.read(0x562E25, 3)[2]
    facts["CS_HELD"] = pe.read(0x562E2F, 4)[2]
    # Unregister's erase loop and tail call the observer for every shifted
    # entry (the state the OFF build never reaches with strong == 1).
    for site in (UNREGISTER_SHIFT_OBSERVER_RVA, UNREGISTER_TAIL_OBSERVER_RVA):
        code = pe.read(site, 0x12)
        require(code[:5] == bytes.fromhex("a12cd6bf00"), f"{site:#x}: mov eax,[0x7fd62c]")
        require(code[5:9] == bytes.fromhex("85c07409") or code[5:9] == bytes.fromhex("85c07406"),
                f"{site:#x}: test eax,eax; je")
    require(pe.read(0x56D427, 5) == bytes.fromhex("ffd083c404"), "shift-loop call eax; add esp,4")
    require(pe.read(0x56D467, 5) == bytes.fromhex("ffd083c404"), "tail call eax; add esp,4")
    facts["UNREGISTER_SHIFT_SITE_RVA"] = 0x56D429
    facts["UNREGISTER_TAIL_SITE_RVA"] = 0x56D469
    # Unregister takes the manager Mutex first: mov eax,[0x7e7d04]; mov ecx,
    # 0x7e7d04; push -1; call [eax+0xc] at 0x56d25b.
    require(pe.read(0x56D25B, 15) == bytes.fromhex("a1047dbe00b9047dbe006affff500c"),
            "Unregister manager Mutex Enter")
    print(f"PE pins: observer {HOOK_RVA:#x} sha256={HOOK_SHA256[:16]} "
          f"sites={facts['ENTER_SITE_RVA']:#x}/{facts['LEAVE_SITE_RVA']:#x}/"
          f"{facts['UNREGISTER_SITE_RVA']:#x} unregister={UNREGISTER_RVA:#x} "
          f"ret4@{UNREGISTER_RET4_RVA:#x} globals={GLOBAL_RVA[0]:#x}/{GLOBAL_RVA[1]:#x} "
          f"vt={VT_RVA['PNG']:#x}/{VT_RVA['PCX']:#x}/{VT_RVA['PIC']:#x} "
          f"ctrl_vt={CTRL_VT_RVA:#x} mutex_vt={MUTEX_VT_RVA:#x} "
          f"enter/leave={MUTEX_ENTER_RVA:#x}/{MUTEX_LEAVE_RVA:#x} held=cs+0x18 "
          f"shift/tail observer sites={UNREGISTER_SHIFT_OBSERVER_RVA:#x}/"
          f"{UNREGISTER_TAIL_OBSERVER_RVA:#x}: PASS")
    return facts


def verify_header(facts: dict[str, int]) -> None:
    text = HEADER.read_text(encoding="utf-8")
    defines = {m.group(1): int(m.group(2), 16) for m in re.finditer(
        r"^#define ISAAC_IR_(\w+)\s+0x([0-9A-Fa-f]+)U", text, re.M)}
    checked = 0
    for key, value in facts.items():
        require(key in defines, f"header lacks ISAAC_IR_{key}")
        require(defines[key] == value,
                f"ISAAC_IR_{key} = {defines[key]:#x} but the PE says {value:#x}")
        checked += 1
    require(defines["TEX_UNSET"] == 0xFFFFFFFF, "TEX_UNSET must be the base ctor value")
    print(f"header cross-check: {checked} ISAAC_IR_* constants equal the PE: PASS")


# ----------------------------------------------------------------- corpus ---

def verify_generated(generated: Path) -> None:
    units = sorted(generated.glob("guest_*.c"))
    require(len(units) > 100, f"{generated} does not look like a generated corpus")
    owner = []
    direct = []
    unregister_sites: set[int] = set()
    call_re = re.compile(r"GPUSH\(0x([0-9a-f]+)U\);[^\n]*sub_0056d220\(c\);")
    for unit in units:
        text = unit.read_text(encoding="utf-8", errors="replace")
        if re.search(r"^void sub_0056d8c0\(CPU \*__restrict c\)$", text, re.M):
            owner.append(unit.name)
        if re.search(r"(?<![A-Za-z0-9_])sub_0056d8c0\(c\)", text):
            direct.append(unit.name)
        for m in call_re.finditer(text):
            unregister_sites.add(int(m.group(1), 16))
    require(owner == ["guest_0168.c"], f"observer owner changed: {owner}")
    require(direct == [], f"direct sub_0056d8c0(c) call would bypass --wrap: {direct}")
    require(unregister_sites == {0x56D8F5, 0x5BFC46, 0x5BFCA6},
            f"direct Unregister callers changed: {sorted(map(hex, unregister_sites))}")

    hook_unit = (generated / "guest_0168.c").read_text(encoding="utf-8")
    body = hook_unit[hook_unit.index("void sub_0056d8c0(CPU *__restrict c)"):]
    body = body[:body.index("\n}\n")]
    for needle in ("/* 0056d8c0  push ebp */", "GPUSH(0xffffffffU);",
                   "GPUSH(0x56d8ddU);", "GPUSH(0x56d8e8U);", "GPUSH(0x56d8f5U);",
                   "ld16((uint32_t)(GR(esi) + 0x4U))", "GR(esi) + 0x8U",
                   "GR(eax) + 0xcU", "GR(eax) + 0x10U", "sub_0056d220(c);",
                   "/* 0056d8f8  ret  */"):
        require(needle in body, f"observer body lost {needle!r}")
    require(body.count("guest_call(") == 2, "observer must make exactly two indirect calls")
    for needle in ("/* 0056d497  ret 4 */", "/* 0056d278  and eax, 4 */",
                   "/* 0056d2b9  and eax, 0x10 */",
                   "ld32((uint32_t)(0x987e7d00U))",
                   "st32((uint32_t)(GR(esi)), (uint32_t)(0x98766028U));",
                   "st32((uint32_t)(GR(esi)), (uint32_t)(0x98765e90U));",
                   "st32((uint32_t)(GR(esi)), (uint32_t)(0x987662f0U));",
                   # Unregister: manager Mutex Enter/Leave, the erase loop's
                   # copy (0x78a0) + Release + observer call with &temp, the
                   # tail's Release + observer call with the last entry.
                   "/* 0056d25b  mov eax, dword ptr [0x987e7d04] */",
                   "/* 0056d3e4  call 0x78a0 */",
                   "/* 0056d41a  mov eax, dword ptr [0x987fd62c] */",
                   "GPUSH(0x56d429U);",
                   "/* 0056d45d  mov eax, dword ptr [0x987fd62c] */",
                   "GPUSH(0x56d469U);",
                   "/* 0056d481  call dword ptr [eax + 0x10] */",
                   # bucket growth destroys the old entries through the observer
                   "/* 0056db39  mov eax, dword ptr [0x987fd62c] */"):
        require(needle in hook_unit, f"guest_0168.c lost {needle!r}")
    unregister = hook_unit[hook_unit.index("void sub_0056d220(CPU *__restrict c)"):]
    unregister = unregister[:unregister.index("\n}\n")]
    require(unregister.count("ld32((uint32_t)(0x987fd62cU))") == 2,
            "Unregister must reach the observer exactly twice (erase loop + tail)")
    mutex_unit = (generated / "guest_0166.c").read_text(encoding="utf-8")
    for needle in ("void sub_00562e00(CPU *__restrict c)",
                   "/* 00562e08  cmp byte ptr [ebx + 4], 0 */",
                   "/* 00562e25  mov esi, dword ptr [ebx + 8] */",
                   "/* 00562e2f  cmp byte ptr [esi + 0x18], 0 */",
                   "/* 00562e40  push 0x3e8 */",
                   "/* 00562e4d  mov byte ptr [esi + 0x18], 1 */",
                   "void sub_00562ec0(CPU *__restrict c)",
                   "/* 00562edc  mov byte ptr [eax + 0x18], 0 */"):
        require(needle in mutex_unit, f"guest_0166.c lost {needle!r}")

    table = (generated / "guest_table.c").read_text(encoding="utf-8")
    require(len(re.findall(r"(?<![A-Za-z0-9_])sub_0056d8c0(?![A-Za-z0-9_])", table)) == 1,
            "guest_table.c must reference the observer exactly once")

    refcount = (generated / "guest_0000.c").read_text(encoding="utf-8")
    require("sub_0056d8c0" not in refcount,
            "guest_0000.c gained a symbol reference to the observer")
    lines = refcount.splitlines()
    for site in (0x2978, 0x2AC5, 0x3610, 0x43A5):
        marker = f"/* {site:08x}  mov eax, dword ptr [0x987fd62c] */"
        index = next((i for i, line in enumerate(lines) if marker in line), None)
        require(index is not None, f"release site {site:#x} lost its observer-global load")
        window = "\n".join(lines[index:index + 16])
        require("call eax */" in window and "guest_call(c, _target);" in window,
                f"release site {site:#x} no longer calls the observer through guest_call")

    install_units = [u.name for u in units
                     if "/* 005bfb78  mov dword ptr [0x987fd62c], 0x9856d8c0 */"
                     in u.read_text(encoding="utf-8", errors="replace")]
    # The install + the two devirtualised observer copies live in a code tail
    # (0x5bfb.. / 0x5bfc..) shared by sub_005bf8f0 (guest_0177.c) and the
    # split function sub_00598c80 (guest_0172.c); both units carry it.
    require(sorted(install_units) == ["guest_0172.c", "guest_0177.c"],
            f"observer install units changed: {install_units}")
    for unit_name, owner_fn in (("guest_0172.c", "sub_00598c80"),
                                ("guest_0177.c", "sub_005bf8f0")):
        text = (generated / unit_name).read_text(encoding="utf-8")
        require("/* 005bfb84  mov dword ptr [0x98803d00], 0x9856d8c0 */" in text,
                f"{unit_name}: second observer install missing")
        require(f"void {owner_fn}(CPU *__restrict c)" in text,
                f"{unit_name}: install tail owner {owner_fn} missing")
        require(text.count("!= (0x9856d8c0U))") == 2,
                f"{unit_name}: devirtualised observer copies changed")

    loader = (generated / "guest_0173.c").read_text(encoding="utf-8")
    for needle in ("/* 005a11c7  mov word ptr [esi + 0x84], ax */",
                   "/* 005a1143  mov dword ptr [esi + 0x88], eax */"):
        require(needle in loader, f"guest_0173.c lost {needle!r}")
    base_ctor = (generated / "guest_0175.c").read_text(encoding="utf-8")
    require("/* 005b080b  mov dword ptr [esi + 0x78], 0xffffffff */" in base_ctor,
            "guest_0175.c lost the base ctor tex0 store")
    print(f"corpus pins ({generated}): owner=guest_0168.c direct_calls=0 "
          f"table_refs=1 release_sites=4(indirect) unregister_callers="
          f"{{0x56d8f5,0x5bfc46,0x5bfca6}} installs={install_units}: PASS")


# ----------------------------------------------------------------- oracle ---

def default_cc() -> list[str]:
    if os.environ.get("CC"):
        return shlex.split(os.environ["CC"])
    for candidate in ("clang", "gcc", "cc"):
        found = shutil.which(candidate)
        if found:
            return [found, "-m32"]
    if os.name == "nt":
        installed = Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "LLVM" / "bin" / "clang.exe"
        if installed.is_file():
            return [str(installed), "-m32"]
    raise AssertionError("no ILP32-capable host compiler found; pass --cc \"clang -m32\"")


def run_oracle(cc: list[str], work: Path) -> None:
    for required in (0, 1):
        for opt in ("-O2", "-O0"):
            exe = work / f"ir-oracle-s{required}{opt}{'.exe' if os.name == 'nt' else ''}"
            command = [*cc, "-std=c11", opt, "-Wall", "-Wextra", "-Werror",
                       "-DISAAC_VITA_IMAGE_RETAIN=1",
                       "-DISAAC_VITA_IMAGE_RETAIN_ORACLE=1",
                       "-DGUEST_IMAGE_BASE=0x98000000U",
                       f"-DGUEST_STACK_REQUIRED={required}",
                       "-I", str(RUNTIME), str(MODULE), str(ORACLE), "-o", str(exe)]
            if os.name != "nt":
                command.append("-no-pie")
            run(command)
            out = run([str(exe)]).strip()
            require(out == ORACLE_PASS, f"oracle (stack={required} {opt}) said: {out}")
            print(f"host oracle stack_required={required} {opt}: {out}")


# -------------------------------------------------------------------- ARM ---

def arm_cross(arm_cc: str, work: Path) -> None:
    obj = work / "host_vita_image_retain.o"
    run([arm_cc, *EBOOT_FLAGS, *RETAIN_DEFS,
         "-include", str(VITA / "isaac_vita_raw_allocator_poison.h"),
         "-I", str(RUNTIME), "-I", str(VITA), "-c", str(MODULE), "-o", str(obj)])
    prefix = arm_cc[:-len("gcc")] if arm_cc.endswith("gcc") else ""
    nm = run([prefix + "nm", str(obj)])
    symbols = {}
    for line in nm.splitlines():
        parts = line.split()
        if len(parts) >= 2:
            symbols[parts[-1]] = parts[-2]
    require(symbols.get("__wrap_sub_0056d8c0") == "T", "__wrap_sub_0056d8c0 not defined (T)")
    require(symbols.get("__real_sub_0056d8c0") == "U", "__real_sub_0056d8c0 not an undefined reference")
    for needed in ("guest_call", "guest_fault", "vglMemFree", "isaac_vita_log",
                   "gpush_at", "gpop_at", "isaac_vita_image_retain_arm",
                   "isaac_vita_image_retain_report", "isaac_vita_image_retain_window",
                   "isaac_vita_image_retain_disarm"):
        require(needed in symbols, f"ARM object lacks {needed}")
    forbidden = [s for s in symbols if symbols[s] == "U" and re.fullmatch(
        r"(malloc|calloc|realloc|free|printf|sceClibPrintf|sub_0056d8c0|sub_0056d220)", s)]
    require(not forbidden, f"ARM object references {forbidden}")
    print(f"ARM cross-compile ({arm_cc}): 0 errors, -Werror clean, "
          f"T __wrap_sub_0056d8c0, U __real_sub_0056d8c0, undefined="
          f"{sorted(s for s in symbols if symbols[s] == 'U')}: PASS")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", required=True, type=Path)
    parser.add_argument("--generated", type=Path)
    parser.add_argument("--cc", help='ILP32 host compiler, e.g. "clang -m32"')
    parser.add_argument("--arm-cc")
    parser.add_argument("--keep", type=Path)
    parser.add_argument("--skip-oracle", action="store_true")
    args = parser.parse_args()

    pe = PE(args.pe)
    facts = verify_pe(pe)
    verify_header(facts)
    if args.generated:
        verify_generated(args.generated)
    else:
        print("corpus pins: skipped (no --generated)")

    tmp = None
    if args.keep:
        work = args.keep
        work.mkdir(parents=True, exist_ok=True)
    else:
        tmp = tempfile.TemporaryDirectory(prefix="isaac-image-retain-")
        work = Path(tmp.name)
    try:
        if not args.skip_oracle:
            cc = shlex.split(args.cc) if args.cc else default_cc()
            run_oracle(cc, work)
        if args.arm_cc:
            arm_cross(args.arm_cc, work)
        else:
            print("ARM cross-compile: skipped (no --arm-cc)")
    finally:
        if tmp is not None:
            tmp.cleanup()
    print("Vita image retain seam: PASS")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except AssertionError as error:
        print(f"FAIL: {error}", file=sys.stderr)
        raise SystemExit(1)
