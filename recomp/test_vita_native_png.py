#!/usr/bin/env python3
"""Differential oracle for the native PNG decoder core (ISAAC_VITA_NATIVE_PNG).

Builds a corpus of PNG files (synthetic ones through Pillow covering every
8-bit colour type, tRNS, odd widths, NEON tails, multi-IDAT chunking and
zero-length IDAT chunks, plus any real files passed on the command line or
found under --corpus), decodes each with an independent reference (zlib +
the PNG filter equations written here, with the game's transform semantics:
raw channel bytes, no expansion, optional gamma_table map on the colour
channels) and with the host oracle built from host_vita_native_png.c, and
asserts byte identity, the reported stream end position (after the data of
the last IDAT chunk, before its CRC) and the running CRC of that chunk.
Then it injects the faults the Vita wrapper falls back on and asserts the
exact rejection code.

Usage: test_vita_native_png.py --oracle <binary> [--corpus DIR] [png ...]
"""

from __future__ import annotations

import argparse
import io
import os
import random
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

try:
    from PIL import Image
except ImportError:  # pragma: no cover
    Image = None

SIG = b"\x89PNG\r\n\x1a\n"
CHANNELS = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}


def chunks(data: bytes):
    pos = 8
    while pos + 8 <= len(data):
        length = struct.unpack(">I", data[pos:pos + 4])[0]
        ctype = data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        yield pos, ctype, body, data[pos + 8 + length:pos + 12 + length]
        pos += 12 + length


def rebuild(data: bytes, idat_sizes) -> bytes:
    """Re-chunk the IDAT payload into pieces of the given sizes (a size of 0
    inserts an empty IDAT); the remainder becomes one final chunk."""
    out = bytearray(SIG)
    idat = b"".join(body for _, t, body, _ in chunks(data) if t == b"IDAT")
    emitted = False
    for pos, ctype, body, _ in chunks(data):
        if ctype == b"IDAT":
            if emitted:
                continue
            emitted = True
            off = 0
            for size in idat_sizes:
                piece = idat[off:off + size]
                off += size
                out += chunk(b"IDAT", piece)
            out += chunk(b"IDAT", idat[off:])
        else:
            out += chunk(ctype, body)
    return bytes(out)


def chunk(ctype: bytes, body: bytes) -> bytes:
    return (struct.pack(">I", len(body)) + ctype + body +
            struct.pack(">I", zlib.crc32(ctype + body) & 0xffffffff))


def reference_decode(data: bytes, gamma=None):
    """Independent decode: returns (rows bytes, info dict) or None when the
    wrapper would not take the image (depth != 8, interlaced)."""
    ihdr = None
    idat = []
    for pos, ctype, body, crc in chunks(data):
        if ctype == b"IHDR":
            ihdr = struct.unpack(">IIBBBBB", body)
        elif ctype == b"IDAT":
            idat.append((pos, body))
    if ihdr is None:
        return None
    width, height, depth, ctype_, _, _, interlace = ihdr
    if depth != 8 or interlace != 0 or ctype_ not in CHANNELS:
        return None
    bpp = CHANNELS[ctype_]
    rowbytes = width * bpp
    payload = b"".join(body for _, body in idat)
    z = zlib.decompressobj()
    raw = z.decompress(payload) + z.flush()
    if len(raw) != height * (rowbytes + 1):
        raise AssertionError("reference: unexpected raw length")
    # png_read_row stops in the chunk where the zlib stream ends (trailing
    # empty IDAT chunks are left for png_read_end), so the end position and
    # the running CRC belong to that chunk, not to the last IDAT in the file
    stream_len = len(payload) - len(z.unused_data)
    consumed = 0
    last_data_end = last_crc = None
    end_chunks = 0
    for pos, body in idat:
        end_chunks += 1
        consumed += len(body)
        last_data_end = pos + 8 + len(body)
        last_crc = zlib.crc32(b"IDAT" + body) & 0xffffffff
        if consumed >= stream_len and consumed > 0:
            break
    rows = bytearray()
    prev = bytes(rowbytes)
    filters = [0] * 5
    for y in range(height):
        f = raw[y * (rowbytes + 1)]
        cur = bytearray(raw[y * (rowbytes + 1) + 1:(y + 1) * (rowbytes + 1)])
        filters[f] += 1
        if f == 1:
            for i in range(bpp, rowbytes):
                cur[i] = (cur[i] + cur[i - bpp]) & 0xff
        elif f == 2:
            for i in range(rowbytes):
                cur[i] = (cur[i] + prev[i]) & 0xff
        elif f == 3:
            for i in range(rowbytes):
                left = cur[i - bpp] if i >= bpp else 0
                cur[i] = (cur[i] + ((left + prev[i]) >> 1)) & 0xff
        elif f == 4:
            for i in range(rowbytes):
                a = cur[i - bpp] if i >= bpp else 0
                b = prev[i]
                c = prev[i - bpp] if i >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                cur[i] = (cur[i] + pred) & 0xff
        elif f != 0:
            raise AssertionError("reference: bad filter")
        prev = bytes(cur)
        rows += cur
    pre_gamma_last = bytes(prev)
    last_filter = raw[(height - 1) * (rowbytes + 1)]
    if gamma is not None and ctype_ != 3:
        # png_do_gamma: colour channels only, alpha untouched
        out = bytearray(rows)
        if ctype_ in (0, 2):
            for i in range(len(out)):
                out[i] = gamma[out[i]]
        elif ctype_ == 4:
            for i in range(0, len(out), 2):
                out[i] = gamma[out[i]]
        elif ctype_ == 6:
            for i in range(0, len(out), 4):
                out[i] = gamma[out[i]]
                out[i + 1] = gamma[out[i + 1]]
                out[i + 2] = gamma[out[i + 2]]
        rows = out
    return bytes(rows), {
        "width": width, "height": height, "ct": ctype_, "ch": bpp,
        "end_offset": last_data_end, "crc": last_crc, "filters": filters,
        "chunks": end_chunks, "pre_gamma_last": pre_gamma_last,
        "last_filter": last_filter,
    }


