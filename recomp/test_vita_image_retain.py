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
ORACLE_PASS = "IMAGE RETAIN ORACLE PASS: checks=667 scenarios=71 records=1024"
ORACLE_PASS_PHYCONT = "IMAGE RETAIN ORACLE PASS: checks=680 scenarios=74 records=1024"
# ISAAC_VITA_IMAGE_RETAIN_BIG_TRACE=1 (MIN_KB=1024): the same scenarios with the
# trace compiled in, plus the big-trace scenarios.
BIG_TRACE_DEFS = ["-DISAAC_VITA_IMAGE_RETAIN_BIG_TRACE=1",
                  "-DISAAC_VITA_IMAGE_RETAIN_BIG_TRACE_MIN_KB=1024"]
ORACLE_PASS_BIG = "IMAGE RETAIN ORACLE PASS: checks=1245 scenarios=79 records=1024"
ORACLE_PASS_BIG_PHYCONT = "IMAGE RETAIN ORACLE PASS: checks=1258 scenarios=82 records=1024"
# Each mutant is one exact, unique text replacement in the module; the
# trace-ON oracle must compile it and FAIL.
BIG_TRACE_MUTANTS = [
    ("threshold-inclusive",
     "    if (bytes < (uint64_t)ISAAC_IRB_MIN_BYTES)\n        return;\n    if (s_irb.lines",
     "    if (bytes <= (uint64_t)ISAAC_IRB_MIN_BYTES)\n        return;\n    if (s_irb.lines"),
    ("reason-rt-mask",
     "    if ((flags & 0x14U) != 0U)\n        return \"inel-rt\";",
     "    if ((flags & 0x10U) != 0U)\n        return \"inel-rt\";"),
    ("reason-tex-unset",
     "    if (tex0 == 0U || tex0 == ISAAC_IR_TEX_UNSET)\n        return \"inel-tex\";",
     "    if (tex0 == 0U)\n        return \"inel-tex\";"),
    ("reason-counter-swap",
     "    case 't': ++s_irb.inel_tex; break;",
     "    case 't': ++s_irb.inel_name; break;"),
    ("foreign-vt-not-latched",
     "++s_irb.inel_vt; s_irb.last_foreign_vt = ld32(image); return;",
     "++s_irb.inel_vt; return;"),
    ("name-head-not-tail",
     "    start = len > ISAAC_IRB_NAME_MAX ? len - ISAAC_IRB_NAME_MAX : 0U;",
     "    start = 0U; if (len > ISAAC_IRB_NAME_MAX) len = ISAAC_IRB_NAME_MAX;"),
    ("name-space-kept", "(ch > 0x20U && ch < 0x7fU)", "(ch >= 0x20U && ch < 0x7fU)"),
    ("name-del-kept", "(ch > 0x20U && ch < 0x7fU)", "(ch > 0x20U && ch <= 0x7fU)"),
    ("line-cap-off-by-one",
     "#define ISAAC_IRB_MAX_LINES 512U", "#define ISAAC_IRB_MAX_LINES 513U"),
    ("suppressed-not-counted",
     "        ++s_irb.suppressed;\n        return;\n    }\n    ++s_irb.lines;\n    name_va",
     "        return;\n    }\n    ++s_irb.lines;\n    name_va"),
    ("new-reuse-swapped", "? \"reuse\" : \"new\";", "? \"new\" : \"reuse\";"),
    ("evict-reason-swapped",
     "reason == IR_REASON_BUDGET ? \"evict-budget\" :",
     "reason == IR_REASON_BUDGET ? \"evict-headroom\" :"),
    ("stale-silent", "        IRB_NOTE_RECORD(image, bytes, \"stale\");\n", ""),
    ("oversize-silent", "        IRB_NOTE(image, \"oversize\");\n", ""),
    ("absent-silent", "        IRB_NOTE(image, \"absent\");\n", ""),
    ("budget0-silent", "        IRB_NOTE(image, \"pass-budget0\");\n", ""),
    ("headroom-pass-silent", "                IRB_NOTE(image, \"headroom-pass\");\n", ""),
    ("ineligible-silent", "        IRB_INELIGIBLE(image);\n", "        (void)irb_ineligible;\n"),
    ("note-after-touch",
     "    IRB_NOTE(image, irb_touch_what(image, ctrl));\n    ir_touch(image, ctrl, bytes, 0);\n",
     "    ir_touch(image, ctrl, bytes, 0);\n    IRB_NOTE(image, irb_touch_what(image, ctrl));\n"),
    ("threshold-wrong-unit",
     "#define ISAAC_IRB_MIN_BYTES ((uint32_t)(ISAAC_VITA_IMAGE_RETAIN_BIG_TRACE_MIN_KB) * 1024U)",
     "#define ISAAC_IRB_MIN_BYTES ((uint32_t)(ISAAC_VITA_IMAGE_RETAIN_BIG_TRACE_MIN_KB) * 1000U)"),
    ("totals-not-reset-at-arm", "    memset(&s_irb, 0, sizeof s_irb);\n", ""),
]
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
    # The PHYCONT leg reruns every scenario (PHYCONT free is 0 in them) plus
    # the one that only passes when the headroom really adds PHYCONT.
    for phy, expect in ((0, ORACLE_PASS), (1, ORACLE_PASS_PHYCONT)):
        for required in (0, 1):
            for opt in ("-O2", "-O0"):
                exe = work / f"ir-oracle-p{phy}s{required}{opt}{'.exe' if os.name == 'nt' else ''}"
                command = [*cc, "-std=c11", opt, "-Wall", "-Wextra", "-Werror",
                           "-DISAAC_VITA_IMAGE_RETAIN=1",
                           "-DISAAC_VITA_IMAGE_RETAIN_ORACLE=1",
                           "-DGUEST_IMAGE_BASE=0x98000000U",
                           f"-DGUEST_STACK_REQUIRED={required}",
                           *(["-DISAAC_VITA_IMAGE_RETAIN_HEADROOM_PHYCONT=1"] if phy else []),
                           "-I", str(RUNTIME), str(MODULE), str(ORACLE), "-o", str(exe)]
                if os.name != "nt":
                    command.append("-no-pie")
                run(command)
                out = run([str(exe)]).strip()
                require(out == expect, f"oracle (phycont={phy} stack={required} {opt}) said: {out}")
                print(f"host oracle phycont={phy} stack_required={required} {opt}: {out}")


def oracle_command(cc: list[str], opt: str, phy: int, module: Path, exe: Path,
                   extra: list[str]) -> list[str]:
    command = [*cc, "-std=c11", opt, "-Wall", "-Wextra", "-Werror",
               "-DISAAC_VITA_IMAGE_RETAIN=1", "-DISAAC_VITA_IMAGE_RETAIN_ORACLE=1",
               "-DGUEST_IMAGE_BASE=0x98000000U", "-DGUEST_STACK_REQUIRED=1",
               *(["-DISAAC_VITA_IMAGE_RETAIN_HEADROOM_PHYCONT=1"] if phy else []),
               *extra, "-I", str(RUNTIME), str(module), str(ORACLE), "-o", str(exe)]
    if os.name != "nt":
        command.append("-no-pie")
    return command


