#!/usr/bin/env python3
"""Host oracle + mutation check for ISAAC_VITA_GPU_FRAME_PROFILE.

Compiles kage_vita_gpu_frame_profile.c with its oracle (gcc, or WSL gcc on
Windows), requires PASS, then applies mutations to the model and requires
each mutant to FAIL.  Also pins that the OFF consumers guard every hook.
"""
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
RT = ROOT / "runtime"
SRC = RT / "kage_vita_gpu_frame_profile.c"
ORACLE = RT / "kage_vita_gpu_frame_profile_oracle.c"
HDR = RT / "kage_vita_gpu_frame_profile.h"

MUTATIONS = [
    ("busy needs one pending scene",
     "uint32_t busy = pending != 0u && pending < 0x80000000u;",
     "uint32_t busy = pending > 1u && pending < 0x80000000u;"),
    ("lag waits for its own display scene",
     "if ((int32_t)(completed - sh->target[slot]) < 0)",
     "if ((int32_t)(completed - sh->target[slot]) <= 0)"),
    ("queue class counts only pending frames",
     "if ((int32_t)(p->ring[i] - completed) > 0)",
     "if ((int32_t)(p->ring[i] - completed) >= 0)"),
    ("frame closes on a new present",
     "            ++w->frames;\n", ""),
    ("rbusy needs the render flag",
     "        if (s->prev_render) {",
     "        if (1) {"),
    # Not listed: right- instead of left-rectangle attribution is an
    # equivalent mutant at poll resolution (both unbiased).
    ("lag ring slot is seq-1",
     "uint32_t slot = s->lag_handled & (KGFP_RING - 1u); /* seq-1 */",
     "uint32_t slot = (s->lag_handled + 1u) & (KGFP_RING - 1u);"),
]


def gcc_cmd():
    if shutil.which("gcc"):
        return ["gcc"], lambda p: str(p)
    if shutil.which("wsl"):
        def conv(p):
            p = str(Path(p).resolve()).replace("\\", "/")
            return "/mnt/" + p[0].lower() + p[2:]
        return ["wsl", "-e", "gcc"], conv
    sys.exit("no gcc")


def build_run(src_text, tmp):
    cc, conv = gcc_cmd()
    src = Path(tmp) / "kage_vita_gpu_frame_profile.c"
    src.write_text(src_text, encoding="utf-8", newline="\n")
    shutil.copy(HDR, Path(tmp) / HDR.name)
    shutil.copy(ORACLE, Path(tmp) / ORACLE.name)
    exe = Path(tmp) / "kgfp"
    r = subprocess.run(cc + ["-std=c11", "-O1", "-Wall", "-Wextra", "-Werror",
                             "-DISAAC_KAGE_VITA_GPU_FRAME_PROFILE_ORACLE",
                             conv(src), conv(Path(tmp) / ORACLE.name), "-o", conv(exe)],
                       capture_output=True, text=True)
    if r.returncode:
        return "COMPILE", r.stdout + r.stderr
    run = (["wsl", "-e", conv(exe)] if cc[0] == "wsl" else [str(exe)])
    r = subprocess.run(run, capture_output=True, text=True)
    return ("PASS" if r.returncode == 0 and "oracle: PASS" in r.stdout else "FAIL"), r.stdout


def guards():
    """OFF builds must preprocess to the base text: every hook and include
    in the consumers sits inside #if defined(ISAAC_VITA_GPU_FRAME_PROFILE)."""
    for name in ("kage_vita_backend.c", "kage_vita_phase_profile.c"):
        lines = (RT / name).read_text(encoding="utf-8").splitlines()
        for i, line in enumerate(lines):
            if "GPU_FRAME_PROFILE" in line and not line.startswith("#"):
                prev = lines[i - 1].strip()
                assert prev == "#if defined(ISAAC_VITA_GPU_FRAME_PROFILE)" or \
                    prev.startswith("KAGE_VITA_GPU_FRAME_PROFILE_REPORT(") or \
                    lines[i - 1].startswith("    KAGE_VITA_GPU_FRAME_PROFILE_REPORT("), \
                    f"{name}:{i+1} unguarded: {line.strip()}"
            if "kage_vita_gpu_frame_profile.h" in line:
                assert lines[i - 1].strip() == "#if defined(ISAAC_VITA_GPU_FRAME_PROFILE)", \
                    f"{name}:{i+1} unguarded include"


def main():
    guards()
    base = SRC.read_text(encoding="utf-8")
    with tempfile.TemporaryDirectory(dir=os.environ.get("TMP")) as tmp:
        status, out = build_run(base, tmp)
        print(out.strip())
        if status != "PASS":
            sys.exit(f"oracle {status}")
        killed = 0
        for name, old, new in MUTATIONS:
            assert base.count(old) == 1, f"mutation anchor not unique: {name}"
            status, out = build_run(base.replace(old, new), tmp)
            ok = status != "PASS"
            killed += ok
            print(f"mutation {'killed' if ok else 'SURVIVED'} ({status}): {name}")
        print(f"mutations killed {killed}/{len(MUTATIONS)}")
        if killed != len(MUTATIONS):
            sys.exit(1)
    print("test_kage_vita_gpu_frame_profile: PASS")


if __name__ == "__main__":
    main()
