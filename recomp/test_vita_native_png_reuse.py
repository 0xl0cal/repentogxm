#!/usr/bin/env python3
"""Fresh OFF/ON builds of the ACTUAL PNG core, with persistent scratch/cache.

No device or VM access. The sibling PNG oracle supplies the independent zlib
and filter reference; the C bridge calls production decode/reuse functions.
The optional manifest pins every real input by SHA-256. No timing claim.
"""
from __future__ import annotations

import argparse
import ctypes as C
import hashlib
import json
import os
from pathlib import Path
import random
import shutil
import struct
import subprocess
import zlib

import test_vita_native_png as png

U8 = C.c_uint8
U32 = C.c_uint32
RESULT_NAMES = ("status", "chunks", "idat_bytes", "stream_bytes", "crc_final",
                "last_filter", "filter0", "filter1", "filter2", "filter3", "filter4",
                "io_us", "strict_attempts", "strict_successes", "strict_refusals",
                "read_pos", "read_calls", "read_requests_hash", "returned_status", "errno")
STAT_NAMES = ("lookups", "hits", "stores", "evictions", "skipped", "hit_kib")
STRICT_FIELDS = {12, 13, 14}


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def build(out: Path, compiler: str | None, tinfl=False, large=False):
    root = Path(__file__).resolve().parent
    runtime = root / "runtime"
    cc = compiler or shutil.which("clang") or "C:/Program Files/LLVM/bin/clang.exe"
    env = os.environ.copy()
    if os.name == "nt":
        from setuptools._distutils._msvccompiler import _get_vc_env
        env.update({k.upper(): v for k, v in _get_vc_env("x86_amd64").items()})
    sources = [runtime / name for name in (
        "host_vita_native_png.c", "host_vita_native_png_miniz.c",
        "host_vita_native_png_adler.c", "host_vita_native_png_libdeflate.c")]
    bridge = root / "vita/host_tests/kage_vita_png_reuse.c"
    sources.append(bridge)
    tracked = [Path(__file__).resolve(), root / "test_vita_native_png.py"] + sources + [runtime / name for name in (
        "host_vita_png_reuse.c", "host_vita_archive_miniz_native.c",
        "host_vita_archive_miniz_native.h")] + list(runtime.glob("*png*.h"))
    vendor = runtime / "vendor/libdeflate-strict"
    if not vendor.is_dir():
        raise RuntimeError(f"missing actual pinned adapter dependency: {vendor}")
    tracked += list(vendor.rglob("*"))
    tracked = sorted(set(p for p in tracked if p.is_file()))
    before = {str(p): sha(p) for p in tracked}
    records = []
    outputs = []
    defines = ["ISAAC_VITA_NATIVE_PNG_ORACLE=1", "ISAAC_VITA_NATIVE_PNG=1",
               "ISAAC_VITA_NATIVE_PNG_ADLER_NEON=1", "ISAAC_VITA_NATIVE_PNG_LZ_NEON=1",
               "ISAAC_VITA_NATIVE_PNG_LZ_D4_NEON=1", "ISAAC_VITA_NATIVE_PNG_TINFL_HEADER_RESET=1",
               "ISAAC_VITA_NATIVE_PNG_LIBDEFLATE_STRICT=1"]
    variants = [("0", 0, 0), ("1", 1, int(large))]
    if large:
        variants.append(("legacy", 1, 0))
    for label, enabled, large_enabled in variants:
        dest = out / (f"png-reuse-{label}.dll" if os.name == "nt" else f"png-reuse-{label}.so")
        command = [cc, "-shared", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                   "-fno-strict-aliasing", "-D_CRT_SECURE_NO_WARNINGS", "-I", str(runtime)]
        if os.name != "nt":
            command.append("-fPIC")
        command += ["-D" + d for d in defines + [f"ISAAC_VITA_NATIVE_PNG_REUSE={enabled}",
            f"ISAAC_VITA_NATIVE_PNG_REUSE_TINFL={int(enabled and tinfl)}",
            f"ISAAC_VITA_NATIVE_PNG_REUSE_LARGE={large_enabled}"]]
        # Keep the core unmodified. Rename only the actual inflater object so
        # the bridge can record each call's real input/output boundary.
        miniz = runtime / "host_vita_native_png_miniz.c"
        obj = out / f"png-miniz-{label}.o"
        miniz_command = [x for x in command if x != "-shared"] + [
            "-Disaac_vita_png_miniz_native=probe_actual_png_miniz_native",
            "-Disaac_vita_png_miniz_reuse=probe_actual_png_miniz_reuse",
            "-c", str(miniz), "-o", str(obj)]
        records.append(miniz_command)
        (out / f"compile-miniz-{label}.json").write_text(json.dumps(miniz_command, indent=2), encoding="utf-8")
        run = subprocess.run(miniz_command, env=env, cwd=out, text=True,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (out / f"compile-miniz-{label}.log").write_text(run.stdout, encoding="utf-8")
        if run.returncode or not obj.is_file():
            raise RuntimeError(f"fresh miniz compile {label} failed: {run.stdout}")
        command += [str(s) for s in sources if s != miniz] + [str(obj)]
        if enabled:
            command.append(str(runtime / "host_vita_png_reuse.c"))
        command += ["-o", str(dest)]
        records.append(command)
        (out / f"compile-{label}.json").write_text(json.dumps(command, indent=2), encoding="utf-8")
        run = subprocess.run(command, env=env, cwd=out, text=True,
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        (out / f"compile-{label}.log").write_text(run.stdout, encoding="utf-8")
        if run.returncode:
            raise RuntimeError(f"fresh compile {label} failed ({run.returncode}):\n{run.stdout}")
        if not dest.is_file():
            raise RuntimeError(f"compiler returned success without {dest}")
        outputs.append(dest)
    after = {str(p): sha(p) for p in tracked}
    if before != after:
        raise RuntimeError("production sources changed during differential builds; rerun")
    return outputs, {"commands": records, "sources": after,
                     "outputs": {str(p): sha(p) for p in outputs}}


class Core:
    def __init__(self, path, cache_bytes=4 * 1024 * 1024, raw_bytes=17 * 1024 * 1024):
        self.lib = C.CDLL(str(path))
        self.lib.probe_create.argtypes = [U32, U32, U32]
        self.lib.probe_create.restype = C.c_void_p
        self.lib.probe_destroy.argtypes = [C.c_void_p]
        self.lib.probe_canaries.argtypes = [C.c_void_p]
        self.lib.probe_scratch.argtypes = [C.c_void_p]
        self.lib.probe_scratch.restype = C.c_void_p
        self.lib.probe_scratch_equal.argtypes = [C.c_void_p, C.c_void_p, U32]
        self.lib.probe_read_trace.argtypes = [C.c_void_p]
        self.lib.probe_read_trace.restype = C.POINTER(U32)
        self.lib.probe_feed_trace.argtypes = [C.c_void_p]
        self.lib.probe_feed_trace.restype = C.POINTER(U32)
        self.lib.probe_feed_count.argtypes = [C.c_void_p]
        self.lib.probe_feed_count.restype = U32
        self.lib.probe_legacy_projection.argtypes = [C.c_void_p, C.c_void_p, U32]
        self.lib.probe_legacy_projection.restype = U32
        self.lib.probe_entry_count.argtypes = [C.c_void_p, U32]
        self.lib.probe_entry_count.restype = U32
        self.lib.probe_corrupt_large_tail.argtypes = [C.c_void_p]
        self.raw_bytes = raw_bytes
        self.lib.probe_stats.argtypes = [C.c_void_p, C.POINTER(U32)]
        self.lib.probe_alias_next.argtypes = [C.c_void_p, U32]
        self.lib.probe_corrupt_cached_input.argtypes = [C.c_void_p]
        self.lib.probe_decode.argtypes = [C.c_void_p, C.POINTER(U32), C.POINTER(U8), U32,
                                         C.POINTER(U8), C.POINTER(U32), C.POINTER(U8), C.POINTER(U8)]
        self.p = self.lib.probe_create(raw_bytes, cache_bytes, 0x51)
        if not self.p:
            raise MemoryError("probe_create")
        self.mutable_gamma = {}
        self.recent_gamma = []

    def close(self):
        if self.p:
            self.lib.probe_destroy(self.p)
            self.p = None

    def stats(self):
        out = (U32 * 6)()
        self.lib.probe_stats(self.p, out)
        return dict(zip(STAT_NAMES, out))

    def scratch(self):
        return C.string_at(self.lib.probe_scratch(self.p), self.raw_bytes)

    def scratch_equal(self, other):
        return self.lib.probe_scratch_equal(self.p, other.lib.probe_scratch(other.p), self.raw_bytes)

    def trace(self, count):
        return list(self.lib.probe_read_trace(self.p)[:count * 2])

    def feeds(self):
        count = self.lib.probe_feed_count(self.p)
        flat = list(self.lib.probe_feed_trace(self.p)[:count * 6])
        return [flat[i:i + 6] for i in range(0, len(flat), 6)]

    def projection(self):
        size = self.lib.probe_legacy_projection(self.p, None, 0)
        data = (U8 * size)()
        assert self.lib.probe_legacy_projection(self.p, data, size) == size
        return bytes(data)

    def entries(self, kind):
        return self.lib.probe_entry_count(self.p, kind)

    def decode(self, words, stream, gamma):
        params = (U32 * 8)(*words)
        data = (U8 * len(stream)).from_buffer_copy(stream)
        table = None
        if isinstance(gamma, bytearray):
            # One actual stable pointer whose bytes change between calls.
            entry = self.mutable_gamma.setdefault(id(gamma), (gamma, (U8 * 256)()))
            assert entry[0] is gamma
            table = entry[1]
            C.memmove(table, bytes(gamma), 256)
        elif gamma is not None:
            # Keep the preceding allocation alive to make the new-address
            # same-bytes case real, rather than relying on allocator reuse.
            table = (U8 * 256).from_buffer_copy(gamma)
            if self.recent_gamma:
                assert C.addressof(table) != C.addressof(self.recent_gamma[-1])
            self.recent_gamma.append(table)
            self.recent_gamma = self.recent_gamma[-2:]
        raw = (U8 * (words[1] * (words[4] + 1)))()
        last = (U8 * words[4])()
        out = (U32 * len(RESULT_NAMES))()
        status = self.lib.probe_decode(self.p, params, data, len(stream), table, out, raw, last)
        assert status == 0, ("bridge/canary failure", status)
        return list(out), bytes(raw), bytes(last)


def image(payload, width=1, height=1, ct=6):
    return (png.SIG + png.chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, ct, 0, 0, 0)) +
            png.chunk(b"IDAT", payload) + png.chunk(b"IEND", b""))