def verify_big_trace_cmake() -> None:
    text = (VITA / "CMakeLists.txt").read_text(encoding="utf-8")
    require(len(re.findall(r"^option\(ISAAC_VITA_IMAGE_RETAIN_BIG_TRACE\n\s+\"[^\"]+\" OFF\)$",
                           text, re.M)) == 1, "cmake: BIG_TRACE is not declared exactly once with default OFF")
    require(len(re.findall(r'^set\(ISAAC_VITA_IMAGE_RETAIN_BIG_TRACE_MIN_KB "1024" CACHE STRING$',
                           text, re.M)) == 1, "cmake: BIG_TRACE_MIN_KB (default 1024) missing")
    block = re.findall(
        r"if\(ISAAC_VITA_IMAGE_RETAIN_BIG_TRACE\)\n(?:\s+#[^\n]*\n)*\s+set_property\(SOURCE\n"
        r"\s+\"\$\{ISAAC_RUNTIME\}/host_vita_image_retain\.c\"\n"
        r"\s+APPEND PROPERTY COMPILE_DEFINITIONS\n"
        r"\s+ISAAC_VITA_IMAGE_RETAIN_BIG_TRACE=1\n"
        r"\s+ISAAC_VITA_IMAGE_RETAIN_BIG_TRACE_MIN_KB=\$\{ISAAC_VITA_IMAGE_RETAIN_BIG_TRACE_MIN_KB\}\)", text)
    require(len(block) == 1, "cmake: the BIG_TRACE defines are not appended to host_vita_image_retain.c alone")
    require(text.count("ISAAC_VITA_IMAGE_RETAIN_BIG_TRACE=1") == 1,
            "cmake: the BIG_TRACE define is appended in more than one place")
    users = sorted(p.name for p in RUNTIME.iterdir() if p.suffix in (".c", ".h")
                   and "ISAAC_VITA_IMAGE_RETAIN_BIG_TRACE" in p.read_text(encoding="utf-8", errors="replace"))
    require(users == ["host_vita_image_retain.c", "vita_image_retain_oracle.c"],
            f"runtime files naming the BIG_TRACE knob: {users}")
    require("vita_image_retain_oracle.c" not in text, "cmake: the oracle is listed in the Vita CMakeLists.txt")
    print("big trace cmake: option OFF, MIN_KB 1024, defines -> host_vita_image_retain.c only; "
          "knob named by the module and its oracle only: PASS")


def run_big_trace(cc: list[str], work: Path) -> None:
    suffix = ".exe" if os.name == "nt" else ""
    verify_big_trace_cmake()
    # OFF leaves no token of the trace in the unit (what the compiler sees).
    pre = [*cc, "-std=c11", "-E", "-P", "-DISAAC_VITA_IMAGE_RETAIN=1",
           "-DISAAC_VITA_IMAGE_RETAIN_ORACLE=1", "-DGUEST_IMAGE_BASE=0x98000000U",
           "-DGUEST_STACK_REQUIRED=1", "-I", str(RUNTIME), str(MODULE)]
    off = run(pre)
    require("isaac_vita_image_retain_report" in off, "preprocessed module lacks the control symbol")
    leaked = [token for token in ("irb_", "s_irb", "RETAIN BIG", "ISAAC_IRB_", "IRB_NOTE", "IRB_INELIGIBLE")
              if token in off]
    require(not leaked, f"BIG_TRACE OFF leaves trace tokens in the unit: {leaked}")
    on = run([*pre, *BIG_TRACE_DEFS])
    require("s_irb" in on and "RETAIN BIG" in on, "BIG_TRACE ON does not reach the unit")
    print("big trace OFF: no irb_/s_irb/'RETAIN BIG'/IRB_ token in the preprocessed unit (ON has them): PASS")
    for phy, expect in ((0, ORACLE_PASS_BIG), (1, ORACLE_PASS_BIG_PHYCONT)):
        for opt in ("-O2", "-O0"):
            exe = work / f"ir-big-p{phy}{opt}{suffix}"
            run(oracle_command(cc, opt, phy, MODULE, exe, BIG_TRACE_DEFS))
            out = run([str(exe)]).strip()
            require(out == expect, f"big trace oracle (phycont={phy} {opt}) said: {out}")
            print(f"big trace oracle phycont={phy} {opt}: {out}")
    text = MODULE.read_bytes().decode("utf-8")
    killed = 0
    for name, old, new in BIG_TRACE_MUTANTS:
        require(text.count(old) == 1, f"mutant {name}: anchor occurs {text.count(old)} times")
        mutant = work / f"mutant-{name}.c"
        mutant.write_bytes(text.replace(old, new).encode("utf-8"))
        exe = work / f"ir-big-mutant-{name}{suffix}"
        run(oracle_command(cc, "-O2", 0, mutant, exe, BIG_TRACE_DEFS))      # must still compile
        result = subprocess.run([str(exe)], check=False, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True)
        verdict = result.stdout.strip().splitlines()[-1] if result.stdout.strip() else ""
        first = result.stderr.strip().splitlines()[0] if result.stderr.strip() else ""
        require(result.returncode == 1 and verdict.startswith("IMAGE RETAIN ORACLE FAIL: "),
                f"mutant {name} SURVIVED: rc={result.returncode} {verdict}")
        killed += 1
        print(f"  mutant {name}: killed ({first[first.find('['):][:150]})")
    print(f"big trace mutants: {killed}/{len(BIG_TRACE_MUTANTS)} killed")


# ------------------------------------------------ batch-flush flags / preload ---
# ISAAC_VITA_IMAGE_RETAIN_ADMIT_TOUCHED (1 = OBSERVE, 2 = ON) and
# ISAAC_VITA_IMAGE_PRELOAD.  Every byte string below was copied from the
# frozen PE (pe.read), not recalled.
FLAG_WORD = 0x10                       # Image flags at +0x10
DIRTY_VEC = (0x7E7D10, 0x7E7D14)
TOUCHED_VEC = (0x7E7D1C, 0x7E7D20)
PAIRS_VEC = (0x7E7D28, 0x7E7D2C)
LAST_PAIR_IMAGE = 0x7E7D38
FLUSH_PE_PINS = [
    # Unregister's scrubs (inside sub_0056d220, before the bucket erase).
    (0x56D280, "a1107dbe008b35147dbe00", "Unregister: mov eax,[DIRTY.begin]; mov esi,[DIRTY.end]"),
    (0x56D2AF, "832d147dbe0004", "Unregister: sub [DIRTY.end],4"),
    (0x56D2C1, "a11c7dbe008b35207dbe00", "Unregister: mov eax,[TOUCHED.begin]; mov esi,[TOUCHED.end]"),
    (0x56D2EF, "832d207dbe0004", "Unregister: sub [TOUCHED.end],4"),
    (0x56D2F6, "8b35287dbe00a12c7dbe00", "Unregister: mov esi,[PAIRS.begin]; mov eax,[PAIRS.end]"),
    (0x56D305, "8d7e088b57fc3bca7540", "Unregister: pair scan compares the word at pair+4 (the Image)"),
    # The last-pair Image latch.
    (0x5608E1, "a1387dbe0085", "pair append: mov eax,[last_pair_image]; test"),
    (0x5608EA, "c7405c00000000", "pair append: mov [eax+0x5c],0"),
    (0x5608F6, "893d387dbe00", "pair append: mov [last_pair_image],edi"),
    # The idle sweep in the clear wrapper sub_00564f50: timeGetTime() -
    # [image+0x74] > 5000 -> call [vt+0x74]; and [image+0x10],~0x10.
    (0x564FD0, "ff15c064a0008b0e2b41743d88130000", "sweep: call timeGetTime; sub eax,[img+0x74]; cmp eax,5000"),
    (0x564FE0, "76328b01ff50748b0e8b411483", "sweep: jbe; call [vt+0x74]"),
    # The flag writers.
    (0x5601F6, "834e1004", "quad queue: or [esi+0x10],4"),
    (0x56D4BC, "836110fb", "sub_0056d4a0: and [ecx+0x10],~4"),
    (0x56D5CF, "83491010", "flush: or [ecx+0x10],0x10"),
    (0x56D7F5, "83491010", "flush: or [ecx+0x10],0x10"),
    (0x56D80F, "83491010", "flush: or [ecx+0x10],0x10"),
    (0x561F81, "836110ef", "sweep sub_00561f40: and [ecx+0x10],~0x10"),
    (0x564FEC, "836110ef", "sweep sub_00564f50: and [ecx+0x10],~0x10"),
    # The Load call the preload replays, and the destructor of its `out`.
    (0x565FAD, "6a006848f9bb008bca5083e1038d85c8fbfffff3a450e818680000",
     "push 0; push 0xbbf948; ...; push eax (name); ...; push eax (&out); call sub_0056c7e0"),
    (0x565FEE, "8b8dccfbffff85c974208b018b400cffd084c07415a12cd6bf0085c0740c8d8dc8fbffff51ffd083",
     "~SmartPointer(out): ctrl->vt[+0xc](); al -> [0xbfd62c](&out); add esp,4"),
    (0x56C7E0, "558bec6aff68", "Load prologue"),
    (0x56CC82, "c21000", "Load: ret 0x10"),
    (0x007AF0, "568bf1576aff8b46088d4e08ff500c0f", "ctrl Release (vtable +0xc): thiscall"),
]
FLUSH_HEADER_FACTS = {
    "IMG_FLAG_DIRTY": 0x04, "IMG_FLAG_TOUCHED": 0x10,
    "DIRTY_BEGIN_RVA": DIRTY_VEC[0], "DIRTY_END_RVA": DIRTY_VEC[1],
    "TOUCHED_BEGIN_RVA": TOUCHED_VEC[0], "TOUCHED_END_RVA": TOUCHED_VEC[1],
    "PAIRS_BEGIN_RVA": PAIRS_VEC[0], "PAIRS_END_RVA": PAIRS_VEC[1],
    "LAST_PAIR_IMAGE_RVA": LAST_PAIR_IMAGE, "TOUCHED_IDLE_MS": 0x1388,
    "LOAD_RVA": 0x56C7E0, "LOAD_SITE_RVA": 0x565FC8,
    "LOAD_VERTEX_DESC_RVA": 0x7BF948, "LOAD_RELEASE_SITE_RVA": 0x565FFF,
    "LOAD_OBSERVER_SITE_RVA": 0x566015, "CTRL_VT_RELEASE": 0x0C,
}
OBSERVE_DEFS = ["-DISAAC_VITA_IMAGE_RETAIN_ADMIT_TOUCHED=1"]
ADMIT_DEFS = ["-DISAAC_VITA_IMAGE_RETAIN_ADMIT_TOUCHED=2"]
PRELOAD_DEFS = ["-DISAAC_VITA_IMAGE_PRELOAD=1",
                '-DISAAC_VITA_IMAGE_PRELOAD_LIST="gfx/BulletAtlas.png,gfx/b.png"',
                "-DISAAC_VITA_IMAGE_PRELOAD_MAX_KB=2048",
                "-DISAAC_VITA_IMAGE_PRELOAD_RESERVE_MB=8"]
