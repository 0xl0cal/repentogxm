"""Actual ARM bounded LZ-copy check plus existing PNG/inflater integration.

No Vita or VM access. ARM execution proves bytes, bounds and rejection only,
not Cortex-A9 performance. Whole-image coverage reuses the existing oracle.
"""
from __future__ import annotations

import argparse
import hashlib
from pathlib import Path
import random
import struct
import tempfile

from unicorn import (Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_MEM_READ,
                     UC_HOOK_MEM_WRITE, UC_MEM_READ, UC_MEM_WRITE)
from unicorn.arm_const import (UC_ARM_REG_C1_C0_2, UC_ARM_REG_FPEXC,
                               UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0,
                               UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3,
                               UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6,
                               UC_ARM_REG_R7, UC_ARM_REG_R8, UC_ARM_REG_R9,
                               UC_ARM_REG_R10, UC_ARM_REG_R11,
                               UC_ARM_REG_SP)
from unicorn import arm_const

from test_vita_native_png_adler_neon import (
    ROOT, flags, run, tool, archive_isolation, host_validation)

BASE = "e1408633fc01812e96a8a9f09305716f7fb49a07"
D4_BASE = "b2098a2"
CODE, DATA, STACK, STOP = 0x10000, 0x30000, 0x60000, 0x70000


def build(clang, work):
    archive_isolation(clang, work)
    # The PNG-only entry must retain its complete object when the new option
    # is OFF, not merely preserve the ordinary archive symbol.
    path = "recomp/runtime/host_vita_native_png_miniz.c"
    before = run(["git", "-C", ROOT.parent, "show", f"{BASE}:{path}"])
    after = (ROOT.parent / path).read_text()
    objects = []
    for name, text in (("before", before), ("after", after)):
        obj = work / f"png-miniz-off-{name}.o"
        run(flags(clang) + ["-I", work, "-I", ROOT / "runtime",
            "-DISAAC_VITA_NATIVE_PNG_ADLER_NEON=1", "-x", "c", "-",
            "-c", "-o", obj], source=text)
        objects.append(obj.read_bytes())
    assert objects[0] == objects[1], "OFF PNG inflater object changed"
    print("OFF PNG inflater: complete ARM object identical to", BASE,
          hashlib.sha256(objects[0]).hexdigest())

    with (work / "string.h").open("a") as header:
        header.write("int memcmp(const void *, const void *, size_t);\n")
    path = "recomp/runtime/host_vita_native_png.c"
    sources = [run(["git", "-C", ROOT.parent, "show", f"{BASE}:{path}"]),
               (ROOT.parent / path).read_text()]
    objects = []
    for index, text in enumerate(sources):
        obj = work / f"png-core-off-{index}.o"
        run(flags(clang) + ["-I", work, "-I", ROOT / "runtime",
            "-DISAAC_VITA_NATIVE_PNG_ADLER_NEON=1",
            "-DISAAC_VITA_NATIVE_PNG_RGBA_NEON=1",
            "-DISAAC_VITA_NATIVE_PNG_TINFL_HEADER_RESET=1",
            "-x", "c", "-", "-c", "-o", obj], source=text)
        objects.append(obj.read_bytes())
    assert objects[0] == objects[1], "OFF PNG core object changed"
    print("OFF PNG core: complete ARM object identical to", BASE,
          hashlib.sha256(objects[0]).hexdigest())

    source = ROOT / "vita/host_tests/kage_vita_native_png_lz_neon.c"
    obj, elf, binary = (work / f"lz.{ext}" for ext in ("o", "elf", "bin"))
    run(flags(clang) + ["-fno-builtin", "-I", work, "-I", ROOT / "runtime",
        "-DISAAC_VITA_NATIVE_PNG_LZ_NEON=1", "-c", source, "-o", obj])
    script = work / "lz.ld"
    script.write_text("SECTIONS { . = 0x10000; .text : { *(.text*) } "
        ".rodata : { *(.rodata*) } /DISCARD/ : { *(.ARM.exidx*) "
        "*(.ARM.extab*) } }\n")
    run([tool(clang, "ld.lld"), "--gc-sections", "-T", script,
         "--entry=np_lz_test", obj, "-o", elf])
    symbols = run([tool(clang, "llvm-nm"), elf])
    entry = next(int(line.split()[0], 16) for line in symbols.splitlines()
                 if line.endswith(" np_lz_test"))
    run([tool(clang, "llvm-objcopy"), "-O", "binary", "--only-section=.text",
         "--only-section=.rodata", elf, binary])
    return binary.read_bytes(), entry