def synth_corpus(out_dir: Path, rng: random.Random):
    """Synthetic PNGs through Pillow; returns [(path, description)]."""
    files = []
    if Image is None:
        return files
    modes = [("L", 0), ("LA", 4), ("RGB", 2), ("RGBA", 6), ("P", 3)]
    sizes = [(1, 1), (1, 7), (7, 1), (3, 5), (15, 9), (16, 16), (17, 33),
             (31, 2), (64, 64), (100, 37), (257, 129), (700, 300)]
    for mode, ct in modes:
        for (w, h) in sizes:
            for level in (0, 1, 6, 9):
                if level != 6 and (w * h) > 2000:
                    continue
                img = Image.new(mode, (w, h))
                px = img.load()
                for y in range(h):
                    for x in range(w):
                        v = (x * 7 + y * 13 + rng.randrange(0, 4)) & 0xff
                        if mode == "L":
                            px[x, y] = v
                        elif mode == "LA":
                            px[x, y] = (v, (x * y) & 0xff)
                        elif mode == "RGB":
                            px[x, y] = (v, (v * 3) & 0xff, (x ^ y) & 0xff)
                        elif mode == "RGBA":
                            px[x, y] = (v, (v * 3) & 0xff, (x ^ y) & 0xff,
                                        (y * 5 + x) & 0xff)
                        else:
                            px[x, y] = v
                buf = io.BytesIO()
                kwargs = {"compress_level": level}
                if mode == "P":
                    img.putpalette([((i * 3) & 0xff, (i * 5) & 0xff, i)
                                    for i in range(256) for _ in (0,)][:256 * 1]
                                   if False else
                                   sum(([(i * 3) & 0xff, (i * 5) & 0xff, i]
                                        for i in range(256)), []))
                    if (w + h) % 2 == 0:
                        kwargs["transparency"] = 3
                if mode in ("L", "RGB") and (w + h) % 3 == 0:
                    kwargs["transparency"] = 1 if mode == "L" else (1, 2, 3)
                img.save(buf, format="PNG", **kwargs)
                data = buf.getvalue()
                name = f"synth-{mode}-{w}x{h}-l{level}.png"
                path = out_dir / name
                path.write_bytes(data)
                files.append((path, name))
    # multi-IDAT variants of a mid-sized RGBA image, including empty chunks
    base = out_dir / "synth-RGBA-100x37-l6.png"
    if base.exists():
        data = base.read_bytes()
        for sizes_ in ([1], [0], [0, 0, 5], [3, 0, 3], [17, 4093, 1, 0],
                       [4096], [65535], [len(data)]):
            variant = rebuild(data, sizes_)
            name = "synth-multi-" + "-".join(str(s) for s in sizes_) + ".png"
            (out_dir / name).write_bytes(variant)
            files.append((out_dir / name, name))
    # adler split across chunks: last chunk holds only the trailer
    if base.exists():
        data = base.read_bytes()
        idat = b"".join(b for _, t, b, _ in chunks(data) if t == b"IDAT")
        variant = rebuild(data, [len(idat) - 4])
        (out_dir / "synth-multi-adler-split.png").write_bytes(variant)
        files.append((out_dir / "synth-multi-adler-split.png", "adler-split"))
        variant = rebuild(data, [len(idat) - 4, 2])
        (out_dir / "synth-multi-adler-split2.png").write_bytes(variant)
        files.append((out_dir / "synth-multi-adler-split2.png", "adler-split2"))
    # unsupported shapes the wrapper must refuse: 16-bit and interlaced
    if Image is not None:
        img = Image.new("RGBA", (9, 9))
        buf = io.BytesIO()
        img.save(buf, format="PNG", interlace=1)
        (out_dir / "synth-interlaced.png").write_bytes(buf.getvalue())
        files.append((out_dir / "synth-interlaced.png", "interlaced"))
        img16 = Image.new("I;16", (9, 9))
        buf = io.BytesIO()
        img16.save(buf, format="PNG")
        (out_dir / "synth-16bit.png").write_bytes(buf.getvalue())
        files.append((out_dir / "synth-16bit.png", "16bit"))
    return files