# The device arms of the fix pass build RESERVE_MB=4 (review B1): the same
# scenarios with the heap gate at 8192 KiB and the bench-route table's other column.
PRELOAD4_DEFS = [d.replace("_RESERVE_MB=8", "_RESERVE_MB=4") for d in PRELOAD_DEFS]
assert PRELOAD4_DEFS != PRELOAD_DEFS and len(PRELOAD4_DEFS) == len(PRELOAD_DEFS)
# ISAAC_VITA_IMAGE_PRELOAD_PIN (lane F-BA): the scenarios that differ by mode
# are pin_scenarios() in the oracle plus the split tail of P7.
PIN_OBSERVE_DEFS = ["-DISAAC_VITA_IMAGE_PRELOAD_PIN=1"]
PIN_ON_DEFS = ["-DISAAC_VITA_IMAGE_PRELOAD_PIN=2"]
PIN_TOKENS = ("irpp_", "s_irpp", "ISAAC_IRPP_", "IMAGE PRELOAD PIN", "IRP_ON_FREE",
              "isaac_ir_oracle_preload_pin")
# (name, phycont, defines, expected verdict)
FLUSH_LEGS = [
    ("observe", 0, OBSERVE_DEFS, "checks=673 scenarios=71"),
    ("on", 0, ADMIT_DEFS, "checks=696 scenarios=73"),
    ("preload", 0, PRELOAD_DEFS, "checks=782 scenarios=96"),
    ("observe+preload", 0, [*OBSERVE_DEFS, *PRELOAD_DEFS], "checks=788 scenarios=96"),
    ("on+preload", 0, [*ADMIT_DEFS, *PRELOAD_DEFS], "checks=811 scenarios=98"),
    ("observe+big", 0, [*OBSERVE_DEFS, *BIG_TRACE_DEFS], "checks=1251 scenarios=79"),
    ("on+big", 0, [*ADMIT_DEFS, *BIG_TRACE_DEFS], "checks=1274 scenarios=81"),
    ("on+preload+big+phycont", 1, [*ADMIT_DEFS, *PRELOAD_DEFS, *BIG_TRACE_DEFS],
     "checks=1402 scenarios=109"),
    ("preload reserve=4", 0, PRELOAD4_DEFS, "checks=782 scenarios=96"),
    ("on+preload reserve=4", 0, [*ADMIT_DEFS, *PRELOAD4_DEFS], "checks=811 scenarios=98"),
    ("pin observe", 0, [*PRELOAD4_DEFS, *PIN_OBSERVE_DEFS], "checks=895 scenarios=110"),
    ("pin on", 0, [*PRELOAD4_DEFS, *PIN_ON_DEFS], "checks=921 scenarios=110"),
    ("on+pin on", 0, [*ADMIT_DEFS, *PRELOAD4_DEFS, *PIN_ON_DEFS], "checks=957 scenarios=113"),
    ("pin on reserve=8", 0, [*PRELOAD_DEFS, *PIN_ON_DEFS], "checks=921 scenarios=110"),
    ("on+pin on+big+phycont", 1, [*ADMIT_DEFS, *PRELOAD4_DEFS, *PIN_ON_DEFS, *BIG_TRACE_DEFS],
     "checks=1548 scenarios=124"),
]
# (name, defines of the leg that must kill it, old, new)
FLUSH_MUTANTS = [
    ("t1-dirty-admitted", ADMIT_DEFS,
     "    if ((flags & ISAAC_IR_IMG_FLAG_DIRTY) != 0U)\n        return IRT_DIRTY;\n",
     "    if ((flags & ISAAC_IR_IMG_FLAG_DIRTY) != 0U && flags == 0U)\n        return IRT_DIRTY;\n"),
    ("t2-touched-count-ignored", ADMIT_DEFS,
     "                      IR_VA(ISAAC_IR_TOUCHED_END_RVA), image) != 1 ||",
     "                      IR_VA(ISAAC_IR_TOUCHED_END_RVA), image) < 0 ||"),
    ("t2-touched-twice-ok", ADMIT_DEFS,
     "                      IR_VA(ISAAC_IR_TOUCHED_END_RVA), image) != 1 ||",
     "                      IR_VA(ISAAC_IR_TOUCHED_END_RVA), image) < 1 ||"),
    ("t2-dirty-vector-ignored", ADMIT_DEFS,
     "                          IR_VA(ISAAC_IR_DIRTY_END_RVA), image) != 0)",
     "                          IR_VA(ISAAC_IR_DIRTY_END_RVA), image) < 0)"),
    ("t2-malformed-vector-empty", ADMIT_DEFS,
     "            ((end - begin) >> 2) > ISAAC_IR_FLUSH_VEC_MAX)\n        return -1;",
     "            ((end - begin) >> 2) > ISAAC_IR_FLUSH_VEC_MAX)\n        return 1;"),
    ("t3-pair-ignored", ADMIT_DEFS,
     "    if (irt_pair_pending(image))\n        return IRT_PAIR;\n",
     "    if (irt_pair_pending(image) && image == 0U)\n        return IRT_PAIR;\n"),
    ("t3-latch-ignored", ADMIT_DEFS,
     "    return irt_pair_count(image) != 0 ||\n           ld32(IR_VA(ISAAC_IR_LAST_PAIR_IMAGE_RVA)) == image;",
     "    return irt_pair_count(image) != 0;"),
    ("t3-pair-first-word", ADMIT_DEFS,
     "        if (ld32(p + 4U) == image)\n            ++n;\n    }\n    return n;\n}\n\nstatic int irt_pair_pending",
     "        if (ld32(p) == image)\n            ++n;\n    }\n    return n;\n}\n\nstatic int irt_pair_pending"),
    ("rule-e-off", ADMIT_DEFS,
     "        if (!irt_record_pinned(idx))\n            return idx;",
     "        if (!irt_record_pinned(idx) || idx != ISAAC_IR_NIL)\n            return idx;"),
    ("rule-e-pins-stale", ADMIT_DEFS,
     "            ld16(ctrl + ISAAC_IR_CTRL_STRONG) != 1U)\n        return 0;\n    return (ld32(image + ISAAC_IR_IMG_FLAGS) & ISAAC_IRT_FLUSH_MASK) != 0U;",
     "            ld16(ctrl + ISAAC_IR_CTRL_STRONG) != 1U)\n        return 1;\n    return (ld32(image + ISAAC_IR_IMG_FLAGS) & ISAAC_IRT_FLUSH_MASK) != 0U;"),
    ("hard-cap-off", ADMIT_DEFS,
     "    if (s_ir.retained_bytes > s_ir.budget_bytes && !ir_manager_lock_held()) {\n        /* Rule E left",
     "    if (s_ir.retained_bytes > s_ir.budget_bytes && ir_manager_lock_held()) {\n        /* Rule E left"),
    ("observe-admits", OBSERVE_DEFS,
     "#define ISAAC_IRT_ON (ISAAC_VITA_IMAGE_RETAIN_ADMIT_TOUCHED == 2)",
     "#define ISAAC_IRT_ON (ISAAC_VITA_IMAGE_RETAIN_ADMIT_TOUCHED >= 1)"),
    ("observe-undercounts", OBSERVE_DEFS,
     "        if (!ISAAC_IRT_ON && irt_rest_eligible(image, &bytes) &&",
     "        if (!ISAAC_IRT_ON && !irt_rest_eligible(image, &bytes) &&"),
    ("admit-bytes-on-reuse", ADMIT_DEFS,
     "        if (ir_hash_find(image) == ISAAC_IR_NIL)\n            s_irt.admit_bytes += bytes;",
     "        s_irt.admit_bytes += bytes;"),
    ("pair-untouched-not-counted", ADMIT_DEFS,
     "        if (irt_pair_pending(image))\n            ++s_irt.pair_untouched;\n", ""),
    ("counters-not-reset-at-arm", ADMIT_DEFS, "    memset(&s_irt, 0, sizeof s_irt);\n    isaac_vita_log(", "    isaac_vita_log("),
    ("preload-own-offer-counted", PRELOAD_DEFS,
     "    if (s_irp.busy || s_irp.n == 0U || !ir_image_vtable_ok(image))",
     "    if (s_irp.n == 0U || !ir_image_vtable_ok(image))"),
    ("preload-hit-every-reuse", PRELOAD_DEFS,
     "                s_irp.state[i] = IRP_USED;\n                ++s_irp.hit;",
     "                ++s_irp.hit;"),
    ("preload-name-substring", PRELOAD_DEFS,
     "    if (len > want && irp_fold((uint8_t)ld8(name + len - want - 1U)) != (uint8_t)'/')\n        return 0;\n", ""),
    ("preload-name-case", PRELOAD_DEFS,
     "    if (ch >= (uint8_t)'A' && ch <= (uint8_t)'Z')\n        return (uint8_t)(ch + 32U);\n    return ch == (uint8_t)'\\\\' ? (uint8_t)'/' : ch;",
     "    return ch == (uint8_t)'\\\\' ? (uint8_t)'/' : ch;"),
    ("preload-under-lock", PRELOAD_DEFS,
     "    if (ir_manager_lock_held()) {\n        ++s_irp.skip[IRP_SKIP_LOCK];\n        return 1;\n    }\n", ""),
    ("preload-terminal-ignored", PRELOAD_DEFS,
     "    if (isaac_vita_guest_heap_terminal()) {\n        ++s_irp.skip[IRP_SKIP_TERMINAL];\n        return 1;\n    }\n", ""),
    ("preload-present-reloaded", PRELOAD_DEFS,
     "    if (irp_find_in_manager(i, &image, &ctrl)) {\n        ++s_irp.present;\n        return 1;\n    }\n", ""),
    ("preload-budget-ignored", PRELOAD_DEFS,
     "    if (est > s_ir.budget_bytes ||\n", "    if (\n"),
    ("preload-headroom-ignored", PRELOAD_DEFS,
     "             ir_free_bytes() < (uint64_t)s_ir.headroom_bytes + (uint64_t)est)) {",
     "             ir_free_bytes() < (uint64_t)s_ir.headroom_bytes)) {"),
    ("preload-heap-gate-factor", PRELOAD_DEFS,
     "    if (hr0 < reserve || (hr0 - reserve) / ISAAC_IRP_HEAP_FACTOR < est) {",
     "    if (hr0 < reserve || (hr0 - reserve) < est) {"),
    ("preload-heap-gate-strict", PRELOAD_DEFS,
     "    if (hr0 < reserve || (hr0 - reserve) / ISAAC_IRP_HEAP_FACTOR < est) {",
     "    if (hr0 < reserve || (hr0 - reserve) / ISAAC_IRP_HEAP_FACTOR <= est) {"),
    ("preload-heap-gate-reserve-ignored", PRELOAD4_DEFS,
     "    if (hr0 < reserve || (hr0 - reserve) / ISAAC_IRP_HEAP_FACTOR < est) {",
     "    if (hr0 / ISAAC_IRP_HEAP_FACTOR < est) {"),
    ("preload-need-kb-without-factor", PRELOAD4_DEFS,
     "                   ISAAC_IRP_HEAP_FACTOR *\n                   (uint32_t)ISAAC_VITA_IMAGE_PRELOAD_MAX_KB),",
     "                   (uint32_t)ISAAC_VITA_IMAGE_PRELOAD_MAX_KB),"),
    ("preload-load-esp-unchecked", PRELOAD_DEFS,
     "    if (c->esp != base) {                  /* Load is `ret 0x10` */",
     "    if (c->esp != base && c->esp == 0U) {  /* Load is `ret 0x10` */"),
    ("preload-release-esp-unchecked", PRELOAD_DEFS,
     "        if (c->esp != base) {              /* Release is a plain `ret` */",
     "        if (c->esp != base && c->esp == 0U) {"),
    ("preload-observer-esp-unchecked", PRELOAD_DEFS,
     "            if (c->esp != base - 4U) {     /* cdecl: the caller drops &out */",
     "            if (c->esp != base - 4U && c->esp == 0U) {"),
    ("preload-esp-not-restored", PRELOAD_DEFS,
     "        }\n    }\n    c->esp = saved;\n    return 1;\n}", "        }\n    }\n    return 1;\n}"),
    ("preload-wrong-load-site", PRELOAD_DEFS,
     "    gpush(c, ISAAC_IR_LOAD_SITE_RVA);", "    gpush(c, ISAAC_IR_LOAD_RELEASE_SITE_RVA);"),
    ("preload-arg-order", PRELOAD_DEFS,
     "    gpush(c, 0U);\n    gpush(c, IR_VA(ISAAC_IR_LOAD_VERTEX_DESC_RVA));\n",
     "    gpush(c, IR_VA(ISAAC_IR_LOAD_VERTEX_DESC_RVA));\n    gpush(c, 0U);\n"),
    ("preload-observer-always", PRELOAD_DEFS,
     "        if ((c->eax & 0xFFU) != 0U && hook != 0U) {", "        if (c->eax != 0U && hook != 0U) {"),
    ("preload-no-release", PRELOAD_DEFS,
     "    if (ctrl != 0U) {\n        gpush(c, ISAAC_IR_LOAD_RELEASE_SITE_RVA);",
     "    if (ctrl != 0U && image == 0U) {\n        gpush(c, ISAAC_IR_LOAD_RELEASE_SITE_RVA);"),
    ("preload-dropped-as-retained", PRELOAD_DEFS,
     "        if (idx != ISAAC_IR_NIL && s_ir.rec[idx].ctrl == ctrl &&\n                ir_present_in_manager(image, ctrl)) {\n            ++s_irp.retained;",
     "        if (idx != ISAAC_IR_NIL || ctrl != 0U) {\n            ++s_irp.retained;"),
    ("preload-evicted-not-counted", PRELOAD_DEFS,
     "#if defined(ISAAC_VITA_IMAGE_PRELOAD)\n    irp_evicting(image, ctrl);\n#endif\n", ""),
    ("preload-foreign-cpu", PRELOAD_DEFS,
     "    if (!s_ir.armed || s_ir.budget_bytes == 0U || s_ir.in_evict ||\n            (s_ir.cpu != NULL && s_ir.cpu != c)) {\n        ++s_irp.skip[IRP_SKIP_STATE];",
     "    if (!s_ir.armed || s_ir.budget_bytes == 0U || s_ir.in_evict) {\n        ++s_irp.skip[IRP_SKIP_STATE];"),
    # IMAGE_PRELOAD_PIN (lane F-BA).
    ("pin-walker-takes-pins", [*PRELOAD4_DEFS, *PIN_ON_DEFS],
     "        k = irpp_pin_of(idx);\n        if (k >= 0) {",
     "        k = irpp_pin_of(idx);\n        if (k >= 0 && s_irpp.skip == 0xFFFFFFFFu) {"),
    ("pin-share-cap-ignored", [*PRELOAD4_DEFS, *PIN_ON_DEFS],
     "    if ((uint64_t)s_irpp.pinned_bytes + bytes > s_ir.budget_bytes / ISAAC_IRPP_SHARE) {",
     "    if ((uint64_t)s_irpp.pinned_bytes + bytes > s_ir.budget_bytes) {"),
    ("pin-floor-kept", [*PRELOAD4_DEFS, *PIN_ON_DEFS], "        irpp_unpin(i, IRPP_FLOOR);\n", ""),
    ("pin-kept-after-use", [*PRELOAD4_DEFS, *PIN_ON_DEFS], "                irpp_unpin(i, IRPP_USED);\n", ""),
    ("pin-held-not-detected", [*PRELOAD4_DEFS, *PIN_ON_DEFS],
     "    if (ld16(ctrl + ISAAC_IR_CTRL_STRONG) >= 2U && (exact || kept)) {",
     "    if (ld16(ctrl + ISAAC_IR_CTRL_STRONG) >= 3U && (exact || kept)) {"),
    ("pin-held-dropped-counted", [*PRELOAD4_DEFS, *PIN_ON_DEFS],
     "        if (!kept && s_irp.dropped != 0U)\n            --s_irp.dropped;",
     "        if (!kept && s_irp.dropped == 0xFFFFFFFFu)\n            --s_irp.dropped;"),
    ("pin-lost-never-settled", [*PRELOAD4_DEFS, *PIN_ON_DEFS],
     "#if defined(ISAAC_VITA_IMAGE_PRELOAD_PIN)\n    irpp_settle();\n#endif\n    if (s_irp.busy",
     "    if (s_irp.busy"),
    ("pin-on-free-not-pinned", [*PRELOAD4_DEFS, *PIN_ON_DEFS],
     "    ++s_irpp.on_free;\n    irpp_pin(i);", "    ++s_irpp.on_free;"),
    ("pin-observe-decides", [*PRELOAD4_DEFS, *PIN_OBSERVE_DEFS],
     "    if (!ISAAC_IRPP_ON) {\n        s_irpp.set_n = 0U;\n        return 1;\n    }\n", ""),
    ("pin-own-replayed", [*PRELOAD4_DEFS, *PIN_ON_DEFS],
     "    if (own) {\n        /* The sheet", "    if (own == 2U) {\n        /* The sheet"),
    ("pin-twins-not-counted", [*PRELOAD4_DEFS, *PIN_ON_DEFS],
     "    if (twins >= 2U)\n        ++s_irpp.twins;\n", ""),
    ("pin-stale-kept", [*PRELOAD4_DEFS, *PIN_ON_DEFS],
     "            irpp_unpin(i, IRPP_STALE);\n            return -1;", "            return (int)i;"),
    ("pin-never-yields", [*PRELOAD4_DEFS, *PIN_ON_DEFS],
     "    if (oldest != ISAAC_IR_NIL) {\n        irpp_unpin((uint32_t)oldest_k, IRPP_YIELD);",
     "    if (oldest != ISAAC_IR_NIL && oldest_k < -1) {\n        irpp_unpin((uint32_t)oldest_k, IRPP_YIELD);"),
    ("pin-dropped-pin-kept", [*PRELOAD4_DEFS, *PIN_ON_DEFS],
     "#if defined(ISAAC_VITA_IMAGE_PRELOAD_PIN)\n    irpp_dropped(*image, *ctrl);\n#endif\n", ""),
    ("pin-alias-as-exact", [*PRELOAD4_DEFS, *PIN_ON_DEFS],
     "        if (s_irpp.set_image[k] == image && s_irpp.set_ctrl[k] == ctrl)\n            exact = 1U;",
     "        if (s_irpp.set_image[k] != 0U)\n            exact = 1U;"),
    ("pin-on-free-ignores-admit", [*ADMIT_DEFS, *PRELOAD4_DEFS, *PIN_ON_DEFS],
     "#if defined(ISAAC_VITA_IMAGE_PRELOAD_PIN)\n    irpp_touched(image, ctrl);\n#endif\n", ""),
    # F-BA fix pass (review items 1, 2, 4, 5): each must die in the scenario written for it.
    ("pin-walker-takes-candidate", [*PRELOAD4_DEFS, *PIN_ON_DEFS],
     "        if (is_cand) {\n            cand = idx;\n            cand_k = -1;",
     "        if (is_cand && cand_k < -1) {\n            cand = idx;\n            cand_k = -1;"),
    ("pin-headroom-takes-candidate", [*ADMIT_DEFS, *PRELOAD4_DEFS, *PIN_ON_DEFS],
     "        if (reason == IR_REASON_BUDGET) {", "        if (reason != IR_REASON_TABLE_FULL) {"),
    ("pin-budget-never-takes-candidate", [*ADMIT_DEFS, *PRELOAD4_DEFS, *PIN_ON_DEFS],
     "        if (reason == IR_REASON_BUDGET) {", "        if (reason == IR_REASON_BUDGET && cand_k < -1) {"),
    ("pin-kept-not-pinned", [*PRELOAD4_DEFS, *PIN_ON_DEFS],
     "#if defined(ISAAC_VITA_IMAGE_PRELOAD_PIN)\n        irpp_kept(image, ctrl);\n#endif\n", ""),
    ("pin-reacquired-lost", [*PRELOAD4_DEFS, *PIN_ON_DEFS],
     "            s_irp.ctrl[i] != s_irpp.pend_ctrl ||\n"
     "            ir_present_in_manager(s_irpp.pend_image, s_irpp.pend_ctrl))",
     "            s_irp.ctrl[i] != s_irpp.pend_ctrl)"),
    ("pin-lost-exit-unread", [*PRELOAD4_DEFS, *PIN_ON_DEFS],
     "    if (s_ir.st.ineligible != s_irpp.pend_st[2])\n", "    if (s_ir.st.ineligible == 0xFFFFFFFFu)\n"),
]
# Mutants that must be killed by one named scenario (the first CHECK failure
# names it): a fix-pass mutant killed elsewhere would mean its scenario does
# not test what it claims.
FLUSH_MUTANT_KILLED_BY = {
    "pin-walker-takes-candidate": "pin-candidate-own-record-headroom",
    "pin-headroom-takes-candidate": "pin-candidate-rule-e-headroom",
    "pin-budget-never-takes-candidate": "touched-rule-e",
    "pin-kept-not-pinned": "pin-held-freed-under-lock",
    "pin-reacquired-lost": "pin-held-reacquired",
    "pin-lost-exit-unread": "pin-held-lost",
}
assert set(FLUSH_MUTANT_KILLED_BY) <= {m[0] for m in FLUSH_MUTANTS}
FLUSH_TOKENS = ("irt_", "s_irt", "ISAAC_IRT_", "RETAIN TOUCHED", "irp_", "s_irp", "ISAAC_IRP_",
                "IMAGE PRELOAD", "isaac_vita_image_preload", "inel-dirty", "pass-pinned")


