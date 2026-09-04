"""Compile and run the opt-in guest lookup cache host oracle."""

from __future__ import annotations

from pathlib import Path
import re
import subprocess
import sys
import tempfile

import build_all as BA


HERE = Path(__file__).resolve().parent
RUNTIME = HERE / "runtime"
VITA_CMAKE = HERE / "vita" / "CMakeLists.txt"
PASS_MARKER = (
    "guest lookup cache oracle: PASS "
    "(14868 RVA+VA; exact collisions; confirmed dispatch bypass; "
    "reset/repeat; "
    "413/438 dense IAT; binary fallback)"
)
OFF_PASS_MARKER = (
    "guest lookup cache OFF oracle: PASS "
    "(binary lookup; re-register; no cache path)"
)
DISPATCH_PASS_MARKER = (
    "guest lookup cache oracle: PASS "
    "(14868 RVA+VA; exact collisions; confirmed dispatch bypass; "
    "reset/repeat; "
    "413/438 dense IAT; binary fallback; "
    "dispatch table 14868 VA hits, RVA/import/dynamic/no-image misses)"
)


def verify_cmake_gate() -> None:
    source = VITA_CMAKE.read_text(encoding="utf-8")
    option = re.compile(
        r"option\(ISAAC_VITA_GUEST_LOOKUP_CACHE\s+"
        r'"[^"]+"\s+OFF\)',
        re.DOTALL,
    )
    if option.search(source) is None:
        raise AssertionError("guest lookup cache is not a default-OFF option")
    scope = re.compile(
        r"if\(ISAAC_VITA_GUEST_LOOKUP_CACHE\)\s+"
        r".*?set_property\(SOURCE\s+"
        r'"\$\{ISAAC_RUNTIME\}/guest\.c"\s+'
        r'"\$\{ISAAC_RUNTIME\}/kage_vita_phase_profile\.c"\s+'
        r"APPEND PROPERTY COMPILE_DEFINITIONS\s+"
        r"ISAAC_VITA_GUEST_LOOKUP_CACHE=1\)\s+"
        r".*?endif\(\)",
        re.DOTALL,
    )
    match = scope.search(source)
    if match is None:
        raise AssertionError("guest lookup cache is not scoped to guest.c")
    if "guest_0" in match.group(0):
        raise AssertionError("guest lookup cache leaked into generated units")


def run(command: list[str], env: dict[str, str]) -> subprocess.CompletedProcess:
    completed = subprocess.run(
        command,
        cwd=HERE,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )
    output = completed.stdout.decode("cp866", errors="replace")
    print(output, end="")
    if completed.returncode != 0:
        raise subprocess.CalledProcessError(completed.returncode, command)
    return completed


