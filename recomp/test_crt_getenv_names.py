#!/usr/bin/env python3
"""Frozen-PE proof of the getenv surface the Vita CRT shim answers.

The image references the getenv import slot from exactly three places: the
save-root probe (USERPROFILE), the HOMEDRIVE/HOMEPATH fallback and libyuv's
InitCpuFlags, which asks for twelve LIBYUV_DISABLE_* switches in a fixed
order.  host_vita_crt.c answers all of them; an unmeasured name still faults.
This test pins the IAT references, the twelve string literals at their RVAs,
the order in which InitCpuFlags pushes them, and the header table.

usage: python test_crt_getenv_names.py --pe <isaac-ng.exe.unpacked.exe>
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
GETENV_IAT_RVA = 0x006064E4
IAT_REFERENCES = [0x0050AA00, 0x0050AB4B, 0x005D651D]
IAT_LOAD_OPCODES = (b"\xff\x15", b"\x8b\x05", b"\x8b\x0d", b"\x8b\x15",
                    b"\x8b\x1d", b"\x8b\x35", b"\x8b\x3d")
LIBYUV_INIT_CPU_FLAGS_START = 0x005D6480
LIBYUV_FIRST_CALL = 0x005D6528
LIBYUV_LITERALS_FIRST_RVA = 0x00769F6C
LIBYUV_LITERALS_LAST_RVA = 0x0076A054
LIBYUV_NAMES_IN_PUSH_ORDER = [
    "LIBYUV_DISABLE_X86", "LIBYUV_DISABLE_SSE2", "LIBYUV_DISABLE_SSSE3",
    "LIBYUV_DISABLE_SSE41", "LIBYUV_DISABLE_SSE42", "LIBYUV_DISABLE_AVX",
    "LIBYUV_DISABLE_AVX2", "LIBYUV_DISABLE_ERMS", "LIBYUV_DISABLE_FMA3",
    "LIBYUV_DISABLE_AVX3", "LIBYUV_DISABLE_F16C", "LIBYUV_DISABLE_ASM",
]


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def header_names() -> list[str]:
    text = (HERE / "runtime" / "host_vita_crt.h").read_text(encoding="utf-8")
    line = next((l for l in text.splitlines()
                 if l.startswith("#define ISAAC_VITA_CRT_GETENV_LIBYUV_NAMES ")), None)
    require(line is not None, "header table missing")
    assert line is not None
    names = re.findall(r'"([A-Z0-9_]+)"', line)
    count = re.search(r"ISAAC_VITA_CRT_GETENV_LIBYUV_NAME_COUNT\s+(\d+)U", text)
    require(count is not None and int(count.group(1)) == len(names) == 12,
            f"header count/table mismatch: {names}")
    require(len(set(names)) == 12, "duplicate names in the header table")
    require(all(len(n) <= 32 for n in names), "a name exceeds GETENV_NAME_MAX")
    source = (HERE / "runtime" / "host_vita_crt.c").read_text(encoding="utf-8")
    require("vita_crt_getenv_is_libyuv_switch(measured)" in source,
            "getenv does not consult the libyuv table")
    require('"getenv received an unmeasured variable name"' in source,
            "unmeasured names must still fault")
    return names


def check_pe(path: Path, names: list[str]) -> None:
    data = path.read_bytes()
    require(len(data) == PE_SIZE, f"PE size {len(data)} != {PE_SIZE}")
    require(hashlib.sha256(data).hexdigest() == PE_SHA256, "PE SHA-256 drifted")
    pe = pefile.PE(data=data, fast_load=True)

    text = next(s for s in pe.sections if s.Name.rstrip(b"\0") == b".text")
    code = pe.get_data(text.VirtualAddress, text.Misc_VirtualSize)
    slot = (IMAGE_BASE + GETENV_IAT_RVA).to_bytes(4, "little")
    references = []
    for match in re.finditer(re.escape(slot), code):
        offset = match.start()
        if code[offset - 2:offset] in IAT_LOAD_OPCODES:
            references.append(text.VirtualAddress + offset - 2)
    require(references == IAT_REFERENCES,
            f"getenv IAT references drifted: {[hex(r) for r in references]}")

    literal_rvas = {}
    for name in names:
        needle = name.encode() + b"\0"
        offsets = [m.start() for m in re.finditer(re.escape(needle), data)
                   if data[m.start() - 1] == 0]
        require(len(offsets) == 1, f"{name}: {len(offsets)} literals")
        literal_rvas[name] = pe.get_rva_from_offset(offsets[0])
    require(min(literal_rvas.values()) == LIBYUV_LITERALS_FIRST_RVA and
            max(literal_rvas.values()) == LIBYUV_LITERALS_LAST_RVA,
            f"libyuv literal range drifted: {literal_rvas}")

    decoder = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    pushed = []
    first_call = None
    for insn in decoder.disasm(pe.get_data(LIBYUV_INIT_CPU_FLAGS_START, 0x300),
                               IMAGE_BASE + LIBYUV_INIT_CPU_FLAGS_START):
        if insn.mnemonic == "push" and insn.op_str.startswith("0x"):
            value = int(insn.op_str, 16) - IMAGE_BASE
            for name, rva in literal_rvas.items():
                if rva == value:
                    pushed.append(name)
        if insn.mnemonic == "call" and insn.op_str == "edi" and first_call is None:
            first_call = insn.address - IMAGE_BASE
        if insn.mnemonic == "ret":
            break
    require(first_call == LIBYUV_FIRST_CALL,
            f"first libyuv getenv call at {first_call and hex(first_call)}")
    require(pushed == LIBYUV_NAMES_IN_PUSH_ORDER,
            f"libyuv getenv push order drifted: {pushed}")
    require(sorted(pushed) == sorted(names), "header table != pushed names")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pe", type=Path,
                        default=Path(os.environ.get("ISAAC_AUDIO_TEST_PE", "")))
    arguments = parser.parse_args()
    names = header_names()
    if not arguments.pe or not arguments.pe.is_file():
        print("crt getenv names: header OK, no --pe given (PE pins not run)")
        return 0
    check_pe(arguments.pe, names)
    print("crt getenv names: PASS (3 IAT references, 12 libyuv literals, push order, header)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
