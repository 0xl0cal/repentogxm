#!/usr/bin/env python3
"""Focused actual-wrapper clock consumer matrix, using a 32-bit host compiler.

OS allocation, guest stream calls and the translated row boundary are mocks;
the wrapper/core and guest CPU/stack helpers are the current production code.
This does not emulate Vita timings or independently re-prove libpng decoding.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


CONSUMERS = (
    "ISAAC_VITA_PNG_WINDOW_PROFILE",
    "ISAAC_VITA_NATIVE_PNG_RECEIPT",
    "ISAAC_VITA_NATIVE_PNG_VERIFY",
)


def records(text: str) -> list[dict[str, str]]:
    result = []
    for line in text.splitlines():
        if not line:
            continue
        fields = dict(item.split("=", 1) for item in line.split())
        result.append(fields)
    if len(result) != 16:
        raise AssertionError(f"expected 12 wrapper + 4 core cases, got {len(result)}")
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path)
    parser.add_argument("--cc")
    parser.add_argument("--mask", type=int, choices=range(8), action="append")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    out = args.out.resolve() if args.out else Path(tempfile.mkdtemp(prefix="png-clocks-"))
    out.mkdir(parents=True, exist_ok=True)
    # These out-of-line helpers otherwise pull the whole dispatch runtime into
    # a focused wrapper test. Regenerate their exact bodies for every run,
    # rather than trusting a frozen test copy of the checked stack operations.
    guest = (root / "runtime/guest.c").read_text(encoding="utf-8")
    stack = guest[
        guest.index("GUEST_STACK_HOT_NOINLINE uint32_t guest_stack_address("):
        guest.index("GUEST_STACK_HOT_NOINLINE int guest_stack_set_generated(")
    ]
    if stack.count("GUEST_STACK_HOT_NOINLINE") != 3:
        raise AssertionError("guest stack-helper extraction changed")
    (out / "native-png-clock-stack.inc").write_text(stack, encoding="utf-8")
    env = os.environ.copy()
    if os.name == "nt":
        from setuptools.msvc import msvc14_get_vc_env
        env = {key.upper(): value for key, value in env.items()}
        env.update({key.upper(): value for key, value in msvc14_get_vc_env("x86").items()})
        cc = args.cc or shutil.which("clang") or "C:/Program Files/LLVM/bin/clang.exe"
        target = ["--target=i686-pc-windows-msvc", "-D_CRT_SECURE_NO_WARNINGS=1"]
        suffix = ".exe"
    else:
        cc = args.cc or os.environ.get("CC", "cc")
        target = ["-m32"]
        suffix = ""
    common = [
        cc, *target, "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
        "-fno-strict-aliasing", "-D__vita__=1", "-DGUEST_STACK_REQUIRED=1",
        "-DGUEST_IMAGE_BASE=0x98000000u", "-DISAAC_VITA_NATIVE_PNG=1",
        '-DISAAC_VITA_NATIVE_PNG_BUILD_ID="png-clocks:host"',
        "-I", str(root / "vita/host_tests/native_png_clock_stubs"),
        "-I", str(out),
        "-I", str(root / "runtime"),
    ]
    summary = []
    for mask in args.mask or range(8):
        variants = []
        # An explicit standalone =0 must also preserve the absent/default
        # producer, including clocks and durations, rather than enabling it.
        for enabled in (None, 0, 1) if mask == 0 else (None, 1):
            tag = f"mask-{mask}-elision-{'absent' if enabled is None else enabled}"
            flags = [f"-D{name}=1" for bit, name in enumerate(CONSUMERS) if mask & (1 << bit)]
            if enabled is not None:
                flags.append(f"-DISAAC_VITA_NATIVE_PNG_CLOCK_ELISION={enabled}")
            executable = out / (tag + suffix)
            command = [
                *common, *flags,
                str(root / "vita/host_tests/kage_vita_native_png_clocks.c"),
                str(root / "runtime/host_vita_archive_miniz_native.c"),
                "-o", str(executable),
            ]
            print(f"compile {tag}", flush=True)
            subprocess.run(command, env=env, check=True)
            run = subprocess.run([str(executable)], env=env, text=True,
                                 stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            (out / (tag + ".log")).write_text(run.stdout, encoding="utf-8")
            if run.returncode:
                print(run.stdout, end="")
                raise RuntimeError(f"{tag} failed, exit {run.returncode}")
            parsed = records(run.stdout)
            variants.append(parsed)
            summary.append({"mask": mask, "elision": enabled, "command": command,
                            "records": parsed})
        before, after = variants[0], variants[-1]
        if not mask and variants[1] != before:
            raise AssertionError(("explicit elision=0 differs from absent", variants[1], before))
        for old, new in zip(before, after):
            comparable_old, comparable_new = dict(old), dict(new)
            if "case" in old and not mask:
                if int(old["clocks"]) <= 0 or int(new["clocks"]) != 0:
                    raise AssertionError((mask, old, new))
                comparable_old.pop("clocks")
                comparable_new.pop("clocks")
                for timing in ("decode_us", "io_us", "serve_us", "translated_us"):
                    if int(comparable_new.pop(timing)):
                        raise AssertionError((mask, "disabled timing", new))
                    comparable_old.pop(timing)
            if comparable_old != comparable_new:
                raise AssertionError((mask, old, new))
        print(f"PASS mask={mask}: 12 wrapper states/streams + 4 generic callback cases", flush=True)
    (out / "results.json").write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(f"native PNG clock matrix PASS: {len(summary)} configurations; {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
