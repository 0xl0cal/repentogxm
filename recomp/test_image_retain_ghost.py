#!/usr/bin/env python3
"""Host test for recomp/runtime/host_vita_image_retain_ghost.h (the
ISAAC_VITA_IMAGE_RETAIN_GHOST stack-distance probe): compiles an assert-based
C driver against the header and runs it.

Usage: test_image_retain_ghost.py [--cc gcc]
"""

from __future__ import annotations

import argparse
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent

DRIVER = r"""
#include <assert.h>
#include <stdio.h>
#include "host_vita_image_retain_ghost.h"

static isaac_irg g;
#define MIB (1024u * 1024u)

int main(void)
{
    uint32_t i;

    /* A(1 MiB) B(2) C(3) freed, then A again: distance = B + C = 5 MiB -> bucket <=8. */
    irg_offer(&g, 0xA, 1u * MIB);
    irg_offer(&g, 0xB, 2u * MIB);
    irg_offer(&g, 0xC, 3u * MIB);
    assert(g.st.offers == 3 && g.st.cold == 3 && g.st.reloads == 0);
    irg_offer(&g, 0xA, 1u * MIB);
    assert(g.st.reloads == 1 && g.st.n[1] == 1 && g.st.kb[1] == 1024);
    assert(g.st.reload_kb == 1024);
    /* C again: only A (1 MiB) was freed since -> bucket <=4. */
    irg_offer(&g, 0xC, 3u * MIB);
    assert(g.st.n[0] == 1 && g.st.kb[0] == 3072);
    /* Immediate re-free: distance 0 -> bucket <=4. */
    irg_offer(&g, 0xC, 3u * MIB);
    assert(g.st.n[0] == 2);
    /* Exact edge: C again with exactly D's 4 MiB freed since is still <=4. */
    irg_offer(&g, 0xD, 4u * MIB);
    irg_offer(&g, 0xC, 3u * MIB);
    assert(g.st.n[0] == 3);
    irg_offer(&g, 0xF, 4u * MIB);
    irg_offer(&g, 0xE, 1u);
    irg_offer(&g, 0xC, 3u * MIB);   /* F + E = 4 MiB + 1 byte -> <=8 */
    assert(g.st.n[1] == 2);
    /* Big distance: 100 MiB of fillers then A -> > 96 bucket. */
    for (i = 0; i < 25; ++i)
        irg_offer(&g, 0x1000 + i, 4u * MIB);
    irg_offer(&g, 0xA, 1u * MIB);
    assert(g.st.n[IRG_BUCKETS - 1] == 1);
    /* Table overflow drops the oldest name; it then counts as cold again. */
    {
        uint32_t cold0 = g.st.cold;
        for (i = 0; i < IRG_SLOTS; ++i)
            irg_offer(&g, 0x100000 + i, 16u);
        assert(g.st.dropped > 0 && g.used == IRG_SLOTS);
        irg_offer(&g, 0xB, 2u * MIB);  /* B was among the oldest */
        assert(g.st.cold == cold0 + IRG_SLOTS + 1);
    }
    /* Memory window minima. */
    {
        isaac_irg_mem m = {0};
        irg_mem_note(&m, 10, 20, 30, 40, 5);
        irg_mem_note(&m, 12, 2, 31, 41, 9);
        irg_mem_note(&m, 0xFFFFFFFFu, 0xFFFFFFFFu, 1, 50, 1);
        assert(m.n == 3 && m.min_ram == 10 && m.min_vram == 2 && m.min_phy == 1);
        assert(m.min_sum == 45 && m.min_user == 40 && m.us_sum == 15 && m.us_max == 9);
    }
    printf("IMAGE RETAIN GHOST PASS: offers=%u reloads=%u dropped=%u\n",
           g.st.offers, g.st.reloads, g.st.dropped);
    return 0;
}
"""


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--cc", default="gcc")
    args = ap.parse_args()
    with tempfile.TemporaryDirectory() as tmp:
        src = Path(tmp) / "ghost_test.c"
        exe = Path(tmp) / "ghost_test"
        src.write_text(DRIVER, encoding="utf-8")
        cmd = shlex.split(args.cc) + [
            "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
            "-I", str(HERE / "runtime"), str(src), "-o", str(exe)]
        subprocess.run(cmd, check=True)
        out = subprocess.run([str(exe)], check=True, capture_output=True,
                             text=True).stdout
    print(out, end="")
    return 0 if "IMAGE RETAIN GHOST PASS" in out else 1


if __name__ == "__main__":
    sys.exit(main())