def verify_flush_pe(pe: PE) -> None:
    for rva, hexbytes, what in FLUSH_PE_PINS:
        want = bytes.fromhex(hexbytes)
        require(pe.read(rva, len(want)) == want, f"PE {rva:#x} changed: {what}")
    _name, va, _vsize, roff, rsize = pe.text
    text = pe.data[roff:roff + rsize]

    def sites(pattern: bytes) -> list[int]:
        return [va + m.start() for m in re.finditer(pattern, text, re.S)]

    # Every `or/and dword [reg+0x10], imm8` in .text that sets or clears the
    # two bits (83 /1 and 83 /4, mod=01 disp8 0x10).
    set10 = sites(rb"\x83[\x48-\x4f]\x10\x10")
    set04 = sites(rb"\x83[\x48-\x4f]\x10\x04")
    clr10 = sites(rb"\x83[\x60-\x67]\x10\xef")
    clr04 = sites(rb"\x83[\x60-\x67]\x10\xfb")
    require(set10 == [0x56D5CF, 0x56D7F5, 0x56D80F], f"writers of |= 0x10: {[hex(s) for s in set10]}")
    require(set04 == [0x5601F6], f"writers of |= 0x04: {[hex(s) for s in set04]}")
    require(clr10 == [0x561F81, 0x564FEC], f"writers of &= ~0x10: {[hex(s) for s in clr10]}")
    require(clr04 == [0x14257, 0x14414, 0x3DC8F8, 0x3EBEDB, 0x471DE4, 0x4E4D7A, 0x504539, 0x56D4BC],
            f"writers of &= ~0x04: {[hex(s) for s in clr04]}")
    # Byte-sized and imm32 forms of the same writes (80 /1 ib, 81 /1 id).
    other = (sites(rb"\x80[\x48-\x4f]\x10[\x10\x14]") + sites(rb"\x81[\x48-\x4f]\x10[\x10\x14]\x00\x00\x00") +
             sites(rb"\x80[\x60-\x67]\x10[\xef\xeb]") + sites(rb"\x81[\x60-\x67]\x10[\xef\xeb]\xff\xff\xff"))
    # The Load call site and the two return words the replay pushes.
    require(pe.read(0x565FC3, 1) == b"\xe8" and
            0x565FC8 + struct.unpack("<i", pe.read(0x565FC4, 4))[0] == 0x56C7E0,
            "0x565fc3 is not `call sub_0056c7e0`")
    require(pe.read(0x565FFD, 2) == bytes.fromhex("ffd0"), "0x565ffd is not `call eax` (Release)")
    require(pe.read(0x566013, 2) == bytes.fromhex("ffd0"), "0x566013 is not `call eax` (observer)")
    require(pe.u32(CTRL_VT_RVA + 0xC) == PE_IMAGE_BASE + 0x7AF0, "ctrl vtable +0xc is not Release 0x7af0")
    for rva in (*DIRTY_VEC, *TOUCHED_VEC, *PAIRS_VEC, LAST_PAIR_IMAGE):
        require(pe.in_bss(rva) or pe.u32(rva) == 0, f"{rva:#x} is not zero-initialised data")
    text_h = HEADER.read_text(encoding="utf-8")
    defines = {m.group(1): int(m.group(2), 16) for m in re.finditer(
        r"^#define ISAAC_IR_(\w+)\s+0x([0-9A-Fa-f]+)U", text_h, re.M)}
    for key, value in FLUSH_HEADER_FACTS.items():
        require(defines.get(key) == value, f"ISAAC_IR_{key} = {defines.get(key)} but the PE says {value:#x}")
    print(f"flush-flag PE pins: {len(FLUSH_PE_PINS)} byte strings; |=0x10 at "
          f"{[hex(s) for s in set10]} only, |=0x04 at {[hex(s) for s in set04]} only, "
          f"&=~0x10 at {[hex(s) for s in clr10]} only, &=~0x04 at {len(clr04)} sites "
          f"(0x56d4bc + 7 more `and [reg+0x10],~4`); byte/imm32 forms of these writes: "
          f"{[hex(s) for s in other] or 'none'}; {len(FLUSH_HEADER_FACTS)} header constants: PASS")


