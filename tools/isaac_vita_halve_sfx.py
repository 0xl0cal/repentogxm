#!/usr/bin/env python3
"""Halve the sample rate of Repentance SFX WAVs whose treble is empty.

Offline and opt-in.  Nothing in the port changes; this rewrites a COPY of the
game's sound catalogue so the engine keeps less PCM resident and reads fewer
bytes when it loads a sound.  It never writes in place.

Why it is safe for THIS engine (frozen PE isaac-ng.exe.unpacked.exe, v1.7.9b
J835; RVAs below are the translator's sub_XXXXXXXX names):

* The static WAV class (vtable RVA 0x766764, chosen by the sound factory at
  0x56f2ec..0x56f31d when the sample path contains ".wav") loads a sample in
  sub_005a3b30: the RIFF walker sub_005bf4e0 copies the 16 fmt bytes verbatim
  to this+0x50 and the data chunk into one guest-heap block of exactly the
  data-chunk size (this+0x4c, size at this+0x48).  The PCM stays resident for
  the life of the sound.
* Every Play (sub_005be5b0) re-uploads that block: alBufferData at 0x5be6dd
  takes format = 0x1100|16-bit(0x1)|stereo(0x2) from channels (word at
  +0x52, sub_005a3c90) and bits (word at +0x5e == 16, sub_005a3ca0), and
  frequency = the fmt nSamplesPerSec dword at +0x54 (sub_0054e710).  Nothing
  else reads the rate; no code compares it with 44100.
* Pitch is AL_PITCH (0x1003) through alSourcef at 0x5bea00 (sub_005be9e0), a
  ratio relative to the buffer's own frequency; volume is AL_GAIN (0x100a) at
  0x5be8f0.  Both are rate independent.
* The shipped catalogue already contains 22050 Hz mono16 samples (for example
  "sfx/Spewer death scream 1.wav" in afterbirthp.a, fmt length 18), and a
  second static sound class (vtable RVA 0x76681c) hard-codes 22050 Hz stereo
  through the same Play path.

What changes in a halved file: fmt.nSamplesPerSec and fmt.nAvgBytesPerSec,
the data chunk length, the RIFF size, and the PCM itself (127-tap Kaiser sinc
low pass, cutoff 0.45 of the old Nyquist, then every second frame).  Channel
count, bit depth, block align, chunk order and every other chunk byte are
kept identical.  Files whose spectral energy above the new Nyquist exceeds
the threshold (default 0.5 %) are left byte for byte alone, as is anything
that is not canonical 16-bit PCM.

Archives: the Vita port reads resources/packed/*.a ("ARCH000").  The codec
here mirrors what the engine does, read off the frozen PE: table hashes djb2
0x1505 / FNV-1a 0x5bb2220e with upper-case and backslash folding; mount
checksum rotr1-add from 0xababeb98 at 0x563b73 over the decoded bytes; raw
(version 0) XOR seed (h2 ^ 0xf9524287) | 1 at 0x59c24b; version-2 pieces of
u32 (len | final<<31) + bytes, deflate into one tinfl state or, after a full
non-final 0x400 piece, stored bytes XORed with an ISAAC keystream whose 256
seeds come from a PCG32 with multiplier 0x5851f42d4c957f2d (0x5c2f50).
Repentance keys its DLC archive as "resources-dlc3/<path>", the older ones as
"resources/<path>"; both prefixes are tried per archive.  LZW (version 1)
archives cannot be re-encoded and pass through untouched.

Mount-time validation (why afterbirth.a is excluded by default and why a
".receipt" sidecar is written next to each rewritten archive):
FileManager::LoadArchiveFile (sub_00563880) decodes every entry of every
archive at mount and folds a checksum (0x563b59..0x563c8c).  The Vita port
skips that per entry when the index checksum matches the executable's baked
table (ISAAC_VITA_ARCHIVE_VALIDATION_SKIP) and, for afterbirth.a, when a
runtime receipt has proven the shipped file's exact identity
(ISAAC_VITA_ARCHIVE_VALIDATION_RECEIPT, pinned to size 145,319,513 / 2,647
entries).  Rewritten entries have new checksums, so the original loop runs
for them on every boot: afterbirth.a alone measured 63-66 s on the device
(vita-debug logs, '[archive-validation-receipt] archive=9 ... original=2647'),
and the rewritten entries of the other archives are read, decoded and folded
at about 2.5 MiB/s.  An eboot built with ISAAC_VITA_ARCHIVE_VALIDATION_SIDECAR
=ON instead reads the 256-byte sidecar this tool writes after verifying every
entry of the written archive, checks the archive against it (size, header,
SHA-256 of the index table, SHA-256 of 64 x 4 KiB samples) and skips the
loop.  Other builds ignore the sidecar.

Usage:
  isaac_vita_halve_sfx.py wav IN.wav|IN_DIR OUT.wav|OUT_DIR
  isaac_vita_halve_sfx.py archive IN.a OUT.a [--catalogue sounds.xml|other.a]
  isaac_vita_halve_sfx.py tree RESOURCES_COPY OUT_DIR [--copy-unchanged]
  isaac_vita_halve_sfx.py receipt ARCHIVE.a [OUT.receipt]
  common: --threshold 0.005 --min-rate 32000 --encode stored|deflate|auto
          --dry-run --measure --report out.json --prefix resources-x/
  archive/tree: --game-log log.txt   (what the rewrite saves for the sounds
          one Vita session actually loaded: 'Preload sound N.' and
          '[warn] Sound N was not preloaded.' lines)
          --include-afterbirth (sidecar-aware eboot only), --no-sidecar

Requires numpy (FFT and convolution); the archive codec, the receipt
subcommand and the sidecar functions do not.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import struct
import sys
import time
import zlib
from dataclasses import dataclass, field
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    import numpy as np
else:
    try:
        import numpy as np
    except ImportError:  # pragma: no cover - reported at run time
        np = None

TAPS = 127
CUTOFF = 0.45            # of the OLD Nyquist; the new Nyquist is 0.5
KAISER_BETA = 8.0        # about 81 dB of stop band
DEFAULT_THRESHOLD = 0.005
DEFAULT_MIN_RATE = 32000
MIN_FRAMES = 256
MEASURE_BAND = 0.9       # of the NEW Nyquist, the band that must survive

MASK32 = 0xFFFFFFFF

# ---------------------------------------------------------------------------
# WAV
# ---------------------------------------------------------------------------


@dataclass
class WavInfo:
    riff_size: int
    fmt_off: int          # offset of the fmt payload
    fmt_len: int
    format_tag: int
    channels: int
    rate: int
    byte_rate: int
    block_align: int
    bits: int
    data_off: int         # offset of the data payload
    data_len: int         # data chunk length as written
    chunks: list = field(default_factory=list)   # (id, header offset, length)

    @property
    def frames(self) -> int:
        return self.data_len // self.block_align if self.block_align else 0


class WavError(ValueError):
    pass


def _engine_walk(data: bytes) -> tuple[int | None, int | None, int | None, int, int]:
    """The chunk walk of sub_005bf4e0: no pad bytes, the fmt extension is
    skipped by its own cbSize word.  The first pass returns at the data chunk;
    the second pass (0x5bf5d9 'add esi,[esp+0x14]; jmp 0x5bf5f3') keeps
    walking after it, so a later 'fmt ' chunk is copied over the 16 header
    bytes and a later 'data' chunk is read again into the same block.  Returns
    (fmt header off, data header off, first data length, fmt count, data
    count) as the second pass sees them."""
    size = len(data)
    pos = 12
    fmt_at = data_at = data_len = None
    fmts = datas = 0
    while pos + 8 <= size:
        cid = data[pos:pos + 4]
        ln, = struct.unpack_from("<I", data, pos + 4)
        here = pos
        pos += 8
        if cid == b"fmt ":
            fmts += 1
            if fmt_at is None:
                fmt_at = here
            pos += 16
            if ln > 16:
                if pos + 2 > size:
                    break
                cb, = struct.unpack_from("<H", data, pos)
                pos += 2 + cb
        elif cid == b"data":
            datas += 1
            if data_at is None:
                data_at, data_len = here, ln
            pos += ln
        else:
            pos += ln
    return fmt_at, data_at, data_len, fmts, datas


def parse_wav(data: bytes) -> WavInfo:
    """Parse a canonical RIFF/WAVE.  Raises WavError for anything the engine
    would read differently from a standards-following walker."""
    if len(data) < 44 or data[:4] != b"RIFF" or data[8:12] != b"WAVE":
        raise WavError("not RIFF/WAVE")
    riff_size, = struct.unpack_from("<I", data, 4)
    pos = 12
    chunks = []
    fmt = None
    data_chunk = None
    while pos + 8 <= len(data):
        cid = data[pos:pos + 4]
        ln, = struct.unpack_from("<I", data, pos + 4)
        body = pos + 8
        chunks.append((cid, pos, ln))
        if cid == b"fmt " and fmt is None:
            if ln < 16 or body + 16 > len(data):
                raise WavError("fmt chunk too short")
            fmt = (body, ln) + struct.unpack_from("<HHIIHH", data, body)
        elif cid == b"data" and data_chunk is None:
            if body + ln > len(data):
                raise WavError("data chunk runs past the end of the file")
            data_chunk = (body, ln)
            if fmt is None:
                raise WavError("data chunk before fmt")
        pos = body + ln + (ln & 1)
    if fmt is None or data_chunk is None:
        raise WavError("missing fmt or data chunk")
    e_fmt, e_data, e_len, fmts, datas = _engine_walk(data)
    if e_fmt != fmt[0] - 8 or e_data != data_chunk[0] - 8 or e_len != data_chunk[1]:
        raise WavError("engine chunk walk disagrees with the RIFF walk")
    if fmts != 1:
        raise WavError("%d fmt chunks: the engine's second walk would copy the later one over the header" % fmts)
    if datas != 1:
        raise WavError("%d data chunks: the engine's second walk would read the later one into the same block" % datas)
    body, ln, tag, ch, rate, byte_rate, align, bits = fmt
    return WavInfo(riff_size, body, ln, tag, ch, rate, byte_rate, align, bits,
                   data_chunk[0], data_chunk[1], chunks)


def wav_skip_reason(info: WavInfo, min_rate: int) -> str | None:
    """Why a parsed WAV is not a halving candidate, or None."""
    if info.format_tag != 1:
        return "format tag %d is not PCM" % info.format_tag
    if info.bits != 16:
        return "%d-bit, not 16-bit" % info.bits
    if info.channels not in (1, 2):
        return "%d channels" % info.channels
    if info.block_align != info.channels * 2:
        return "block align %d does not match %d channels" % (info.block_align, info.channels)
    if info.byte_rate != info.rate * info.block_align:
        return "byte rate %d is not rate * block align" % info.byte_rate
    if info.fmt_len > 16:
        cb_ok = info.fmt_len >= 18
        if not cb_ok:
            return "fmt length %d" % info.fmt_len
    if info.rate < min_rate:
        return "rate %d below --min-rate %d" % (info.rate, min_rate)
    if info.rate % 2:
        return "odd rate %d" % info.rate
    if info.data_len == 0 or info.data_len % info.block_align:
        return "data length %d is not whole frames" % info.data_len
    if info.frames < MIN_FRAMES:
        return "only %d frames" % info.frames
    return None


def pcm_frames(data: bytes, info: WavInfo):
    """int16 (frames, channels) view of the data chunk."""
    raw = data[info.data_off:info.data_off + info.data_len]
    return np.frombuffer(raw, dtype="<i2").reshape(info.frames, info.channels)


def hf_share(samples, rate: int) -> float:
    """Share of spectral energy above rate/4 (the halved file's Nyquist),
    the maximum over channels, Hann-windowed over the whole sample."""
    x = samples.astype(np.float64)
    n = x.shape[0]
    if n < MIN_FRAMES:
        return 0.0
    w = np.hanning(n)
    freqs = np.fft.rfftfreq(n, 1.0 / rate)
    above = freqs > rate / 4.0
    worst = 0.0
    for c in range(x.shape[1]):
        spec = np.abs(np.fft.rfft(x[:, c] * w)) ** 2
        tot = float(spec.sum())
        if tot > 0.0:
            worst = max(worst, float(spec[above].sum()) / tot)
    return worst


def kaiser_lowpass(taps: int = TAPS, cutoff: float = CUTOFF, beta: float = KAISER_BETA):
    """Linear-phase windowed sinc; `cutoff` in units of the old Nyquist."""
    n = np.arange(taps) - (taps - 1) / 2.0
    fc = cutoff / 2.0                       # cycles per sample
    h = 2.0 * fc * np.sinc(2.0 * fc * n)
    h *= np.kaiser(taps, beta)
    return h / h.sum()


def halve_frames(samples):
    """Filter and decimate int16 (frames, ch) by two.  Length ceil(frames/2),
    group delay removed so an attack stays where it was."""
    frames, channels = samples.shape
    h = kaiser_lowpass()
    out = np.empty(((frames + 1) // 2, channels), dtype="<i2")
    for c in range(channels):
        y = np.convolve(samples[:, c].astype(np.float64), h, mode="same")[::2]
        np.clip(np.rint(y), -32768, 32767, out=y)
        out[:, c] = y.astype("<i2")
    return out


def rewrite_wav(data: bytes, info: WavInfo, halved) -> bytes:
    """The same file with the halved PCM: rate, byte rate, data length and RIFF
    size patched; every other byte kept."""
    pcm = halved.tobytes()
    out = bytearray(data[:info.data_off])
    out += pcm
    out += data[info.data_off + info.data_len:]
    new_rate = info.rate // 2
    struct.pack_into("<I", out, info.fmt_off + 4, new_rate)
    struct.pack_into("<I", out, info.fmt_off + 8, new_rate * info.block_align)
    struct.pack_into("<H", out, info.fmt_off + 12, info.block_align)
    struct.pack_into("<I", out, info.data_off - 4, len(pcm))
    removed = info.data_len - len(pcm)
    struct.pack_into("<I", out, 4, (info.riff_size - removed) & MASK32)
    return bytes(out)


def measure(original, halved, rate: int) -> dict:
    """Offline quality figures for one halved sample: relative band-power
    error below MEASURE_BAND of the new Nyquist (the filter alone, and after
    the 2x linear interpolation OpenAL Soft's default resampler applies when
    the mixer runs at the old rate) and the peak level change.  Power per
    sample is sum|X|^2 / N^2 (Parseval), so signals of different lengths
    compare directly."""
    x = original.astype(np.float64)
    y = halved.astype(np.float64)
    limit = MEASURE_BAND * rate / 4.0
    res = {"band_error": 0.0, "band_error_linear": 0.0, "peak_change": 0.0}
    for c in range(x.shape[1]):
        xo = x[:, c]
        yo = y[:, c]
        n = len(xo)
        fo = np.fft.rfftfreq(n, 1.0 / rate)
        so = np.abs(np.fft.rfft(xo * np.hanning(n))) ** 2 / (n * n)
        eo = float(so[fo <= limit].sum())
        m = len(yo)
        fh = np.fft.rfftfreq(m, 2.0 / rate)
        sh = np.abs(np.fft.rfft(yo * np.hanning(m))) ** 2 / (m * m)
        eh = float(sh[fh <= limit].sum())
        # what the mixer hears: linear interpolation back to the old rate
        up = np.interp(np.arange(n) / 2.0, np.arange(m), yo)
        su = np.abs(np.fft.rfft(up * np.hanning(n))) ** 2 / (n * n)
        eu = float(su[fo <= limit].sum())
        if eo > 0.0:
            res["band_error"] = max(res["band_error"], abs(eh - eo) / eo)
            res["band_error_linear"] = max(res["band_error_linear"], abs(eu - eo) / eo)
        po = float(np.abs(xo).max())
        if po > 0.0:
            res["peak_change"] = max(res["peak_change"], abs(float(np.abs(yo).max()) - po) / po)
    return res


@dataclass
class Decision:
    action: str            # "halved" | "skipped"
    reason: str
    info: WavInfo | None = None
    share: float | None = None
    before: int = 0        # PCM bytes
    after: int = 0
    output: bytes | None = None
    metrics: dict | None = None


def decide(data: bytes, threshold: float = DEFAULT_THRESHOLD,
           min_rate: int = DEFAULT_MIN_RATE, want_measure: bool = False,
           dry_run: bool = False) -> Decision:
    try:
        info = parse_wav(data)
    except WavError as exc:
        return Decision("skipped", str(exc))
    why = wav_skip_reason(info, min_rate)
    if why:
        return Decision("skipped", why, info, before=info.data_len, after=info.data_len)
    frames = pcm_frames(data, info)
    share = hf_share(frames, info.rate)
    if share > threshold:
        return Decision("skipped", "%.2f %% of the energy is above %d Hz" % (
            share * 100.0, info.rate // 4), info, share, info.data_len, info.data_len)
    if dry_run and not want_measure:
        after = ((info.frames + 1) // 2) * info.block_align
        return Decision("halved", "dry run", info, share, info.data_len, after)
    halved = halve_frames(frames)
    out = rewrite_wav(data, info, halved)
    metrics = measure(frames, halved, info.rate) if want_measure else None
    return Decision("halved", "%.2f %% above %d Hz" % (share * 100.0, info.rate // 4),
                    info, share, info.data_len, halved.nbytes, None if dry_run else out, metrics)


# ---------------------------------------------------------------------------
# archive codec (ARCH000)
# ---------------------------------------------------------------------------

ARCHIVE_MAGIC = b"ARCH000"
HEADER_SIZE = 14
TABLE_RECORD = struct.Struct("<IIIII")
CHECKSUM_INIT = 0xABABEB98      # mov esi, 0xababeb98 at RVA 0x563b73
RAW_XOR_CONST = 0xF9524287      # xor eax, 0xf9524287 at RVA 0x59c24b
PIECE_MAX = 0x7FF
BLOCK = 0x400
CHUNK = 0x200
KNOWN_PREFIXES = ("resources/", "resources-dlc3/")


def fold_byte(c: int) -> int:
    if 65 <= c <= 90:
        c += 32
    if c == 92:
        c = 47
    return c


def djb2(path: str) -> int:
    h = 0x1505
    for c in path.encode("utf-8"):
        h = (h * 33 + fold_byte(c)) & MASK32
    return h


def fnv1a(path: str) -> int:
    h = 0x5BB2220E
    for c in path.encode("utf-8"):
        h = ((h ^ fold_byte(c)) * 0x1000193) & MASK32
    return h


def key_of(path: str) -> tuple[int, int]:
    return djb2(path), fnv1a(path)


def mount_checksum(data: bytes) -> int:
    """The mount loop's fold: 0x200-byte chunks into one buffer zeroed once per
    entry, ceil(got/4) dwords each, h = rotr1(h) + dword.  The tail bytes of a
    final partial dword are whatever the previous chunk left there."""
    h = CHECKSUM_INIT
    size = len(data)
    buf = bytearray(CHUNK)
    pos = 0
    remaining = size
    while True:
        want = remaining if remaining < CHUNK else CHUNK
        got = want if pos + want <= size else max(size - pos, 0)
        if got:
            buf[:got] = data[pos:pos + got]
        ndw = (got + 3) >> 2
        if ndw:
            for d in memoryview(buf)[:ndw * 4].cast("I"):
                h = (((h >> 1) | ((h & 1) << 31)) + d) & MASK32
        remaining -= want
        pos += want
        if not (remaining > 0 and got == want):
            break
    return h


def raw_keys(h2: int, ndw: int) -> list[int]:
    k = ((h2 ^ RAW_XOR_CONST) | 1) & MASK32
    keys = [0] * ndw
    for i in range(ndw):
        keys[i] = k
        k ^= (k << 8) & MASK32
        k ^= k >> 9
        k ^= (k << 23) & MASK32
    return keys


def _permute(buf: bytearray, keys: list[int]) -> None:
    for i, k in enumerate(keys):
        sel = k & 0xF
        if sel not in (2, 9, 13):
            continue
        o = i * 4
        b = buf[o:o + 4]
        if sel == 2:
            buf[o:o + 4] = b[::-1]
        elif sel == 9:
            buf[o:o + 4] = bytes((b[1], b[0], b[3], b[2]))
        else:
            buf[o:o + 4] = bytes((b[2], b[3], b[0], b[1]))


def _xor_words(buf: bytes, keys: list[int]) -> bytearray:
    ks = struct.pack("<%dI" % len(keys), *keys)[:len(buf)]
    return bytearray((int.from_bytes(buf, "little") ^ int.from_bytes(ks, "little")).to_bytes(len(buf), "little"))


def raw_decode(raw: bytes, h2: int, size: int) -> bytes:
    if len(raw) % 4:
        raise ValueError("raw payload is not a multiple of 4 bytes")
    ndw = len(raw) // 4
    if not ndw:
        return b""
    keys = raw_keys(h2, ndw)
    buf = _xor_words(raw, keys)
    _permute(buf, keys)
    return bytes(buf[:size])


def raw_encode(data: bytes, h2: int) -> bytes:
    buf = bytearray(data) + b"\0" * ((-len(data)) % 4)
    ndw = len(buf) // 4
    if not ndw:
        return b""
    keys = raw_keys(h2, ndw)
    _permute(buf, keys)
    return bytes(_xor_words(bytes(buf), keys))


class IsaacKeystream:
    """Stored-piece keystream: 256 PCG32 outputs seed ISAAC randinit(TRUE); the
    consumer takes one u32 per 4 bytes and regenerates after 256 words."""

    MULT = 0x5851F42D4C957F2D   # push 0x5851f42d / push 0x4c957f2d at RVA 0x5c2f50
    INC = 0x7F

    def __init__(self, seed: int) -> None:
        seed &= MASK32
        low = ((((seed << 15) ^ seed) << 8) ^ (seed >> 9) ^ seed) & MASK32
        state = (seed << 32) | low
        rsl = [0] * 256
        for i in range(256):
            xs = ((state ^ (state >> 18)) >> 27) & MASK32
            rot = state >> 59
            rsl[i] = ((xs >> rot) | (xs << ((32 - rot) & 31))) & MASK32
            state = (state * self.MULT + self.INC) & 0xFFFFFFFFFFFFFFFF
        self.rsl = rsl
        self.mm = [0] * 256
        self.aa = self.bb = self.cc = 0
        self._randinit()
        self.idx = 0

    @staticmethod
    def _mix(v: list[int]) -> None:
        a, b, c, d, e, f, g, h = v
        a ^= (b << 11) & MASK32; d = (d + a) & MASK32; b = (b + c) & MASK32
        b ^= c >> 2;              e = (e + b) & MASK32; c = (c + d) & MASK32
        c ^= (d << 8) & MASK32;   f = (f + c) & MASK32; d = (d + e) & MASK32
        d ^= e >> 16;             g = (g + d) & MASK32; e = (e + f) & MASK32
        e ^= (f << 10) & MASK32;  h = (h + e) & MASK32; f = (f + g) & MASK32
        f ^= g >> 4;              a = (a + f) & MASK32; g = (g + h) & MASK32
        g ^= (h << 8) & MASK32;   b = (b + g) & MASK32; h = (h + a) & MASK32
        h ^= a >> 9;              c = (c + h) & MASK32; a = (a + b) & MASK32
        v[:] = [a, b, c, d, e, f, g, h]

    def _randinit(self) -> None:
        v = [0x9E3779B9] * 8
        for _ in range(4):
            self._mix(v)
        for src in (self.rsl, self.mm):
            for i in range(0, 256, 8):
                for j in range(8):
                    v[j] = (v[j] + src[i + j]) & MASK32
                self._mix(v)
                self.mm[i:i + 8] = v
        self.isaac()

    def isaac(self) -> None:
        mm, rsl = self.mm, self.rsl
        aa, bb = self.aa, self.bb
        self.cc = (self.cc + 1) & MASK32
        bb = (bb + self.cc) & MASK32
        for i in range(256):
            x = mm[i]
            s = i & 3
            if s == 0:
                aa ^= (aa << 13) & MASK32
            elif s == 1:
                aa ^= aa >> 6
            elif s == 2:
                aa ^= (aa << 2) & MASK32
            else:
                aa ^= aa >> 16
            aa = (aa + mm[(i + 128) & 0xFF]) & MASK32
            y = (mm[(x >> 2) & 0xFF] + aa + bb) & MASK32
            mm[i] = y
            bb = (mm[(y >> 10) & 0xFF] + x) & MASK32
            rsl[i] = bb
        self.aa, self.bb = aa, bb

    def apply(self, data: bytes) -> bytes:
        n = len(data)
        if not n:
            return b""
        words = []
        need = (n + 3) >> 2
        while need:
            take = min(need, 256 - self.idx)
            words.extend(self.rsl[self.idx:self.idx + take])
            self.idx += take
            need -= take
            if self.idx > 0xFF:
                self.isaac()
                self.idx = 0
        ks = struct.pack("<%dI" % len(words), *words)[:n]
        return (int.from_bytes(data, "little") ^ int.from_bytes(ks, "little")).to_bytes(n, "little")


class PieceError(ValueError):
    pass


def iter_pieces(buf, offset: int, size: int):
    pos = offset
    for _ in range((size + BLOCK - 1) // BLOCK):
        if pos + 4 > len(buf):
            raise PieceError("piece header past the end of the archive")
        hdr, = struct.unpack_from("<I", buf, pos)
        ln = hdr & 0x7FFFFFFF
        if ln > PIECE_MAX:
            raise PieceError("piece length %d > 0x7ff" % ln)
        pos += 4
        yield bytes(buf[pos:pos + ln]), hdr >> 31, pos + ln
        pos += ln


def miniz_decode(buf, offset: int, size: int, h2: int, limit: int | None = None) -> tuple[bytes, int]:
    """Decode a version-2 entry (or its first `limit` bytes).  Returns (data,
    raw length; only meaningful when the whole entry was decoded)."""
    out = bytearray()
    d = zlib.decompressobj(-15)
    stored = False
    ks = None
    end = offset
    for payload, fin, end in iter_pieces(buf, offset, size):
        if not stored:
            stored = (fin == 0 and len(payload) == BLOCK)
        if stored:
            if ks is None:
                ks = IsaacKeystream(h2)
            out += ks.apply(payload)
        else:
            out += d.decompress(payload)
        if limit is not None and len(out) >= limit:
            return bytes(out[:limit]), 0
    if len(out) < size:
        raise PieceError("decoded %d bytes, table says %d" % (len(out), size))
    return bytes(out[:size]), end - offset


def _deflate_piece(block: bytes, final: bool, level: int) -> bytes:
    c = zlib.compressobj(level, zlib.DEFLATED, -15, 8, zlib.Z_DEFAULT_STRATEGY)
    return c.compress(block) + c.flush(zlib.Z_FINISH if final else zlib.Z_FULL_FLUSH)


def miniz_encode(data: bytes, h2: int, mode: str = "stored", level: int = 9) -> bytes:
    """Encode a version-2 payload.  stored: memcpy+XOR pieces (what the
    shipped WAV entries mostly use; needs more than 0x400 bytes), deflate:
    per-block raw deflate, auto: the smaller of the two."""
    size = len(data)
    n = (size + BLOCK - 1) // BLOCK
    if n == 0:
        return b""
    blocks = [data[i * BLOCK:(i + 1) * BLOCK] for i in range(n)]
    stored_ok = size > BLOCK
    if mode not in ("stored", "deflate", "auto"):
        raise ValueError("encode mode %r" % mode)
    pieces: list[tuple[bytes, bool]] = []
    if mode in ("deflate", "auto") or not stored_ok:
        pieces = []
        for i, block in enumerate(blocks):
            final = i == n - 1
            piece = _deflate_piece(block, final, level)
            if not final and len(piece) == BLOCK:
                for alt in (level - 1, 1, 0, 6, 3):
                    if alt < 0:
                        continue
                    piece = _deflate_piece(block, final, alt)
                    if len(piece) != BLOCK:
                        break
                if len(piece) == BLOCK:
                    raise PieceError("could not avoid a 0x400-byte deflate piece")
            if len(piece) > PIECE_MAX:
                raise PieceError("deflate piece of %d bytes > 0x7ff" % len(piece))
            pieces.append((piece, final))
    if stored_ok and (mode == "stored" or (mode == "auto" and
                                           sum(4 + len(p) for p, _ in pieces) > 4 * n + size)):
        ks = IsaacKeystream(h2)
        pieces = [(ks.apply(block), i == n - 1) for i, block in enumerate(blocks)]
    out = bytearray()
    for piece, final in pieces:
        out += struct.pack("<I", len(piece) | (0x80000000 if final else 0))
        out += piece
    return bytes(out)


@dataclass
class Entry:
    index: int
    h1: int
    h2: int
    offset: int
    size: int
    x: int

    @property
    def key(self) -> tuple[int, int]:
        return self.h1, self.h2

    @property
    def tag(self) -> str:
        return "%08x-%08x" % (self.h1, self.h2)


class Archive:
    def __init__(self, path: str) -> None:
        self.path = path
        with open(path, "rb") as fh:
            self.buf = fh.read()
        hdr = self.buf[:HEADER_SIZE]
        if len(hdr) < HEADER_SIZE or hdr[:7] != ARCHIVE_MAGIC:
            raise ValueError("%s: not an ARCH000 archive" % path)
        self.version = hdr[7]
        self.table_offset, = struct.unpack_from("<I", hdr, 8)
        self.count, = struct.unpack_from("<H", hdr, 12)
        self.entries = [Entry(i, *TABLE_RECORD.unpack_from(self.buf, self.table_offset + TABLE_RECORD.size * i))
                        for i in range(self.count)]
        self.by_key = {e.key: e for e in self.entries}

    def raw_extent(self, e: Entry) -> tuple[int, int]:
        if self.version == 0:
            return e.offset, e.size + ((-e.size) % 4)
        if self.version == 2:
            end = e.offset
            for _payload, _fin, end in iter_pieces(self.buf, e.offset, e.size):
                pass
            return e.offset, end - e.offset
        raise PieceError("archive version %d is not supported" % self.version)

    def raw(self, e: Entry) -> bytes:
        off, ln = self.raw_extent(e)
        return self.buf[off:off + ln]

    def decode(self, e: Entry) -> bytes:
        if self.version == 0:
            off, ln = self.raw_extent(e)
            return raw_decode(self.buf[off:off + ln], e.h2, e.size)
        if self.version == 2:
            return miniz_decode(self.buf, e.offset, e.size, e.h2)[0]
        raise PieceError("archive version %d is not supported" % self.version)

    def decode_prefix(self, e: Entry, n: int) -> bytes:
        """Cheap sniff: the first n decoded bytes (one piece for version 2)."""
        n = min(n, e.size)
        if self.version == 0:
            off, _ln = self.raw_extent(e)
            ndw = (n + 3) // 4
            return raw_decode(self.buf[off:off + ndw * 4], e.h2, n)
        if self.version == 2:
            return miniz_decode(self.buf, e.offset, e.size, e.h2, limit=n)[0]
        raise PieceError("archive version %d is not supported" % self.version)

    def verify(self, e: Entry) -> bool:
        return mount_checksum(self.decode(e)) == e.x


def write_archive(path: str, version: int, items: list[dict], mode: str = "stored") -> dict:
    """items in table order: {'h1','h2','src': Archive,'entry': Entry} copied
    raw, or {'h1','h2','data': bytes} encoded here.  Deterministic."""
    if len(items) > 0xFFFF:
        raise ValueError("too many entries (%d)" % len(items))
    if version not in (0, 2):
        raise PieceError("cannot write archive version %d" % version)
    records = []
    encoded = 0
    with open(path, "wb") as f:
        f.write(ARCHIVE_MAGIC + bytes([version]) + struct.pack("<IH", 0, len(items)))
        pos = HEADER_SIZE
        for it in items:
            h1, h2 = it["h1"] & MASK32, it["h2"] & MASK32
            if "src" in it:
                e = it["entry"]
                raw = it["src"].raw(e)
                size, x = e.size, e.x
            else:
                data = it["data"]
                raw = raw_encode(data, h2) if version == 0 else miniz_encode(data, h2, mode)
                size, x = len(data), mount_checksum(data)
                encoded += 1
            f.write(raw)
            records.append((h1, h2, pos, size, x))
            pos += len(raw)
        for r in records:
            f.write(TABLE_RECORD.pack(*r))
        f.seek(8)
        f.write(struct.pack("<I", pos))
    return {"entries": len(items), "encoded": encoded, "size": pos + TABLE_RECORD.size * len(items)}


# ---------------------------------------------------------------------------
# validation sidecar ("<archive>.receipt")
# ---------------------------------------------------------------------------
#
# FileManager::LoadArchiveFile (sub_00563880) decodes and checksums every
# entry of every archive at mount.  The Vita port skips that for entries whose
# index checksum matches the executable's baked table and, for afterbirth.a,
# for a file whose pinned identity its runtime receipt has proven (measured on
# the device: afterbirth.a validated the original way costs 63-66 s per boot).
# Rewritten entries have new checksums, so a port built with
# ISAAC_VITA_ARCHIVE_VALIDATION_SIDECAR=ON reads this 256-byte record next to
# the archive, checks the archive's identity against it and skips the loop.
# The layout is mirrored byte for byte in recomp/runtime/host_vita_archive_sidecar.c.

SIDECAR_MAGIC = b"ISAAC-A-SIDECAR1"
SIDECAR_SIZE = 256
SIDECAR_VERSION = 1
SIDECAR_SAMPLE_COUNT = 64
SIDECAR_SAMPLE_SIZE = 4096
SIDECAR_NAME_CAPACITY = 40
SIDECAR_SUFFIX = ".receipt"
SIDECAR_SAMPLE_DOMAIN = b"isaac-vita-sidecar-sample-v1"
SIDECAR_NUMBERS = struct.Struct("<IQIIII")     # mode, size, index offset, entries, count, sample size
SIDECAR_WINDOW = struct.Struct("<QI")          # offset, length


class SidecarError(ValueError):
    pass


def sidecar_windows(index_offset: int, count: int = SIDECAR_SAMPLE_COUNT, size: int = SIDECAR_SAMPLE_SIZE):
    """(offset, length) windows of the entry region [0, index_offset): the
    whole region in consecutive windows when it is small, else `count`
    windows spread from 0 to index_offset - size."""
    if index_offset <= count * size:
        off = 0
        while off < index_offset:
            yield off, min(size, index_offset - off)
            off += size
    else:
        for i in range(count):
            yield ((index_offset - size) * i) // (count - 1), size


def sidecar_sample_sha256(buf, mode: int, size: int, index_offset: int, entries: int) -> bytes:
    h = hashlib.sha256(SIDECAR_SAMPLE_DOMAIN)
    h.update(SIDECAR_NUMBERS.pack(mode, size, index_offset, entries, SIDECAR_SAMPLE_COUNT, SIDECAR_SAMPLE_SIZE))
    for off, ln in sidecar_windows(index_offset):
        h.update(SIDECAR_WINDOW.pack(off, ln))
        h.update(buf[off:off + ln])
    return h.digest()


def sidecar_bytes(buf, base_name: str) -> bytes:
    """The sidecar record for an ARCH000 archive image (bytes) named
    base_name ("afterbirthp.a")."""
    if len(buf) < HEADER_SIZE or buf[:7] != ARCHIVE_MAGIC:
        raise SidecarError("not an ARCH000 archive")
    mode = buf[7]
    index_offset, = struct.unpack_from("<I", buf, 8)
    entries, = struct.unpack_from("<H", buf, 12)
    if entries == 0 or index_offset < HEADER_SIZE:
        raise SidecarError("empty archive")
    if index_offset + entries * TABLE_RECORD.size != len(buf):
        raise SidecarError("archive size %d is not index offset %d + %d entries" % (len(buf), index_offset, entries))
    name = base_name.encode("ascii")
    if not name or len(name) >= SIDECAR_NAME_CAPACITY or b"/" in name or b"\\" in name or b":" in name \
            or name[:1] == b"." or any(c < 0x20 for c in name):
        raise SidecarError("archive name %r does not fit the sidecar" % base_name)
    rec = bytearray(SIDECAR_SIZE)
    rec[0:16] = SIDECAR_MAGIC
    struct.pack_into("<IIQIIIII", rec, 16, SIDECAR_VERSION, SIDECAR_SIZE, len(buf), index_offset, entries, mode,
                     SIDECAR_SAMPLE_COUNT, SIDECAR_SAMPLE_SIZE)
    rec[64:96] = hashlib.sha256(buf[index_offset:index_offset + entries * TABLE_RECORD.size]).digest()
    rec[96:128] = sidecar_sample_sha256(buf, mode, len(buf), index_offset, entries)
    rec[128:128 + len(name)] = name
    rec[224:256] = hashlib.sha256(rec[:224]).digest()
    return bytes(rec)


def sidecar_check(buf, sidecar: bytes, base_name: str) -> str | None:
    """Why the port would NOT trust `sidecar` for the archive image `buf`
    named base_name, or None when it would (mirrors the runtime's stages)."""
    if len(sidecar) != SIDECAR_SIZE:
        return "sidecar-identity: %d bytes" % len(sidecar)
    if sidecar[:16] != SIDECAR_MAGIC or struct.unpack_from("<II", sidecar, 16) != (SIDECAR_VERSION, SIDECAR_SIZE):
        return "sidecar-decode: magic/version"
    if any(sidecar[52:64]) or any(sidecar[168:224]):
        return "sidecar-decode: reserved bytes"
    if hashlib.sha256(sidecar[:224]).digest() != sidecar[224:256]:
        return "sidecar-decode: digest"
    size, index_offset, entries, mode, count, sample = struct.unpack_from("<QIIIII", sidecar, 24)
    if (count, sample) != (SIDECAR_SAMPLE_COUNT, SIDECAR_SAMPLE_SIZE):
        return "sidecar-decode: sample scheme"
    if entries == 0 or entries > 0xFFFF or index_offset < HEADER_SIZE or mode > 0xFF \
            or index_offset + entries * TABLE_RECORD.size != size:
        return "sidecar-decode: geometry"
    name = sidecar[128:168]
    if name[-1] != 0 or name.split(b"\0", 1)[0] != base_name.encode("ascii", "replace"):
        return "sidecar-name"
    if len(buf) != size:
        return "archive-identity: %d bytes, sidecar says %d" % (len(buf), size)
    hdr_index, = struct.unpack_from("<I", buf, 8)
    hdr_entries, = struct.unpack_from("<H", buf, 12)
    if buf[:7] != ARCHIVE_MAGIC or buf[7] != mode or hdr_index != index_offset or hdr_entries != entries:
        return "archive-header"
    if hashlib.sha256(buf[index_offset:index_offset + entries * TABLE_RECORD.size]).digest() != sidecar[64:96]:
        return "archive-index-hash"
    if sidecar_sample_sha256(buf, mode, size, index_offset, entries) != sidecar[96:128]:
        return "archive-sample-hash"
    return None


def write_sidecar(archive_path: str, sidecar_path: str | None = None, verify_entries: bool = True) -> dict:
    """Verify every entry of the archive at archive_path against its index
    checksum (the same test the engine applies at mount), then write the
    sidecar (default: next to the archive) and read it back through
    sidecar_check.  Raises SidecarError when an entry fails."""
    out = sidecar_path or archive_path + SIDECAR_SUFFIX
    if os.path.abspath(out) == os.path.abspath(archive_path):
        raise SystemExit("refusing to overwrite the archive with its sidecar")
    base = os.path.basename(archive_path)
    a = Archive(archive_path)
    bad = []
    if verify_entries:
        if a.version not in (0, 2):
            raise SidecarError("%s: archive version %d cannot be verified here" % (base, a.version))
        for e in a.entries:
            try:
                ok = a.verify(e)
            except (PieceError, zlib.error):
                ok = False
            if not ok:
                bad.append(e.tag)
        if bad:
            raise SidecarError("%s: %d entries fail their index checksum (%s...); no sidecar written" % (
                base, len(bad), bad[0]))
    rec = sidecar_bytes(a.buf, base)
    with open(out, "wb") as fh:
        fh.write(rec)
    with open(out, "rb") as fh:
        back = fh.read()
    why = sidecar_check(a.buf, back, base)
    if why is not None:
        raise SidecarError("%s: written sidecar does not verify (%s)" % (base, why))
    return {"path": out, "entries": a.count, "size": len(a.buf), "verified": verify_entries}


# ---------------------------------------------------------------------------
# catalogue
# ---------------------------------------------------------------------------

def catalogue_paths(xml: bytes) -> list[str]:
    """'<root><path>' for every <sample path=...> of a sounds.xml, comments
    dropped, document order, case-insensitive duplicates removed."""
    text = re.sub(rb"<!--.*?-->", b"", xml, flags=re.S).decode("utf-8", "replace")
    m = re.search(r'<sounds\b[^>]*\broot="([^"]*)"', text)
    root = m.group(1) if m else "sfx/"
    out, seen = [], set()
    for p in re.findall(r'<sample\b[^>]*\bpath="([^"]*)"', text):
        full = (root + p).replace("\\", "/")
        k = full.lower()
        if k not in seen:
            seen.add(k)
            out.append(full)
    return out


def catalogue_index(xml: bytes) -> dict[int, list[str]]:
    """sound id -> ['<root><path>', ...] for every <sound id=...> of a
    sounds.xml (the ids the game log prints as 'Preload sound N')."""
    text = re.sub(rb"<!--.*?-->", b"", xml, flags=re.S).decode("utf-8", "replace")
    m = re.search(r'<sounds\b[^>]*\broot="([^"]*)"', text)
    root = m.group(1) if m else "sfx/"
    out = {}
    for sm in re.finditer(r"<sound\b([^>]*)>(.*?)</sound>", text, flags=re.S):
        idm = re.search(r'\bid="(\d+)"', sm.group(1))
        if idm:
            out[int(idm.group(1))] = [(root + p).replace("\\", "/") for p in
                                      re.findall(r'<sample\b[^>]*\bpath="([^"]*)"', sm.group(2))]
    return out


def merge_catalogue_index(ids: dict, xml: bytes, override: bool) -> None:
    """Fold one sounds.xml into ids.  The DLC catalogue (resources-dlc3/) is
    the one the engine ends up with, so it overrides; older ones only fill."""
    for i, paths in catalogue_index(xml).items():
        if override or i not in ids:
            ids[i] = paths


GAME_LOG_PATTERNS = (re.compile(r"Preload sound (\d+)\."), re.compile(r"Sound (\d+) was not preloaded"))


def game_log_sound_ids(path: str) -> list[int]:
    """Sound ids a game log.txt shows being loaded: 'Preload sound N.' (boot and
    entity preloads) and '[warn] Sound N was not preloaded.' (loaded on first
    play).  First-seen order, no duplicates."""
    with open(path, encoding="utf-8", errors="replace") as fh:
        text = fh.read()
    seen = []
    for pat in GAME_LOG_PATTERNS:
        for m in pat.finditer(text):
            i = int(m.group(1))
            if i not in seen:
                seen.append(i)
    return seen


def find_catalogues(archive: Archive, prefixes) -> list[tuple[str, bytes]]:
    """(prefix, sounds.xml bytes) for every prefix under which the archive
    carries a sounds.xml."""
    found = []
    for prefix in prefixes:
        e = archive.by_key.get(key_of(prefix + "sounds.xml"))
        if e is not None:
            found.append((prefix, archive.decode(e)))
    return found


def is_wav_head(head: bytes) -> bool:
    return len(head) >= 12 and head[:4] == b"RIFF" and head[8:12] == b"WAVE"


# ---------------------------------------------------------------------------
# drivers
# ---------------------------------------------------------------------------

class Report:
    def __init__(self, args) -> None:
        self.args = args
        self.files = []
        self.halved = self.skipped = 0
        self.before = self.after = 0
        self.notes = []
        self.ids = {}            # sound id -> sample paths, filled by the drivers
        self.sidecars = []       # written "<archive>.receipt" paths
        self.excluded = []       # archives left untouched by policy (afterbirth.a)
        self.t0 = time.time()

    def add(self, name: str, container: str, d: Decision) -> None:
        row = {"file": name, "in": container, "action": d.action, "reason": d.reason,
               "pcm_before": d.before, "pcm_after": d.after}
        if d.info:
            row.update(rate=d.info.rate, channels=d.info.channels, bits=d.info.bits,
                       frames=d.info.frames,
                       chunks=[c[0].decode("latin-1") for c in d.info.chunks])
        if d.share is not None:
            row["share_above_new_nyquist"] = round(d.share, 6)
        if d.metrics:
            row["metrics"] = {k: round(v, 6) for k, v in d.metrics.items()}
        self.files.append(row)
        if d.action == "halved":
            self.halved += 1
        else:
            self.skipped += 1
        self.before += d.before
        self.after += d.after
        if self.args.verbose:
            print("  %-7s %s (%s)" % (d.action, name, d.reason))

    def note(self, text: str) -> None:
        self.notes.append(text)
        print(text)

    def summary(self) -> dict:
        s = {"halved": self.halved, "skipped": self.skipped,
             "pcm_before": self.before, "pcm_after": self.after,
             "seconds": round(time.time() - self.t0, 1),
             "threshold": self.args.threshold, "min_rate": self.args.min_rate,
             "dry_run": bool(self.args.dry_run),
             "sidecars": list(self.sidecars), "excluded": list(self.excluded)}
        halved = [f["metrics"] for f in self.files if f.get("metrics")]
        if halved:
            for k in ("band_error", "band_error_linear", "peak_change"):
                vals = sorted(m[k] for m in halved)
                s[k + "_median"] = round(vals[len(vals) // 2], 6)
                s[k + "_max"] = round(vals[-1], 6)
        return s

    def game_log_subset(self, log_path: str) -> dict:
        """What this rewrite does for the sounds one game log.txt loaded: the
        PCM the engine kept resident for them, before and after."""
        rows = {}
        for f in self.files:
            rows.setdefault(f["file"].lower(), f)
        sound_ids = game_log_sound_ids(log_path)
        files, halved, kept, unknown_ids, missing = set(), 0, 0, 0, 0
        before = after = 0
        for i in sound_ids:
            paths = self.ids.get(i)
            if paths is None:
                unknown_ids += 1
                continue
            for p in paths:
                k = p.lower()
                if k in files:
                    continue
                files.add(k)
                r = rows.get(k)
                if r is None:
                    missing += 1
                elif r["action"] == "halved":
                    halved += 1
                    before += r["pcm_before"]
                    after += r["pcm_after"]
                else:
                    kept += 1
                    before += r["pcm_before"]
                    after += r["pcm_after"]
        return {"log": log_path, "sound_ids": len(sound_ids), "unknown_ids": unknown_ids,
                "files": len(files), "halved": halved, "kept": kept, "missing": missing,
                "pcm_before": before, "pcm_after": after}

    def finish(self) -> None:
        s = self.summary()
        logs = getattr(self.args, "game_log", None) or ()
        if logs and self.ids:
            s["game_logs"] = []
            for log_path in logs:
                g = self.game_log_subset(log_path)
                s["game_logs"].append(g)
                print("%s: %d sound ids -> %d files (%d halved, %d kept, %d not in these archives, %d ids not in sounds.xml); "
                      "resident PCM %.1f MiB -> %.1f MiB (%.1f MiB less)" % (
                          os.path.basename(log_path), g["sound_ids"], g["files"], g["halved"], g["kept"],
                          g["missing"], g["unknown_ids"], g["pcm_before"] / 1048576.0,
                          g["pcm_after"] / 1048576.0, (g["pcm_before"] - g["pcm_after"]) / 1048576.0))
        print("%s%d halved, %d skipped, PCM %.1f MiB -> %.1f MiB (%.1f MiB less), %.1f s" % (
            "DRY RUN: " if self.args.dry_run else "", s["halved"], s["skipped"],
            s["pcm_before"] / 1048576.0, s["pcm_after"] / 1048576.0,
            (s["pcm_before"] - s["pcm_after"]) / 1048576.0, s["seconds"]))
        if "band_error_median" in s:
            print("quality (halved files): band<%.0f%% new Nyquist energy error median %.2f %% max %.2f %%; "
                  "after 2x linear interpolation median %.2f %% max %.2f %%; peak change median %.2f %% max %.2f %%" % (
                      MEASURE_BAND * 100, s["band_error_median"] * 100, s["band_error_max"] * 100,
                      s["band_error_linear_median"] * 100, s["band_error_linear_max"] * 100,
                      s["peak_change_median"] * 100, s["peak_change_max"] * 100))
        if self.halved and not self.args.dry_run and not getattr(self.args, "sidecar", True):
            print("note: no .receipt sidecars written; rewritten entries are validated the original way "
                  "on every boot (about 2.5 MiB/s on the Vita)")
        if self.sidecars:
            print("note: the .receipt sidecars only matter to an eboot built with "
                  "ISAAC_VITA_ARCHIVE_VALIDATION_SIDECAR=ON; other builds ignore them and validate the "
                  "rewritten entries on every boot")
        if self.args.report:
            with open(self.args.report, "w", encoding="utf-8") as fh:
                json.dump({"summary": s, "notes": self.notes, "files": self.files}, fh, indent=1)
            print("report: %s" % self.args.report)


def _refuse_in_place(src: str, dst: str) -> None:
    a = os.path.abspath(src).lower().rstrip("\\/")
    b = os.path.abspath(dst).lower().rstrip("\\/")
    if a == b or b.startswith(a + os.sep) or b.startswith(a + "/"):
        raise SystemExit("refusing to write into the input (%s -> %s): use a separate output" % (src, dst))


def _decide(data: bytes, args) -> Decision:
    return decide(data, args.threshold, args.min_rate, args.measure, args.dry_run)


def cmd_wav(args) -> int:
    rep = Report(args)
    src, dst = args.input, args.output
    if os.path.isdir(src):
        _refuse_in_place(src, dst)
        for root, _dirs, files in os.walk(src):
            for name in sorted(files):
                if not name.lower().endswith(".wav"):
                    continue
                path = os.path.join(root, name)
                rel = os.path.relpath(path, src)
                with open(path, "rb") as fh:
                    data = fh.read()
                d = _decide(data, args)
                rep.add(rel.replace("\\", "/"), src, d)
                if not args.dry_run:
                    out = os.path.join(dst, rel)
                    os.makedirs(os.path.dirname(out), exist_ok=True)
                    with open(out, "wb") as fh:
                        fh.write(d.output if d.output is not None else data)
    else:
        if os.path.abspath(src) == os.path.abspath(dst):
            raise SystemExit("refusing to overwrite the input file")
        with open(src, "rb") as fh:
            data = fh.read()
        d = _decide(data, args)
        rep.add(os.path.basename(src), os.path.dirname(src) or ".", d)
        if not args.dry_run:
            os.makedirs(os.path.dirname(os.path.abspath(dst)), exist_ok=True)
            with open(dst, "wb") as fh:
                fh.write(d.output if d.output is not None else data)
    rep.finish()
    return 0


def load_extra_catalogues(paths, prefixes, ids: dict | None = None) -> list[str]:
    out = []
    for p in paths or ():
        with open(p, "rb") as fh:
            head = fh.read(HEADER_SIZE)
        if head[:7] == ARCHIVE_MAGIC:
            a = Archive(p)
            for prefix, xml in find_catalogues(a, prefixes):
                out += catalogue_paths(xml)
                if ids is not None:
                    merge_catalogue_index(ids, xml, prefix == "resources-dlc3/")
        else:
            with open(p, "rb") as fh:
                xml = fh.read()
            out += catalogue_paths(xml)
            if ids is not None:
                merge_catalogue_index(ids, xml, False)
    return out


def plan_archive(a: Archive, paths: list[str], prefixes, rep: Report, name: str, args) -> tuple[list[dict], int]:
    """Decide every catalogued WAV of one archive.  Returns (items for
    write_archive, halved count)."""
    wanted = {}
    for prefix in prefixes:
        hits = 0
        for p in paths:
            k = key_of(prefix + p)
            if k in a.by_key:
                wanted.setdefault(k, p)
                hits += 1
        if hits:
            rep.note("%s: %d catalogued samples under key prefix %r" % (name, hits, prefix))
    outside = 0
    for e in a.entries:
        if e.key in wanted:
            continue
        try:
            if is_wav_head(a.decode_prefix(e, 12)):
                outside += 1
        except (PieceError, zlib.error):
            pass
    if outside:
        rep.note("%s: %d WAV entries are not in any sounds.xml; left untouched" % (name, outside))
    items = []
    halved = 0
    for e in a.entries:
        if e.key not in wanted:
            items.append({"h1": e.h1, "h2": e.h2, "src": a, "entry": e})
            continue
        data = a.decode(e)
        d = _decide(data, args)
        rep.add(wanted[e.key], name, d)
        if d.action == "halved":
            halved += 1
        if d.action == "halved" and d.output is not None:
            items.append({"h1": e.h1, "h2": e.h2, "data": d.output})
        else:
            items.append({"h1": e.h1, "h2": e.h2, "src": a, "entry": e})
    return items, halved


def process_archive(in_path: str, out_path: str | None, paths: list[str], prefixes, rep: Report, args) -> int:
    name = os.path.basename(in_path)
    a = Archive(in_path)
    if a.version not in (0, 2):
        rep.note("%s: archive version %d (LZW) cannot be re-encoded; left untouched" % (name, a.version))
        return 0
    items, halved = plan_archive(a, paths, prefixes, rep, name, args)
    if halved and not args.dry_run and out_path:
        os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
        r = write_archive(out_path, a.version, items, args.encode)
        rep.note("%s: wrote %s (%d entries, %d re-encoded, %d bytes; was %d)" % (
            name, out_path, r["entries"], r["encoded"], r["size"], len(a.buf)))
        if getattr(args, "sidecar", True):
            t0 = time.time()
            s = write_sidecar(out_path)
            rep.sidecars.append(s["path"])
            rep.note("%s: verified all %d entries of the written archive, wrote %s (%.1f s)" % (
                name, s["entries"], s["path"], time.time() - t0))
    return halved


AFTERBIRTH_NAME = "afterbirth.a"
AFTERBIRTH_NOTE = (
    "%s: left untouched. The port's trusted validation receipt is pinned to the shipped "
    "file (size 145,319,513, 2,647 entries); a rewritten afterbirth.a is validated the "
    "original way on every boot (63-66 s, measured on the Vita for the shipped file validated "
    "entry by entry, same entry count) unless the eboot was built "
    "with ISAAC_VITA_ARCHIVE_VALIDATION_SIDECAR=ON. Pass --include-afterbirth for such a build.")


def cmd_archive(args) -> int:
    rep = Report(args)
    if os.path.abspath(args.input) == os.path.abspath(args.output):
        raise SystemExit("refusing to overwrite the input archive")
    prefixes = tuple(args.prefix or ()) + KNOWN_PREFIXES
    a = Archive(args.input)
    paths = []
    for prefix, xml in find_catalogues(a, prefixes):
        paths += catalogue_paths(xml)
        merge_catalogue_index(rep.ids, xml, prefix == "resources-dlc3/")
    paths += load_extra_catalogues(args.catalogue, prefixes, rep.ids)
    if not paths:
        raise SystemExit("no sounds.xml in %s; pass --catalogue sounds.xml or the archive that carries it" % args.input)
    if os.path.basename(args.input).lower() == AFTERBIRTH_NAME and not args.include_afterbirth:
        raise SystemExit(AFTERBIRTH_NOTE % os.path.basename(args.input))
    process_archive(args.input, args.output, paths, prefixes, rep, args)
    rep.finish()
    return 0


def cmd_receipt(args) -> int:
    """Write the validation sidecar for an already rewritten archive."""
    if args.output and os.path.abspath(args.output) == os.path.abspath(args.input):
        raise SystemExit("refusing to overwrite the archive")
    t0 = time.time()
    s = write_sidecar(args.input, args.output, verify_entries=not args.no_verify)
    print("%s: %s%d entries, %d bytes; wrote %s (%.1f s)" % (
        os.path.basename(args.input), "verified " if s["verified"] else "", s["entries"], s["size"],
        s["path"], time.time() - t0))
    return 0


def cmd_tree(args) -> int:
    rep = Report(args)
    root = args.input
    packed = os.path.join(root, "packed")
    if not os.path.isdir(packed) and os.path.isdir(os.path.join(root, "resources", "packed")):
        root = os.path.join(root, "resources")
        packed = os.path.join(root, "packed")
    if not os.path.isdir(packed):
        raise SystemExit("%s has no packed/ directory of archives" % args.input)
    _refuse_in_place(root, args.output)
    prefixes = tuple(args.prefix or ()) + KNOWN_PREFIXES
    archives = sorted(n for n in os.listdir(packed) if n.lower().endswith(".a"))
    paths = []
    opened = {}
    for n in archives:
        p = os.path.join(packed, n)
        try:
            a = Archive(p)
        except ValueError as exc:
            rep.note("%s: %s; skipped" % (n, exc))
            continue
        opened[n] = a
        if a.version in (0, 2):
            for prefix, xml in find_catalogues(a, prefixes):
                cat = catalogue_paths(xml)
                rep.note("%s: sounds.xml under %r names %d samples" % (n, prefix, len(cat)))
                paths += cat
                merge_catalogue_index(rep.ids, xml, prefix == "resources-dlc3/")
    paths += load_extra_catalogues(args.catalogue, prefixes, rep.ids)
    if not paths:
        raise SystemExit("no sounds.xml found in %s" % packed)
    seen = set()
    paths = [p for p in paths if not (p.lower() in seen or seen.add(p.lower()))]
    rep.note("catalogue: %d unique sample paths" % len(paths))
    rewritten = []
    for n in archives:
        a = opened.get(n)
        if a is None:
            continue
        del opened[n]
        out = os.path.join(args.output, "packed", n)
        if n.lower() == AFTERBIRTH_NAME and not args.include_afterbirth:
            rep.note(AFTERBIRTH_NOTE % n)
            rep.excluded.append(n)
            if args.copy_unchanged and not args.dry_run:
                os.makedirs(os.path.dirname(out), exist_ok=True)
                with open(os.path.join(packed, n), "rb") as src, open(out, "wb") as dst:
                    dst.write(src.read())
            continue
        halved = process_archive(os.path.join(packed, n), out, paths, prefixes, rep, args)
        if halved and not args.dry_run:
            rewritten.append(n)
        elif args.copy_unchanged and not args.dry_run:
            os.makedirs(os.path.dirname(out), exist_ok=True)
            with open(os.path.join(packed, n), "rb") as src, open(out, "wb") as dst:
                dst.write(src.read())
    if args.copy_unchanged and not args.dry_run:
        for dirpath, _dirs, files in os.walk(root):
            rel = os.path.relpath(dirpath, root)
            if rel.split(os.sep)[0] == "packed":
                continue
            for name in files:
                src = os.path.join(dirpath, name)
                dst = os.path.join(args.output, rel, name)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                with open(src, "rb") as s, open(dst, "wb") as d:
                    d.write(s.read())
    if rewritten:
        rep.note("rewritten archives: %s -> copy them%s over resources/packed/ on the console" % (
            ", ".join(rewritten), " and their .receipt sidecars" if rep.sidecars else ""))
    rep.finish()
    return 0


def build_parser() -> argparse.ArgumentParser:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="command", required=True)

    def common(p):
        p.add_argument("--threshold", type=float, default=DEFAULT_THRESHOLD,
                       help="maximum share of energy above the new Nyquist (default %g)" % DEFAULT_THRESHOLD)
        p.add_argument("--min-rate", type=int, default=DEFAULT_MIN_RATE,
                       help="leave files below this rate alone (default %d)" % DEFAULT_MIN_RATE)
        p.add_argument("--dry-run", action="store_true", help="decide and report, write nothing")
        p.add_argument("--measure", action="store_true", help="add offline quality figures per halved file")
        p.add_argument("--report", help="write per-file decisions as JSON")
        p.add_argument("-v", "--verbose", action="store_true")

    p = sub.add_parser("wav", help="halve a WAV file or every *.wav under a directory")
    p.add_argument("input")
    p.add_argument("output")
    common(p)
    p.set_defaults(func=cmd_wav)

    def archive_common(p):
        p.add_argument("--catalogue", action="append", help="extra sounds.xml (file or archive carrying it)")
        p.add_argument("--prefix", action="append", help="extra key prefix to try (default: resources/, resources-dlc3/)")
        p.add_argument("--encode", choices=("stored", "deflate", "auto"), default="stored",
                       help="piece encoding for rewritten entries (default stored: memcpy+XOR, cheapest to load)")
        p.add_argument("--game-log", action="append", metavar="LOG.TXT",
                       help="a game log.txt; report the resident PCM before/after for the sounds it loaded")
        p.add_argument("--include-afterbirth", action="store_true",
                       help="also rewrite afterbirth.a (only for an eboot with ISAAC_VITA_ARCHIVE_VALIDATION_SIDECAR; "
                            "otherwise every boot validates it the original way, 63-66 s measured on "
                            "the shipped file as the proxy)")
        p.add_argument("--no-sidecar", dest="sidecar", action="store_false",
                       help="do not verify the written archive and write its .receipt sidecar")

    p = sub.add_parser("archive", help="halve the catalogued WAV entries of one ARCH000 archive")
    p.add_argument("input")
    p.add_argument("output")
    archive_common(p)
    common(p)
    p.set_defaults(func=cmd_archive)

    p = sub.add_parser("tree", help="halve every catalogued WAV in RESOURCES_COPY/packed/*.a into OUT/packed/")
    p.add_argument("input", help="a copy of the game's resources directory (or its parent)")
    p.add_argument("output")
    archive_common(p)
    p.add_argument("--copy-unchanged", action="store_true",
                   help="also copy untouched archives and every non-packed file, for a complete tree")
    common(p)
    p.set_defaults(func=cmd_tree)

    p = sub.add_parser("receipt", help="verify every entry of a rewritten archive and write its .receipt sidecar")
    p.add_argument("input", help="an ARCH000 archive")
    p.add_argument("output", nargs="?", help="sidecar path (default: <archive>.receipt next to it)")
    p.add_argument("--no-verify", action="store_true", help="skip the per-entry checksum pass (not recommended)")
    p.set_defaults(func=cmd_receipt)
    return ap


def main(argv=None) -> int:
    args = build_parser().parse_args(argv)
    if np is None:
        raise SystemExit("numpy is required (pip install numpy)")
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
