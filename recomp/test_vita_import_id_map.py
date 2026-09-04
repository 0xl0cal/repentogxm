#!/usr/bin/env python3
"""Static oracle for the checked-in 413-row Vita import-ID contract."""

from __future__ import annotations

import argparse
import collections
import pathlib
import re
import sys


ROOT = pathlib.Path(__file__).resolve().parent
RUNTIME = ROOT / "runtime"
MAP = RUNTIME / "host_vita_import_id_map.inc"

ROW_RE = re.compile(
    r'^ISAAC_VITA_IMPORT_ID_ROW\((\d+)U, (0x[0-9a-f]+)U, '
    r'"([^"]+)", (ISAAC_VITA_IMPORT_[A-Z0-9_]+), (\d+)U\)$'
)
GUEST_ROW_RE = re.compile(
    r'^\s*\{\s*(0x[0-9a-fA-F]+)U,\s*"([^"]+)"\s*\},\s*$'
)
SET_WINDOW_LONG_CALL_BYTES = bytes.fromhex("ff15cc63a000")
SET_WINDOW_LONG_CALL_RVAS = (
    0x00481149, 0x004811B6, 0x00561521,
    0x005616C0, 0x005619D7, 0x00597223,
)
SET_WINDOW_POS_CALL_BYTES = bytes.fromhex("ff15c463a000")
SET_WINDOW_POS_CALL_RVAS = (
    0x0048115D, 0x00561225, 0x00561547, 0x00598382, 0x00598455,
)
FULLSCREEN_WINDOW_REQUEST_RVA = 0x0048112F
FULLSCREEN_WINDOW_REQUEST_BYTES = bytes.fromhex(
    "8b35c863a0006a00ffd66a018bf8ffd668000000906af0538bf0"
    "ff15cc63a0006a20568d4701506a006a006a0053ff15c463a000"
)

EXPECTED_COUNTS = {
    "ISAAC_VITA_IMPORT_UNRESOLVED": 161,
    "ISAAC_VITA_IMPORT_BASELINE": 5,
    "ISAAC_VITA_IMPORT_CRT": 59,
    "ISAAC_VITA_IMPORT_MATH": 21,
    "ISAAC_VITA_IMPORT_LUA": 67,
    "ISAAC_VITA_IMPORT_RTTI": 1,
    "ISAAC_VITA_IMPORT_EXCEPTION": 2,
    "ISAAC_VITA_IMPORT_FILE_LOCK": 2,
    "ISAAC_VITA_IMPORT_FILESYSTEM": 2,
    "ISAAC_VITA_IMPORT_FIND": 3,
    "ISAAC_VITA_IMPORT_COM": 3,
    "ISAAC_VITA_IMPORT_POST_COM": 6,
    "ISAAC_VITA_IMPORT_HEAP": 6,
    "ISAAC_VITA_IMPORT_STEAM": 7,
    "ISAAC_VITA_IMPORT_AUDIO": 24,
    "ISAAC_VITA_IMPORT_GL_WGL": 1,
    "ISAAC_VITA_IMPORT_SYNC": 13,
    "ISAAC_VITA_IMPORT_MEMORY": 6,
    "ISAAC_VITA_IMPORT_CONSOLE": 2,
    "ISAAC_VITA_IMPORT_USER32": 6,
    "ISAAC_VITA_IMPORT_STARTUP": 9,
    "ISAAC_VITA_IMPORT_FLS": 3,
    "ISAAC_VITA_IMPORT_FREAD": 1,
    "ISAAC_VITA_IMPORT_SHARED_LOADER": 3,
}

