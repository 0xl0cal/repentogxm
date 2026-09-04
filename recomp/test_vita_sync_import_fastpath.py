"""Focused host oracle for validated Enter/Leave sync-import routing."""

from __future__ import annotations

from pathlib import Path
import re
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE / "vita"))

import build_all as BA
import vita_raw_allocator_gate as RAW_GATE


RUNTIME = HERE / "runtime"
VITA_CMAKE = HERE / "vita" / "CMakeLists.txt"
BUILD_WRAPPER = HERE.parent / "tools" / "build_vita.py"
PASS_MARKER = (
    "Vita sync import fast path oracle: PASS "
    "(exact 413 rows; Enter/Leave RVA+VA; import precedence; "
    "three-site direct-IAT differential; registration and local-ID fail closed)"
)
INLINE_PASS_MARKER = (
    "Vita sync import fast path oracle: PASS "
    "(exact 413 rows; Enter/Leave RVA+VA; import precedence; "
    "three-site direct-IAT differential; registration and local-ID fail closed; "
    "inline single-owner Enter/Leave without the endpoint)"
)


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


def verify_build_gate() -> None:
    cmake = VITA_CMAKE.read_text(encoding="utf-8")
    if re.search(
        r"option\(ISAAC_VITA_SYNC_IMPORT_FASTPATH\s+"
        r'"[^"]+"\s+OFF\)',
        cmake,
        re.DOTALL,
    ) is None:
        raise AssertionError("sync-import fast path is not default OFF")
    if re.search(
        r"option\(ISAAC_VITA_SYNC_INLINE_FASTPATH\s+"
        r'"[^"]+"\s+OFF\)',
        cmake,
        re.DOTALL,
    ) is None:
        raise AssertionError("sync inline fast path is not default OFF")
    inline_scope = re.search(
        r"if\(ISAAC_VITA_SYNC_INLINE_FASTPATH\)\s+"
        r".*?set_property\(SOURCE\s+"
        r'"\$\{ISAAC_RUNTIME\}/guest\.c"\s+'
        r'"\$\{ISAAC_RUNTIME\}/host_vita_sync\.c"\s+'
        r"APPEND PROPERTY COMPILE_DEFINITIONS\s+"
        r"ISAAC_VITA_SYNC_INLINE_FASTPATH=1\)\s+.*?endif\(\)",
        cmake,
        re.DOTALL,
    )
    if inline_scope is None or "guest_0" in inline_scope.group(0):
        raise AssertionError(
            "sync inline macro is not scoped to guest.c + host_vita_sync.c"
        )
    if (
        "if(ISAAC_VITA_SYNC_INLINE_FASTPATH AND NOT "
        "ISAAC_VITA_SYNC_IMPORT_FASTPATH)" not in cmake
    ):
        raise AssertionError("inline fast path does not require the import fast path")
    scope = re.search(
        r"if\(ISAAC_VITA_SYNC_IMPORT_FASTPATH\)\s+"
        r".*?set_property\(SOURCE\s+"
        r'"\$\{ISAAC_RUNTIME\}/guest\.c"\s+'
        r"APPEND PROPERTY COMPILE_DEFINITIONS\s+"
        r"ISAAC_VITA_SYNC_IMPORT_FASTPATH=1\)\s+.*?endif\(\)",
        cmake,
        re.DOTALL,
    )
    if scope is None or "guest_0" in scope.group(0):
        raise AssertionError("sync-import macro is not scoped to guest.c")
    wrapper = BUILD_WRAPPER.read_text(encoding="utf-8")
    if (
        wrapper.count('"-DISAAC_VITA_SYNC_IMPORT_FASTPATH=ON"') != 2
        or wrapper.count(
            '"ISAAC_VITA_SYNC_IMPORT_FASTPATH:BOOL=ON"'
        ) != 1
        or wrapper.count('"ISAAC_VITA_SYNC_IMPORT_FASTPATH": True') != 1
        or "ISAAC_VITA_SYNC_IMPORT_FASTPATH=OFF" in wrapper
    ):
        raise AssertionError("canonical build wrapper does not select the A/B ON")