def run_oracle(oracle: str, png: Path, raw: Path, *extra):
    proc = subprocess.run([oracle, *extra, str(png), str(raw)],
                          capture_output=True, text=True, check=False)
    if proc.returncode != 0:
        raise AssertionError(f"oracle failed on {png}: {proc.stderr}")
    fields = {}
    for token in proc.stdout.split():
        if "=" in token:
            key, value = token.split("=", 1)
            fields[key] = value
    return fields


def check_identity(oracle, png: Path, work: Path, gamma_args, gamma_table,
                   staging=None):
    data = png.read_bytes()
    ref = reference_decode(data, gamma_table)
    raw = work / (png.name + ".raw")
    extra = list(gamma_args)
    if staging is not None:
        extra += ["--staging", str(staging)]
    fields = run_oracle(oracle, png, raw, *extra)
    if ref is None:
        assert fields["status"] == "unsupported", (png, fields)
        return "unsupported"
    rows, info = ref
    assert fields["status"] == "ok", (png, fields)
    got = raw.read_bytes()
    assert got == rows, f"{png}: rows differ at {first_diff(got, rows)}"
    assert int(fields["end_offset"]) == info["end_offset"], (png, fields, info)
    assert int(fields["crc"], 16) == info["crc"], (png, fields, info)
    assert int(fields["chunks"]) == info["chunks"], (png, fields, info)
    assert fields["filters"] == "/".join(str(f) for f in info["filters"]), (
        png, fields, info)
    last = (work / (png.name + ".raw.last")).read_bytes()
    assert last[0] == info["last_filter"], (png, last[0], info["last_filter"])
    assert last[1:] == info["pre_gamma_last"], f"{png}: last row (pre-gamma)"
    return "ok"


def first_diff(a: bytes, b: bytes):
    n = min(len(a), len(b))
    for i in range(n):
        if a[i] != b[i]:
            return i
    return n if len(a) != len(b) else -1