CONTIGUOUS_LOCAL_COUNTS = {
    "ISAAC_VITA_IMPORT_BASELINE": 5,
    "ISAAC_VITA_IMPORT_MATH": 21,
    "ISAAC_VITA_IMPORT_LUA": 67,
    "ISAAC_VITA_IMPORT_RTTI": 1,
    "ISAAC_VITA_IMPORT_EXCEPTION": 2,
    "ISAAC_VITA_IMPORT_FILE_LOCK": 2,
    "ISAAC_VITA_IMPORT_FILESYSTEM": 2,
    "ISAAC_VITA_IMPORT_FIND": 3,
    "ISAAC_VITA_IMPORT_COM": 3,
    "ISAAC_VITA_IMPORT_POST_COM": 6,
    "ISAAC_VITA_IMPORT_HEAP": 6,
    "ISAAC_VITA_IMPORT_STEAM": 7,
    "ISAAC_VITA_IMPORT_AUDIO": 24,
    "ISAAC_VITA_IMPORT_GL_WGL": 1,
    "ISAAC_VITA_IMPORT_MEMORY": 6,
    "ISAAC_VITA_IMPORT_CONSOLE": 2,
    "ISAAC_VITA_IMPORT_USER32": 6,
    "ISAAC_VITA_IMPORT_FLS": 3,
    "ISAAC_VITA_IMPORT_SHARED_LOADER": 3,
}


def fail(message: str) -> None:
    raise AssertionError(message)


def read_rows() -> list[tuple[int, int, str, str, int]]:
    rows = []
    for line_number, line in enumerate(MAP.read_text(encoding="utf-8").splitlines(), 1):
        if not line.startswith("ISAAC_VITA_IMPORT_ID_ROW"):
            continue
        match = ROW_RE.fullmatch(line)
        if not match:
            fail(f"malformed map row at line {line_number}: {line}")
        row_id, slot, name, kind, local = match.groups()
        rows.append((int(row_id), int(slot, 16), name, kind, int(local)))
    return rows


def verify_contract(rows: list[tuple[int, int, str, str, int]]) -> None:
    if len(rows) != 413:
        fail(f"expected exactly 413 rows, got {len(rows)}")
    if [row[0] for row in rows] != list(range(413)):
        fail("IDs are not the exact dense sequence 0..412")

    slots = [row[1] for row in rows]
    names = [row[2] for row in rows]
    if slots != sorted(slots) or len(slots) != len(set(slots)):
        fail("slot RVAs are duplicate or not strictly increasing")
    if any(slot & 3 for slot in slots):
        fail("frozen IAT contains a non-4-byte-aligned slot")
    if (slots[-1] - slots[0]) // 4 + 1 != 438:
        fail("frozen IAT no longer spans exactly 438 aligned slots")
    if len(names) != len(set(names)):
        duplicates = [
            name for name, count in collections.Counter(names).items()
            if count != 1
        ]
        fail(f"duplicate import names: {duplicates}")

    if rows[0][1:3] != (0x00606000, "ADVAPI32.dll!OpenProcessToken"):
        fail("first frozen import changed")
    if rows[-1][1:3] != (
        0x006066D4, "steam_api.dll!SteamAPI_UnregisterCallback"
    ):
        fail("last frozen import changed")

    counts = collections.Counter(row[3] for row in rows)
    if dict(counts) != EXPECTED_COUNTS:
        fail(f"binding-kind census drifted: {dict(counts)}")

    locals_by_kind: dict[str, list[int]] = collections.defaultdict(list)
    for _, _, _, kind, local in rows:
        locals_by_kind[kind].append(local)
    if set(locals_by_kind["ISAAC_VITA_IMPORT_UNRESOLVED"]) != {0}:
        fail("unresolved rows must use local index zero")
    for kind, count in CONTIGUOUS_LOCAL_COUNTS.items():
        actual = locals_by_kind[kind]
        if sorted(actual) != list(range(count)):
            fail(f"{kind} local indexes are not exactly 0..{count - 1}")

    crt_indexes = (
        locals_by_kind["ISAAC_VITA_IMPORT_CRT"]
        + locals_by_kind["ISAAC_VITA_IMPORT_FREAD"]
    )
    if sorted(crt_indexes) != list(range(60)):
        fail("CRT plus the fread side-effect route are not exact indexes 0..59")
    if sorted(locals_by_kind["ISAAC_VITA_IMPORT_SYNC"]) != [
        0, 1, *range(3, 14)
    ]:
        fail("sync direct indexes drifted around composite GetProcAddress")
    if sorted(locals_by_kind["ISAAC_VITA_IMPORT_STARTUP"]) != [
        1, 2, 3, 5, 6, 7, 8, 9, 10
    ]:
        fail("startup direct indexes drifted around composite loader names")

    shared = {
        name: (row_id, slot, local)
        for row_id, slot, name, kind, local in rows
        if kind == "ISAAC_VITA_IMPORT_SHARED_LOADER"
    }
    if shared != {
        "KERNEL32.dll!LoadLibraryA": (71, 0x00606124, 0),
        "KERNEL32.dll!GetProcAddress": (68, 0x00606118, 1),
        "KERNEL32.dll!FreeLibrary": (63, 0x00606104, 2),
    }:
        fail(f"shared loader composite mapping drifted: {shared}")
    fread = [row for row in rows if row[3] == "ISAAC_VITA_IMPORT_FREAD"]
    if fread != [(
        359, 0x006065E4, "api-ms-win-crt-stdio-l1-1-0.dll!fread",
        "ISAAC_VITA_IMPORT_FREAD", 0
    )]:
        fail("fread side-effect mapping drifted")


