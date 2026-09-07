"""Exact PNG fixed-Huffman table initialization, fresh original differential.

Builds the pinned original and current production sources on every invocation.
Compares full tinfl state (including untouched scratch), output, status, cursor,
and history observation after every coroutine call. ARM execution is functional
evidence, not a Vita performance measurement. No device access.
"""
from __future__ import annotations
import argparse
import ctypes as C
import hashlib
import json
import os
from pathlib import Path
import random
import re
import shutil
import struct
import subprocess
import zlib

from test_vita_native_png_adler_neon import flags, tool
from test_vita_native_png_reuse import zero_distance_payload

ROOT = Path(__file__).resolve().parent
BASE = "694e2250e8d705abbb775cfd30ec1a3a8e614a38"
STATE_BYTES = 0x2af0
FILES = ("host_vita_archive_miniz_native.c", "host_vita_archive_miniz_native.h",
         "host_vita_native_png_miniz.c", "host_vita_native_png_adler.c",
         "host_vita_native_png_adler.h", "host_vita_native_png_lz_neon.h")


def digest(data):
    return hashlib.sha256(data).hexdigest()


def execute(cmd, out, name, env=None, source=None):
    result = subprocess.run([str(v) for v in cmd], input=source, text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env)
    (out / (name + ".command.json")).write_text(json.dumps([str(v) for v in cmd], indent=2))
    (out / (name + ".log")).write_text(result.stdout)
    if result.returncode:
        raise RuntimeError(f"{name}: {result.returncode}\n{result.stdout}")
    return result.stdout


def snapshot(out):
    paths = {}
    for label in ("original", "current"):
        folder = out / label
        folder.mkdir(parents=True, exist_ok=True)
        for name in FILES:
            if label == "original":
                result = subprocess.run(["git", "-C", str(ROOT.parent), "show",
                    f"{BASE}:recomp/runtime/{name}"], capture_output=True, check=True)
                data = result.stdout
            else:
                data = (ROOT / "runtime" / name).read_bytes()
            (folder / name).write_bytes(data)
        if label == "current":
            name = "host_vita_native_png_fixed_tables.h"
            (folder / name).write_bytes((ROOT / "runtime" / name).read_bytes())
        paths[label] = folder
    return paths


def build_host(clang, out, paths, observed):
    env = os.environ.copy()
    if os.name == "nt":
        from setuptools._distutils._msvccompiler import _get_vc_env
        env = {k.upper(): v for k, v in env.items()}
        env.update({k.upper(): v for k, v in _get_vc_env("x86_amd64").items()})
    libs = []
    for label, enabled in (("original", 0), ("current", 0), ("current", 1)):
        name = f"host-{label}-fixed{enabled}-observed{observed}"
        lib = out / (name + (".dll" if os.name == "nt" else ".so"))
        src = paths[label]
        symbols = ["isaac_vita_png_miniz_native"]
        if observed:
            symbols.append("isaac_vita_png_miniz_reuse")
        exports = [f"-Wl,/export:{s}" for s in symbols] if os.name == "nt" else ["-fPIC"]
        cmd = [clang, "-shared", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
            "-fno-strict-aliasing", "-D_CRT_SECURE_NO_WARNINGS=1",
            "-DISAAC_VITA_NATIVE_PNG_ADLER_NEON=1",
            "-DISAAC_VITA_NATIVE_PNG_LZ_NEON=1",
            "-DISAAC_VITA_NATIVE_PNG_LZ_D4_NEON=1",
            f"-DISAAC_VITA_NATIVE_PNG_REUSE_TINFL={observed}",
            f"-DISAAC_VITA_NATIVE_PNG_FIXED_TABLES={enabled}", *exports,
            src / "host_vita_native_png_miniz.c", src / "host_vita_native_png_adler.c",
            "-o", lib]
        execute(cmd, out, name, env)
        assert lib.is_file()
        libs.append(Host(lib, observed))
    return libs


