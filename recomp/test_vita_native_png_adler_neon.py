"""Focused actual-ARM checks for the PNG-only Adler helper and archive isolation.

Uses installed Clang/LLD/llvm-objcopy and Python Unicorn; no Vita or build VM.
The unchanged archive entry is compiled from the pinned integration base and
current source with identical stdin/source-name/options, then compared as an
entire ARM object. The new helper is executed, not merely cross-compiled.
"""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import os
from pathlib import Path
import random
import subprocess
import sys
import tempfile
import zlib

from unicorn import (Uc, UC_ARCH_ARM, UC_MODE_THUMB, UC_HOOK_MEM_READ,
                     UC_HOOK_MEM_WRITE, UC_MEM_READ)
from unicorn.arm_const import (UC_ARM_REG_C1_C0_2, UC_ARM_REG_FPEXC,
                               UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1,
                               UC_ARM_REG_R2, UC_ARM_REG_SP)

ROOT = Path(__file__).resolve().parent
BASE = "1676f4b4b3ccc307813609e2f72cd08f50f9ad53"
CODE, DATA, STACK, STOP = 0x10000, 0x30000, 0x80000, 0x90000


def run(command, *, source=None, env=None):
    result = subprocess.run([str(x) for x in command], input=source,
                            capture_output=True, text=True, env=env)
    if result.returncode:
        raise RuntimeError(f"{command}:\n{result.stdout}{result.stderr}")
    return result.stdout


def tool(clang, name):
    return clang.parent / (name + (".exe" if clang.suffix == ".exe" else ""))


def flags(clang):
    return [clang, "--target=armv7-none-eabi", "-std=c11", "-O2", "-Wall",
            "-Wextra", "-Werror", "-ffreestanding", "-fno-strict-aliasing",
            "-ffunction-sections", "-fdata-sections", "-mcpu=cortex-a9",
            "-mfpu=neon", "-mfloat-abi=softfp", "-mthumb"]


def compile_arm(clang, work, enabled):
    stem = work / f"adler-{enabled}"
    run(flags(clang) + [f"-DISAAC_VITA_NATIVE_PNG_ADLER_NEON={enabled}",
        "-c", ROOT / "runtime/host_vita_native_png_adler.c",
        "-o", stem.with_suffix(".o")])
    # Include constant pools with the code. No platform runtime is linked.
    linker = work / "adler.ld"
    linker.write_text("SECTIONS { . = 0x10000; .text : { *(.text*) } "
                      ".rodata : { *(.rodata*) } /DISCARD/ : { *(.ARM.exidx*) "
                      "*(.ARM.extab*) } }\n", encoding="ascii")
    run([tool(clang, "ld.lld"), "--gc-sections", "-T", linker,
         "--entry=isaac_np_adler32_update", stem.with_suffix(".o"),
         "-o", stem.with_suffix(".elf")])
    symbols = run([tool(clang, "llvm-nm"), stem.with_suffix(".elf")])
    entry = next(int(line.split()[0], 16) for line in symbols.splitlines()
                 if line.endswith(" isaac_np_adler32_update"))
    run([tool(clang, "llvm-objcopy"), "-O", "binary", "--only-section=.text",
         "--only-section=.rodata", stem.with_suffix(".elf"),
         stem.with_suffix(".bin")])
    return stem.with_suffix(".bin").read_bytes(), entry


def archive_isolation(clang, work):
    (work / "string.h").write_text(
        "#include <stddef.h>\nvoid *memset(void *, int, size_t);\n"
        "void *memcpy(void *, const void *, size_t);\n", encoding="ascii")
    path = "recomp/runtime/host_vita_archive_miniz_native.c"
    before = run(["git", "-C", ROOT.parent, "show", f"{BASE}:{path}"])
    after = (ROOT.parent / path).read_text(encoding="utf-8")
    objects = []
    for label, source in (("before", before), ("after", after)):
        obj = work / f"archive-{label}.o"
        run(flags(clang) + ["-I", work, "-I", ROOT / "runtime", "-x", "c",
                           "-", "-c", "-o", obj], source=source)
        objects.append(obj.read_bytes())
    assert objects[0] == objects[1], "ordinary archive ARM object changed"
    print("Archive isolation: whole ARM object byte-identical to", BASE,
          "sha256=" + hashlib.sha256(objects[1]).hexdigest())
    # Measure, do not conceal, the opt-in duplicate inflater's text cost.
    obj = work / "png-miniz.o"
    run(flags(clang) + ["-DISAAC_VITA_NATIVE_PNG_ADLER_NEON=1", "-I", work,
        "-c", ROOT / "runtime/host_vita_native_png_miniz.c", "-o", obj])
    print(run([tool(clang, "llvm-size"), "-A", obj]).strip())