def verify_generated_guest_table(
    rows: list[tuple[int, int, str, str, int]], path: pathlib.Path
) -> None:
    generated = []
    for line in path.read_text(encoding="utf-8").splitlines():
        match = GUEST_ROW_RE.fullmatch(line)
        if match:
            generated.append((int(match.group(1), 16), match.group(2)))
    expected = [(row[1], row[2]) for row in rows]
    if generated != expected:
        fail(
            f"{path}: generated guest import table is not the exact 413-row "
            "frozen contract"
        )


def verify_frozen_pe(
    rows: list[tuple[int, int, str, str, int]], path: pathlib.Path
) -> None:
    from iat_meta import read_imports
    import pefile

    expected = [(row[1], row[2]) for row in rows]
    actual = read_imports(str(path))
    if actual != expected:
        fail(
            f"{path}: PE imports are not the exact 413-row frozen contract"
        )

    pe = pefile.PE(str(path), fast_load=True)
    call_rvas = []
    for section in pe.sections:
        if not section.Characteristics & 0x20000000:
            continue
        data = section.get_data()
        at = 0
        while True:
            at = data.find(SET_WINDOW_LONG_CALL_BYTES, at)
            if at < 0:
                break
            call_rvas.append(section.VirtualAddress + at)
            at += 1
    if tuple(call_rvas) != SET_WINDOW_LONG_CALL_RVAS:
        fail(
            "SetWindowLongA direct-call census drifted: "
            f"{[hex(rva) for rva in call_rvas]}"
        )

    call_rvas = []
    for section in pe.sections:
        if not section.Characteristics & 0x20000000:
            continue
        data = section.get_data()
        at = 0
        while True:
            at = data.find(SET_WINDOW_POS_CALL_BYTES, at)
            if at < 0:
                break
            call_rvas.append(section.VirtualAddress + at)
            at += 1
    if tuple(call_rvas) != SET_WINDOW_POS_CALL_RVAS:
        fail(
            "SetWindowPos direct-call census drifted: "
            f"{[hex(rva) for rva in call_rvas]}"
        )
    actual_request = pe.get_data(
        FULLSCREEN_WINDOW_REQUEST_RVA, len(FULLSCREEN_WINDOW_REQUEST_BYTES)
    )
    if actual_request != FULLSCREEN_WINDOW_REQUEST_BYTES:
        fail("OptionsConfig::SetFullScreen window request changed")


def logical_source(path: pathlib.Path) -> str:
    return path.read_text(encoding="utf-8").replace("\\\n", " ")


def braced_block(text: str, marker: str) -> str:
    marker_at = text.find(marker)
    if marker_at < 0:
        fail(f"missing inventory marker: {marker}")
    start = text.find("{", marker_at)
    if start < 0:
        fail(f"missing inventory body after: {marker}")
    depth = 0
    for offset in range(start, len(text)):
        if text[offset] == "{":
            depth += 1
        elif text[offset] == "}":
            depth -= 1
            if depth == 0:
                return text[start + 1:offset]
    fail(f"unterminated inventory body after: {marker}")
    raise AssertionError("unreachable")


