"""Focused poisoned/reused PNG tinfl reset checks, actual ARM + whole PNG.

The ARM read tracker rejects any read of state scratch before this stream
writes it. It compares full versus header-only reset after every inflater
call, including damaged and interrupted streams. No device or VM required.
"""
from __future__ import annotations

import argparse
import hashlib
import os
from pathlib import Path
import random
import struct
import sys
import zlib

from unicorn import (Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_MEM_READ,
                     UC_HOOK_MEM_WRITE, UC_MEM_WRITE)
from unicorn.arm_const import (UC_CPU_ARM_CORTEX_A9, UC_ARM_REG_C1_C0_2,
                               UC_ARM_REG_FPEXC, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0,
                               UC_ARM_REG_SP)

from test_vita_native_png_adler_neon import ROOT, flags, run, tool, archive_isolation

CODE, STATE, INPUT, OUTPUT, ARGS, STACK, STOP = (
    0x10000, 0x20000, 0x30000, 0x50000, 0x70000, 0x80000, 0x90000)
STATE_BYTES, HEADER_BYTES = 0x2af0, 0x40
BASE = "a2efd537cce7c059e0108691d7426089ed67c4a0"


def build_arm(clang, work):
    # archive_isolation also supplies the minimal string.h for bare Clang.
    archive_isolation(clang, work)
    with (work / "string.h").open("a", encoding="ascii") as header:
        header.write("int memcmp(const void *, const void *, size_t);\n")
    # OFF must retain the full reset. Compare the complete pure PNG core
    # object with its integration base, using identical headers/source name.
    path = "recomp/runtime/host_vita_native_png.c"
    sources = [run(["git", "-C", ROOT.parent, "show", f"{BASE}:{path}"]),
               (ROOT.parent / path).read_text(encoding="utf-8")]
    core_objects = []
    for index, source in enumerate(sources):
        obj = work / f"png-off-{index}.o"
        run(flags(clang) + ["-I", work, "-I", ROOT / "runtime", "-x", "c",
                           "-", "-c", "-o", obj], source=source)
        core_objects.append(obj.read_bytes())
    assert core_objects[0] == core_objects[1], "default-OFF PNG core changed"
    print("Default-OFF PNG core: ARM object byte-identical to", BASE,
          "sha256=" + hashlib.sha256(core_objects[1]).hexdigest())
    objects = []
    for source in (ROOT / "runtime/host_vita_archive_miniz_native.c",
                   ROOT / "vita/host_tests/kage_vita_png_tinfl_reset_arm.c"):
        obj = work / (source.stem + ".o")
        # Use ARM mode for this large coroutine: the installed Unicorn
        # mis-executes a Thumb multi-instruction IT error epilogue even in
        # the unchanged full-reset baseline. This is not a Vita binary test.
        run(flags(clang) + ["-marm", "-fno-builtin", "-I", work, "-I", ROOT / "runtime",
                           "-c", source, "-o", obj])
        objects.append(obj)
    linker = work / "reset.ld"
    linker.write_text("SECTIONS { . = 0x10000; .text : { *(.text*) } "
                      ".rodata : { *(.rodata*) } /DISCARD/ : { *(.ARM.exidx*) "
                      "*(.ARM.extab*) } }\n", encoding="ascii")
    elf, binary = work / "reset.elf", work / "reset.bin"
    run([tool(clang, "ld.lld"), "--gc-sections", "-T", linker,
         "--entry=np_test_step"] + objects + ["-o", elf])
    symbols = run([tool(clang, "llvm-nm"), elf])
    entry = next(int(line.split()[0], 16) for line in symbols.splitlines()
                 if line.endswith(" np_test_step"))
    run([tool(clang, "llvm-objcopy"), "-O", "binary", "--only-section=.text",
         "--only-section=.rodata", elf, binary])
    return binary.read_bytes(), entry