class Host:
    def __init__(self, lib, observed):
        self.lib = C.CDLL(str(lib))
        self.fn = getattr(self.lib, "isaac_vita_png_miniz_reuse" if observed
                          else "isaac_vita_png_miniz_native")
        self.fn.argtypes = [C.c_void_p, C.c_void_p, C.POINTER(C.c_uint32),
            C.c_void_p, C.c_void_p, C.POINTER(C.c_uint32), C.c_uint32]
        if observed:
            self.fn.argtypes.append(C.POINTER(C.c_uint32))
        self.fn.restype = C.c_int
        self.observed = observed

    def start(self, packed, initial, capacity, seed, alignment=0):
        self.state = C.create_string_buffer(STATE_BYTES + 32)
        self.sp = C.addressof(self.state) + 16
        C.memset(C.addressof(self.state), 0x6b, STATE_BYTES + 32)
        C.memmove(self.sp, initial, STATE_BYTES)
        self.input = C.create_string_buffer(b"\xb7" * alignment + packed + b"\xd4" * 32)
        self.ip = C.addressof(self.input) + alignment
        self.output = C.create_string_buffer(capacity + 64 + alignment)
        self.op = C.addressof(self.output) + 32 + alignment
        C.memset(C.addressof(self.output), 0xa6, len(self.output))
        C.memset(self.op, seed, capacity)
        self.capacity = capacity
        self.history = C.c_uint32(0)

    def step(self, ip, ni, op, no, flag):
        consumed, produced = C.c_uint32(ni), C.c_uint32(no)
        args = [self.sp, self.ip + ip, C.byref(consumed), self.op,
                self.op + op, C.byref(produced), flag]
        if self.observed:
            args.append(C.byref(self.history))
        status = self.fn(*args)
        assert consumed.value <= ni and produced.value <= no
        assert C.string_at(self.sp - 16, 16) == b"\x6b" * 16
        assert C.string_at(self.sp + STATE_BYTES, 16) == b"\x6b" * 16
        assert C.string_at(self.op - 16, 16) == b"\xa6" * 16
        assert C.string_at(self.op + self.capacity, 16) == b"\xa6" * 16
        return (status, consumed.value, produced.value, self.history.value,
                C.string_at(self.sp, STATE_BYTES), C.string_at(self.op, self.capacity))


def pack(data, level=6, strategy=zlib.Z_DEFAULT_STRATEGY):
    c = zlib.compressobj(level, zlib.DEFLATED, 15, 8, strategy)
    return c.compress(data) + c.flush()