def main() -> int:
    verify_cmake_gate()
    env, compiler = BA.msvc_env()
    with tempfile.TemporaryDirectory(
            prefix="isaac-guest-lookup-cache-") as temporary:
        root = Path(temporary)
        cache_only_object = root / "guest-cache-only.obj"
        run([
            compiler, "/nologo", "/c", "/W3", "/WX", "/O2", "/Gy",
            "/std:c11", "/wd4310", "/wd4996",
            "/DISAAC_VITA_GUEST_LOOKUP_CACHE=1",
            "/I", str(RUNTIME), str(RUNTIME / "guest.c"),
            "/Fo:" + str(cache_only_object),
        ], env)
        if not cache_only_object.is_file():
            raise AssertionError("cache-only guest.c compile produced no object")

        off_object = root / "guest-cache-off.obj"
        run([
            compiler, "/nologo", "/c", "/W3", "/WX", "/O2", "/Gy",
            "/std:c11", "/wd4310", "/wd4996",
            "/DISAAC_VITA_PHASE_PROFILE=1",
            "/I", str(RUNTIME), str(RUNTIME / "guest.c"),
            "/Fo:" + str(off_object),
        ], env)
        preprocessed = root / "guest-cache-off.i"
        run([
            compiler, "/nologo", "/P", "/W3", "/WX", "/std:c11",
            "/wd4310", "/wd4996", "/DISAAC_VITA_PHASE_PROFILE=1",
            "/I", str(RUNTIME), str(RUNTIME / "guest.c"),
            "/Fi:" + str(preprocessed),
        ], env)
        forbidden = (
            "guest_lookup_cache_entry", "guest_lookup_cache_index",
            "g_guest_lookup_cache", "g_guest_import_dense",
            "GUEST_PHASE_PROFILE_NOTE_LOOKUP_CACHE",
        )
        preprocessed_text = preprocessed.read_text(
            encoding="utf-8", errors="replace"
        )
        leaked = [token for token in forbidden if token in preprocessed_text]
        if leaked:
            raise AssertionError(
                f"cache-OFF preprocessed guest.c retained cache code: {leaked}"
            )

        dumpbin = Path(compiler).with_name("dumpbin.exe")
        if not dumpbin.is_file():
            raise AssertionError(f"MSVC dumpbin is missing: {dumpbin}")
        on_symbols = subprocess.run(
            [str(dumpbin), "/symbols", str(cache_only_object)],
            cwd=HERE, env=env, check=True, text=True,
            encoding="cp866", errors="replace", capture_output=True,
        ).stdout
        off_symbols = subprocess.run(
            [str(dumpbin), "/symbols", str(off_object)],
            cwd=HERE, env=env, check=True, text=True,
            encoding="cp866", errors="replace", capture_output=True,
        ).stdout
        proof_symbols = ("g_guest_lookup_cache", "g_guest_import_dense")
        if not all(symbol in on_symbols for symbol in proof_symbols):
            raise AssertionError("cache-ON object lacks expected cache symbols")
        leaked_symbols = [
            symbol for symbol in proof_symbols if symbol in off_symbols
        ]
        if leaked_symbols:
            raise AssertionError(
                f"cache-OFF object retained cache symbols: {leaked_symbols}"
            )
        print(
            "guest lookup cache gate oracle: PASS "
            "(default OFF; preprocessed/object clean; two-source scope)"
        )

        off_executable = root / "guest-lookup-cache-off-oracle.exe"
        run([
            compiler, "/nologo", "/W4", "/WX", "/O2", "/Gy",
            "/std:c11", "/wd4310", "/wd4702", "/wd4996",
            "/DISAAC_VITA_PHASE_PROFILE=1",
            "/I", str(RUNTIME),
            str(RUNTIME / "guest_lookup_cache_oracle.c"),
            "/Fe:" + str(off_executable),
            "/Fo:" + str(root / "guest-lookup-cache-off-oracle.obj"),
            "/link", "/OPT:REF", "/INCREMENTAL:NO",
        ], env)
        off_ran = run([str(off_executable)], env)
        off_output = off_ran.stdout.decode("cp866", errors="replace")
        if OFF_PASS_MARKER not in off_output:
            raise AssertionError(
                f"guest lookup cache OFF oracle returned unexpected output: "
                f"{off_output!r}"
            )

        executable = root / "guest-lookup-cache-oracle.exe"
        run([
            compiler, "/nologo", "/W4", "/WX", "/O2", "/Gy",
            "/std:c11", "/wd4310", "/wd4702", "/wd4996",
            "/DISAAC_VITA_GUEST_LOOKUP_CACHE=1",
            "/DISAAC_VITA_PHASE_PROFILE=1",
            "/I", str(RUNTIME),
            str(RUNTIME / "guest_lookup_cache_oracle.c"),
            "/Fe:" + str(executable),
            "/Fo:" + str(root / "guest-lookup-cache-oracle.obj"),
            "/link", "/OPT:REF", "/INCREMENTAL:NO",
        ], env)
        ran = run([str(executable)], env)
        output = ran.stdout.decode("cp866", errors="replace")
        if PASS_MARKER not in output:
            raise AssertionError(
                f"guest lookup cache oracle returned unexpected output: "
                f"{output!r}"
            )

        # Third leg: the O(1) dispatch table on top of the cache (the perf
        # configuration).  Same oracle, same synthetic table, plus the
        # table-specific contract checks.
        dispatch_executable = root / "guest-lookup-cache-dispatch-oracle.exe"
        run([
            compiler, "/nologo", "/W4", "/WX", "/O2", "/Gy",
            "/std:c11", "/wd4310", "/wd4702", "/wd4996",
            "/DISAAC_VITA_GUEST_LOOKUP_CACHE=1",
            "/DISAAC_VITA_GUEST_DISPATCH_TABLE=1",
            "/DISAAC_VITA_PHASE_PROFILE=1",
            "/I", str(RUNTIME),
            str(RUNTIME / "guest_lookup_cache_oracle.c"),
            "/Fe:" + str(dispatch_executable),
            "/Fo:" + str(root / "guest-lookup-cache-dispatch-oracle.obj"),
            "/link", "/OPT:REF", "/INCREMENTAL:NO",
        ], env)
        ran = run([str(dispatch_executable)], env)
        output = ran.stdout.decode("cp866", errors="replace")
        if DISPATCH_PASS_MARKER not in output:
            raise AssertionError(
                f"guest lookup cache + dispatch table oracle returned "
                f"unexpected output: {output!r}"
            )
    print(PASS_MARKER)
    print(DISPATCH_PASS_MARKER)
    return 0


if __name__ == "__main__":
    sys.exit(main())
