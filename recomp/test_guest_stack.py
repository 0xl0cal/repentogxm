"""Build and run the exhaustive x86 synthetic guest-stack oracle."""

import os
import subprocess
import sys
import tempfile

import build_all as BA


HERE = os.path.dirname(os.path.abspath(__file__))
RUNTIME = os.path.join(HERE, "runtime")
PASS_LINE = "guest stack oracle: PASS (owner/floor/ceiling/high-water/native-LR;"


def build_and_run(env, cl, root, optimization):
    work = os.path.join(root, optimization[1:].lower())
    os.makedirs(work)
    exe = os.path.join(work, "guest-stack-oracle.exe")
    command = [
        cl, "/nologo", "/W3", "/WX", "/wd4310", optimization, "/Gy",
        "/std:c11", "/DGUEST_STACK_REQUIRED=1", "/I", RUNTIME,
        os.path.join(RUNTIME, "guest.c"),
        os.path.join(RUNTIME, "guest_stack_oracle.c"),
        "/Fe:" + exe, "/Fo:" + work + os.sep,
        "/link", "/OPT:REF", "/INCREMENTAL:NO",
    ]
    built = subprocess.run(
        command, cwd=HERE, env=env,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    print(built.stdout.decode("cp866", errors="replace"), end="")
    if built.returncode != 0:
        print("guest stack oracle %s compile: FAIL rc=%d" %
              (optimization, built.returncode))
        return built.returncode
    try:
        ran = subprocess.run(
            [exe], cwd=work, timeout=10,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    except subprocess.TimeoutExpired as exc:
        print((exc.stdout or b"").decode("cp866", errors="replace"), end="")
        print("guest stack oracle %s: TIMEOUT" % optimization)
        return 2
    output = ran.stdout.decode("cp866", errors="replace")
    print(output, end="")
    if ran.returncode != 0 or PASS_LINE not in output:
        print("guest stack oracle %s run: FAIL rc=%d" %
              (optimization, ran.returncode))
        return ran.returncode or 2
    return 0


def main():
    env, cl = BA.msvc_env()
    with tempfile.TemporaryDirectory(prefix="isaac-guest-stack-") as work:
        for optimization in ("/O2", "/Od"):
            result = build_and_run(env, cl, work, optimization)
            if result:
                return result
    print("guest stack host gates: PASS (/O2 + /Od)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