def fault_cases(oracle, png: Path, work: Path):
    """Inject the wrapper's fallback triggers and assert the exact code."""
    data = png.read_bytes()
    idats = [(pos, body) for pos, t, body, _ in chunks(data) if t == b"IDAT"]
    assert idats
    first_pos, first_body = idats[0]
    last_pos, last_body = idats[-1]
    raw = work / "fault.raw"
    results = {}
    # short read inside the first chunk's data
    if len(first_body) > 3:
        f = run_oracle(oracle, png, raw, "--truncate", str(first_pos + 8 + 2))
        results["truncate-data"] = f["status"]
        assert f["status"] == "read", f
    # CRC of a middle/first chunk (only checked when another chunk follows)
    if len(idats) > 1:
        f = run_oracle(oracle, png, raw, "--flip", str(first_pos + 8 + len(first_body)))
        results["flip-crc"] = f["status"]
        assert f["status"] == "crc", f
        # chunk type of the second IDAT
        second_pos = idats[1][0]
        f = run_oracle(oracle, png, raw, "--flip", str(second_pos + 4))
        results["flip-type"] = f["status"]
        assert f["status"] == "not-idat", f
        # a truncated file before the second chunk header
        f = run_oracle(oracle, png, raw, "--truncate", str(second_pos + 3))
        results["truncate-header"] = f["status"]
        assert f["status"] == "read", f
    # corrupt compressed data (CRC of that chunk is now wrong as well; the
    # decoder sees the inflate error first when the corruption is inside
    # the deflate payload of the last chunk with no CRC read after it)
    if len(last_body) > 12:
        off = last_pos + 8 + len(last_body) // 2
        f = run_oracle(oracle, png, raw, "--flip", str(off))
        results["flip-data"] = f["status"]
        assert f["status"] in ("inflate", "extra", "truncated", "filter", "crc"), f
    # trailing garbage inside the last IDAT after the zlib stream
    tail = rebuild(data, []) if False else None
    body = b"".join(b for _, b in idats) + b"\x00\x01\x02\x03"
    variant = bytearray(SIG)
    done = False
    for _, t, b, _ in chunks(data):
        if t == b"IDAT":
            if not done:
                variant += chunk(b"IDAT", body)
                done = True
        else:
            variant += chunk(t, b)
    p = work / "fault-extra.png"
    p.write_bytes(bytes(variant))
    f = run_oracle(oracle, p, raw)
    results["extra-in-chunk"] = f["status"]
    assert f["status"] == "extra", f
    # the zlib stream ends before the last row: drop the last row from the
    # payload and recompress
    ref = reference_decode(data)
    if ref is not None:
        rows, info = ref
        rowbytes = info["width"] * info["ch"]
        raw_rows = b"".join(b"\x00" + rows[y * rowbytes:(y + 1) * rowbytes]
                            for y in range(info["height"] - 1))
        payload = zlib.compress(raw_rows)
        variant = bytearray(SIG)
        done = False
        for _, t, b, _ in chunks(data):
            if t == b"IDAT":
                if not done:
                    variant += chunk(b"IDAT", payload)
                    done = True
            else:
                variant += chunk(t, b)
        p = work / "fault-short.png"
        p.write_bytes(bytes(variant))
        f = run_oracle(oracle, p, raw)
        results["stream-short"] = f["status"]
        assert f["status"] == "truncated", f
        # one row too many
        raw_rows = b"".join(b"\x00" + rows[y * rowbytes:(y + 1) * rowbytes]
                            for y in range(info["height"]))
        payload = zlib.compress(raw_rows + b"\x00" * (rowbytes + 1))
        variant = bytearray(SIG)
        done = False
        for _, t, b, _ in chunks(data):
            if t == b"IDAT":
                if not done:
                    variant += chunk(b"IDAT", payload)
                    done = True
            else:
                variant += chunk(t, b)
        p = work / "fault-long.png"
        p.write_bytes(bytes(variant))
        f = run_oracle(oracle, p, raw)
        results["stream-long"] = f["status"]
        assert f["status"] == "extra", f
        # a filter byte above 4 in the first row
        bad = bytearray(raw_rows)
        bad[0] = 5
        payload = zlib.compress(bytes(bad))
        variant = bytearray(SIG)
        done = False
        for _, t, b, _ in chunks(data):
            if t == b"IDAT":
                if not done:
                    variant += chunk(b"IDAT", payload)
                    done = True
            else:
                variant += chunk(t, b)
        p = work / "fault-filter.png"
        p.write_bytes(bytes(variant))
        f = run_oracle(oracle, p, raw)
        results["bad-filter"] = f["status"]
        assert f["status"] == "filter", f
        # a wrong Adler-32 (payload recompressed, last 4 bytes flipped)
        payload = bytearray(zlib.compress(raw_rows))
        payload[-1] ^= 0xff
        variant = bytearray(SIG)
        done = False
        for _, t, b, _ in chunks(data):
            if t == b"IDAT":
                if not done:
                    variant += chunk(b"IDAT", bytes(payload))
                    done = True
            else:
                variant += chunk(t, b)
        p = work / "fault-adler.png"
        p.write_bytes(bytes(variant))
        f = run_oracle(oracle, p, raw)
        results["bad-adler"] = f["status"]
        assert f["status"] == "inflate", f
    return results