def must_gate_fail(label: str, callback, needle: str) -> None:
    try:
        callback()
    except RAW_GATE.GateError as exc:
        if needle not in str(exc):
            raise AssertionError(f"{label}: wrong failure: {exc}") from exc
    else:
        raise AssertionError(f"{label}: hostile raw-gate fixture passed")


def verify_raw_gate_scope() -> None:
    feature = RAW_GATE.SYNC_IMPORT_FASTPATH_CACHE_KEY
    guest = (RUNTIME / "guest.c").resolve()
    other = (RUNTIME / "host_vita_sync.c").resolve()
    guest_definitions: dict[str, list[str]] = {}
    other_definitions: dict[str, list[str]] = {}
    records = {
        guest: {
            "source": guest,
            "definitions": guest_definitions,
        },
        other: {
            "source": other,
            "definitions": other_definitions,
        },
    }
    RAW_GATE.verify_sync_import_fastpath_compile_scope(
        records, {feature: "OFF"}
    )
    active = {feature: "ON"}
    guest_definitions[feature] = ["1"]
    RAW_GATE.verify_sync_import_fastpath_compile_scope(records, active)

    guest_definitions[feature] = ["0"]
    must_gate_fail(
        "non-canonical guest definition",
        lambda: RAW_GATE.verify_sync_import_fastpath_compile_scope(
            records, active
        ),
        "non-canonical sync-import definition",
    )
    guest_definitions[feature] = ["1"]
    other_definitions[feature] = ["1"]
    must_gate_fail(
        "definition leaked outside guest.c",
        lambda: RAW_GATE.verify_sync_import_fastpath_compile_scope(
            records, active
        ),
        "compile-definition scope changed",
    )
    other_definitions.clear()
    records[guest]["undefinitions"] = [feature]
    must_gate_fail(
        "guest owner undefines policy",
        lambda: RAW_GATE.verify_sync_import_fastpath_compile_scope(
            records, active
        ),
        "compile policy is undefined",
    )
    records[guest].pop("undefinitions")
    guest_definitions.clear()
    must_gate_fail(
        "active feature lacks owner definition",
        lambda: RAW_GATE.verify_sync_import_fastpath_compile_scope(
            records, active
        ),
        "compile-definition scope changed",
    )


def verify_frozen_constants() -> None:
    header = (RUNTIME / "host_vita_import_id.h").read_text(encoding="utf-8")
    values = {}
    for name, value in re.findall(
        r"^#define\s+(ISAAC_VITA_IMPORT_(?:ID|LOCAL|SLOT)_SYNC_"
        r"(?:ENTER|LEAVE)_CS)\s+(0x[0-9a-f]+|\d+)U$",
        header,
        re.MULTILINE,
    ):
        values[name] = int(value, 0)
    expected = {
        "ISAAC_VITA_IMPORT_ID_SYNC_LEAVE_CS": 60,
        "ISAAC_VITA_IMPORT_ID_SYNC_ENTER_CS": 61,
        "ISAAC_VITA_IMPORT_LOCAL_SYNC_LEAVE_CS": 6,
        "ISAAC_VITA_IMPORT_LOCAL_SYNC_ENTER_CS": 5,
        "ISAAC_VITA_IMPORT_SLOT_SYNC_LEAVE_CS": 0x006060F8,
        "ISAAC_VITA_IMPORT_SLOT_SYNC_ENTER_CS": 0x006060FC,
    }
    if values != expected:
        raise AssertionError(f"named sync constants drifted: {values!r}")
    rows = (RUNTIME / "host_vita_import_id_map.inc").read_text(
        encoding="utf-8"
    ).splitlines()
    expected_rows = {
        60: (
            0x006060F8,
            "KERNEL32.dll!LeaveCriticalSection",
            "ISAAC_VITA_IMPORT_SYNC",
            6,
        ),
        61: (
            0x006060FC,
            "KERNEL32.dll!EnterCriticalSection",
            "ISAAC_VITA_IMPORT_SYNC",
            5,
        ),
    }
    for import_id, (slot, name, kind, local) in expected_rows.items():
        pattern = re.compile(
            rf"^ISAAC_VITA_IMPORT_ID_ROW\({import_id}U, 0x{slot:08x}U, "
            rf'"{re.escape(name)}", {kind}, {local}U\)$'
        )
        if not any(pattern.fullmatch(row) for row in rows):
            raise AssertionError(f"frozen sync map row {import_id} drifted")


