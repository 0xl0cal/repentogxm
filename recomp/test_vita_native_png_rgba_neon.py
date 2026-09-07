"""Compile the actual decoder dispatch/tails to ARM and compare in Unicorn.

No Vita/VM use. The emulated ARM result is a byte/bounds check, NOT a timing
prediction. Requires Clang + LLD + llvm-objcopy and Python unicorn. The whole
PNG identity/fault suite remains test_vita_native_png.py.
"""
from __future__ import annotations

import argparse
from pathlib import Path
import random
import subprocess
import tempfile

from unicorn import (Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_MEM_READ,
                     UC_HOOK_MEM_WRITE, UC_MEM_READ)
from unicorn.arm_const import (UC_ARM_REG_C1_C0_2, UC_ARM_REG_FPEXC,
                               UC_ARM_REG_LR, UC_ARM_REG_R0, UC_ARM_REG_R1,
                               UC_ARM_REG_R2, UC_ARM_REG_R3, UC_ARM_REG_SP)

ROOT = Path(__file__).resolve().parent
CODE, MEMORY, STACK, STOP = 0x10000, 0x20000, 0x50000, 0x60000


def run(command):
    result = subprocess.run([str(x) for x in command], capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(f"{command}:\n{result.stdout}{result.stderr}")
    return result.stdout


def compile_arm(clang, work, enabled):
    # The bare-metal translation only needs libc declarations, not implementations;
    # gc-sections removes the full decode path, leaving production filter dispatch.
    (work / "string.h").write_text(
        "#include <stddef.h>\nvoid *memset(void *, int, size_t);\n"
        "void *memcpy(void *, const void *, size_t);\n"
        "int memcmp(const void *, const void *, size_t);\n", encoding="ascii")
    stem = work / f"rgba-{enabled}"
    source = ROOT / "vita/host_tests/kage_vita_native_png_rgba_neon.c"
    run([clang, "--target=armv7-none-eabi", "-std=c11", "-O2", "-Wall", "-Wextra",
         "-Werror", "-ffreestanding", "-fno-strict-aliasing", "-ffunction-sections",
         "-fdata-sections", "-mcpu=cortex-a9", "-mfpu=neon", "-mfloat-abi=softfp",
         "-mthumb", f"-DISAAC_VITA_NATIVE_PNG_RGBA_NEON={enabled}", "-I", work,
         "-c", source, "-o", stem.with_suffix(".o")])
    tools = clang.parent
    suffix = ".exe" if clang.suffix == ".exe" else ""
    run([tools / f"ld.lld{suffix}", "--gc-sections", "--entry=isaac_np_test_rgba_filter",
         "-Ttext=0x10000", stem.with_suffix(".o"), "-o", stem.with_suffix(".elf")])
    symbols = run([tools / f"llvm-nm{suffix}", stem.with_suffix(".elf")])
    entry = next(int(line.split()[0], 16) for line in symbols.splitlines()
                 if line.endswith(" isaac_np_test_rgba_filter"))
    run([tools / f"llvm-objcopy{suffix}", "-O", "binary", "--only-section=.text",
         stem.with_suffix(".elf"), stem.with_suffix(".bin")])
    return stem.with_suffix(".bin").read_bytes(), entry


def reference(raw, above, kind):
    result = bytearray(raw)
    for i in range(len(raw)):
        left = result[i - 4] if i >= 4 else 0
        up = above[i] if above is not None else 0
        corner = above[i - 4] if above is not None and i >= 4 else 0
        if kind == 1:
            predictor = left
        elif kind == 3:
            predictor = (left + up) // 2
        else:
            p = left + up - corner
            distances = (abs(p - left), abs(p - up), abs(p - corner))
            predictor = (left, up, corner)[distances.index(min(distances))]
        result[i] = (raw[i] + predictor) & 255
    return bytes(result)


class Machine:
    def __init__(self, code, entry):
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        self.uc.mem_map(CODE, 0x10000)
        self.uc.mem_write(CODE, code)
        self.uc.mem_map(MEMORY, 0x20000)
        self.uc.mem_map(STACK, 0x10000)
        self.uc.mem_map(STOP, 0x1000)
        self.uc.reg_write(UC_ARM_REG_C1_C0_2, 0xF << 20)
        self.uc.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
        self.entry = entry
        self.allowed = []
        self.accesses = []
        self.uc.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, self.access)

    def access(self, uc, access, address, size, value, user):
        if STACK <= address < STACK + 0x10000:
            return
        if access == UC_MEM_READ and CODE <= address < CODE + 0x10000:
            return  # Compiler-owned literal pools, not source row bytes.
        if not any(start <= address and address + size <= end
                   for start, end in self.allowed):
            raise AssertionError(f"row access outside logical bounds: {address:x}+{size}")
        self.accesses.append((address, size))

    def check(self, raw, above, kind, alignment):
        n = len(raw)
        row = MEMORY + 0x100 + alignment
        previous = MEMORY + 0x11000 + ((alignment * 7) & 15)
        self.allowed = [(row, row + n)]
        if above is not None:
            self.allowed.append((previous, previous + n))
        self.accesses = []
        self.uc.mem_write(row - 16, b"\x5a" * (n + 32))
        self.uc.mem_write(row, raw)
        if above is not None:
            self.uc.mem_write(previous, above)
        self.uc.reg_write(UC_ARM_REG_SP, STACK + 0x8000)
        self.uc.reg_write(UC_ARM_REG_LR, STOP | 1)
        self.uc.reg_write(UC_ARM_REG_R0, row)
        self.uc.reg_write(UC_ARM_REG_R1, previous if above is not None else 0)
        self.uc.reg_write(UC_ARM_REG_R2, n)
        self.uc.reg_write(UC_ARM_REG_R3, kind)
        self.uc.emu_start(self.entry | 1, STOP, count=2000000)
        actual = bytes(self.uc.mem_read(row, n))
        assert actual == reference(raw, above, kind), (n, kind, alignment)
        assert bytes(self.uc.mem_read(row - 16, 16)) == b"\x5a" * 16
        assert bytes(self.uc.mem_read(row + n, 16)) == b"\x5a" * 16
        if above is not None:
            assert bytes(self.uc.mem_read(previous, n)) == above


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--clang", type=Path, required=True)
    parser.add_argument("--out", type=Path)
    args = parser.parse_args()
    work = args.out or Path(tempfile.mkdtemp(prefix="isaac-png-rgba-neon-"))
    work.mkdir(parents=True, exist_ok=True)
    rng = random.Random(20260905)
    cases = []
    # Every four-byte scalar tail and row-base alignment, plus long rows. Values
    # include all-zero, all-255, wraparound and independent random neighbour rows.
    for pixels in list(range(1, 66)) + [127, 128, 129, 255, 256, 257, 1024, 2048]:
        n = pixels * 4
        for pattern in range(4):
            raw = bytes((0 if pattern == 0 else 255 if pattern == 1 else
                         (i * 127 + 255) & 255 if pattern == 2 else
                         rng.randrange(256)) for i in range(n))
            above = bytes(rng.randrange(256) for _ in range(n))
            for first in (False, True):
                for kind in (1, 3, 4):
                    cases.append((raw, None if first else above, kind,
                                  (pixels + pattern * 5) & 15))
    for enabled in (0, 1):
        code, entry = compile_arm(args.clang, work, enabled)
        machine = Machine(code, entry)
        for case in cases:
            machine.check(*case)
        print(f"ARM RGBA filters: mode={enabled} PASS cases={len(cases)} code_bytes={len(code)}; "
              "exact logical read/write bounds, scalar tails, first rows, wraparound")


if __name__ == "__main__":
    main()
