"""Focused MSVC proof of the file-backed child writer and import-ID hook."""

import os
import re
import struct
import subprocess
import tempfile

import build_all as BA
import guest_coverage as C
from test_guest_coverage import artifact, fixture_document, write_document


HERE = os.path.dirname(os.path.abspath(__file__))
RT = os.path.join(HERE, "runtime")
SOURCE = r'''
#include <stdio.h>
#include <string.h>
#include "guest.h"

unsigned g_host_import_calls;
unsigned g_host_dynamic_calls;
int g_guest_gl_inventory_mode;

int guest_host_import(CPU *__restrict c, const char *name)
{
    (void)c;
    return strcmp(name, "fixture!first") == 0;
}

int guest_host_dynamic(CPU *__restrict c, uint32_t token)
{
    (void)c;
    (void)token;
    return 0;
}

int main(void)
{
    static const guest_import imports[] = {
        { 0x00005000U, "fixture!first" },
        { 0x00005004U, "fixture!second" },
    };
    CPU cpu;
    memset(&cpu, 0, sizeof cpu);
    guest_register_imports(imports, 2U);
    guest_register_coverage_contract("@SCHEMA@", "@BUILD@", 3U, 2U, 3U);
    if (guest_coverage_init_from_env() != 0) {
        fprintf(stderr, "coverage init failed: %s\n", guest_coverage_error());
        return 2;
    }
    if (!g_guest_coverage_functions || !g_guest_coverage_imports ||
        !g_guest_coverage_cases)
        return 3;
    guest_coverage_function(1U);
    guest_coverage_case(0U);
    guest_call(&cpu, 0x00005000U);
    if (cpu.fault)
        return 4;
    puts("coverage runtime fixture PASS");
    return 0;
}
'''


def compile_fixture(temp, contract, compiler, environment):
    source_path = os.path.join(temp, "coverage_runtime_smoke.c")
    executable = os.path.join(temp, "coverage_runtime_smoke.exe")
    source = SOURCE.replace("@SCHEMA@", contract.schema_id).replace(
        "@BUILD@", contract.build_id
    )
    with open(source_path, "w", encoding="ascii", newline="\n") as stream:
        stream.write(source)
    command = [
        compiler, "/nologo", "/W3", "/O1", "/std:c11", "/GS-",
        "/I", RT, "/Fe:" + executable, "/Fo:" + temp + os.sep,
        os.path.join(RT, "guest.c"), source_path,
    ]
    built = subprocess.run(
        command, cwd=temp, env=environment,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60,
    )
    if built.returncode or not os.path.isfile(executable):
        raise AssertionError(built.stdout.decode("cp866", errors="replace"))
    return executable