def reference(seed, data):
    if not data:
        return seed
    a, b = seed & 0xffff, seed >> 16
    for value in data:
        a = (a + value) % 65521
        b = (b + a) % 65521
    return (b << 16) | a


class Machine:
    def __init__(self, code, entry):
        self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB)
        self.uc.mem_map(CODE, 0x10000)
        self.uc.mem_write(CODE, code)
        self.uc.mem_map(DATA, 0x40000)
        self.uc.mem_map(STACK, 0x10000)
        self.uc.mem_map(STOP, 0x1000)
        self.uc.reg_write(UC_ARM_REG_C1_C0_2, 0xf << 20)
        self.uc.reg_write(UC_ARM_REG_FPEXC, 0x40000000)
        self.entry = entry
        self.start = self.end = 0
        self.uc.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, self.access)

    def access(self, uc, access, address, size, value, user):
        if STACK <= address and address + size <= STACK + 0x10000:
            return
        if access == UC_MEM_READ and CODE <= address and address + size <= CODE + 0x10000:
            return
        if access == UC_MEM_READ and self.start <= address and address + size <= self.end:
            return
        raise AssertionError(f"unexpected Adler memory access: {access} {address:x}+{size}")

    def call(self, seed, data, alignment):
        pointer = DATA + 0x100 + alignment
        self.start, self.end = pointer, pointer + len(data)
        self.uc.mem_write(pointer - 16, b"\x5a" * (len(data) + 32))
        if data:
            self.uc.mem_write(pointer, data)
        self.uc.reg_write(UC_ARM_REG_SP, STACK + 0x8000)
        self.uc.reg_write(UC_ARM_REG_LR, STOP | 1)
        self.uc.reg_write(UC_ARM_REG_R0, seed)
        self.uc.reg_write(UC_ARM_REG_R1, pointer if data else 0)
        self.uc.reg_write(UC_ARM_REG_R2, len(data))
        self.uc.emu_start(self.entry | 1, STOP, count=2000000)
        assert self.uc.reg_read(UC_ARM_REG_PC) == STOP, "Adler did not return"
        value = self.uc.reg_read(UC_ARM_REG_R0)
        assert value == reference(seed, data), (seed, len(data), alignment, value)
        assert bytes(self.uc.mem_read(pointer - 16, len(data) + 32)) == b"\x5a" * 16 + data + b"\x5a" * 16
        return value