def integration_arm(clang, work, d4=False):
    # Reuse the existing complete coroutine/reset driver and its malformed,
    # interrupted and tiny-buffer cases. Both sides use PNG Adler; only the
    # copy helper differs (and the established full/header reset comparison).
    from test_vita_native_png_tinfl_reset import arm_cases
    variants = []
    for enabled in (0, 1):
        objects = []
        for name in ("runtime/host_vita_native_png_miniz.c",
                     "runtime/host_vita_native_png_adler.c",
                     "vita/host_tests/kage_vita_png_tinfl_reset_arm.c"):
            obj = work / (Path(name).stem + f"-lz{enabled}.o")
            extra = (["-Disaac_vita_archive_miniz_native=isaac_vita_png_miniz_native"]
                     if "host_tests" in name else [])
            run(flags(clang) + ["-marm", "-fno-builtin", "-I", work,
                "-I", ROOT / "runtime", "-DISAAC_VITA_NATIVE_PNG_ADLER_NEON=1",
                f"-DISAAC_VITA_NATIVE_PNG_LZ_NEON={1 if d4 else enabled}",
                f"-DISAAC_VITA_NATIVE_PNG_LZ_D4_NEON={enabled if d4 else 0}", *extra,
                "-c", ROOT / name, "-o", obj])
            objects.append(obj)
        elf, binary = work / f"tinfl-lz{enabled}.elf", work / f"tinfl-lz{enabled}.bin"
        run([tool(clang, "ld.lld"), "--gc-sections", "-T", work / "lz.ld",
            "--entry=np_test_step", *objects, "-o", elf])
        symbols = run([tool(clang, "llvm-nm"), elf])
        entry = next(int(line.split()[0], 16) for line in symbols.splitlines()
                     if line.endswith(" np_test_step"))
        run([tool(clang, "llvm-objcopy"), "-O", "binary", "--only-section=.text",
             "--only-section=.rodata", elf, binary])
        variants.append((binary.read_bytes(), entry))
    payloads = ([b"abcd" * 1025, bytes(range(16)) + b"wxyz" * 4097,
                 bytes(range(31)) + b"qrst" * 133] if d4 else [])
    arm_cases(*variants[0], comparison=variants[1], extra_payloads=payloads,
              output_initialized=d4)
    print(f"Above ARM tinfl comparison: PNG {'D4' if d4 else 'LZ'} "
          "OFF/full-reset vs ON/header-reset; "
          "both use the actual NEON Adler entry")


def build_d4(clang, work):
    before_dir = work / "d4-before"
    before_dir.mkdir(exist_ok=True)
    # The old entry includes its old four-argument callback callsite. Freeze
    # every changed included production source, not only the top-level file.
    for name in ("host_vita_native_png_miniz.c", "host_vita_native_png_lz_neon.h",
                 "host_vita_archive_miniz_native.c"):
        path = "recomp/runtime/" + name
        (before_dir / name).write_text(run(["git", "-C", ROOT.parent,
                                           "show", f"{D4_BASE}:{path}"]))
    for mode in ("-marm", "-mthumb"):
        for label, name in (("miniz", "host_vita_native_png_miniz.c"),
                            ("helper", None)):
            objects = []
            for version, include in (("before", before_dir), ("after", ROOT / "runtime")):
                text = ((include / name).read_text() if name else
                        (ROOT / "vita/host_tests/kage_vita_native_png_lz_neon.c").read_text())
                obj = work / f"d4-off-{label}-{mode[2:]}-{version}.o"
                run(flags(clang) + [mode, "-I", work, "-I", include,
                    "-I", ROOT / "runtime", "-DISAAC_VITA_NATIVE_PNG_ADLER_NEON=1",
                    "-DISAAC_VITA_NATIVE_PNG_LZ_NEON=1", "-x", "c", "-",
                    "-c", "-o", obj], source=text)
                objects.append(obj.read_bytes())
            assert objects[0] == objects[1], ("D4-OFF object changed", label, mode)
            print(f"D4 OFF / existing LZ ON {label} {mode}: whole object identical "
                  f"to {D4_BASE}, sha256={hashlib.sha256(objects[0]).hexdigest()}")
    source = ROOT / "vita/host_tests/kage_vita_native_png_lz_neon.c"
    obj, elf, binary = (work / f"lz-d4.{ext}" for ext in ("o", "elf", "bin"))
    run(flags(clang) + ["-fno-builtin", "-I", work, "-I", ROOT / "runtime",
        "-DISAAC_VITA_NATIVE_PNG_LZ_NEON=1", "-DISAAC_VITA_NATIVE_PNG_LZ_D4_NEON=1",
        "-c", source, "-o", obj])
    run([tool(clang, "ld.lld"), "--gc-sections", "-T", work / "lz.ld",
         "--entry=np_lz_d4_test", obj, "-o", elf])
    symbols = run([tool(clang, "llvm-nm"), elf])
    entry = next(int(line.split()[0], 16) for line in symbols.splitlines()
                 if line.endswith(" np_lz_d4_test"))
    run([tool(clang, "llvm-objcopy"), "-O", "binary", "--only-section=.text",
         "--only-section=.rodata", elf, binary])
    return binary.read_bytes(), entry


