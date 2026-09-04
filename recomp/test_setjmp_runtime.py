"""Build and run the focused x86 setjmp/longjmp production oracle."""

import os
import subprocess
import sys
import tempfile

import build_all as BA


HERE = os.path.dirname(os.path.abspath(__file__))
RUNTIME = os.path.join(HERE, "runtime")


def main():
    env, cl = BA.msvc_env()
    with tempfile.TemporaryDirectory(prefix="isaac-setjmp-runtime-") as work:
        exe = os.path.join(work, "setjmp-runtime-oracle.exe")
        command = [
            cl, "/nologo", "/W3", "/WX", "/wd4310", "/O2", "/Gy",
            "/DGUEST_STACK_REQUIRED=1",
            "/std:c11", "/I", RUNTIME,
            os.path.join(RUNTIME, "guest.c"),
            os.path.join(RUNTIME, "setjmp_runtime_oracle.c"),
            "/Fe:" + exe, "/Fo:" + work + os.sep,
            "/link", "/OPT:REF", "/INCREMENTAL:NO",
        ]
        built = subprocess.run(
            command, cwd=HERE, env=env,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        print(built.stdout.decode("cp866", errors="replace"), end="")
        if built.returncode != 0:
            print("setjmp runtime oracle compile: FAIL rc=%d" %
                  built.returncode)
            return built.returncode

        try:
            ran = subprocess.run(
                [exe], cwd=work, timeout=10,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        except subprocess.TimeoutExpired as exc:
            output = exc.stdout or b""
            print(output.decode("cp866", errors="replace"), end="")
            print("setjmp runtime oracle: TIMEOUT")
            return 2
        output = ran.stdout.decode("cp866", errors="replace")
        print(output, end="")
        if (ran.returncode != 0 or
                "setjmp runtime oracle: PASS (dead return slot reused)" not in
                output):
            print("setjmp runtime oracle run: FAIL rc=%d" % ran.returncode)
            return ran.returncode or 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