def compile_manual_hooks(temp, compiler, environment):
    header = os.path.join(temp, "guest_coverage_generated.h")
    guest_funcs = os.path.join(temp, "guest_funcs.h")
    macros = {
        "GUEST_COVERAGE_MANUAL_KAGE_SET_VSYNC_ID": 6782,
        "GUEST_COVERAGE_MANUAL_KAGE_INITIALIZE_ID": 7901,
        "GUEST_COVERAGE_MANUAL_KAGE_SHUTDOWN_ID": 7902,
        "GUEST_COVERAGE_MANUAL_KAGE_PRESENT_ID": 7904,
        "GUEST_COVERAGE_MANUAL_KAGE_GET_FRAMEBUFFER_WIDTH_ID": 7905,
        "GUEST_COVERAGE_MANUAL_KAGE_GET_FRAMEBUFFER_HEIGHT_ID": 7906,
        "GUEST_COVERAGE_MANUAL_KAGE_INITIALIZE_RENDER_DISPLAY_ID": 7916,
        "GUEST_COVERAGE_MANUAL_KAGE_SOUND_INITIALIZE_ID": 8228,
        "GUEST_COVERAGE_MANUAL_KAGE_GL_PROVIDER_RESOLVER_ID": 8251,
        "GUEST_COVERAGE_MANUAL_PORTABLE_SPLIT_APPEND_ID": 2305,
        "GUEST_COVERAGE_MANUAL_PORTABLE_SPLIT_CONSTRUCT_ID": 2306,
        "GUEST_COVERAGE_MANUAL_PORTABLE_PATH_FROM_BASE_ID": 2308,
        "GUEST_COVERAGE_MANUAL_PORTABLE_PERFORMANCE_COUNTERS_ID": 2970,
    }
    with open(header, "w", encoding="ascii", newline="\n") as stream:
        stream.write("#ifndef GUEST_COVERAGE_GENERATED_H\n")
        stream.write("#define GUEST_COVERAGE_GENERATED_H\n")
        for name, value in macros.items():
            stream.write("#define %s %dU\n" % (name, value))
        stream.write("#endif\n")
    declarations = set()
    for filename in ("manual_kage.c", "manual_portable.c"):
        with open(os.path.join(RT, filename), "r", encoding="utf-8") as stream:
            declarations.update(re.findall(r"\bsub_[0-9a-fA-F]{8}\b",
                                           stream.read()))
    with open(guest_funcs, "w", encoding="ascii", newline="\n") as stream:
        stream.write("#ifndef GUEST_FUNCS_H\n#define GUEST_FUNCS_H\n")
        stream.write('#include "guest.h"\n')
        for name in sorted(declarations):
            stream.write("void %s(CPU *__restrict c);\n" % name)
        stream.write("#endif\n")
    for filename in ("manual_kage.c", "manual_portable.c"):
        output = os.path.join(temp, os.path.splitext(filename)[0] + ".obj")
        command = [
            compiler, "/nologo", "/W3", "/O1", "/std:c11", "/GS-", "/c",
            "/I", temp, "/I", RT, "/Fo:" + output,
            os.path.join(RT, filename),
        ]
        built = subprocess.run(
            command, cwd=temp, env=environment,
            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60,
        )
        if built.returncode or not os.path.isfile(output):
            raise AssertionError(built.stdout.decode("cp866", errors="replace"))


def run_child(executable, run, environment):
    child_environment = run.child_environment(environment)
    child = subprocess.run(
        [executable], cwd=os.path.dirname(executable), env=child_environment,
        stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=15,
    )
    return child


def main():
    environment, compiler = BA.msvc_env()
    with tempfile.TemporaryDirectory(prefix="coverage-runtime-") as temp:
        contract_path = os.path.join(temp, "guest_coverage.json")
        write_document(contract_path, fixture_document())
        contract = C.load_contract(contract_path)
        executable = compile_fixture(temp, contract, compiler, environment)
        compile_manual_hooks(temp, compiler, environment)
        root = os.path.join(temp, "archive")

        run = C.CoverageRun(contract, root=root, timestamp_ns=11, pid=1)
        child = run_child(executable, run, environment)
        assert child.returncode == 0, child.stdout.decode(
            "cp866", errors="replace"
        )
        live_stdout = os.path.join(temp, "live.log")
        with open(live_stdout, "wb") as stream:
            stream.write(child.stdout)
        summary = run.finish({
            "returncode": child.returncode,
            "output": child.stdout,
            "stop_reason": None,
            "matched_line": None,
            "artifact": artifact(executable),
        }, live_stdout)
        assert summary["run"] == {
            "functions": 1, "imports": 1, "cases": 1,
        }

        # The child validates build identity before setting writer-ready.  A
        # mismatched map therefore fails loudly and cannot look like a valid
        # all-zero run from a stale executable.
        mismatch = C.CoverageRun(
            contract, root=root, timestamp_ns=12, pid=1
        )
        mismatch.mapping[
            C.BUILD_ID_OFFSET:C.BUILD_ID_OFFSET + C.IDENTITY_BYTES
        ] = b"a" * C.IDENTITY_BYTES
        mismatch.mapping.flush()
        failed = run_child(executable, mismatch, environment)
        assert failed.returncode == 2
        assert b"coverage header/schema/build mismatch" in failed.stdout
        assert not C.parse_header(mismatch.mapping)["writer_ready"]
        mismatch.close()

    print("guest coverage runtime: mmap/ready/import-before-call PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