class Machine:
    def __init__(self, code, entry):
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        self.uc.mem_map(CODE, 0x10000)
        self.uc.mem_write(CODE, code)
        self.uc.mem_map(DATA, 0x20000)
        self.uc.mem_map(STACK, 0x10000)
        self.uc.mem_map(STOP, 0x1000)
        self.uc.reg_write(UC_ARM_REG_C1_C0_2, 0xf << 20)
        self.uc.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
        self.uc.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, self.access)
        self.entry = entry

    def access(self, uc, access, address, size, value, user):
        if STACK <= address and address + size <= STACK + 0x10000:
            return
        if access == UC_MEM_READ and CODE <= address and address + size <= CODE + 0x10000:
            return
        offset = address - self.source
        if access == UC_MEM_READ:
            assert self.accept and 0 <= offset and offset + size <= len(self.known)
            assert all(self.known[offset:offset + size]), "LZ read before initialization"
        else:
            assert access == UC_MEM_WRITE and self.accept
            assert self.output <= address and address + size <= self.output + self.length
            self.known[offset:offset + size] = b"\1" * size

    def call(self, distance, length, alignment, seed):
        source = DATA + 0x100 + alignment
        output = source + distance
        self.source, self.output, self.length = source, output, length
        self.accept = length >= 16 and (distance == 1 or distance >= 16)
        rng = random.Random(seed)
        prefix = bytes(rng.randrange(256) for _ in range(distance))
        original = b"\xa5" * 16 + prefix + b"\xcc" * length + b"\x5a" * 16
        expected = bytearray(original)
        if self.accept:
            for i in range(length):
                expected[16 + distance + i] = expected[16 + i]
        self.known = bytearray(b"\1" * distance + b"\0" * length)
        self.uc.mem_write(source - 16, original)
        self.uc.reg_write(UC_ARM_REG_SP, STACK + 0x8000)
        self.uc.reg_write(UC_ARM_REG_LR, STOP | 1)
        preserved = (UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7,
                     UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11)
        for register in preserved:
            self.uc.reg_write(register, 0x50a00 + register)
        for register, value in ((UC_ARM_REG_R0, output), (UC_ARM_REG_R1, source),
                                (UC_ARM_REG_R2, length), (UC_ARM_REG_R3, distance)):
            self.uc.reg_write(register, value)
        self.uc.emu_start(self.entry | 1, STOP, count=100000)
        assert self.uc.reg_read(UC_ARM_REG_PC) == STOP
        assert self.uc.reg_read(UC_ARM_REG_R0) == int(self.accept)
        assert self.uc.reg_read(UC_ARM_REG_SP) == STACK + 0x8000
        assert all(self.uc.reg_read(register) == 0x50a00 + register for register in preserved)
        assert self.uc.mem_read(source - 16, len(original)) == expected, (distance, length, alignment)


