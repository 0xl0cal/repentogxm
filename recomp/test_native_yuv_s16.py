#!/usr/bin/env python3
"""Exactness proof for the int16 NEON row of host_vita_native_yuv.c.

The seam's fast row (vita_ny_row16) keeps every intermediate of libyuv's C
YuvPixel in int16 lanes:

    y1 = ((y * 0x0101) * yg) >> 16          u16 x u16 -> u32, narrowed by 16
    b  = sat16((y1 + bb) - u * ub)          vqsubq_s16
    g  = sat16(((y1 + bg) - u * ug) - v * vg)   vmlsq_s16 then vqsubq_s16
    r  = sat16((y1 + br) - v * vr)          vqsubq_s16
    out = clamp255(x >> 6)                  vshrq_n_s16 + vqmovun_s16

and libyuv's reference is

    b  = clamp255((y1 - u * ub + bb) >> 6)  (32-bit, no saturation)

A saturated int16 is >= 32767 or <= -32768, and (32767 >> 6) = 511 clamps to
255 exactly like the unsaturated value would, (-32768 >> 6) = -512 clamps to 0
the same way; so the only conditions the guard vita_ny_consts_fit_s16 has to
check are the ones the model asserts below: y1 fits, y1 + bias fits, the chroma
products fit, and g's first (non-saturating) step fits.  This script evaluates
both frozen tables over all 256^3 (y, u, v) inputs and fails if the int16 model
differs from the reference anywhere, or if a guard condition fails for a table
the seam would take the fast path on.

Run:  python recomp/test_native_yuv_s16.py   (needs numpy)
"""
from __future__ import annotations

import pathlib
import re
import sys

import numpy as np

HERE = pathlib.Path(__file__).resolve().parent

# (ub, ug, vg, vr, bb, bg, br, yg) as the frozen PE stores them; the RVAs and
# values are pinned against the PE by test_native_theora_sites.py.
TABLES = {
    "YVU 0x73d260 (RGB entry, planes swapped)": (-102, 52, 25, -128, -14216, 8696, -17544, 18997),
    "YUV 0x73d340 (BGR entry, planes straight)": (-128, 25, 52, -102, -17544, 8696, -14216, 18997),
}


def fits16(x) -> bool:
    return int(np.min(x)) >= -32768 and int(np.max(x)) <= 32767


def guard(ub, ug, vg, vr, bb, bg, br, yg) -> bool:
    """Mirror of vita_ny_consts_fit_s16 in host_vita_native_yuv.c.  ub, vg and
    vr take no part: any int8 coefficient times 255 fits int16, and their sums
    only feed saturating subtracts."""
    del ub, vg, vr
    if yg > 32767:
        return False
    y1max = (65535 * yg) >> 16
    if y1max + bb > 32767 or y1max + bg > 32767 or y1max + br > 32767:
        return False
    lo = bg - (255 * ug if ug > 0 else 0)
    hi = y1max + bg - (255 * ug if ug < 0 else 0)
    if lo < -32768 or hi > 32767:
        return False
    return True


def prove(name: str, consts) -> None:
    ub, ug, vg, vr, bb, bg, br, yg = consts
    y = np.arange(256, dtype=np.int64)[:, None, None]
    u = np.arange(256, dtype=np.int64)[None, :, None]
    v = np.arange(256, dtype=np.int64)[None, None, :]

    def clamp(x):
        return np.clip(x, 0, 255)

    def sat16(x):
        return np.clip(x, -32768, 32767)

    # libyuv C reference (32-bit, unsigned wrap on the y1 product as in C).
    y1 = ((y * 0x0101 * yg) & 0xFFFFFFFF) >> 16
    ref_b = clamp((y1 - u * ub + bb) >> 6)
    ref_g = clamp((y1 - u * ug - v * vg + bg) >> 6)
    ref_r = clamp((y1 - v * vr + br) >> 6)

    # int16 model: the guard's conditions must hold for the fast path.
    assert guard(*consts), f"{name}: guard rejects the frozen table"
    assert fits16(y1), name
    assert fits16(y1 + bb) and fits16(y1 + bg) and fits16(y1 + br), name
    assert fits16(u * ub) and fits16(u * ug) and fits16(v * vg) and fits16(v * vr), name
    g_first = (y1 + bg) - u * ug
    assert fits16(g_first), name
    b16 = sat16((y1 + bb) - u * ub)
    g16 = sat16(g_first - v * vg)
    r16 = sat16((y1 + br) - v * vr)
    out_b = clamp(b16 >> 6)
    out_g = clamp(g16 >> 6)
    out_r = clamp(r16 >> 6)
    saturations = int((((y1 + bb) - u * ub) != b16).sum()
                      + ((g_first - v * vg) != g16).sum()
                      + (((y1 + br) - v * vr) != r16).sum())
    exact = bool((out_b == ref_b).all() and (out_g == ref_g).all() and (out_r == ref_r).all())
    print(f"{name}: exact={exact} saturations={saturations} of {3 * 256 ** 3} "
          f"y1max={int(y1.max())} g_first=[{int(g_first.min())}, {int(g_first.max())}]")
    assert exact, f"{name}: int16 model differs from the reference"


def check_guard_rejects() -> None:
    # A table the fast path must refuse: yg large enough that y1 + bg overflows.
    assert not guard(-102, 52, 25, -128, -14216, 8696, -17544, 40000)
    # bg so large that y1 + bg overflows int16.
    assert not guard(-102, 52, 25, -128, -14216, 20000, -17544, 18997)
    # ug negative and large so g's first step exceeds int16 at the top.
    assert not guard(-102, -128, 25, -128, -14216, 8696, -17544, 18997)
    print("guard rejects the three constructed overflow tables")


def check_source_mirror() -> None:
    """The C guard must spell the same four conditions this model uses."""
    src = (HERE / "runtime" / "host_vita_native_yuv.c").read_text(encoding="utf-8")
    start = src.index("static int vita_ny_consts_fit_s16(")
    body = src[start:src.index("\n}\n", start)]
    for text in ("k->yg > 32767U", "(65535U * k->yg) >> 16",
                 "y1max + k->bb > 32767", "y1max + k->bg > 32767", "y1max + k->br > 32767",
                 "255 * k->ug", "lo < -32768 || hi > 32767"):
        assert text in body, f"guard text `{text}` missing from vita_ny_consts_fit_s16"
    for text in ("vqsubq_s16(", "vmlsq_lane_s16(", "vmulq_lane_s16(", "vshrn_n_u32(vmull_lane_u16(",
                 "vqmovun_s16(vshrq_n_s16(", "vmlaq_lane_u16(y16, y16, k->ymul, 1)",
                 "{ (uint16_t)k->yg, 256U, 0U, 0U }"):
        assert text in src, f"row16 text `{text}` missing"
    assert re.search(r"vita_ny_consts_fit_s16\(&k\)", src), "convert consults the guard"
    print("source mirrors the model")


def main() -> int:
    for name, consts in TABLES.items():
        prove(name, consts)
    check_guard_rejects()
    check_source_mirror()
    print("PASS test_native_yuv_s16")
    return 0


if __name__ == "__main__":
    sys.exit(main())