def string_name_macros() -> dict[str, str]:
    macros: dict[str, str] = {}
    for path in [*RUNTIME.glob("*.h"), *RUNTIME.glob("*.c")]:
        for match in re.finditer(
            r"^#define\s+([A-Za-z_]\w*)\s+(.+)$",
            logical_source(path), re.MULTILINE,
        ):
            literals = re.findall(
                r'"([^"\\]*(?:\\.[^"\\]*)*)"', match.group(2)
            )
            if not literals:
                continue
            value = "".join(literals)
            previous = macros.get(match.group(1))
            if previous is not None and previous != value:
                fail(f"conflicting string macro {match.group(1)}")
            macros[match.group(1)] = value
    return macros


def explicit_table_tokens(family: str) -> list[str]:
    text = (RUNTIME / f"host_vita_{family}.c").read_text(encoding="utf-8")
    body = braced_block(text, f"s_vita_{family}_imports[]")
    return re.findall(
        r"(?:\[[^\]]+\]\s*=\s*)?\{\s*"
        r"(ISAAC_[A-Z0-9_]+_NAME)\s*,",
        body,
    )


def macro_table_tokens(family: str, macro: str) -> list[str]:
    text = logical_source(RUNTIME / f"host_vita_{family}.h")
    match = re.search(
        rf"^#define\s+{re.escape(macro)}\(X\)\s+(.+)$",
        text, re.MULTILINE,
    )
    if not match:
        fail(f"host_vita_{family}.h: missing inventory macro {macro}")
    return re.findall(
        r"\bX\(\s*[^,]+,\s*(ISAAC_[A-Z0-9_]+_NAME)\s*,",
        match.group(1),
    )


def accessor_array_tokens(family: str, symbol: str) -> list[str]:
    text = (RUNTIME / f"host_vita_{family}.c").read_text(encoding="utf-8")
    symbol_at = text.find(symbol)
    if symbol_at < 0:
        fail(f"host_vita_{family}.c: missing {symbol}")
    body = braced_block(text[symbol_at:], "static const char *const names")
    return re.findall(r"\b(ISAAC_[A-Z0-9_]+_NAME)\b", body)