def host_validation(clang, work, corpora, definitions=()):
    environment = os.environ.copy()
    if os.name == "nt":
        from setuptools.msvc import msvc14_get_vc_env
        environment = {key.upper(): value for key, value in environment.items()}
        environment.update({key.upper(): value for key, value in
                            msvc14_get_vc_env("x64").items()})
    common = [clang, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
              "-fno-strict-aliasing", "-D_CRT_SECURE_NO_WARNINGS=1",
              "-DISAAC_VITA_NATIVE_PNG_ADLER_NEON=1", "-I", ROOT / "runtime", *definitions]
    sources = [ROOT / "runtime" / name for name in (
        "host_vita_archive_miniz_native.c", "host_vita_native_png_miniz.c",
        "host_vita_native_png_adler.c")]
    library = work / ("png-adler.dll" if os.name == "nt" else "png-adler.so")
    exports = ([f"-Wl,/export:{name}" for name in
        ("isaac_vita_archive_miniz_native", "isaac_vita_png_miniz_native")]
        if os.name == "nt" else ["-fPIC"])
    run(common + ["-shared"] + exports + sources + ["-o", library], env=environment)
    dll = ctypes.CDLL(str(library))
    functions = [getattr(dll, name) for name in
                 ("isaac_vita_archive_miniz_native", "isaac_vita_png_miniz_native")]
    for fn in functions:
        fn.argtypes = [ctypes.c_void_p, ctypes.c_void_p,
                       ctypes.POINTER(ctypes.c_uint32), ctypes.c_void_p,
                       ctypes.c_void_p, ctypes.POINTER(ctypes.c_uint32), ctypes.c_uint32]
        fn.restype = ctypes.c_int
    rng = random.Random(0xAD1E2026)
    payloads = [b"", b"\0" * 4097, b"\xff" * 70001,
                bytes(range(256)) * 33, bytes(rng.randrange(256) for _ in range(8193))]
    calls = streams = 0
    for data in payloads:
        for corrupt in (False, True):
            packed = bytearray(zlib.compress(data))
            if corrupt:
                packed[-1] ^= 0xff
            for input_chunks, output_chunks in (([1, 2, 7, 17, 65536], [1, 16, 4096]),
                                                 ([16, 37, 4096], [65536])):
                source = ctypes.create_string_buffer(bytes(packed))
                states = [ctypes.create_string_buffer(0x2af0) for _ in functions]
                capacity = len(data) + 32
                outputs = [ctypes.create_string_buffer(b"\xa5" * capacity, capacity)
                           for _ in functions]
                input_pos = output_pos = 0
                for call_index in range(100000):
                    insize = min(input_chunks[call_index % len(input_chunks)],
                                 len(packed) - input_pos)
                    outsize = min(output_chunks[call_index % len(output_chunks)],
                                  capacity - output_pos)
                    records = []
                    for fn, state, output in zip(functions, states, outputs):
                        consumed, produced = ctypes.c_uint32(insize), ctypes.c_uint32(outsize)
                        status = fn(state, ctypes.addressof(source) + input_pos,
                                    ctypes.byref(consumed), output,
                                    ctypes.addressof(output) + output_pos,
                                    ctypes.byref(produced), 15)
                        records.append((status, consumed.value, produced.value,
                                        state.raw, output.raw))
                    assert records[0] == records[1], (len(data), corrupt, call_index)
                    status, consumed, produced = records[0][:3]
                    input_pos += consumed
                    output_pos += produced
                    calls += 1
                    if status <= 0:
                        assert status == (-2 if corrupt else 0)
                        assert outputs[0].raw[:output_pos] == data
                        assert outputs[0].raw[output_pos:] == b"\xa5" * (capacity - output_pos)
                        break
                    assert consumed or produced or insize != 0, "stream made no progress"
                else:
                    raise AssertionError("stream fixture exceeded call bound")
                streams += 1
    print(f"Host tinfl integration: PASS {streams} valid/bad-Adler streams / {calls} calls; "
          "status, consumed/produced, full state and output match archive entry")
    # Reuse the existing full decoder and image/failure reference; no second PNG oracle.
    executable = work / ("native-png-adler.exe" if os.name == "nt" else "native-png-adler")
    run(common + ["-DISAAC_VITA_NATIVE_PNG_ORACLE=1", "-DISAAC_VITA_NATIVE_PNG=1",
        ROOT / "runtime/host_vita_native_png.c"] + sources +
        [ROOT / "runtime/vita_native_png_oracle.c", "-o", executable] +
        ([] if os.name == "nt" else ["-lm"]), env=environment)
    arguments = [sys.executable, ROOT / "test_vita_native_png.py", "--oracle",
                 executable, "--keep", work / "whole-png-corpus"]
    for corpus in corpora:
        arguments += ["--corpus", corpus]
    print(run(arguments, env=environment).strip())


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--clang", type=Path, required=True)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--host", action="store_true",
                        help="also compare both tinfl entries and run existing whole-PNG checks")
    parser.add_argument("--corpus", type=Path, action="append", default=[])
    args = parser.parse_args()
    work = args.out or Path(tempfile.mkdtemp(prefix="isaac-png-adler-"))
    work.mkdir(parents=True, exist_ok=True)
    archive_isolation(args.clang, work)
    rng = random.Random(20260905)
    seeds = [0, 1, 0xffff, 0xffff0000, 0xffffffff, 0xfff0fff0, rng.getrandbits(32)]
    lengths = list(range(81)) + [255, 256, 257, 4095, 4096, 4097, 5551, 5552,
        5553, 8191, 8192, 8193, 65535, 65536, 65537]
    cases = []
    for n in lengths:
        for pattern in range(3):
            data = bytes((0 if pattern == 0 else 255 if pattern == 1 else
                          rng.randrange(256)) for _ in range(n))
            for index, seed in enumerate(seeds):
                cases.append((seed, data, (n + index * 5 + pattern) & 15))
    for enabled in (0, 1):
        code, entry = compile_arm(args.clang, work, enabled)
        machine = Machine(code, entry)
        for seed, data, alignment in cases:
            machine.call(seed, data, alignment)
        calls = 0
        # Noncanonical seeds, empty pieces, every tail size and boundaries
        # crossing both the 4096-byte vector block and old 5552 scalar block.
        for seed in seeds:
            data = bytes(rng.randrange(256) for _ in range(16397))
            offset, current = 0, seed
            for size in [0, 1, 15, 0, 16, 17, 4095, 1, 0, 4096, 5552, 2604]:
                piece = data[offset:offset + size]
                current = machine.call(current, piece, offset & 15)
                offset += len(piece)
                calls += 1
            assert offset == len(data)
            assert current == reference(seed, data)
        print(f"ARM Adler mode={enabled}: PASS {len(cases)} independent cases + "
              f"{calls} chained calls; logical bounds/read-only input; "
              f"text+constants={len(code)} bytes")
    if args.host:
        host_validation(args.clang, work, args.corpus)


if __name__ == "__main__":
    main()