class Inflater:
    def __init__(self, code, entry, short, output_initialized=False):
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
        self.uc.ctl_set_cpu_model(UC_CPU_ARM_CORTEX_A9)
        self.uc.reg_write(UC_ARM_REG_C1_C0_2, 0xf << 20)
        self.uc.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
        for address, size in ((CODE, 0x10000), (STATE, 0x10000),
                              (INPUT, 0x20000), (OUTPUT, 0x20000),
                              (ARGS, 0x10000), (STACK, 0x10000), (STOP, 0x1000)):
            self.uc.mem_map(address, size)
        self.uc.mem_write(CODE, code)
        self.uc.mem_write(STATE, b"\xa5" * STATE_BYTES)
        self.entry, self.short = entry, short
        self.known = bytearray(STATE_BYTES)
        self.state_reads = 0
        self.output_known = bytearray(0x20000) if output_initialized else None
        self.output_reads = 0
        self.uc.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, self.access)

    def access(self, uc, access, address, size, value, user):
        if STATE <= address < STATE + 0x10000:
            offset = address - STATE
            assert offset + size <= STATE_BYTES, ("state overflow", offset, size)
            if access == UC_MEM_WRITE:
                self.known[offset:offset + size] = b"\1" * size
            else:
                assert all(self.known[offset:offset + size]), (
                    "state scratch read before stream initialization", offset, size)
                self.state_reads += 1
        elif self.output_known is not None and OUTPUT <= address < OUTPUT + 0x20000:
            offset = address - OUTPUT
            assert offset + size <= len(self.output_known), "output allocation overflow"
            if access == UC_MEM_WRITE:
                self.output_known[offset:offset + size] = b"\1" * size
            else:
                assert all(self.output_known[offset:offset + size]), (
                    "output read before stream initialization", offset, size)
                self.output_reads += 1

    def start(self, packed):
        count = HEADER_BYTES if self.short else STATE_BYTES
        # Deliberately retain the previous stream's scratch, including after
        # failure/interruption. Only the new stream's reset marks bytes known.
        self.uc.mem_write(STATE, b"\0" * count)
        self.known[:] = b"\0" * STATE_BYTES
        self.known[:count] = b"\1" * count
        self.uc.mem_write(INPUT, packed)
        self.uc.mem_write(OUTPUT, b"\xa7" * 0x20000)
        if self.output_known is not None:
            self.output_known[:] = b"\0" * len(self.output_known)

    def step(self, input_pos, input_size, output_pos, output_size, more):
        self.uc.mem_write(ARGS, struct.pack("<7I", STATE, INPUT + input_pos,
            ARGS + 32, OUTPUT, OUTPUT + output_pos, ARGS + 36,
            15))  # native PNG always sets PARSE|MORE_INPUT|NONWRAP|ADLER
        self.uc.mem_write(ARGS + 32, struct.pack("<2I", input_size, output_size))
        self.uc.reg_write(UC_ARM_REG_SP, STACK + 0x8000)
        self.uc.reg_write(UC_ARM_REG_LR, STOP)
        self.uc.reg_write(UC_ARM_REG_R0, ARGS)
        self.uc.emu_start(self.entry, STOP, count=3000000)
        assert self.uc.reg_read(UC_ARM_REG_PC) == STOP, "tinfl did not return"
        status = self.uc.reg_read(UC_ARM_REG_R0)
        if status & 0x80000000:
            status -= 1 << 32
        consumed, produced = struct.unpack("<2I", self.uc.mem_read(ARGS + 32, 8))
        assert consumed <= input_size and produced <= output_size
        return status, consumed, produced