def verify_actual_family_bindings(
    rows: list[tuple[int, int, str, str, int]]
) -> None:
    macros = string_name_macros()
    inventories: dict[str, list[str]] = {
        family: explicit_table_tokens(family)
        for family in (
            "com", "console", "crt", "file_lock", "find", "fls",
            "heap", "memory", "post_com", "startup", "sync", "user32",
        )
    }
    inventories.update({
        "filesystem": macro_table_tokens(
            "filesystem", "ISAAC_VITA_FILESYSTEM_IMPORTS"
        ),
        "math": macro_table_tokens("math", "ISAAC_VITA_MATH_PROVEN_IMPORTS"),
        "audio": macro_table_tokens("audio", "ISAAC_VITA_AUDIO_IMPORTS"),
        "baseline": accessor_array_tokens(
            "first_fault", "isaac_vita_baseline_import_name"
        ),
        "exception": accessor_array_tokens(
            "exception", "isaac_vita_exception_import_name"
        ),
        "steam": accessor_array_tokens(
            "steam", "isaac_vita_steam_import_name"
        ),
        "rtti": ["ISAAC_VITA_RTTI_IMPORT_NAME"],
        "gl_wgl": ["ISAAC_VITA_GL_WGL_GET_PROC_NAME"],
    })
    family_kind = {
        "baseline": "ISAAC_VITA_IMPORT_BASELINE",
        "crt": "ISAAC_VITA_IMPORT_CRT",
        "math": "ISAAC_VITA_IMPORT_MATH",
        "rtti": "ISAAC_VITA_IMPORT_RTTI",
        "exception": "ISAAC_VITA_IMPORT_EXCEPTION",
        "file_lock": "ISAAC_VITA_IMPORT_FILE_LOCK",
        "filesystem": "ISAAC_VITA_IMPORT_FILESYSTEM",
        "find": "ISAAC_VITA_IMPORT_FIND",
        "com": "ISAAC_VITA_IMPORT_COM",
        "post_com": "ISAAC_VITA_IMPORT_POST_COM",
        "heap": "ISAAC_VITA_IMPORT_HEAP",
        "steam": "ISAAC_VITA_IMPORT_STEAM",
        "audio": "ISAAC_VITA_IMPORT_AUDIO",
        "gl_wgl": "ISAAC_VITA_IMPORT_GL_WGL",
        "sync": "ISAAC_VITA_IMPORT_SYNC",
        "memory": "ISAAC_VITA_IMPORT_MEMORY",
        "console": "ISAAC_VITA_IMPORT_CONSOLE",
        "user32": "ISAAC_VITA_IMPORT_USER32",
        "startup": "ISAAC_VITA_IMPORT_STARTUP",
        "fls": "ISAAC_VITA_IMPORT_FLS",
    }
    expected_inventory_counts = {
        "baseline": 5, "crt": 60, "math": 21, "rtti": 1,
        "exception": 2, "file_lock": 2, "filesystem": 2, "find": 3,
        "com": 3, "post_com": 6, "heap": 6, "steam": 7,
        "audio": 24, "gl_wgl": 1, "sync": 14, "memory": 6,
        "console": 2, "user32": 6, "startup": 11, "fls": 3,
    }
    binding_rows = {(kind, local): name for _, _, name, kind, local in rows}
    shared_names = {
        name for _, _, name, kind, _ in rows
        if kind == "ISAAC_VITA_IMPORT_SHARED_LOADER"
    }

    for family, tokens in inventories.items():
        if len(tokens) != expected_inventory_counts[family]:
            fail(
                f"actual {family} inventory has {len(tokens)} entries; "
                f"expected {expected_inventory_counts[family]}"
            )
        names = []
        for token in tokens:
            if token not in macros:
                fail(f"actual {family} inventory has unresolved macro {token}")
            names.append(macros[token])
        if len(names) != len(set(names)):
            fail(f"actual {family} inventory contains duplicate names")

        for local, name in enumerate(names):
            kind = family_kind[family]
            if family == "crt" and local == 0:
                kind = "ISAAC_VITA_IMPORT_FREAD"
            if (family, local) in {("startup", 0), ("startup", 4),
                                  ("sync", 2)}:
                if name not in shared_names:
                    fail(
                        f"actual {family}[{local}]={name} is not composite"
                    )
                continue
            mapped = binding_rows.get((kind, local))
            if mapped != name:
                fail(
                    f"actual {family}[{local}]={name} maps to {mapped!r}"
                )


def macro_string(path: pathlib.Path, name: str) -> str:
    text = path.read_text(encoding="utf-8").replace("\\\n", " ")
    match = re.search(rf"^#define\s+{re.escape(name)}\s+(.+)$", text, re.MULTILINE)
    if not match:
        fail(f"{path.name}: missing string macro {name}")
    literals = re.findall(r'"([^"]*)"', match.group(1))
    if not literals:
        fail(f"{path.name}: {name} is not a string macro")
    return "".join(literals)