def strict_cases(oracle, work: Path):
    """Small admission/fallback boundaries; opt-in only, same whole-PNG oracle."""
    def image(payload, height=1):
        return (SIG + chunk(b"IHDR", struct.pack(">IIBBBBB", 1, height, 8, 6, 0, 0, 0)) +
                chunk(b"IDAT", payload) + chunk(b"IEND", b""))
    raw = bytes((0, 1, 0, 1, 0))
    stored = zlib.compress(raw, 0)
    cases = [
        ("stored", stored, "ok", "1/1/0", None),
        ("stored-exact-staging", stored, "ok", "1/1/0", len(stored)),
        ("stored-short-staging", zlib.compress(raw * 2, 0), "ok", "0/0/0", 16),
        ("fixed-refusal", zlib.compress(raw, 6), "ok", "1/0/1", None),
        ("singleton-valid", bytes.fromhex("78010dc1010900000080a0fa7fba1017000b0003"), "ok", "1/0/1", None),
        ("singleton-invalid", bytes.fromhex("78010dc1010900000080a0fa7fba101f000b0003"), "extra", "1/0/1", None),
        ("nonexact-success", stored + b"\x00", "extra", "1/0/1", None),
        ("bad-adler", stored[:-1] + bytes([stored[-1] ^ 1]), "inflate", "1/0/1", None),
        ("wrong-output-small", zlib.compress(raw[:-1], 0), "truncated", "1/0/1", None),
        ("wrong-output-large", zlib.compress(raw + b"\x00", 0), "extra", "1/0/1", None),
    ]
    results = {}
    for name, payload, status, counts, staging in cases:
        path = work / ("strict-" + name + ".png")
        path.write_bytes(image(payload, 2 if name == "stored-short-staging" else 1))
        extra = ["--staging", str(staging)] if staging is not None else []
        fields = run_oracle(oracle, path, work / "strict.raw", *extra)
        assert fields["status"] == status and fields["strict"] == counts, (name, fields)
        a, s, r = map(int, fields["strict"].split("/"))
        assert a == s + r
        results[name] = fields
    # A short first IDAT is tried and refused; no subsequent chunk is tried.
    # A leading empty IDAT advances the chunk counter before any probe.
    for name, sizes in (("split-idat", [3, 2]), ("empty-first", [0])):
        path = work / ("strict-" + name + ".png")
        path.write_bytes(rebuild(image(stored), sizes))
        fields = run_oracle(oracle, path, work / "strict.raw")
        expected = "1/0/1" if name == "split-idat" else "0/0/0"
        assert fields["status"] == "ok" and fields["strict"] == expected, (name, fields)
        results[name] = fields
    return results


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--oracle", required=True)
    parser.add_argument("--corpus", action="append", default=[])
    parser.add_argument("--keep", default=None,
                        help="directory to keep the generated corpus in")
    parser.add_argument("--strict", action="store_true",
                        help="also check strict first-IDAT admission and old-tinfl fallback")
    parser.add_argument("pngs", nargs="*")
    args = parser.parse_args()
    rng = random.Random(20260903)
    work_dir = Path(args.keep) if args.keep else Path(tempfile.mkdtemp(
        prefix="isaac-native-png."))
    work_dir.mkdir(parents=True, exist_ok=True)
    files = synth_corpus(work_dir, rng)
    for corpus in args.corpus:
        for path in sorted(Path(corpus).rglob("*.png")):
            files.append((path, f"corpus:{path.name}"))
    for path in args.pngs:
        files.append((Path(path), f"arg:{Path(path).name}"))
    if not files:
        print("no corpus (Pillow missing and no files given)", file=sys.stderr)
        return 2
    gamma_pow = [int(pow(i / 255.0, 1.0 / 0.87) * 255.0 + 0.5) & 0xff
                 for i in range(256)]
    gamma_inv = [255 - i for i in range(256)]
    ok = unsupported = 0
    gamma_checked = 0
    staging_checked = 0
    for path, desc in files:
        result = check_identity(args.oracle, path, work_dir, [], None)
        if result == "unsupported":
            unsupported += 1
            continue
        ok += 1
        # the same file through the gamma map and through a tiny staging
        # buffer (chunk boundaries inside inflate calls)
        if ok % 3 == 0:
            check_identity(args.oracle, path, work_dir, ["--gamma-invert"],
                           gamma_inv)
            check_identity(args.oracle, path, work_dir,
                           ["--gamma-pow", "1.149425287"], gamma_pow)
            gamma_checked += 1
        if ok % 4 == 0:
            check_identity(args.oracle, path, work_dir, [], None, staging=16)
            check_identity(args.oracle, path, work_dir, [], None, staging=37)
            staging_checked += 1
    faults = {}
    for name in ("synth-RGBA-100x37-l6.png", "synth-multi-17-4093-1-0.png",
                 "synth-P-64x64-l6.png"):
        path = work_dir / name
        if path.exists():
            faults[name] = fault_cases(args.oracle, path, work_dir)
    print(f"native png oracle: identity PASS on {ok} images "
          f"({gamma_checked} also through two gamma tables, {staging_checked} "
          f"through 16/37-byte staging), {unsupported} unsupported shapes "
          f"refused, fault cases: {faults}")
    if args.strict:
        print("strict first-IDAT boundaries:", strict_cases(args.oracle, work_dir))
    return 0


if __name__ == "__main__":
    sys.exit(main())