def fixtures():
    rng = random.Random(0xf17ed)
    records = []
    payloads = [b"", b"x", bytes(range(256)) * 3, b"a" * 8193,
        b"abcd" * 2049, bytes(rng.randrange(7) for _ in range(8193)),
        bytes(rng.randrange(256) for _ in range(4097))]
    for data in payloads:
        for mode, level, strategy in (("stored", 0, zlib.Z_DEFAULT_STRATEGY),
                                    ("fixed", 6, zlib.Z_FIXED),
                                    ("dynamic", 9, zlib.Z_DEFAULT_STRATEGY)):
            raw = pack(data, level, strategy)
            records.append((mode, raw, data, False))
            records.append((mode + "-adler", raw[:-1] + bytes([raw[-1] ^ 0x53]), None, False))
            records.append((mode + "-short", raw[:max(1, len(raw) // 2)], None, False))
            records.append((mode + "-abandon", raw, None, True))
        # Real multi-block sequence: empty fixed/stored delimiters as well as data.
        c = zlib.compressobj(6)
        mixed = c.compress(data[:len(data)//2]) + c.flush(zlib.Z_SYNC_FLUSH)
        mixed += c.compress(data[len(data)//2:]) + c.flush()
        records.append(("multiblock", mixed, data, False))
    for seed in (0x39, 0x81):
        initial = bytes([0, 77, seed, seed, seed])
        records.append(("reserved-distance-zero", zero_distance_payload(initial), None, False))
    records += [("singleton-unsafe", bytes.fromhex("78010dc1010900000080a0fa7fba101f000b0003"), None, False),
                ("singleton-valid", bytes.fromhex("78010dc1010900000080a0fa7fba1017000b0003"), None, False)]
    # Corrupt actual fixed codes, headers, dynamic tables and block transitions.
    originals = list(records)
    for i in range(80):
        raw = bytearray(originals[rng.randrange(len(originals))][1])
        raw[rng.randrange(len(raw))] ^= 1 << rng.randrange(8)
        records.append(("mutated", bytes(raw), None, False))
    for n in range(1, 33):
        records.append(("random", bytes(rng.randrange(256) for _ in range(n)), None, False))
    return records


def check_sequences(machines, records, schedules, output_limit=2*1024*1024):
    rng = random.Random(0x51a7e)
    initial = bytes(rng.randrange(256) for _ in range(STATE_BYTES))
    calls = streams = 0
    states = set()
    for number, (name, packed, expected, abandon) in enumerate(records):
        for schedule_index, (ins, outs) in enumerate(schedules):
            # Only reset the original scalar header; keep the prior full scratch.
            initial = b"\0" * 64 + initial[64:]
            capacity = min(output_limit, max(1024, len(expected) + 32 if expected is not None else 65536))
            for machine in machines:
                machine.start(packed, initial, capacity, 0x39 if number & 1 else 0x81,
                              number & 15)
            ip = op = 0
            for index in range(20000):
                ni = min(ins[index % len(ins)], len(packed) - ip)
                no = min(outs[index % len(outs)], capacity - op)
                values = [m.step(ip, ni, op, no, 15) for m in machines]
                assert all(v == values[0] for v in values[1:]), (name, number, schedule_index, index, ip, op)
                status, consumed, produced, history, initial, output = values[0]
                states.add(struct.unpack_from("<I", initial)[0])
                ip += consumed
                op += produced
                calls += 1
                if abandon or status <= 0 or (status == 1 and ip == len(packed)) or op == capacity:
                    if expected is not None:
                        assert status == 0 and ip == len(packed), (name, status, ip, len(packed))
                        assert output[:op] == expected, name
                    break
                if not consumed and not produced:
                    # Zero-size output is an explicit, bounded caller pause.
                    assert no == 0 and status == 2, (name, status, ni, no)
            else:
                raise AssertionError((name, "call bound"))
            streams += 1
    return {"streams": streams, "calls": calls, "coroutine_states": sorted(states)}


def corpus(manifest):
    data = json.loads(manifest.read_text())
    records = []
    for row in data["images"]:
        path = Path(data["source"]) / row["path"]
        blob = path.read_bytes()
        assert digest(blob) == row["sha256"], path
        pos, idats = 8, []
        while pos + 12 <= len(blob):
            size = struct.unpack_from(">I", blob, pos)[0]
            tag = blob[pos+4:pos+8]
            if tag == b"IDAT":
                idats.append(blob[pos+8:pos+8+size])
            pos += size + 12
            if tag == b"IEND":
                break
        packed = b"".join(idats)
        expected = zlib.decompress(packed)
        assert digest(expected) == row["raw_sha256"], path
        records.append((row["path"], packed, expected, False))
    assert len(records) == 377
    return records


def table_check(machines, out):
    initial = b"\0" * 64 + b"\xe3" * (STATE_BYTES - 64)
    for m in machines:
        m.start(bytes.fromhex("7801030000000001"), initial, 32, 0x39)
    values = [m.step(0, 8, 0, 32, 15) for m in machines]
    assert all(v == values[0] for v in values[1:])
    state = values[0][4]
    source = (ROOT / "runtime/host_vita_native_png_fixed_tables.h").read_text()
    hashes = {}
    for index, name in enumerate(("lit", "dist")):
        match = re.search(r"isaac_np_fixed_" + name + r"_lookup\[1024\] = \{(.*?)\};", source, re.S)
        array = [int(v) for v in re.findall(r"\d+", match.group(1))]
        assert len(array) == 1024
        original = state[0x40 + index * 0xda0 + 288:0x40 + index * 0xda0 + 288 + 2048]
        assert struct.pack("<1024h", *array) == original, name
        hashes[name] = digest(original)
    (out / "fresh-original-fixed-state.bin").write_bytes(state)
    return hashes


def build_arm(clang, out, paths):
    # Full objects use identical stdin names, so OFF comparison includes all bytes.
    shim = out / "string.h"
    shim.write_text("#include <stddef.h>\nvoid *memcpy(void *,const void *,size_t);\n"
                    "void *memset(void *,int,size_t);\n")
    artifacts = []
    for kind in ("archive", "png"):
        objects = []
        for label in ("original", "current"):
            source = paths[label] / ("host_vita_archive_miniz_native.c" if kind == "archive"
                                    else "host_vita_native_png_miniz.c")
            obj = out / f"arm-off-{kind}-{label}.o"
            cmd = flags(clang) + ["-I", out, "-I", paths[label],
                "-DISAAC_VITA_NATIVE_PNG_ADLER_NEON=1",
                "-DISAAC_VITA_NATIVE_PNG_REUSE_TINFL=1",
                "-DISAAC_VITA_NATIVE_PNG_LZ_NEON=1",
                "-DISAAC_VITA_NATIVE_PNG_LZ_D4_NEON=1",
                "-DISAAC_VITA_NATIVE_PNG_FIXED_TABLES=0", "-x", "c", "-", "-c", "-o", obj]
            execute(cmd, out, obj.stem, source=source.read_text())
            objects.append(obj.read_bytes())
        assert objects[0] == objects[1], f"{kind} OFF ARM object changed"
        artifacts.append({"kind": kind, "sha256": digest(objects[0])})
    linker = out / "fixed.ld"
    linker.write_text("SECTIONS { . = 0x10000; .text : { *(.text*) } .rodata : { *(.rodata*) } "
                      "/DISCARD/ : { *(.ARM.exidx*) *(.ARM.extab*) } }\n")
    variants = []
    for label, enabled in (("original", 0), ("current", 1)):
        objects = []
        for name in ("host_vita_native_png_miniz.c", "host_vita_native_png_adler.c",
                     "kage_vita_png_tinfl_reset_arm.c"):
            src = paths[label] / name if name != "kage_vita_png_tinfl_reset_arm.c" else (
                ROOT / "vita/host_tests" / name)
            obj = out / f"arm-{label}-{name}.o"
            defs = ["-Disaac_vita_archive_miniz_native=isaac_vita_png_miniz_native"] if "host_tests" in str(src) else []
            execute(flags(clang) + ["-marm", "-fno-builtin", "-I", out, "-I", paths[label],
                "-DISAAC_VITA_NATIVE_PNG_ADLER_NEON=1", "-DISAAC_VITA_NATIVE_PNG_LZ_NEON=1",
                "-DISAAC_VITA_NATIVE_PNG_LZ_D4_NEON=1", "-DISAAC_VITA_NATIVE_PNG_REUSE_TINFL=1",
                f"-DISAAC_VITA_NATIVE_PNG_FIXED_TABLES={enabled}", *defs, "-c", src, "-o", obj],
                out, obj.stem)
            objects.append(obj)
        elf, binary = out / f"arm-{label}.elf", out / f"arm-{label}.bin"
        execute([tool(clang, "ld.lld"), "--gc-sections", "-T", linker, "--entry=np_test_step",
                 *objects, "-o", elf], out, f"link-{label}")
        symbols = execute([tool(clang, "llvm-nm"), elf], out, f"symbols-{label}")
        entry = next(int(line.split()[0], 16) for line in symbols.splitlines()
                     if line.endswith(" np_test_step"))
        execute([tool(clang, "llvm-objcopy"), "-O", "binary", "--only-section=.text",
                 "--only-section=.rodata", elf, binary], out, f"binary-{label}")
        variants.append((binary.read_bytes(), entry))
    return artifacts, variants


def exact_arm(variants, records):
    from test_vita_native_png_tinfl_reset import Inflater, STATE, INPUT, OUTPUT

    class ExactArm(Inflater):
        def __init__(self, code, entry):
            super().__init__(code, entry, False)

        def start(self, packed, initial, capacity, seed, alignment=0):
            # Both sides get precisely the same prior state, not a full/short
            # reset pair. ARM state/output alignment is the real 4-byte ABI;
            # all sixteen input/output byte alignments are covered on host.
            assert len(packed) <= 0x20000 and capacity < 0x20000
            self.uc.mem_write(STATE, initial)
            self.known[:] = b"\1" * STATE_BYTES
            self.uc.mem_write(INPUT, packed)
            self.uc.mem_write(OUTPUT, bytes([seed]) * capacity + b"\xa6" * 16)
            self.capacity = capacity

        def step(self, ip, ni, op, no, flag):
            status, consumed, produced = super().step(ip, ni, op, no, True)
            assert bytes(self.uc.mem_read(OUTPUT + self.capacity, 16)) == b"\xa6" * 16
            return (status, consumed, produced, 0,
                    bytes(self.uc.mem_read(STATE, STATE_BYTES)),
                    bytes(self.uc.mem_read(OUTPUT, self.capacity)))

    machines = [ExactArm(*v) for v in variants]
    return check_sequences(machines, records,
        [([1, 2, 7, 19, 4096], [0, 1, 17, 4096]), ([37, 65536], [65536])],
        output_limit=0x1fff0)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--clang", type=Path, default=Path("C:/Program Files/LLVM/bin/clang.exe"))
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--skip-arm", action="store_true")
    parser.add_argument("--whole-png", action="store_true",
                        help="also run the existing complete PNG byte/CRC/chunk/gamma/failure oracle")
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    paths = snapshot(args.out)
    result = {"base": BASE, "scope": "fresh production state/bytes differential, not timing",
              "runner_sha256": digest(Path(__file__).read_bytes()),
              "manifest_sha256": digest(args.manifest.read_bytes()) if args.manifest else None,
              "sources": {str(p): digest(p.read_bytes()) for folder in paths.values() for p in folder.iterdir()},
              "host": []}
    synthetic = fixtures()
    real = corpus(args.manifest) if args.manifest else []
    for observed in (0, 1):
        machines = build_host(args.clang, args.out, paths, observed)
        tables = table_check(machines, args.out)
        bounded = check_sequences(machines, synthetic, [([1, 2, 7, 19, 4096], [0, 1, 17, 4096]),
                                                       ([37, 65536], [65536])])
        real_result = check_sequences(machines, real, [([8192], [65536]), ([65536], [2*1024*1024])])
        result["host"].append({"observed": observed, "tables": tables, "bounded": bounded, "real": real_result})
        print(json.dumps(result["host"][-1]), flush=True)
    if not args.skip_arm:
        from test_vita_native_png_tinfl_reset import arm_cases
        result["arm_off_objects"], variants = build_arm(args.clang, args.out, paths)
        # Existing production ARM driver covers initialized reads, coroutine
        # resumes, fixed/stored/dynamic, bad Adler, truncation and interruption.
        arm_cases(*variants[0], comparison=variants[1], extra_payloads=[b"abcd"*1025])
        result["arm_differential"] = "PASS existing full coroutine/reset driver"
        result["arm_full_state"] = exact_arm(variants, synthetic)
        print("Exact full-state ARM: " + json.dumps(result["arm_full_state"]), flush=True)
    if args.whole_png:
        from test_vita_native_png_adler_neon import host_validation
        whole = args.out / "whole-png"
        whole.mkdir(exist_ok=True)
        corpora = [Path(json.loads(args.manifest.read_text())["source"])] if args.manifest else []
        host_validation(args.clang, whole, corpora,
            ["-DISAAC_VITA_NATIVE_PNG_FIXED_TABLES=1",
             "-DISAAC_VITA_NATIVE_PNG_LZ_NEON=1", "-DISAAC_VITA_NATIVE_PNG_LZ_D4_NEON=1",
             "-DISAAC_VITA_NATIVE_PNG_TINFL_HEADER_RESET=1"])
        result["whole_png"] = "PASS existing full-image reference and rejection oracle"
    result["outputs"] = {str(p): digest(p.read_bytes()) for p in args.out.iterdir()
                          if p.suffix in (".dll", ".so", ".o", ".bin", ".elf")}
    result["pass"] = True
    (args.out / "results.json").write_text(json.dumps(result, indent=2))
    print("PASS PNG fixed tables; full fresh-original state and output identity; no speed claim")


if __name__ == "__main__":
    main()
