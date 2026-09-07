#!/usr/bin/env python3
"""Compile/run the low-overhead simulation-cadence receipt oracle."""

from __future__ import annotations

import re
import shutil
import subprocess
import tempfile
from pathlib import Path


def compiler() -> str:
    for name in ("cc", "gcc", "clang"):
        found = shutil.which(name)
        if found:
            return found
    candidates = (
        Path(r"C:\Program Files\LLVM\bin\clang.exe"),
        Path(r"C:\Program Files\Microsoft Visual Studio\2022\Community") /
        "VC/Tools/Llvm/x64/bin/clang.exe",
    )
    for candidate in candidates:
        if candidate.is_file():
            return str(candidate)
    raise RuntimeError("no host C compiler found")


def run(command: list[str]) -> str:
    completed = subprocess.run(
        command, check=False, text=True, capture_output=True
    )
    if completed.returncode != 0:
        raise RuntimeError(
            f"command failed ({completed.returncode}): {command!r}\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    return completed.stdout


def function_body(source: str, name: str) -> str:
    match = re.search(
        rf"void {re.escape(name)}\([^)]*\)\s*\{{(?P<body>.*?)\n\}}",
        source,
        re.DOTALL,
    )
    if match is None:
        raise AssertionError(f"missing function body: {name}")
    return match.group("body")


def check_present_heartbeat(backend: str, cmake: str, temp: Path) -> None:
    """Compile the real backend selector, not a duplicated reference helper."""
    match = re.search(
        r"static int kage_vita_log_heartbeat\(unsigned count\)\s*\{.*?\n\}",
        backend, re.DOTALL,
    )
    if match is None:
        raise AssertionError("backend heartbeat selector missing")
    start = cmake.index("    if(ISAAC_VITA_STAGE_HEARTBEAT)\n")
    scope = cmake[start:cmake.index("    endif()", start)]
    for name in ("kage_vita_generated_hooks.c", "kage_vita_backend.c"):
        if scope.count('"${ISAAC_RUNTIME}/' + name + '"') != 1:
            raise AssertionError(f"stage heartbeat definition owner lost: {name}")

    # PHASE still suppresses this legacy report; SIM explicitly opts into
    # the original cadence even with STAGE off. Defined-zero means off too.
    variants = (
        ("default", (), 0),
        ("zero", ("ISAAC_VITA_STAGE_HEARTBEAT=0", "ISAAC_VITA_SIM_CADENCE_RECEIPT=0"), 0),
        ("stage", ("ISAAC_VITA_STAGE_HEARTBEAT=1",), 1),
        ("cadence", ("ISAAC_VITA_SIM_CADENCE_RECEIPT=1",), 1),
        ("both", ("ISAAC_VITA_STAGE_HEARTBEAT=1", "ISAAC_VITA_SIM_CADENCE_RECEIPT=1"), 1),
        ("phase", ("ISAAC_VITA_PHASE_PROFILE=1",), 0),
        ("phase-stage", ("ISAAC_VITA_PHASE_PROFILE=1", "ISAAC_VITA_STAGE_HEARTBEAT=1"), 0),
        ("phase-cadence", ("ISAAC_VITA_PHASE_PROFILE=1", "ISAAC_VITA_SIM_CADENCE_RECEIPT=1"), 0),
    )
    source = temp / "present_heartbeat.c"
    source.write_text("#include <limits.h>\n" + match.group(0) + r'''
static int check(unsigned count)
{
    int early = count == 1 || count == 2 || count == 4 || count == 8 ||
                count == 16 || count == 32 || count == 64;
    int wanted = EXPECT_HEARTBEAT && count != 0 &&
                 (early || count % 120 == 0);
    return kage_vita_log_heartbeat(count) == wanted;
}
int main(void)
{
    for (unsigned count = 0; count <= 10000; ++count)
        if (!check(count)) return 1;
    return check(UINT_MAX) ? 0 : 2;
}
''', encoding="utf-8")
    for name, defines, expected in variants:
        executable = temp / ("heartbeat-" + name + ".exe")
        run([compiler(), "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
             *("-D" + value for value in defines),
             "-DEXPECT_HEARTBEAT=" + str(expected), str(source), "-o", str(executable)])
        run([str(executable)])
    print("present heartbeat selector: PASS (8 modes, 10002 counts each)")


def main() -> int:
    root = Path(__file__).resolve().parents[2]
    runtime = root / "recomp" / "runtime"
    source_path = runtime / "kage_vita_sim_cadence_receipt.c"
    oracle_path = runtime / "kage_vita_sim_cadence_receipt_oracle.c"
    backend = (runtime / "kage_vita_backend.c").read_text(encoding="utf-8")
    cmake = (root / "recomp" / "vita" / "CMakeLists.txt").read_text(
        encoding="utf-8"
    )
    wrapper = (root / "tools" / "build_vita.py").read_text(encoding="utf-8")
    raw_gate = (root / "recomp" / "vita" /
                "vita_raw_allocator_gate.py").read_text(encoding="utf-8")
    source = source_path.read_text(encoding="utf-8")

    note = function_body(
        source, "kage_vita_sim_cadence_note_game_update"
    )
    if note.count("++s_cadence.update_count;") != 1:
        raise AssertionError("Game::Update note lost its single increment")
    for forbidden in (
        "Printf", "printf", "sceKernel", "time", "clock", "atomic",
        "sync", "delay", "sleep", "scheduler",
    ):
        if forbidden in note:
            raise AssertionError(
                f"Game::Update hot note acquired forbidden work: {forbidden}"
            )

    present_log = backend.index(
        '"[kage-vita] present heartbeat count=%u elapsed_ms=%u\\n"'
    )
    cadence_log = backend.index(
        "kage_vita_sim_cadence_report_present_heartbeat(", present_log
    )
    if cadence_log <= present_log:
        raise AssertionError("cadence report is not adjacent after heartbeat")
    if backend.count("kage_vita_sim_cadence_reset();") != 1:
        raise AssertionError("cadence epoch reset ownership changed")

    required_cmake = (
        'option(ISAAC_VITA_SIM_CADENCE_RECEIPT',
        '"${ISAAC_RUNTIME}/kage_vita_sim_cadence_receipt.c"',
        'REGEX "^void sub_002cdcf0\\\\(CPU \\\\*__restrict c\\\\)$"',
        "ISAAC_VITA_SIM_CADENCE_OWNER_COUNT EQUAL 1",
        "ISAAC_VITA_SIM_CADENCE_RECEIPT requires ISAAC_VITA_PHASE_PROFILE=OFF",
        "ISAAC_VITA_SIM_CADENCE_RECEIPT_BUILD_ID=",
    )
    for needle in required_cmake:
        if needle not in cmake:
            raise AssertionError(f"CMake cadence receipt contract lost: {needle}")
    for needle in (
        '"-DISAAC_VITA_SIM_CADENCE_RECEIPT=',
        '"ISAAC_VITA_SIM_CADENCE_RECEIPT": sim_cadence_receipt',
        '"--sim-cadence-receipt"',
        "--release rejects the diagnostic --sim-cadence-receipt feature",
    ):
        if needle not in wrapper:
            raise AssertionError(f"canonical build receipt contract lost: {needle}")
    for needle in (
        'SIM_CADENCE_RECEIPT_CACHE_KEY = "ISAAC_VITA_SIM_CADENCE_RECEIPT"',
        'runtime.add("kage_vita_sim_cadence_receipt.c")',
        "def verify_sim_cadence_receipt_compile_scope(",
        "verify_sim_cadence_receipt_compile_scope(records, cache)",
    ):
        if needle not in raw_gate:
            raise AssertionError(f"raw gate cadence contract lost: {needle}")

    with tempfile.TemporaryDirectory(prefix="isaac-sim-cadence-") as temp:
        check_present_heartbeat(backend, cmake, Path(temp))
        executable = Path(temp) / (
            "sim_cadence_oracle.exe" if __import__("os").name == "nt"
            else "sim_cadence_oracle"
        )
        output = run([
            compiler(), "-std=c11", "-Wall", "-Wextra", "-Werror",
            "-DISAAC_KAGE_VITA_SIM_CADENCE_ORACLE=1",
            '-DISAAC_VITA_SIM_CADENCE_RECEIPT_BUILD_ID="sim:oracle"',
            f"-I{runtime}", str(source_path), str(oracle_path),
            "-o", str(executable),
        ])
        if output:
            raise AssertionError(f"compiler emitted unexpected stdout: {output}")
        oracle_output = run([str(executable)])
    if not re.fullmatch(
            r"sim cadence receipt oracle: PASS \([0-9]+ checks\)\n",
            oracle_output):
        raise AssertionError(f"unexpected oracle output: {oracle_output!r}")
    print(oracle_output, end="")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