def arm_cases(code, entry, comparison=None, extra_payloads=(), output_initialized=False):
    new_code, new_entry = comparison or (code, entry)
    old = Inflater(code, entry, False, output_initialized)
    new = Inflater(new_code, new_entry, True, output_initialized)
    rng = random.Random(0x5a1200)
    payloads = [b"", b"a", bytes(range(256)) * 8, b"\xff" * 9001,
                bytes(rng.randrange(256) for _ in range(4097)),
                bytes(rng.randrange(7) for _ in range(8193))]
    payloads.extend(extra_payloads)
    fixtures = []
    for data in payloads:
        for level, strategy in ((0, zlib.Z_DEFAULT_STRATEGY),
                                (6, zlib.Z_FIXED), (9, zlib.Z_DEFAULT_STRATEGY)):
            compressor = zlib.compressobj(level, zlib.DEFLATED, 15, 8, strategy)
            packed = compressor.compress(data) + compressor.flush()
            fixtures.append((packed, data, False))
            fixtures.append((packed[:-1] + bytes([packed[-1] ^ 0x5a]), None, False))
            fixtures.append((packed[:max(1, len(packed) // 2)], None, False))
            fixtures.append((packed, None, True))  # abandon partway, then reuse
    # Partial/random invalid headers, block types and Huffman descriptions.
    fixtures += [(bytes(rng.randrange(256) for _ in range(n)), None, False)
                 for n in range(1, 65)]
    for _ in range(32):
        packed = bytearray(fixtures[rng.randrange(0, 72, 4)][0])
        packed[rng.randrange(len(packed))] ^= 1 << rng.randrange(8)
        fixtures.append((bytes(packed), None, False))
    calls = 0
    for fixture_index, (packed, expected, abandon) in enumerate(fixtures):
        for ins, outs in (([1, 2, 7, 19, 4096], [1, 17, 4096]),
                          ([37, 65536], [65536])):
            old.start(packed)
            new.start(packed)
            ip = op = 0
            for index in range(10000):
                ni = min(ins[index % len(ins)], len(packed) - ip)
                no = min(outs[index % len(outs)], 0x20000 - op)
                more = ip + ni < len(packed)
                try:
                    a = old.step(ip, ni, op, no, more)
                    b = new.step(ip, ni, op, no, more)
                except Exception as error:
                    raise AssertionError((fixture_index, len(packed), index,
                                          ip, ni, op, no, more)) from error
                assert a == b, (len(packed), index, a, b)
                before = old.uc.mem_read(STATE, STATE_BYTES)
                after = new.uc.mem_read(STATE, STATE_BYTES)
                assert all(a == b for a, b, known in zip(before, after, new.known) if known)
                assert old.uc.mem_read(OUTPUT, op + no) == new.uc.mem_read(OUTPUT, op + no)
                status, consumed, produced = a
                ip += consumed
                op += produced
                calls += 1
                if abandon or status <= 0:
                    if expected is not None:
                        assert status == 0 and ip == len(packed)
                        assert bytes(new.uc.mem_read(OUTPUT, op)) == expected
                    break
                if status == 1 and ip == len(packed):
                    # Native PNG asks its reader for more IDAT bytes; a
                    # truncated fixture fails there, with this state unused.
                    assert expected is None
                    break
                if op == 0x20000:
                    # A damaged stream can expand until the caller's fixed
                    # output bound; production PNG rejects HAS_MORE_OUTPUT.
                    assert expected is None and status == 2
                    break
                assert consumed or produced, ("nonterminal tinfl made no progress",
                    fixture_index, index, ip, ni, op, no, more, a)
            else:
                raise AssertionError("tinfl exceeded fixture call bound")
    print(f"ARM tinfl header reset: PASS {len(fixtures) * 2} reused/poisoned streams / "
          f"{calls} calls; {new.state_reads} state reads initialized before use; "
          "status, consumed/produced, all initialized state and output identical")
    if output_initialized:
        print(f"ARM tinfl initialized-only output: PASS {new.output_reads} reads")


def whole_png(clang, work, corpora):
    environment = os.environ.copy()
    if os.name == "nt":
        from setuptools.msvc import msvc14_get_vc_env
        environment = {key.upper(): value for key, value in environment.items()}
        environment.update({key.upper(): value for key, value in
                            msvc14_get_vc_env("x64").items()})
    for enabled in (0, 1):
        executable = work / (f"png-reset-{enabled}" + (".exe" if os.name == "nt" else ""))
        run([clang, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
             "-fno-strict-aliasing", "-D_CRT_SECURE_NO_WARNINGS=1",
             "-DISAAC_VITA_NATIVE_PNG_ORACLE=1",
             f"-DISAAC_VITA_NATIVE_PNG_TINFL_HEADER_RESET={enabled}",
             ROOT / "runtime/host_vita_native_png.c",
             ROOT / "runtime/host_vita_archive_miniz_native.c",
             ROOT / "runtime/vita_native_png_oracle.c", "-o", executable] +
             ([] if os.name == "nt" else ["-lm"]), env=environment)
        command = [sys.executable, ROOT / "test_vita_native_png.py", "--oracle",
                   executable, "--keep", work / f"whole-png-{enabled}"]
        for corpus in corpora:
            command += ["--corpus", corpus]
        print(f"PNG header reset={enabled}: " + run(command, env=environment).strip())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--clang", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--corpus", type=Path, action="append", default=[])
    parser.add_argument("--host", action="store_true")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    code, entry = build_arm(args.clang, args.out)
    arm_cases(code, entry)
    if args.host:
        whole_png(args.clang, args.out, args.corpus)


if __name__ == "__main__":
    main()