def verify_overlap_census(rows: list[tuple[int, int, str, str, int]]) -> None:
    kind_to_family = {
        "ISAAC_VITA_IMPORT_BASELINE": "baseline",
        "ISAAC_VITA_IMPORT_CRT": "crt",
        "ISAAC_VITA_IMPORT_MATH": "math",
        "ISAAC_VITA_IMPORT_RTTI": "rtti",
        "ISAAC_VITA_IMPORT_EXCEPTION": "exception",
        "ISAAC_VITA_IMPORT_FILE_LOCK": "file_lock",
        "ISAAC_VITA_IMPORT_FILESYSTEM": "filesystem",
        "ISAAC_VITA_IMPORT_FIND": "find",
        "ISAAC_VITA_IMPORT_COM": "com",
        "ISAAC_VITA_IMPORT_POST_COM": "post_com",
        "ISAAC_VITA_IMPORT_HEAP": "heap",
        "ISAAC_VITA_IMPORT_STEAM": "steam",
        "ISAAC_VITA_IMPORT_AUDIO": "audio",
        "ISAAC_VITA_IMPORT_GL_WGL": "gl",
        "ISAAC_VITA_IMPORT_SYNC": "sync",
        "ISAAC_VITA_IMPORT_MEMORY": "memory",
        "ISAAC_VITA_IMPORT_CONSOLE": "console",
        "ISAAC_VITA_IMPORT_USER32": "user32",
        "ISAAC_VITA_IMPORT_STARTUP": "startup",
        "ISAAC_VITA_IMPORT_FLS": "fls",
        "ISAAC_VITA_IMPORT_FREAD": "crt",
    }
    memberships: dict[str, set[str]] = collections.defaultdict(set)
    kinds = {}
    for _, _, name, kind, _ in rows:
        kinds[name] = kind
        family = kind_to_family.get(kind)
        if family:
            memberships[name].add(family)

    gl_header = RUNTIME / "host_vita_gl.h"
    xinput_header = RUNTIME / "host_vita_xinput.h"
    special_macros = {
        "ISAAC_VITA_GL_LOAD_LIBRARY_NAME": (gl_header, "gl"),
        "ISAAC_VITA_GL_GET_PROC_NAME": (gl_header, "gl"),
        "ISAAC_VITA_GL_FREE_LIBRARY_NAME": (gl_header, "gl"),
        "ISAAC_VITA_XINPUT_LOAD_LIBRARY_NAME": (xinput_header, "xinput"),
        "ISAAC_VITA_XINPUT_GET_PROC_NAME": (xinput_header, "xinput"),
        "ISAAC_VITA_XINPUT_FREE_LIBRARY_NAME": (xinput_header, "xinput"),
    }
    for macro, (path, family) in special_macros.items():
        memberships[macro_string(path, macro)].add(family)

    # The actual contiguous local tables contain these composite entries;
    # direct map rows cover all of their remaining indexes.
    memberships["KERNEL32.dll!GetProcAddress"].add("sync")
    memberships["KERNEL32.dll!FreeLibrary"].add("startup")
    memberships["KERNEL32.dll!LoadLibraryA"].add("startup")

    expected_overlaps = {
        "KERNEL32.dll!LoadLibraryA": {"gl", "startup", "xinput"},
        "KERNEL32.dll!GetProcAddress": {"gl", "sync", "xinput"},
        "KERNEL32.dll!FreeLibrary": {"gl", "startup", "xinput"},
    }
    actual_overlaps = {
        name: families for name, families in memberships.items()
        if len(families) > 1
    }
    if actual_overlaps != expected_overlaps:
        fail(f"cross-family import-name overlap census drifted: {actual_overlaps}")
    for name, families in memberships.items():
        if name not in kinds:
            fail(f"family inventory name is absent from PE contract: {name}")
        if len(families) > 1:
            if kinds[name] != "ISAAC_VITA_IMPORT_SHARED_LOADER":
                fail(f"overlap {name} is not mapped to composite dispatch")
        elif kinds[name] == "ISAAC_VITA_IMPORT_SHARED_LOADER":
            fail(f"composite mapping {name} has no actual family overlap")

    sync = (RUNTIME / "host_vita_sync.c").read_text(encoding="utf-8")
    startup = (RUNTIME / "host_vita_startup.c").read_text(encoding="utf-8")
    if not re.search(
        r"s_vita_sync_imports\[\].*?\{\s*ISAAC_VITA_SYNC_GET_PROC_NAME,",
        sync, re.DOTALL,
    ):
        fail("actual sync table no longer contains composite GetProcAddress")
    for macro in (
        "ISAAC_VITA_STARTUP_FREE_LIBRARY_NAME",
        "ISAAC_VITA_STARTUP_LOAD_LIBRARY_NAME",
    ):
        if not re.search(
            rf"s_vita_startup_imports\[\].*?\{{\s*{macro},",
            startup, re.DOTALL,
        ):
            fail(f"actual startup table no longer contains {macro}")