def verify_flush_cmake() -> None:
    text = (VITA / "CMakeLists.txt").read_text(encoding="utf-8")
    require(len(re.findall(r'^set\(ISAAC_VITA_IMAGE_RETAIN_ADMIT_TOUCHED "OFF" CACHE STRING$', text, re.M)) == 1,
            "cmake: ADMIT_TOUCHED is not a STRING knob with default OFF")
    require('if(NOT ISAAC_VITA_IMAGE_RETAIN_ADMIT_TOUCHED MATCHES "^(OFF|OBSERVE|ON)$")' in text,
            "cmake: ADMIT_TOUCHED values are not validated")
    for value, define in (("OBSERVE", 1), ("ON", 2)):
        block = re.findall(
            r'if\(ISAAC_VITA_IMAGE_RETAIN_ADMIT_TOUCHED STREQUAL "%s"\)\n(?:\s+#[^\n]*\n)*\s+set_property\(SOURCE\n'
            r'\s+"\$\{ISAAC_RUNTIME\}/host_vita_image_retain\.c"\n'
            r"\s+APPEND PROPERTY COMPILE_DEFINITIONS\n"
            r"\s+ISAAC_VITA_IMAGE_RETAIN_ADMIT_TOUCHED=%d\)" % (value, define), text)
        require(len(block) == 1, f"cmake: ADMIT_TOUCHED={value} does not append ={define} to the module alone")
    require(len(re.findall(r"ISAAC_VITA_IMAGE_RETAIN_ADMIT_TOUCHED=[0-9]", text)) == 2,
            "cmake: an ADMIT_TOUCHED define is appended in an unexpected place")
    require(len(re.findall(r"^option\(ISAAC_VITA_IMAGE_PRELOAD\n\s+\"[^\"]+\" OFF\)$", text, re.M)) == 1,
            "cmake: IMAGE_PRELOAD is not declared exactly once with default OFF")
    require('set(ISAAC_VITA_IMAGE_PRELOAD_LIST "gfx/BulletAtlas.png" CACHE STRING' in text,
            "cmake: IMAGE_PRELOAD_LIST default changed")
    require('set(ISAAC_VITA_IMAGE_PRELOAD_MAX_KB "2048" CACHE STRING' in text and
            'set(ISAAC_VITA_IMAGE_PRELOAD_RESERVE_MB "8" CACHE STRING' in text,
            "cmake: IMAGE_PRELOAD_MAX_KB / RESERVE_MB defaults changed")
    require("if(NOT ISAAC_VITA_IMAGE_RETAIN OR NOT ISAAC_VITA_SFX_FLOOR_PRELOAD)" in text,
            "cmake: IMAGE_PRELOAD does not require IMAGE_RETAIN and SFX_FLOOR_PRELOAD")
    require(text.count("ISAAC_VITA_IMAGE_PRELOAD=1") == 2 and
            text.count('ISAAC_VITA_IMAGE_PRELOAD_LIST=\\"${ISAAC_VITA_IMAGE_PRELOAD_LIST}\\"') == 1,
            "cmake: the IMAGE_PRELOAD defines are not appended to exactly the two units")
    require("Isaac Vita image preload: ON, list '${ISAAC_VITA_IMAGE_PRELOAD_LIST}'" in text and
            "Isaac Vita image retain admit touched (flag 0x10): ${ISAAC_VITA_IMAGE_RETAIN_ADMIT_TOUCHED}" in text,
            "cmake: the resolved values are not printed at configure time")
    users = sorted(p.name for p in RUNTIME.iterdir() if p.suffix in (".c", ".h")
                   and "ISAAC_VITA_IMAGE_PRELOAD" in p.read_text(encoding="utf-8", errors="replace"))
    require(users == ["host_vita_image_retain.c", "host_vita_image_retain.h",
                      "host_vita_sfx_floor_preload.c", "vita_image_retain_oracle.c"],
            f"runtime files naming the IMAGE_PRELOAD knob: {users}")
    users = sorted(p.name for p in RUNTIME.iterdir() if p.suffix in (".c", ".h")
                   and "ISAAC_VITA_IMAGE_RETAIN_ADMIT_TOUCHED" in p.read_text(encoding="utf-8", errors="replace"))
    require(users == ["host_vita_image_retain.c", "host_vita_image_retain.h", "vita_image_retain_oracle.c"],
            f"runtime files naming the ADMIT_TOUCHED knob: {users}")
    require(len(re.findall(r'^set\(ISAAC_VITA_IMAGE_PRELOAD_PIN "OFF" CACHE STRING$', text, re.M)) == 1,
            "cmake: IMAGE_PRELOAD_PIN is not a STRING knob with default OFF")
    require('if(NOT ISAAC_VITA_IMAGE_PRELOAD_PIN MATCHES "^(OFF|OBSERVE|ON)$")' in text,
            "cmake: IMAGE_PRELOAD_PIN values are not validated")
    require(re.search(r'if\(NOT ISAAC_VITA_IMAGE_PRELOAD_PIN STREQUAL "OFF" AND\n\s+NOT ISAAC_VITA_IMAGE_PRELOAD\)\n'
                      r'\s+message\(FATAL_ERROR', text) is not None,
            "cmake: IMAGE_PRELOAD_PIN does not require IMAGE_PRELOAD")
    for value, define in (("OBSERVE", 1), ("ON", 2)):
        block = re.findall(
            r'if\(ISAAC_VITA_IMAGE_PRELOAD_PIN STREQUAL "%s"\)\n(?:\s+#[^\n]*\n)*\s+set_property\(SOURCE\n'
            r'\s+"\$\{ISAAC_RUNTIME\}/host_vita_image_retain\.c"\n'
            r"\s+APPEND PROPERTY COMPILE_DEFINITIONS\n"
            r"\s+ISAAC_VITA_IMAGE_PRELOAD_PIN=%d\)" % (value, define), text)
        require(len(block) == 1, f"cmake: IMAGE_PRELOAD_PIN={value} does not append ={define} to the module alone")
    require(len(re.findall(r"ISAAC_VITA_IMAGE_PRELOAD_PIN=[0-9]", text)) == 2,
            "cmake: an IMAGE_PRELOAD_PIN define is appended in an unexpected place")
    # The PIN defines sit inside `if(ISAAC_VITA_IMAGE_PRELOAD)` of the define block.
    preload_on = text.index("            ISAAC_VITA_IMAGE_PRELOAD=1\n")
    preload_else = text.index('        message(STATUS "Isaac Vita image preload: OFF")')
    require(all(preload_on < m.start() < preload_else
                for m in re.finditer(r"ISAAC_VITA_IMAGE_PRELOAD_PIN=[0-9]", text)),
            "cmake: an IMAGE_PRELOAD_PIN define is appended outside the IMAGE_PRELOAD=ON branch")
    require('message(STATUS "Isaac Vita image preload pin: ${ISAAC_VITA_IMAGE_PRELOAD_PIN}")' in text,
            "cmake: the resolved IMAGE_PRELOAD_PIN value is not printed at configure time")
    users = sorted(p.name for p in RUNTIME.iterdir() if p.suffix in (".c", ".h")
                   and "ISAAC_VITA_IMAGE_PRELOAD_PIN" in p.read_text(encoding="utf-8", errors="replace"))
    require(users == ["host_vita_image_retain.c", "host_vita_image_retain.h", "vita_image_retain_oracle.c"],
            f"runtime files naming the IMAGE_PRELOAD_PIN knob: {users}")
    print("flush/preload cmake: ADMIT_TOUCHED STRING OFF|OBSERVE|ON (default OFF, =1/=2 on the module only), "
          "IMAGE_PRELOAD OFF with LIST/MAX_KB/RESERVE_MB, needs IMAGE_RETAIN + SFX_FLOOR_PRELOAD, "
          "IMAGE_PRELOAD_PIN STRING OFF|OBSERVE|ON (default OFF, needs IMAGE_PRELOAD, =1/=2 on the module "
          "only, inside the PRELOAD=ON branch), resolved values printed: PASS")


