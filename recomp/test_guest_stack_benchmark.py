"""Build and run the reproducible x86 guest-stack hot-path benchmark."""

import os
import subprocess
import sys
import tempfile

import build_all as BA


HERE = os.path.dirname(os.path.abspath(__file__))
RUNTIME = os.path.join(HERE, "runtime")


def main():
    env, cl = BA.msvc_env()
    with tempfile.TemporaryDirectory(prefix="isaac-guest-stack-bench-") as work:
        exe = os.path.join(work, "guest-stack-benchmark.exe")
        command = [
            cl, "/nologo", "/W3", "/WX", "/wd4310", "/O2", "/Gy",
            "/std:c11", "/DGUEST_STACK_REQUIRED=1", "/I", RUNTIME,
            os.path.join(RUNTIME, "guest.c"),
            os.path.join(RUNTIME, "guest_stack_benchmark.c"),
            "/Fe:" + exe, "/Fo:" + work + os.sep,
            "/link", "/OPT:REF", "/INCREMENTAL:NO",
        ]
        built = subprocess.run(
            command, cwd=HERE, env=env,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        print(built.stdout.decode("cp866", errors="replace"), end="")
        if built.returncode:
            return built.returncode
        ran = subprocess.run(
            [exe], cwd=work, timeout=60,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        output = ran.stdout.decode("cp866", errors="replace")
        print(output, end="")
        if ran.returncode or "guest stack benchmark: PASS" not in output:
            return ran.returncode or 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