class D4Machine(Machine):
    def call(self, distance, length, history, alignment, seed):
        start = DATA + 0x100 + alignment
        output = start + history
        source = output - distance
        self.source, self.output, self.length = start, output, length
        self.accept = distance == 4 and length >= 32 and history >= 16
        rng = random.Random(seed)
        prefix = bytes(rng.randrange(256) for _ in range(history))
        original = b"\xa5" * 16 + prefix + b"\xcc" * length + b"\x5a" * 16
        expected = bytearray(original)
        if self.accept:
            for i in range(length):
                expected[16 + history + i] = expected[16 + history + i - 4]
        self.known = bytearray(b"\1" * history + b"\0" * length)
        self.uc.mem_write(start - 16, original)
        sp = STACK + 0x8000
        stacked = struct.pack("<4I", history, 0xa1b2c3d4, 0x99aa55cc, 0x10203040)
        self.uc.mem_write(sp, stacked)
        self.uc.reg_write(UC_ARM_REG_SP, sp)
        self.uc.reg_write(UC_ARM_REG_LR, STOP | 1)
        preserved = (UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7,
                     UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11)
        fp_preserved = tuple(getattr(arm_const, f"UC_ARM_REG_D{i}") for i in range(8, 16))
        for register in preserved:
            self.uc.reg_write(register, 0x50a00 + register)
        for register in fp_preserved:
            self.uc.reg_write(register, 0x123456789abc0000 + register)
        for register, value in ((UC_ARM_REG_R0, output), (UC_ARM_REG_R1, source),
                                (UC_ARM_REG_R2, length), (UC_ARM_REG_R3, distance)):
            self.uc.reg_write(register, value)
        self.uc.emu_start(self.entry | 1, STOP, count=100000)
        assert self.uc.reg_read(UC_ARM_REG_PC) == STOP
        assert self.uc.reg_read(UC_ARM_REG_R0) == int(self.accept), (distance, length, history)
        assert self.uc.reg_read(UC_ARM_REG_SP) == sp
        assert self.uc.mem_read(sp, len(stacked)) == stacked
        assert all(self.uc.reg_read(r) == 0x50a00 + r for r in preserved)
        assert all(self.uc.reg_read(r) == 0x123456789abc0000 + r for r in fp_preserved)
        assert self.uc.mem_read(start - 16, len(original)) == expected, (distance, length, history, alignment)


def d4_cases(clang, work):
    code, entry = build_d4(clang, work)
    machine = D4Machine(code, entry)
    lengths = list(range(34)) + [47, 48, 49, 63, 64, 65, 127, 128, 129, 255, 256, 257, 258]
    cases = 0
    for distance, histories in [(4, list(range(18)) + [31, 32, 33, 63, 64, 127])] + [
            (d, [16]) for d in list(range(18)) + [127, 256, 32768] if d != 4]:
        for history in histories:
            for length in lengths:
                for alignment in range(16):
                    machine.call(distance, length, history, alignment, cases)
                    cases += 1
    for history in (16, 32, 256):
        for length in range(32, 259):
            for alignment in range(16):
                machine.call(4, length, history, alignment, cases)
                cases += 1
    print(f"ARM D4: PASS {cases} cases; fifth AAPCS stack argument, "
          f"GPR/D8-D15 preserved, initialized-only bounds/tails; helper text={len(code)}B")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--clang", required=True, type=Path)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--host", action="store_true")
    parser.add_argument("--d4", action="store_true", help="also validate the separate D4 extension")
    parser.add_argument("--corpus", action="append", default=[], type=Path)
    args = parser.parse_args()
    work = args.out or Path(tempfile.mkdtemp(prefix="isaac-png-lz-"))
    work.mkdir(parents=True, exist_ok=True)
    code, entry = build(args.clang, work)
    machine = Machine(code, entry)
    cases = 0
    for distance in list(range(65)) + [127, 128, 255, 256, 257, 1024, 32768]:
        for length in list(range(34)) + [47, 48, 49, 63, 64, 65, 127, 128, 129, 255, 256, 257, 258]:
            for alignment in range(16):
                machine.call(distance, length, alignment, cases)
                cases += 1
    print(f"ARM LZ: PASS {cases} cases; initialized-only reads, exact output bounds, "
          f"unaligned/overlapping/rejected paths; helper text={len(code)}B")
    integration_arm(args.clang, work)
    if args.d4:
        d4_cases(args.clang, work)
        integration_arm(args.clang, work, d4=True)
    if args.host:
        definitions = ["-DISAAC_VITA_NATIVE_PNG_LZ_NEON=1"]
        if args.d4:
            definitions.append("-DISAAC_VITA_NATIVE_PNG_LZ_D4_NEON=1")
        host_validation(args.clang, work, args.corpus,
                        definitions=definitions)


if __name__ == "__main__":
    main()