def run_flush(cc: list[str], work: Path) -> None:
    suffix = ".exe" if os.name == "nt" else ""
    verify_flush_cmake()
    pre = [*cc, "-std=c11", "-E", "-P", "-DISAAC_VITA_IMAGE_RETAIN=1",
           "-DISAAC_VITA_IMAGE_RETAIN_ORACLE=1", "-DGUEST_IMAGE_BASE=0x98000000U",
           "-DGUEST_STACK_REQUIRED=1", "-I", str(RUNTIME), str(MODULE)]
    off = run(pre)
    leaked = [token for token in FLUSH_TOKENS if token in off]
    require(not leaked, f"ADMIT_TOUCHED/IMAGE_PRELOAD OFF leave tokens in the unit: {leaked}")
    leaked = [token for token in FLUSH_TOKENS if token in run([*pre, *BIG_TRACE_DEFS])]
    require(not leaked, f"ADMIT_TOUCHED/IMAGE_PRELOAD OFF leave tokens in the BIG_TRACE unit: {leaked}")
    on = run([*pre, *ADMIT_DEFS, *PRELOAD_DEFS, *BIG_TRACE_DEFS])
    missing = [token for token in FLUSH_TOKENS if token not in on and token != "ISAAC_IRT_" and token != "ISAAC_IRP_"]
    require(not missing, f"the ON unit lacks {missing}")
    print(f"ADMIT_TOUCHED=OFF + IMAGE_PRELOAD=OFF: none of {len(FLUSH_TOKENS)} tokens in the "
          f"preprocessed unit (ON has them): PASS")
    for what, unit in (("OFF", off), ("IMAGE_PRELOAD without PIN", run([*pre, *ADMIT_DEFS, *PRELOAD4_DEFS])),
                       ("BIG_TRACE", run([*pre, *BIG_TRACE_DEFS]))):
        leaked = [token for token in PIN_TOKENS if token in unit]
        require(not leaked, f"IMAGE_PRELOAD_PIN OFF leaves tokens in the {what} unit: {leaked}")
    for defs in (PIN_OBSERVE_DEFS, PIN_ON_DEFS):
        unit = run([*pre, *ADMIT_DEFS, *PRELOAD4_DEFS, *defs])
        missing = [token for token in PIN_TOKENS if token not in unit]
        require(not missing, f"the IMAGE_PRELOAD_PIN unit ({defs[0]}) lacks {missing}")
    print(f"IMAGE_PRELOAD_PIN=OFF: none of {len(PIN_TOKENS)} tokens in the OFF, PRELOAD-without-PIN "
          f"and BIG_TRACE units (OBSERVE and ON have all of them): PASS")
    bad = subprocess.run([*pre, *PRELOAD4_DEFS, "-DISAAC_VITA_IMAGE_PRELOAD_PIN=3"], stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT)
    require(bad.returncode != 0 and b"ISAAC_VITA_IMAGE_PRELOAD_PIN must be 1 (OBSERVE) or 2 (ON)" in bad.stdout,
            "IMAGE_PRELOAD_PIN=3 is accepted")
    bad = subprocess.run([*pre, *PIN_ON_DEFS], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    require(bad.returncode != 0 and b"ISAAC_VITA_IMAGE_PRELOAD_PIN needs ISAAC_VITA_IMAGE_PRELOAD" in bad.stdout,
            "IMAGE_PRELOAD_PIN without IMAGE_PRELOAD is accepted")
    print("IMAGE_PRELOAD_PIN=3 and IMAGE_PRELOAD_PIN without IMAGE_PRELOAD: #error: PASS")
    bad = subprocess.run([*pre, "-DISAAC_VITA_IMAGE_RETAIN_ADMIT_TOUCHED=3"], stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT)
    require(bad.returncode != 0 and b"must be 1 (OBSERVE) or 2 (ON)" in bad.stdout,
            "ADMIT_TOUCHED=3 is accepted")
    bad = subprocess.run([*pre, "-DISAAC_VITA_IMAGE_PRELOAD=1"], stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT)
    require(bad.returncode != 0 and b"needs _LIST, _MAX_KB and _RESERVE_MB" in bad.stdout,
            "IMAGE_PRELOAD without its parameters is accepted")
    print("ADMIT_TOUCHED=3 and IMAGE_PRELOAD without parameters: #error: PASS")
    for name, phy, defs, expect in FLUSH_LEGS:
        for opt in ("-O2", "-O0"):
            exe = work / f"ir-flush-{name.replace('+', '-')}{opt}{suffix}"
            run(oracle_command(cc, opt, phy, MODULE, exe, defs))
            out = run([str(exe)]).strip()
            require(out == f"IMAGE RETAIN ORACLE PASS: {expect} records=1024",
                    f"oracle ({name} {opt}) said: {out}")
            print(f"host oracle {name} {opt}: {out}")
    text = MODULE.read_bytes().decode("utf-8")
    killed = 0
    for name, defs, old, new in FLUSH_MUTANTS:
        require(text.count(old) == 1, f"mutant {name}: anchor occurs {text.count(old)} times")
        mutant = work / f"mutant-{name}.c"
        mutant.write_bytes(text.replace(old, new).encode("utf-8"))
        exe = work / f"ir-flush-mutant-{name}{suffix}"
        command = oracle_command(cc, "-O2", 0, mutant, exe, defs)
        command.insert(command.index("-Werror") + 1, "-Wno-unused")       # a mutant may orphan a helper
        run(command)
        result = subprocess.run([str(exe)], check=False, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, text=True)
        verdict = result.stdout.strip().splitlines()[-1] if result.stdout.strip() else ""
        first = result.stderr.strip().splitlines()[0] if result.stderr.strip() else ""
        require(result.returncode == 1 and verdict.startswith("IMAGE RETAIN ORACLE FAIL: "),
                f"mutant {name} SURVIVED: rc={result.returncode} {verdict}")
        if name in FLUSH_MUTANT_KILLED_BY:
            want = f"[{FLUSH_MUTANT_KILLED_BY[name]}]: "
            require(want in first, f"mutant {name} killed outside {want!r}: {first}")
        killed += 1
        print(f"  mutant {name}: killed ({first[first.find('['):][:150]})")
    print(f"flush/preload mutants: {killed}/{len(FLUSH_MUTANTS)} killed "
          f"({len(FLUSH_MUTANT_KILLED_BY)} by their named scenario)")


# Production free-memory readers: the module (not the oracle build) picks
# them by ISAAC_VITA_GL_THREAD_WORKER x ISAAC_VITA_IMAGE_RETAIN_HEADROOM_PHYCONT.
# The PHYCONT reader once existed only in the direct-vitaGL branch, so a
# worker + PHYCONT build did not compile; a syntax pass over all four
# flavours against stand-in vitaGL/psp2 declarations keeps that closed.
STUB_HEADERS = {
    "vitaGL.h": "#pragma once\n#include <stddef.h>\n"
                "typedef enum { VGL_MEM_VRAM, VGL_MEM_RAM, VGL_MEM_SLOW, "
                "VGL_MEM_BUDGET, VGL_MEM_ALL } vglMemType;\n"
                "size_t vglMemFree(vglMemType type);\n"
                "size_t vglMemTotal(vglMemType type);\n",
    "psp2/kernel/processmgr.h": "#pragma once\n#include <stdint.h>\n"
                                "uint64_t sceKernelGetProcessTimeWide(void);\n",
    "psp2/kernel/sysmem.h": "#pragma once\n",
}
FLUSH_PRODUCTION_DEFS = [
    ("admit_touched=observe", OBSERVE_DEFS), ("admit_touched=on", ADMIT_DEFS),
    ("image_preload", PRELOAD_DEFS), ("admit_touched=on image_preload", [*ADMIT_DEFS, *PRELOAD_DEFS]),
    ("admit_touched=on image_preload big_trace", [*ADMIT_DEFS, *PRELOAD_DEFS, *BIG_TRACE_DEFS]),
    ("image_preload pin=observe", [*PRELOAD4_DEFS, *PIN_OBSERVE_DEFS]),
    ("image_preload pin=on", [*PRELOAD4_DEFS, *PIN_ON_DEFS]),
    ("admit_touched=on image_preload pin=on big_trace",
     [*ADMIT_DEFS, *PRELOAD4_DEFS, *PIN_ON_DEFS, *BIG_TRACE_DEFS]),
]


def run_flavours(cc: list[str], work: Path) -> None:
    stubs = work / "stub-include"
    for name, text in STUB_HEADERS.items():
        path = stubs / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8", newline="\n")
    for worker in (0, 1):
        for phy in (0, 1):
            run([*cc, "-std=c11", "-fsyntax-only", "-Wall", "-Wextra", "-Werror",
                 *RETAIN_DEFS, "-DGUEST_IMAGE_BASE=0x98000000U",
                 *(["-DISAAC_VITA_GL_THREAD_WORKER=1"] if worker else []),
                 *(["-DISAAC_VITA_IMAGE_RETAIN_HEADROOM_PHYCONT=1"] if phy else []),
                 "-I", str(RUNTIME), "-I", str(stubs), str(MODULE)])
            print(f"production flavour gl_thread_worker={worker} phycont={phy}: "
                  f"compiles (-Werror): PASS")
    run([*cc, "-std=c11", "-fsyntax-only", "-Wall", "-Wextra", "-Werror",
         *RETAIN_DEFS, *BIG_TRACE_DEFS, "-DGUEST_IMAGE_BASE=0x98000000U",
         "-DISAAC_VITA_GL_THREAD_WORKER=1", "-DISAAC_VITA_IMAGE_RETAIN_HEADROOM_PHYCONT=1",
         "-I", str(RUNTIME), "-I", str(stubs), str(MODULE)])
    print("production flavour gl_thread_worker=1 phycont=1 big_trace=1: compiles (-Werror): PASS")
    for name, defs in FLUSH_PRODUCTION_DEFS:
        for worker in (0, 1):
            run([*cc, "-std=c11", "-fsyntax-only", "-Wall", "-Wextra", "-Werror",
                 *RETAIN_DEFS, *defs, "-DGUEST_IMAGE_BASE=0x98000000U",
                 *(["-DISAAC_VITA_GL_THREAD_WORKER=1"] if worker else []),
                 "-DISAAC_VITA_IMAGE_RETAIN_HEADROOM_PHYCONT=1",
                 "-I", str(RUNTIME), "-I", str(stubs), str(MODULE)])
        print(f"production flavour {name} (gl_thread_worker=0/1, phycont=1): compiles (-Werror): PASS")


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
    require("isaac_vita_image_preload_level_init" not in symbols and
            not any(s.startswith(("irt_", "irp_", "irpp_")) for s in symbols),
            "the default ARM object carries ADMIT_TOUCHED / IMAGE_PRELOAD / IMAGE_PRELOAD_PIN symbols")
    for name, defs, wanted in (("admit_touched=observe", OBSERVE_DEFS, ()),
                               ("admit_touched=on", ADMIT_DEFS, ()),
                               ("image_preload", PRELOAD_DEFS,
                                ("isaac_vita_image_preload_level_init",)),
                               ("image_preload reserve=4", PRELOAD4_DEFS,
                                ("isaac_vita_image_preload_level_init",)),
                               ("admit_touched=on image_preload", [*ADMIT_DEFS, *PRELOAD_DEFS],
                                ("isaac_vita_image_preload_level_init",)),
                               ("image_preload pin=observe", [*PRELOAD4_DEFS, *PIN_OBSERVE_DEFS],
                                ("isaac_vita_image_preload_level_init",)),
                               ("image_preload pin=on", [*PRELOAD4_DEFS, *PIN_ON_DEFS],
                                ("isaac_vita_image_preload_level_init",)),
                               ("admit_touched=on image_preload pin=on",
                                [*ADMIT_DEFS, *PRELOAD4_DEFS, *PIN_ON_DEFS],
                                ("isaac_vita_image_preload_level_init",))):
        flavour = work / f"host_vita_image_retain.{name.replace('=', '-').replace(' ', '-')}.o"
        run([arm_cc, *EBOOT_FLAGS, *RETAIN_DEFS, *defs,
             "-include", str(VITA / "isaac_vita_raw_allocator_poison.h"),
             "-I", str(RUNTIME), "-I", str(VITA), "-c", str(MODULE), "-o", str(flavour)])
        table = {}
        for line in run([prefix + "nm", str(flavour)]).splitlines():
            parts = line.split()
            if len(parts) >= 2:
                table[parts[-1]] = parts[-2]
        for needed in wanted:
            require(table.get(needed) == "T", f"ARM {name}: {needed} not defined (T)")
        bad = [s for s in table if table[s] == "U" and re.fullmatch(
            r"(malloc|calloc|realloc|free|printf|sceClibPrintf|sub_0056d8c0|sub_0056d220|sub_0056c7e0)", s)]
        require(not bad, f"ARM {name} references {bad}")
        print(f"ARM cross-compile {name}: 0 errors, -Werror clean, new undefined="
              f"{sorted(s for s in table if table[s] == 'U' and s not in symbols)}: PASS")


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
    verify_flush_pe(pe)
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
            run_big_trace(cc, work)
            run_flush(cc, work)
            run_flavours(cc, work)
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
