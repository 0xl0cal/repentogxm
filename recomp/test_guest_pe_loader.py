"""Compile and run the bounded PE loader host oracle in an isolated directory."""

import os
import subprocess
import sys
import tempfile

import build_all as BA


HERE = os.path.dirname(os.path.abspath(__file__))
RUNTIME = os.path.join(HERE, "runtime")
DEFAULT_PE = os.path.abspath(os.environ.get(
    "REPENTOGXM_PE",
    os.path.join(HERE, "..", "build", "recompiler",
                 "isaac-ng.exe.unpacked.exe"),
))


def main():
    pe_path = os.path.abspath(
        sys.argv[1] if len(sys.argv) > 1 else
        os.environ.get("ISAAC_PE_PATH", DEFAULT_PE))
    if not os.path.isfile(pe_path):
        print("exact unpacked PE is missing: %s" % pe_path)
        return 2

    env, cl = BA.msvc_env()
    with tempfile.TemporaryDirectory(prefix="isaac-guest-pe-loader-") as work:
        exe = os.path.join(work, "guest-pe-loader-oracle.exe")
        command = [
            cl, "/nologo", "/W4", "/WX", "/O2", "/Gy", "/std:c11",
            "/D_CRT_SECURE_NO_WARNINGS",
            "/I", RUNTIME,
            os.path.join(RUNTIME, "guest_pe_loader_oracle.c"),
            "/Fe:" + exe, "/Fo:" + work + os.sep,
            "/link", "/OPT:REF", "/INCREMENTAL:NO",
        ]
        built = subprocess.run(
            command, cwd=HERE, env=env,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        print(built.stdout.decode("cp866", errors="replace"), end="")
        if built.returncode != 0:
            print("guest PE loader oracle compile: FAIL rc=%d" %
                  built.returncode)
            return built.returncode
        ran = subprocess.run(
            [exe, pe_path], cwd=work, timeout=90,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        output = ran.stdout.decode("cp866", errors="replace")
        print(output, end="")
        marker = ("guest PE loader oracle: PASS "
                  "(bounded parse/map/auth/transaction)")
        if ran.returncode != 0 or marker not in output:
            print("guest PE loader oracle run: FAIL rc=%d" % ran.returncode)
            return ran.returncode or 2

        integration_exe = os.path.join(work, "guest-pe-integration-oracle.exe")
        integration_command = [
            cl, "/nologo", "/W3", "/WX", "/wd4310", "/O2", "/Gy",
            "/std:c11", "/I", RUNTIME,
            os.path.join(RUNTIME, "guest.c"),
            os.path.join(RUNTIME, "guest_pe_integration_oracle.c"),
            "/Fe:" + integration_exe, "/Fo:" + work + os.sep,
            "/link", "/OPT:REF", "/INCREMENTAL:NO",
        ]
        built = subprocess.run(
            integration_command, cwd=HERE, env=env,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        print(built.stdout.decode("cp866", errors="replace"), end="")
        if built.returncode != 0:
            print("guest PE integration oracle compile: FAIL rc=%d" %
                  built.returncode)
            return built.returncode
        ran = subprocess.run(
            [integration_exe, pe_path], cwd=work, timeout=90,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        output = ran.stdout.decode("cp866", errors="replace")
        print(output, end="")
        marker = ("guest PE integration oracle: PASS "
                  "(publish/rollback/retry)")
        if ran.returncode != 0 or marker not in output:
            print("guest PE integration oracle run: FAIL rc=%d" %
                  ran.returncode)
            return ran.returncode or 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