def main() -> int:
    verify_build_gate()
    verify_raw_gate_scope()
    verify_frozen_constants()
    env, compiler = BA.msvc_env()
    with tempfile.TemporaryDirectory(
        prefix="isaac-vita-sync-import-fastpath-"
    ) as temporary:
        root = Path(temporary)

        # Default-OFF must preprocess away all trusted routing state/code.
        preprocessed = root / "guest-sync-fastpath-off.i"
        run(
            [
                compiler,
                "/nologo",
                "/P",
                "/std:c11",
                "/wd4310",
                "/wd4996",
                "/DISAAC_VITA_IMPORT_ID_DISPATCH=1",
                "/I",
                str(RUNTIME),
                str(RUNTIME / "guest.c"),
                "/Fi:" + str(preprocessed),
            ],
            env,
        )
        off_text = preprocessed.read_text(encoding="utf-8", errors="replace")
        forbidden = (
            "g_vita_sync_import_fastpath_ready",
            "validated Vita sync import rejected its local ID",
            "isaac_vita_sync_inline_enter",
            "isaac_vita_sync_inline_leave",
            "g_isaac_vita_sync_inline_owner",
        )
        leaked = [token for token in forbidden if token in off_text]
        if leaked:
            raise AssertionError(
                f"default-OFF guest.c retained fast path: {leaked}"
            )

        executable = root / "vita-sync-import-fastpath-oracle.exe"
        run(
            [
                compiler,
                "/nologo",
                "/W4",
                "/WX",
                "/O2",
                "/Gy",
                "/std:c11",
                "/wd4310",
                "/wd4702",
                "/wd4996",
                "/DISAAC_VITA_IMPORT_ID_DISPATCH=1",
                "/DISAAC_VITA_SYNC_IMPORT_FASTPATH=1",
                "/DISAAC_VITA_GUEST_LOOKUP_CACHE=1",
                "/DISAAC_VITA_PHASE_PROFILE=1",
                "/I",
                str(RUNTIME),
                str(RUNTIME / "vita_sync_import_fastpath_oracle.c"),
                "/Fe:" + str(executable),
                "/Fo:" + str(root / "vita-sync-import-fastpath-oracle.obj"),
                "/link",
                "/OPT:REF",
                "/INCREMENTAL:NO",
            ],
            env,
        )
        ran = run([str(executable)], env)
        output = ran.stdout.decode("cp866", errors="replace")
        if PASS_MARKER not in output:
            raise AssertionError(f"unexpected oracle output: {output!r}")

        # The import fast path with the inline single-owner path armed:
        # inline states never reach the endpoint stub, other states do.
        inline_executable = root / "vita-sync-inline-fastpath-oracle.exe"
        run(
            [
                compiler,
                "/nologo",
                "/W4",
                "/WX",
                "/O2",
                "/Gy",
                "/std:c11",
                "/wd4310",
                "/wd4702",
                "/wd4996",
                "/DISAAC_VITA_IMPORT_ID_DISPATCH=1",
                "/DISAAC_VITA_SYNC_IMPORT_FASTPATH=1",
                "/DISAAC_VITA_SYNC_INLINE_FASTPATH=1",
                "/DISAAC_VITA_GUEST_LOOKUP_CACHE=1",
                "/DISAAC_VITA_PHASE_PROFILE=1",
                "/I",
                str(RUNTIME),
                str(RUNTIME / "vita_sync_import_fastpath_oracle.c"),
                "/Fe:" + str(inline_executable),
                "/Fo:" + str(root / "vita-sync-inline-fastpath-oracle.obj"),
                "/link",
                "/OPT:REF",
                "/INCREMENTAL:NO",
            ],
            env,
        )
        ran = run([str(inline_executable)], env)
        output = ran.stdout.decode("cp866", errors="replace")
        if INLINE_PASS_MARKER not in output:
            raise AssertionError(f"unexpected inline oracle output: {output!r}")
    print(PASS_MARKER)
    print(INLINE_PASS_MARKER)
    return 0


if __name__ == "__main__":
    sys.exit(main())