def zero_distance_payload(expected):
    # Fixed-code lengths/canonical ranges are copied from the pinned tinfl
    # table initializer. Emit filter0, literal77, length3, RESERVED distance30,
    # EOB. Old tinfl maps that distance to zero and reads prior scratch bytes.
    bits = [1, 1, 0]  # final fixed-Huffman block, LSB-first block header
    for value, width in ((48, 8), (48 + 77, 8), (1, 7), (30, 5), (0, 7)):
        bits.extend((value >> i) & 1 for i in range(width - 1, -1, -1))
    data = bytearray((len(bits) + 7) // 8)
    for i, bit in enumerate(bits):
        data[i // 8] |= bit << (i % 8)
    return b"\x78\x01" + bytes(data) + struct.pack(">I", zlib.adler32(expected))


def inputs(data, staging=65536, crc_entry=None):
    records = list(png.chunks(data))
    header = next(b for _, t, b, _ in records if t == b"IHDR")
    w, h, depth, ct, _, _, interlace = struct.unpack(">IIBBBBB", header)
    if depth != 8 or interlace or ct not in png.CHANNELS:
        return None
    pos, _, body, _ = next(r for r in records if r[1] == b"IDAT")
    return ([w, h, png.CHANNELS[ct], ct, w * png.CHANNELS[ct], len(body),
             zlib.crc32(b"IDAT") if crc_entry is None else crc_entry, staging],
            data[pos + 8:], pos + 8)


class Pair:
    def __init__(self, dlls, records, cache_bytes=4 * 1024 * 1024, tinfl=False, large=False):
        self.off, self.on = [Core(p, cache_bytes) for p in dlls[:2]]
        self.legacy = Core(dlls[2], cache_bytes) if large else None
        self.records = records
        self.tinfl = tinfl
        self.large = large
        self.last_stored = False

    def close(self):
        self.off.close()
        self.on.close()
        if self.legacy:
            self.legacy.close()

    def check(self, name, data, gamma=None, staging=None, expect_hit=None,
              reference=True, cut=None, crc_entry=None, expect_status=None):
        parsed = inputs(data, 65536 if staging is None else staging, crc_entry)
        if parsed is None:
            return None
        words, stream, start = parsed
        if cut is not None:
            stream = stream[:cut]
        previous = self.on.stats()
        a, raw_a, last_a = self.off.decode(words, stream, gamma)
        on_words = words.copy()
        if self.large and staging is None:
            on_words[7] = 131072
        b, raw_b, last_b = self.on.decode(on_words, stream, gamma)
        current = self.on.stats()
        self.last_stored = current["stores"] != previous["stores"]
        hit = current["hits"] - previous["hits"]
        assert raw_a == raw_b and last_a == last_b, (name, "scratch/lastrow mismatch", a, b)
        assert self.off.scratch_equal(self.on), (name, "whole persistent scratch mismatch")
        trace = self.off.trace(a[16])
        assert trace == self.on.trace(b[16]), (name, "exact read requests/returns mismatch")
        assert all(n <= 65536 for n in trace[::2]), (name, "read boundary changed")
        feeds_a, feeds_b = self.off.feeds(), self.on.feeds()
        if hit:
            expected_feeds = [f for f in feeds_a if f[0] == 1] if words[5] > 65536 else []
            assert feeds_b == expected_feeds, (name, "hit did not retain exact first feed", feeds_a, feeds_b)
            if words[5] > 65536:
                assert feeds_b and feeds_b[0][1] == 65536, (name, "late hit skipped first feed")
        else:
            assert feeds_a == feeds_b, (name, "cold/error inflater feed changed", feeds_a, feeds_b)
        assert all(f[1] <= 65536 for f in feeds_b), (name, "inflater feed grew")
        mismatches = [RESULT_NAMES[i] for i in range(len(RESULT_NAMES)) if a[i] != b[i] and i not in STRICT_FIELDS]
        assert not mismatches, (name, mismatches, a, b)
        assert a[0] == a[18] and a[19] == 123, (name, "return/errno contract", a)
        if hit:
            assert a[0] == 0 and b[12:15] == [0, 0, 0], (name, a, b)
            if self.large and words[5] > 65536:
                # Late hits have already decoded the first original 64KiB.
                # Strict is never attempted for this >64KiB first-IDAT shape.
                assert a[12:15] == [0, 0, 0], (name, "large strict domain", a, b)
            else:
                assert a[12] == 1, (name, a, b)
            if not self.tinfl:
                assert a[12:15] == [1, 1, 0], (name, a, b)
        else:
            assert a[12:15] == b[12:15], (name, "strict counts changed on miss", a, b)
        if expect_hit is not None:
            assert hit == int(expect_hit), (name, "unexpected hit", hit, previous, current, a, b)
        if expect_status is not None:
            assert a[0] == expect_status, (name, a)
        legacy_hit = None
        if self.legacy:
            old_stats = self.legacy.stats()
            old, raw_old, last_old = self.legacy.decode(words, stream, gamma)
            legacy_hit = self.legacy.stats()["hits"] - old_stats["hits"]
            if legacy_hit:
                assert old[12:15] == [0, 0, 0] and a[12] == 1, (name, "legacy strict hit", old, a)
            else:
                assert old[12:15] == a[12:15], (name, "legacy strict miss", old, a)
            assert raw_old == raw_a and last_old == last_a, (name, "legacy output")
            assert self.off.scratch_equal(self.legacy), (name, "legacy persistent scratch")
            assert self.legacy.trace(old[16]) == trace, (name, "legacy read trace")
            assert all(old[i] == a[i] for i in range(len(a)) if i not in STRICT_FIELDS), (name, "legacy status", old, a)
            # Same <=64KiB calls must retain every old optional AND strict hit.
            # The legacy suite's deliberate small-cache corruption/alias tests
            # use two cores; all three-core cases require this projection.
            if words[5] <= 65536:
                assert legacy_hit == hit, (name, "legacy hit changed", legacy_hit, hit)
            assert self.legacy.projection() == self.on.projection(), (name, "legacy cache projection changed")
        if reference:
            rows, info = png.reference_decode(data, gamma)
            stride = words[4] + 1
            actual_rows = b"".join(raw_a[y * stride + 1:(y + 1) * stride] for y in range(words[1]))
            assert a[0] == 0 and actual_rows == rows, (name, "independent reference", a)
            assert last_a == info["pre_gamma_last"] and a[5] == info["last_filter"], name
            assert a[4] == info["crc"] or crc_entry is not None, name
            assert a[1] == info["chunks"] and a[6:11] == info["filters"], name
            assert a[15] + start == info["end_offset"], (name, "stream cursor", a, info)
        self.records.append({"name": name, "result": dict(zip(RESULT_NAMES, a)),
                             "candidate_result": dict(zip(RESULT_NAMES, b)),
                             "hit": bool(hit), "legacy_hit": legacy_hit,
                             "read_requests_returns": trace,
                             "control_feeds": feeds_a, "candidate_feeds": feeds_b,
                             "staging_control_candidate": [words[7], on_words[7]],
                             "whole_persistent_scratch_equal": True,
                             "raw_sha256": hashlib.sha256(raw_a).hexdigest(),
                             "last_sha256": hashlib.sha256(last_a).hexdigest()})
        return a, hit


def bounded_cases(dlls, records, tinfl=False):
    # The established suite keeps its two-core domain; large-specific tests
    # below add the third legacy-reuse projection without corruption leakage.
    pair = Pair(dlls, records, tinfl=tinfl)
    try:
        base = image(zlib.compress(bytes((0, 1, 0, 1, 0)), 0))
        pair.check("stored-cold", base, expect_hit=False)
        pair.check("stored-hit", base, expect_hit=True)
        inv = bytes(255 - i for i in range(256))
        gamma = bytearray(inv)
        pair.check("gamma-new", base, gamma, expect_hit=False)
        pair.check("gamma-equal-bytes-new-address", base, bytes(gamma), expect_hit=True)
        gamma[0] ^= 7
        pair.check("gamma-mutated-bytes", base, gamma, expect_hit=False)
        pair.check("gamma-mutated-hit", base, gamma, expect_hit=True)
        pair.check("gamma-old-value", base, inv, expect_hit=True)
        pair.check("gamma-null-return", base, expect_hit=True)
        pair.check("same-raw-different-geometry", image(zlib.compress(bytes((0, 1, 0, 1, 0)), 0), 4, 1, 0),
                   expect_hit=False)
        palette = image(zlib.compress(bytes((0, 1, 0, 1, 0)), 0), 4, 1, 3)
        pair.check("palette-ignored-gamma-cold", palette, gamma, expect_hit=False)
        pair.check("palette-ignored-gamma-hit", palette, inv, expect_hit=True)
        pair.check("palette-ignored-gamma-null", palette, expect_hit=True)
        pair.check("crc-entry-changed", base, crc_entry=0x12345678, expect_hit=False)
        pair.check("crc-entry-original", base, expect_hit=True)
        # The final IDAT CRC remains guest-owned, so core stops before it even
        # on a hit. A corrupt final CRC is deliberately not checked by this API.
        first = inputs(base)[0][5]
        modified = bytearray(base)
        modified[inputs(base)[2] + first] ^= 1
        pair.check("final-crc-still-unread", bytes(modified), expect_hit=True)
        pair.check("short-read-after-warm", base, cut=first - 1, reference=False, expect_hit=False)
        pair.check("single-staging-exact", base, staging=first, expect_hit=True)
        long_stored = image(zlib.compress(bytes((0, 1, 0, 1, 0)) * 2, 0), height=2)
        pair.check("staging16", long_stored, staging=16, expect_hit=False)
        for label, sizes in (("split", [3, 2]), ("empty-first", [0]), ("several-empty", [0, 0, 3])):
            multi = png.rebuild(base, sizes)
            pair.check(label, multi, expect_hit=False)
            pair.check(label + "-repeat", multi, expect_hit=False)
        for alias in range(1, 7):
            pair.on.lib.probe_alias_next(pair.on.p, alias)
            pair.check(f"cache-alias-or-null-{alias}", base, gamma=inv,
                       expect_hit=False)
        assert pair.on.lib.probe_corrupt_cached_input(pair.on.p) == 1
        pair.check("crc-geometry-candidate-wrong-bytes", base, expect_hit=False)
        # These payloads are copied from the established actual-core strict
        # boundary oracle, not inferred from the new cache implementation.
        raw = bytes((0, 1, 0, 1, 0))
        stored = zlib.compress(raw, 0)
        cases = [
            ("fixed-refusal", zlib.compress(raw, 6), True),
            ("singleton-valid", bytes.fromhex("78010dc1010900000080a0fa7fba1017000b0003"), True),
            ("singleton-invalid", bytes.fromhex("78010dc1010900000080a0fa7fba101f000b0003"), False),
            ("nonexact-success", stored + b"\x00", False),
            ("bad-adler", stored[:-1] + bytes([stored[-1] ^ 1]), False),
            ("wrong-output-small", zlib.compress(raw[:-1], 0), False),
            ("wrong-output-large", zlib.compress(raw + b"\x00", 0), False),
            ("strict-ok-bad-filter", zlib.compress(bytes((5, 1, 0, 1, 0)), 0), False),
        ]
        for name, payload, valid in cases:
            before = pair.on.stats()["stores"]
            pair.check(name, image(payload), reference=valid, expect_hit=False)
            accepted = tinfl and name == "fixed-refusal"
            pair.check(name + "-repeat", image(payload), reference=valid, expect_hit=accepted)
            assert pair.on.stats()["stores"] == before + int(accepted), (name, "wrong admission")
        compressor = zlib.compressobj(6)
        two_blocks = (compressor.compress(raw[:2]) + compressor.flush(zlib.Z_SYNC_FLUSH) +
                      compressor.compress(raw[2:]) + compressor.flush())
        pair.check("multiple-block-cold", image(two_blocks), expect_hit=False)
        pair.check("multiple-block-repeat", image(two_blocks), expect_hit=tinfl)
        # A SUCCESS from the old decoder is not automatically cacheable.
        # Prove real scratch dependence, not merely an invalid-input refusal.
        initial = bytes((0, 77, 11, 22, 33))
        pair.check("history-prime", image(zlib.compress(initial, 0)))
        unsafe = image(zero_distance_payload(initial))
        before = pair.on.stats()["stores"]
        pair.check("distance-zero-success", unsafe, reference=False,
                   expect_status=0, expect_hit=False)
        pair.check("distance-zero-repeat", unsafe, reference=False,
                   expect_status=0, expect_hit=False)
        assert pair.on.stats()["stores"] == before, "scratch-dependent success cached"
        pair.check("history-change", image(zlib.compress(bytes((0, 77, 44, 55, 66)), 0)))
        changed = pair.check("distance-zero-changed-history", unsafe,
                             reference=False, expect_hit=False)
        assert changed[0][0] != 0, "fixture must actually observe prior scratch"
        pair.check("valid-after-errors", base, expect_hit=True)
        assert pair.off.scratch() == pair.on.scratch(), "persistent whole-scratch tail changed"
    finally:
        pair.close()
    # A deliberately tiny independent arena exercises wrap and overlap
    # eviction without adding a memory cache to production.
    pair = Pair(dlls, records, cache_bytes=160, tinfl=tinfl)
    try:
        entries = [image(zlib.compress(bytes((0, i, i, i, i)), 0)) for i in range(18)]
        for i, data in enumerate(entries):
            pair.check(f"wrap-cold-{i}", data, expect_hit=False)
            pair.check(f"wrap-hit-{i}", data, expect_hit=True)
        assert pair.on.stats()["evictions"] > 0
        pair.check("wrap-old-evicted", entries[0], expect_hit=False)
        large = image(zlib.compress(b"\0" * (65 * 4 + 1), 0), width=65)
        pair.check("entry-exceeds-budget", large, expect_hit=False)
        pair.check("entry-exceeds-budget-repeat", large, expect_hit=False)
        assert pair.on.stats()["skipped"] == 2
    finally:
        pair.close()
    pair = Pair(dlls, records, tinfl=tinfl)
    try:
        for i in range(40):
            pair.check(f"slot-fill-{i}", image(zlib.compress(bytes((0, i, 0, i, 0)), 0)), expect_hit=False)
        assert pair.on.stats()["evictions"] == 8
        pair.check("slot32-evicted", image(zlib.compress(bytes((0, 0, 0, 0, 0)), 0)), expect_hit=False)
    finally:
        pair.close()


def exact_stored_image(input_bytes, tail_change=0, bad_filter=False):
    """Exact compressed-size fixture using ordinary byte-aligned stored blocks.

    Eight grayscale rows keep geometry within the frozen dimension bound.
    Empty stored blocks adjust framing length, not post-zlib padding; Python's
    independent zlib verifies the complete stream before the actual core sees it.
    """
    blocks = next(n for n in range(1, 17)
                  if (input_bytes - 6 - 5 * n) % 8 == 0 and
                  0 < input_bytes - 6 - 5 * n <= n * 65535)
    raw_bytes = input_bytes - 6 - 5 * blocks
    stride = raw_bytes // 8
    raw = bytearray((i * 13 + 7) & 255 for i in range(raw_bytes))
    for y in range(8):
        raw[y * stride] = 0
    raw[-1] ^= tail_change
    if bad_filter:
        raw[7 * stride] = 5
    payload = bytearray(b"\x78\x01")
    pos = 0
    for i in range(blocks):
        count = min(65535, len(raw) - pos)
        payload += bytes([int(i == blocks - 1)]) + struct.pack("<HH", count, count ^ 65535)
        payload += raw[pos:pos + count]
        pos += count
    payload += struct.pack(">I", zlib.adler32(raw))
    assert len(payload) == input_bytes and zlib.decompress(payload) == raw
    return image(bytes(payload), width=stride - 1, height=8, ct=0)


def large_cases(dlls, records):
    pair = Pair(dlls, records, tinfl=True, large=True)
    try:
        for n in (65536, 65537, 131072, 131073):
            data = exact_stored_image(n)
            before = pair.on.stats()["stores"]
            pair.check(f"boundary-{n}-cold", data, expect_hit=False)
            admitted = n <= 131072
            assert pair.last_stored == admitted, (n, "boundary admission")
            pair.check(f"boundary-{n}-repeat", data, expect_hit=admitted)
            assert pair.on.stats()["stores"] == before + int(admitted)
        data = exact_stored_image(131072)
        gamma = bytearray(255 - i for i in range(256))
        pair.check("large-gamma-cold", data, gamma, expect_hit=False)
        pair.check("large-gamma-repeat", data, bytes(gamma), expect_hit=True)
        gamma[7] ^= 31
        pair.check("large-gamma-key-change", data, gamma, expect_hit=False)
        pair.check("large-gamma-key-repeat", data, gamma, expect_hit=True)
        for cut in (0, 65535, 65536, 65537, 131071):
            before = pair.on.stats()["stores"]
            pair.check(f"large-short-read-{cut}", data, gamma, cut=cut,
                       reference=False, expect_hit=False, expect_status=1)
            assert pair.on.stats()["stores"] == before
        for sizes in ([65536], [65537], [0, 65536], [1, 65536]):
            split = png.rebuild(data, sizes)
            before = pair.on.stats()["stores"]
            pair.check(f"large-split-{sizes}", split, expect_hit=False)
            pair.check(f"large-split-{sizes}-repeat", split, expect_hit=False)
            assert pair.on.stats()["stores"] == before, (sizes, "split IDAT admitted")
        payload = next(b for _, t, b, _ in png.chunks(data) if t == b"IDAT")
        words = inputs(data)[0]
        damaged_header = b"\x78\x00" + payload[2:]
        damaged_adler = payload[:-1] + bytes([payload[-1] ^ 1])
        damaged_block = bytearray(payload)
        # The second stored block header lies after the first 64KiB feed.
        damaged_block[2 + 5 + 65535 + 3] ^= 1
        for label, body in (("header", damaged_header), ("adler", damaged_adler),
                            ("late-stored-block", bytes(damaged_block))):
            invalid = image(body, words[0], words[1], 0)
            before = pair.on.stats()["stores"]
            pair.check(f"large-invalid-{label}", invalid, reference=False,
                       expect_hit=False, expect_status=5)
            pair.check(f"large-invalid-{label}-repeat", invalid, reference=False,
                       expect_hit=False, expect_status=5)
            assert pair.on.stats()["stores"] == before
        invalid_filter = exact_stored_image(131072, bad_filter=True)
        before = pair.on.stats()["stores"]
        for label in ("cold", "repeat"):
            pair.check(f"large-invalid-last-filter-{label}", invalid_filter,
                       reference=False, expect_hit=False, expect_status=8)
        assert pair.on.stats()["stores"] == before
        # CRC of a completed intermediate IDAT remains checked on both paths.
        split = bytearray(png.rebuild(data, [65537]))
        split[inputs(split)[2] + 65537] ^= 1
        pair.check("large-intermediate-crc", bytes(split), reference=False,
                   expect_hit=False, expect_status=2)
        changed = exact_stored_image(131072, tail_change=0x55)
        assert inputs(data)[1][:65536] == inputs(changed)[1][:65536]
        pair.check("large-changed-key-tail-cold", changed, expect_hit=False)
        pair.check("large-changed-key-tail-repeat", changed, expect_hit=True)
    finally:
        pair.close()
    # Fresh history proves the exact malformed prefix write count. Merely
    # matching error codes would miss the unsafe earlier 128KiB-read design.
    pair = Pair(dlls, records, tinfl=True, large=True)
    try:
        body = b"\x78\x01\x00\xff\xff\x00\x00" + bytes(65529 + 14)
        assert len(body) == 65550
        invalid = image(body, width=255, height=256, ct=0)
        pair.check("prefix65529-survives-short-tail", invalid, cut=65536,
                   reference=False, expect_hit=False, expect_status=1)
        expected = bytes(65529) + bytes([0x51]) * (pair.off.raw_bytes - 65529)
        assert pair.off.scratch() == expected and pair.on.scratch() == expected
        assert pair.off.trace(2) == [65536, 65536, 14, 0]
    finally:
        pair.close()
    pair = Pair(dlls, records, tinfl=True, large=True)
    try:
        data = exact_stored_image(131072)
        pair.check("tail-key-prime", data, expect_hit=False)
        assert pair.on.lib.probe_corrupt_large_tail(pair.on.p) == 1
        pair.check("tail-key-corrupt-same-crc-must-miss", data, expect_hit=False)
        pair.check("tail-key-restored-hit", data, expect_hit=True)
    finally:
        pair.close()
    pair = Pair(dlls, records, tinfl=True, large=True)
    try:
        # A history-dependent old-tinfl SUCCESS in the SECOND feed is still
        # inadmissible. Reuse the established reserved-distance fixture,
        # preceded by a genuine non-final stored block of 65535 bytes.
        raw = bytes(65535) + bytes((0, 77, 11, 22, 33))
        prime = image(zlib.compress(raw, 0), width=16384, height=4, ct=0)
        pair.check("large-history-prime", prime)
        bad_payload = (b"\x78\x01\x00\xff\xff\x00\x00" + raw[:65535] +
                       zero_distance_payload(raw[-5:])[2:-4] +
                       struct.pack(">I", zlib.adler32(raw)))
        assert 65536 < len(bad_payload) <= 131072
        unsafe = image(bad_payload, width=16384, height=4, ct=0)
        before = pair.on.stats()["stores"]
        pair.check("large-distance-zero-success", unsafe, reference=False,
                   expect_hit=False, expect_status=0)
        pair.check("large-distance-zero-repeat", unsafe, reference=False,
                   expect_hit=False, expect_status=0)
        assert pair.on.stats()["stores"] == before
        changed = bytes(65535) + bytes((0, 77, 44, 55, 66))
        pair.check("large-history-change", image(zlib.compress(changed, 0), width=16384, height=4, ct=0))
        pair.check("large-distance-zero-changed-history", unsafe, reference=False,
                   expect_hit=False, expect_status=5)
    finally:
        pair.close()
    # Class 3 may use gaps, but may not evict/reposition old strict OR optional
    # records. Compare the actual serialized old-live projection every call.
    pair = Pair(dlls, records, cache_bytes=200000, tinfl=True, large=True)
    try:
        strict = image(zlib.compress(bytes((0, 1, 0, 1, 0)), 0))
        optional = image(zlib.compress(bytes((0, 1, 0, 1, 0)), 6))
        large = exact_stored_image(65537)
        pair.check("priority-strict-cold", strict, expect_hit=False)
        pair.check("priority-optional-cold", optional, expect_hit=False)
        assert pair.on.entries(1) == pair.on.entries(2) == 1
        pair.check("priority-large-cold", large, expect_hit=False)
        assert pair.on.entries(3) == 1
        pair.check("priority-strict-old-hit", strict, expect_hit=True)
        pair.check("priority-optional-old-hit", optional, expect_hit=True)
        before = pair.on.stats()
        no_gap = exact_stored_image(65537, tail_change=1)
        pair.check("priority-no-free-gap", no_gap, expect_hit=False)
        assert pair.on.stats()["skipped"] == before["skipped"] + 1
        assert pair.on.stats()["evictions"] == before["evictions"]
        pair.check("priority-large-still-hit", large, expect_hit=True)
        pair.check("priority-new-optional", image(zlib.compress(bytes((0, 3, 0, 3, 0)), 6)), expect_hit=False)
        assert pair.on.entries(3) == 0, "small optional insertion must drop large first"
        pair.check("priority-large-reinsert", large, expect_hit=False)
        pair.check("priority-new-strict", image(zlib.compress(bytes((0, 4, 0, 4, 0)), 0)), expect_hit=False)
        assert pair.on.entries(3) == pair.on.entries(2) == 0
        pair.check("priority-small-return", optional, expect_hit=False)
    finally:
        pair.close()
    pair = Pair(dlls, records, tinfl=True, large=True)
    try:
        for i in range(32):
            pair.check(f"priority-slot-optional-{i}", image(zlib.compress(bytes((0, i, 0, i, 0)), 6)), expect_hit=False)
        assert pair.on.entries(2) == 32
        before = pair.on.stats()
        pair.check("priority-no-free-slot", exact_stored_image(65537), expect_hit=False)
        assert pair.on.stats()["skipped"] == before["skipped"] + 1
        assert pair.on.stats()["evictions"] == before["evictions"]
    finally:
        pair.close()


def isolated_large_files(dlls, records, files):
    eligible = []
    for path, name in files:
        data = path.read_bytes()
        parsed = inputs(data)
        if parsed and 65536 < parsed[0][5] <= 131072:
            eligible.append((data, name))
    pair = Pair(dlls, records, tinfl=True, large=True)
    try:
        for data, name in eligible:
            before = pair.on.stats()["stores"]
            pair.check(name + ":isolated-large-cold", data, expect_hit=False)
            assert pair.on.stats()["stores"] == before + 1, (name, "isolated real input not admitted")
            pair.check(name + ":isolated-large-hit", data, expect_hit=True)
        # Revisits after the OTHER real backdrop, not just immediate repeats.
        for data, name in eligible:
            pair.check(name + ":alternating-large-hit", data, expect_hit=True)
        stats = pair.on.stats()
    finally:
        pair.close()
    return {"files": len(eligible), "cache": stats}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--cc")
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--tinfl", action="store_true", help="enable observed original-tinfl reuse in ON")
    parser.add_argument("--large", action="store_true", help="compare 64KiB legacy feed with 128KiB late reuse; requires --tinfl")
    parser.add_argument("--extra-manifest", type=Path, action="append", default=[],
                        help="additional SHA-pinned real inputs in the existing census format")
    args = parser.parse_args()
    if args.large and not args.tinfl:
        parser.error("--large requires --tinfl")
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=True)
    # A failed rerun must not leave an earlier PASS looking like its result.
    (args.out / "results.json").write_text(
        json.dumps({"pass": False, "status": "in-progress; fresh build/test required"}), encoding="utf-8")
    dlls, builds = build(args.out, args.cc, args.tinfl, args.large)
    records = []
    bounded_cases(dlls, records, args.tinfl)
    if args.large:
        large_cases(dlls, records)
    synth = args.out / "synthetic"
    synth.mkdir(exist_ok=True)
    files = png.synth_corpus(synth, random.Random(20260906))
    manifest_info = None
    extra_manifest_info = []
    extra_files = []
    for manifest in ([args.manifest] if args.manifest else []) + args.extra_manifest:
        census = json.loads(manifest.read_text(encoding="utf-8-sig"))
        source = Path(census["source"])
        for item in census["images"]:
            path = source / item["path"]
            assert sha(path) == item["sha256"], ("corpus changed", path)
            files.append((path, "real:" + item["path"]))
            if manifest in args.extra_manifest:
                extra_files.append((path, "real:" + item["path"]))
        info = {"path": str(manifest), "sha256": sha(manifest),
                "verified_images": len(census["images"])}
        if manifest == args.manifest:
            manifest_info = info
        else:
            extra_manifest_info.append(info)
    pair = Pair(dlls, records, tinfl=args.tinfl, large=args.large)
    eligible = real_eligible = unsupported = 0
    try:
        for i, (path, name) in enumerate(files):
            data = path.read_bytes()
            first = pair.check(name + ":cold", data)
            if first is None:
                unsupported += 1
                continue
            result, hit = first
            expected = bool(hit or pair.last_stored)
            pair.check(name + ":repeat", data, expect_hit=expected)
            eligible += expected
            real_eligible += expected and name.startswith("real:")
            if i % 4 == 0:
                gamma = bytes(255 - x for x in range(256))
                pair.check(name + ":gamma", data, gamma=gamma)
                pair.check(name + ":gamma-repeat", data, gamma=gamma, expect_hit=expected)
            if i % 5 == 0:
                pair.check(name + ":staging16", data, staging=16)
                pair.check(name + ":staging37", data, staging=37)
        final_stats = pair.on.stats()
    finally:
        pair.close()
    isolated = isolated_large_files(dlls, records, extra_files) if args.large else None
    summary = {"pass": True, "differential_calls": len(records), "files": len(files),
               "unsupported_wrapper_shapes_skipped": unsupported, "immediate_repeat_eligible": eligible,
               "real_immediate_repeat_eligible": real_eligible, "final_corpus_cache_stats": final_stats,
               "manifest": manifest_info, "build": builds,
               "extra_manifests": extra_manifest_info,
               "large": args.large, "tinfl": args.tinfl,
               "isolated_large_real_inputs": isolated,
               "scope": "Host scalar actual core; no Vita wrapper/ARM/timing/memory-pressure validation.",
               "records": records}
    (args.out / "results.json").write_text(json.dumps(summary, indent=2), encoding="utf-8")
    print(f"PASS {len(records)} actual-core OFF/ON calls; {len(files)} images; "
          f"{eligible} immediate-repeat eligible ({real_eligible} real); cache {final_stats}")


if __name__ == "__main__":
    main()