def verify_runtime_wiring() -> None:
    header = (RUNTIME / "host_vita_import_id.h").read_text(encoding="utf-8")
    source = (RUNTIME / "host_vita_import_id.c").read_text(encoding="utf-8")
    guest = (RUNTIME / "guest.c").read_text(encoding="utf-8")
    cmake = (ROOT / "vita" / "CMakeLists.txt").read_text(encoding="utf-8")

    for marker in (
        "guest_host_import_ids_register(imports, n)",
        "guest_host_import_id(c, import_id)",
        "guest_host_import_id_error()",
    ):
        if marker not in guest:
            fail(f"guest runtime is missing dense-dispatch edge: {marker}")
    if guest.count("#ifdef ISAAC_VITA_IMPORT_ID_DISPATCH") != 3:
        fail("guest dense-ID include/register/call edges are not source-scoped")
    if '"${ISAAC_RUNTIME}/host_vita_import_id.c"' not in cmake:
        fail("Vita target does not compile host_vita_import_id.c")
    if "ISAAC_VITA_IMPORT_ID_DISPATCH=1" not in cmake:
        fail("production guest.c does not enable its source-scoped ID path")
    if "#include \"host_vita_import_id_map.inc\"" not in source:
        fail("runtime dispatcher does not consume the checked-in map")
    if "family_inventories_and_overlaps_are_valid()" not in source:
        fail("runtime registration does not validate actual family overlaps")

    families = {
        "baseline": "host_vita_first_fault.c",
        "crt": "host_vita_crt.c",
        "math": "host_vita_math.c",
        "rtti": "host_vita_rtti.c",
        "exception": "host_vita_exception.c",
        "file_lock": "host_vita_file_lock.c",
        "filesystem": "host_vita_filesystem.c",
        "find": "host_vita_find.c",
        "com": "host_vita_com.c",
        "post_com": "host_vita_post_com.c",
        "heap": "host_vita_heap.c",
        "steam": "host_vita_steam.c",
        "audio": "host_vita_audio.c",
        "gl_wgl": "host_vita_gl.c",
        "sync": "host_vita_sync.c",
        "memory": "host_vita_memory.c",
        "console": "host_vita_console.c",
        "user32": "host_vita_user32.c",
        "startup": "host_vita_startup.c",
        "fls": "host_vita_fls.c",
    }
    for family, filename in families.items():
        name_symbol = f"isaac_vita_{family}_import_name"
        indexed_symbol = f"isaac_vita_{family}_import_indexed"
        module = (RUNTIME / filename).read_text(encoding="utf-8")
        for symbol in (name_symbol, indexed_symbol):
            if symbol not in header or symbol not in source or symbol not in module:
                fail(f"runtime-validated binding is missing {symbol}")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--guest-table", type=pathlib.Path,
        help="optional generated guest_table.c to compare row-for-row",
    )
    parser.add_argument(
        "--pe", type=pathlib.Path,
        help="optional frozen PE to extract with iat_meta and compare row-for-row",
    )
    args = parser.parse_args()

    try:
        rows = read_rows()
        verify_contract(rows)
        verify_actual_family_bindings(rows)
        verify_overlap_census(rows)
        verify_runtime_wiring()
        if args.guest_table:
            verify_generated_guest_table(rows, args.guest_table)
        if args.pe:
            verify_frozen_pe(rows, args.pe)
    except (AssertionError, OSError, ValueError) as exc:
        print(f"FAIL: {exc}", file=sys.stderr)
        return 1
    suffix = ""
    if args.guest_table:
        suffix += " + generated table"
    if args.pe:
        suffix += " + frozen PE"
    print(
        f"PASS: exact 413-row Vita import-ID contract{suffix}; "
        "cross-family overlaps=3, all composite"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
