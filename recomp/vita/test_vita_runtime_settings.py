#!/usr/bin/env python3
"""Compile/run the shared port.ini parser oracle and pin its source contract.

Runtime settings (knob
ISAAC_VITA_RUNTIME_SETTINGS) start with one pure-C translation unit shared by
the game eboot and the LiveArea manager: recomp/runtime/host_vita_settings_parse.c.
This test

  * compiles recomp/runtime/host_vita_settings_oracle.c with the parser TU
    (-std=c11 -O2 -Wall -Wextra -Werror) and runs it (grammar corpus, every
    key value, guard, serialize round trip, receipt fields);
  * syntax-checks the parser TU alone with -Wundef and the header alone as a
    standalone C file (both must compile in the runtime and in the manager);
  * compiles the parser TU to an object (-fno-pie -c) and, when nm is
    available, asserts it defines no .data/.bss symbol (the observable form
    of "no mutable static state"); prints an explicit skip line otherwise;
  * when $VITASDK/bin/arm-vita-eabi-gcc exists, compiles the parser TU and
    the oracle TU with the test_kage_vita_input.sh production flags
    (-std=gnu11 -O2 -mcpu=cortex-a9 -mfpu=neon -mfloat-abi=softfp -mthumb
    -Wall -Wextra -Werror) and asserts the parser object's undefined symbols
    are only memcmp/memcpy/memset/strlen/strncmp/__aeabi_*; prints
    "ARM gate skipped: no VITASDK" otherwise (never silently);
  * pins the source contract of this slice: the parser TU has no stdio /
    stdlib include, no allocator token (poison list of
    recomp/vita/vita_raw_allocator_gate.py), no SDK header and no mutable
    static state (any `static` declarator that is not `const`, at any
    indentation, and no `static const T *p` mutable pointer); the only knob
    the TU and header test is ISAAC_VITA_DEBUG_OVERLAY (the debug_overlay
    key, every mention fenced, layout-dependent entry points renamed with a
    _dbgov suffix under it so a mismatched includer fails to link), whose own
    oracle host_vita_settings_debug_overlay_oracle.c runs the parser and the
    loader with the knob; host_tests/CMake names the oracle target.

Compiler discovery: env CC, then cc/gcc/clang in PATH, then the LLVM clang
locations used by test_kage_vita_fullspeed_scheduler.py.  Without any of
them the test fails (it never skips).  On a Windows host without clang run it
through WSL, e.g.
  wsl -d Ubuntu-22.04 -- bash -lc 'cd /mnt/d/<repo> && python3 recomp/vita/test_vita_runtime_settings.py'

The runtime half (host_vita_runtime_settings.c, the scheduler duty term,
the CMake option block, the build wrapper flag, the allocator-gate owner set)
adds, in the same run:

  * the loader oracle recomp/runtime/host_vita_runtime_settings_oracle.c,
    compiled twice with the loader TU and the parser TU
    (-DISAAC_VITA_RUNTIME_SETTINGS=1 -DISAAC_VITA_RUNTIME_SETTINGS_ORACLE=1,
    with and without -DISAAC_VITA_FULLSPEED_SCHEDULER=1): the
    loader's receipt goldens for every status, chunked/failing reads, the a/b guard,
    the REJECTED line and the duty pushes; the loader TU and header
    preprocess to nothing without the knob (-Wundef clean either way);
  * source-contract pins: every ISAAC_VITA_RUNTIME_SETTINGS mention in the
    two pre-existing TUs (entry_vita.c, kage_vita_fullspeed_scheduler.c)
    sits inside an `#if defined(ISAAC_VITA_RUNTIME_SETTINGS)` region with
    today's text in #else or absent; the duty term is the only change to
    the plan_render skip statement, clear() never touches s_duty_k, the
    banner suffix follows the pinned "parity=4a264 frame=1a30dc" substring
    with its argument last; the entry hook sits between the null-PE check
    and guest_register_all_imports(); CMake names the two new TUs and the
    =1 definition only inside if(ISAAC_VITA_RUNTIME_SETTINGS), placed after
    the two pinned scheduler/stable30 windows, with the nested
    ISAAC_VITA_FULLSPEED_SCHEDULER definition for the loader inside both
    if()s; the CMake owner list equals the allocator gate's
    RUNTIME_SETTINGS_DEFINITION_OWNERS; tools/build_vita.py exposes
    --runtime-settings (default False, requires --kage).

The manager core (manager_settings.c/.h, the fenced text primitives of
manager_storage.c/.h, host_tests/manager_settings_host_test with the shared
fake filesystem fake_fs.h) adds:

  * source-contract pins: every MANAGER_SETTINGS_UI mention in
    manager_storage.c/.h sits inside `#if defined(MANAGER_SETTINGS_UI)` (so
    manager_storage.o is byte-identical when the manager knob is OFF);
    manager_settings.c/.h have no SDK include (no <psp2/...>), no allocator,
    no stdio beyond snprintf, and preprocess to nothing without the knob;
    every string literal of manager_settings.c containing ':' is a log
    search key (status strings never carry a colon); host_tests/CMake
    builds manager_settings_host_test from the four TUs with
    MANAGER_SETTINGS_UI=1 -Werror and both storage tests include fake_fs.h;
    recomp/vita/CMakeLists.txt names the manager settings TUs / definition
    only inside if(ISAAC_VITA_MANAGER_SETTINGS_UI);
  * host build+run of manager_settings_host_test and of the unchanged
    manager_storage_host_test with the host compiler;
  * ARM (when VITASDK is present): manager_storage.c and manager_settings.c
    with -DMANAGER_SETTINGS_UI=1 under the manager's production flags.

The manager UI (manager_settings_ui.c/.h, the fenced manager_main.c edits,
the ISAAC_VITA_MANAGER_SETTINGS_UI option, host_tests/
manager_settings_ui_host_test, recomp/vita/test_manager_host.py) adds:

  * source-contract pins: the page model TU/header declare nothing without
    the knob, are SDK-free, allocator-free and snprintf-only; every literal
    they emit uses the manager_draw_text glyph set (A-Z 0-9 space : . - _
    > < ?) and every status literal the manager_display_text set (A-Z 0-9
    space . - _, no ':'); every MANAGER_SETTINGS_UI mention and every UI
    identifier in manager_main.c sits inside an `#if defined()` region and
    the #include set outside those regions is today's (so the OFF manager
    object is byte-identical); production CMake defaults the option OFF,
    FATALs without ISAAC_VITA_RUNTIME_SETTINGS and adds the units to
    isaac_manager through target_sources() only inside if(option), after
    the add_executable/target_link_libraries text pinned by
    test_livearea_package.py; the parser TU may appear in that block (the
    runtime-block pin counts it); tools/build_vita.py exposes
    --manager-settings-ui (default False, requires --runtime-settings);
    the allocator gate reads the option as a cache key that affects no
    eboot source and rejects its definition in any eboot record;
  * host build+run of manager_settings_ui_host_test; the UI TU and header
    -Wundef clean with and without the knob, the TU empty without it.
"""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import tempfile
from pathlib import Path
from typing import Callable

PARSER_HEADER = "host_vita_settings_parse.h"
PARSER_SOURCE = "host_vita_settings_parse.c"
ORACLE_SOURCE = "host_vita_settings_oracle.c"
RUNTIME_HEADER = "host_vita_runtime_settings.h"
RUNTIME_SOURCE = "host_vita_runtime_settings.c"
RUNTIME_ORACLE_SOURCE = "host_vita_runtime_settings_oracle.c"
SCHEDULER_HEADER = "kage_vita_fullspeed_scheduler.h"
SCHEDULER_SOURCE = "kage_vita_fullspeed_scheduler.c"
ENTRY_SOURCE = "entry_vita.c"
KNOB = "ISAAC_VITA_RUNTIME_SETTINGS"
KNOB_GUARD = f"#if defined({KNOB})"
# On-screen debug overlay: the only knob the shared parser may test (it adds
# the debug_overlay key); its oracle runs the parser and the loader with it.
OVERLAY_KNOB = "ISAAC_VITA_DEBUG_OVERLAY"
OVERLAY_KNOB_GUARD = f"#if defined({OVERLAY_KNOB})"
OVERLAY_ORACLE_SOURCE = "host_vita_settings_debug_overlay_oracle.c"
OVERLAY_RENAMED_FUNCTIONS = (
    "isaac_vita_settings_defaults", "isaac_vita_settings_parse",
    "isaac_vita_settings_validate_remap", "isaac_vita_settings_reset_remap",
    "isaac_vita_settings_serialize", "isaac_vita_settings_equal",
    "isaac_vita_settings_key_name", "isaac_vita_settings_key_kind",
    "isaac_vita_settings_key_from_text",
    "isaac_vita_settings_key_accepts_element",
    "isaac_vita_settings_get_text", "isaac_vita_settings_set_text",
)
# options.ini MaxRenderScale normalizer (owner decision 2026-09-16: the
# render_scale key, default 1, is written into the game's options.ini at
# boot); nested in the runtime-settings CMake block and the loader.
RENDER_SCALE_KNOB = "ISAAC_VITA_RENDER_SCALE_DEFAULT"
RENDER_SCALE_GUARD = f"#if defined({RENDER_SCALE_KNOB})"
RENDER_SCALE_SOURCE = "host_vita_render_scale.c"
RENDER_SCALE_HEADER = "host_vita_render_scale.h"
RENDER_SCALE_ORACLE_SOURCE = "host_vita_render_scale_oracle.c"
RENDER_SCALE_ORACLE_TARGET = "host_vita_render_scale_oracle"
# Manager half: the per-target definition the
# manager receives when ISAAC_VITA_MANAGER_SETTINGS_UI is ON.
MANAGER_KNOB = "MANAGER_SETTINGS_UI"
MANAGER_KNOB_GUARD = f"#if defined({MANAGER_KNOB})"
MANAGER_CMAKE_OPTION = "ISAAC_VITA_MANAGER_SETTINGS_UI"
MANAGER_SETTINGS_SOURCE = "manager_settings.c"
MANAGER_SETTINGS_HEADER = "manager_settings.h"
MANAGER_STORAGE_SOURCE = "manager_storage.c"
MANAGER_STORAGE_HEADER = "manager_storage.h"
MANAGER_SHA256_SOURCE = "manager_sha256.c"
FAKE_FS_HEADER = "fake_fs.h"
MANAGER_SETTINGS_TEST = "manager_settings_host_test.c"
MANAGER_STORAGE_TEST = "manager_storage_host_test.c"
# String literals of manager_settings.c that may carry ':' (log search
# keys); every other literal is a status string for manager_status.
MANAGER_COLON_LITERALS = frozenset(("ISAAC VITA SETTINGS: ",))
# Manager UI: the page model and the fenced
# manager_main.c edits.
MANAGER_UI_SOURCE = "manager_settings_ui.c"
MANAGER_UI_HEADER = "manager_settings_ui.h"
MANAGER_MAIN_SOURCE = "manager_main.c"
MANAGER_UI_TEST = "manager_settings_ui_host_test.c"
MANAGER_UI_TARGET = "manager_settings_ui_host_test"
MANAGER_HOST_RUNNER = "test_manager_host.py"
# The #include lines of manager_main.c outside every knob region: today's
# set, so the OFF build sees exactly the headers it saw before the settings page.
MANAGER_MAIN_BASELINE_INCLUDES = frozenset((
    '#include "manager_ftp_server.h"',
    '#include "manager_storage.h"',
    '#include "manager_storage_vita.h"',
    "#include <psp2/appmgr.h>",
    "#include <psp2/ctrl.h>",
    "#include <psp2/display.h>",
    "#include <psp2/kernel/sysmem.h>",
    "#include <stdint.h>",
    "#include <stdio.h>",
    "#include <string.h>",
))
# Identifiers the manager UI introduces into manager_main.c; each may
# appear only on a line inside a MANAGER_SETTINGS_UI region.
MANAGER_MAIN_UI_TOKENS = (
    "manager_settings_", "MANAGER_SETTINGS_", "MANAGER_PAGE_SETTINGS",
    "manager_draw_settings", "START: SETTINGS",
)
# Glyph sets of the two manager text paths: rows and legend go
# through manager_draw_text, the status line through manager_display_text.
MANAGER_WIDE_GLYPHS = frozenset("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 :.-_><?")
MANAGER_NARROW_GLYPHS = frozenset("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789 .-_")
# Status literals of the page model that the host test drives; pinned here
# so a rewording that adds ':' or a lowercase letter fails before the build.
MANAGER_UI_STATUS_LITERALS = (
    "LR CHANGES VALUE  X SAVES PORT.INI",
    "PORT.INI IO ERROR - EDITS REFUSED",
    "CANCELLED - NOTHING WRITTEN",
    "UNSAVED - O AGAIN TO DISCARD",
    "CHANGES DISCARDED",
    "TRI AGAIN - ARCHIVE PORT.INI AND USE DEFAULTS",
    "GAME BUILD IGNORES PORT.INI",
    "FPS LOCK %u - UNEVEN CADENCE - X TWICE",
    "FPS LOCK %u - X SAVES",
    "ROOM RENDER %s - X SAVES",
    "UNEVEN CADENCE - X AGAIN TO SAVE %u",
    "X RESTORES PORT-%04u.INI",
)
MANAGER_UI_ROW_LITERALS = (
    "%c FPS LOCK            %-18.18s%.7s",
    "  40 45 - UNEVEN CADENCE 30 UPS",
    "%c ROOM RENDER         %-18.18s%.7s",
    "%c UNDO                %-18.18sX RESTORE",
    "  LAST BOOT: %.54s",
    "  PENDING: %u ROW%s NOT SAVED",
)
MANAGER_UI_LEGEND = "UD ROW  LR VALUE  X SAVE  TRI DEFAULTS  O BACK"
MANAGER_UI_TITLE = "ISAAC SETTINGS"
# Production flags of the isaac_manager target (CMakeLists.txt).
ARM_MANAGER_FLAGS = [
    "-std=gnu11", "-O2", "-mcpu=cortex-a9", "-mfpu=neon",
    "-mfloat-abi=softfp", "-mthumb", "-ffunction-sections",
    "-fdata-sections", "-Wall", "-Wextra", "-Werror",
]
# CMake owner list of the per-source definition.
RUNTIME_SETTINGS_OWNERS = frozenset((
    ENTRY_SOURCE, SCHEDULER_SOURCE, PARSER_SOURCE, RUNTIME_SOURCE,
))
# Symbols the loader object may import on ARM besides string.h / __aeabi_*.
LOADER_ALLOWED_UNDEFINED = {
    "memcmp", "memcpy", "memset", "strlen",
    "sceIoOpen", "sceIoRead", "sceIoClose", "isaac_vita_log",
    "kage_vita_fullspeed_scheduler_set_duty",
}

# Allocator poison list mirrored from vita_raw_allocator_gate.py RAW_SYMBOLS.
ALLOCATOR_TOKENS = (
    "malloc", "calloc", "realloc", "free", "memalign", "aligned_alloc",
    "strdup",
)

# The only symbols the parser object may import (string.h plus the ARM EABI
# helpers gcc emits for memcpy/memset lowering).
PARSER_ALLOWED_UNDEFINED = {"memcmp", "memcpy", "memset", "strlen", "strncmp"}

# Production flags of test_kage_vita_input.sh common_flags.
ARM_COMMON_FLAGS = [
    "-std=gnu11", "-O2", "-mcpu=cortex-a9", "-mfpu=neon",
    "-mfloat-abi=softfp", "-mthumb", "-fno-strict-aliasing",
    "-ffunction-sections", "-fdata-sections", "-Wall", "-Wextra", "-Werror",
]


def compiler() -> str:
    override = os.environ.get("CC")
    if override:
        found = shutil.which(override)
        if found:
            return found
        if Path(override).is_file():
            return override
        raise RuntimeError(f"CC={override!r} is not an executable")
    for name in ("cc", "gcc", "clang"):
        found = shutil.which(name)
        if found:
            return found
    candidates = (
        Path(os.environ.get("ProgramFiles", r"C:\Program Files")) /
        "LLVM" / "bin" / "clang.exe",
        Path(os.environ.get("ProgramFiles", r"C:\Program Files")) /
        "Microsoft Visual Studio" / "2022" / "Community" / "VC" /
        "Tools" / "Llvm" / "x64" / "bin" / "clang.exe",
    )
    for candidate in candidates:
        if candidate.is_file():
            return str(candidate)
    raise RuntimeError(
        "no host C compiler found (set CC, or run under WSL python3)"
    )


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


def _strip_comments(text: str) -> str:
    return re.sub(r"/\*.*?\*/", "", text, flags=re.DOTALL)


def verify_parser_contract(root: Path) -> None:
    """The shared TU stays pure C11: no stdio/stdlib/SDK, no allocator, no
    mutable static state; the header has no knob conditionals."""
    runtime = root / "recomp" / "runtime"
    source = (runtime / PARSER_SOURCE).read_text(encoding="utf-8")
    header = (runtime / PARSER_HEADER).read_text(encoding="utf-8")
    code = _strip_comments(source)
    header_code = _strip_comments(header)

    for text, name in ((code, PARSER_SOURCE), (header_code, PARSER_HEADER)):
        if re.search(r"#\s*include\s*<std(io|lib)\.h>", text):
            raise AssertionError(f"{name} must not include stdio.h/stdlib.h")
        if re.search(r"#\s*include\s*<(psp2|vitasdk|vitaGL|ctype\.h)", text):
            raise AssertionError(f"{name} must not include SDK/ctype headers")
        for token in ALLOCATOR_TOKENS:
            if re.search(rf"\b{token}\s*\(", text):
                raise AssertionError(f"{name} calls poisoned {token}()")
        if re.search(r"\b(printf|snprintf|fprintf|puts|sceClib\w+)\s*\(", text):
            raise AssertionError(f"{name} must not format or print")
    includes = re.findall(r"#\s*include\s*[<\"]([^>\"]+)[>\"]", code)
    allowed = {PARSER_HEADER, "string.h", "stddef.h", "stdint.h"}
    if set(includes) - allowed:
        raise AssertionError(
            f"{PARSER_SOURCE} includes outside the allowed set: "
            f"{sorted(set(includes) - allowed)}"
        )
    for line in code.splitlines():
        if not re.match(r"\s*static\b", line):
            continue
        # A function definition/prototype has its '(' before any '=', ';'
        # or '{'; everything else is a data declarator (file scope or
        # function-local, any indentation).
        head = re.split(r"[=;{]", line, maxsplit=1)[0]
        if "(" in head:
            continue
        if not re.match(r"\s*static\s+const\b", line):
            raise AssertionError(
                f"{PARSER_SOURCE} has mutable static state: {line!r}"
            )
        # `static const char *p` is a mutable pointer to const; every '*'
        # in the declarator must be followed by `const`.
        if re.search(r"\*\s*(?!const\b)[A-Za-z_]", head):
            raise AssertionError(
                f"{PARSER_SOURCE} has a mutable static pointer: {line!r}"
            )
    directives = re.findall(r"^\s*#\s*(?:if|ifdef|ifndef|elif)\b.*$",
                            header_code, flags=re.MULTILINE)
    # The one knob the shared TU may test: ISAAC_VITA_DEBUG_OVERLAY adds the
    # debug_overlay key (the TU is in the release eboot, which must stay
    # byte-identical with the overlay compiled out).
    allowed_directives = {
        "#ifndef HOST_VITA_SETTINGS_PARSE_H",
        "#ifdef __cplusplus",
        OVERLAY_KNOB_GUARD,
    }
    for directive in directives:
        if directive.strip() not in allowed_directives:
            raise AssertionError(
                f"{PARSER_HEADER} carries a conditional: {directive.strip()!r}"
            )
    if not directives:
        raise AssertionError(f"{PARSER_HEADER} lost its include guard")
    for text, name in ((code, PARSER_SOURCE), (header_code, PARSER_HEADER)):
        for directive in re.findall(
                r"^\s*#\s*(?:if|ifdef|ifndef|elif)\b.*ISAAC_VITA_.*$", text,
                flags=re.MULTILINE):
            if directive.strip() != OVERLAY_KNOB_GUARD:
                raise AssertionError(
                    f"{name} tests a knob macro other than "
                    f"{OVERLAY_KNOB}: {directive.strip()!r}")
        # every mention of the overlay knob sits in its #if defined() region
        _knob_mentions_fenced(text, name, OVERLAY_KNOB)
    # A TU that disagrees on the macro must fail to link, not mismatch the
    # struct: every layout-dependent entry point is renamed under the knob.
    for function in OVERLAY_RENAMED_FUNCTIONS:
        pattern = (rf"#define {function}\s*(?:\\\s*)?{function}_dbgov\b")
        if not re.search(pattern, header_code):
            raise AssertionError(
                f"{PARSER_HEADER} lost the ODR rename of {function}")
    for needle in ('{ "debug_overlay", ISAAC_VITA_SETTINGS_KIND_DEBUG_OVERLAY }',
                   '{ "off", "compact", "full" }'):
        if code.count(needle) != 1:
            raise AssertionError(f"{PARSER_SOURCE} lost {needle!r}")
    if ("#if defined(ISAAC_VITA_DEBUG_OVERLAY)\n"
            "#define ISAAC_VITA_SETTINGS_SERIALIZED_MAX 365u\n#else\n"
            "#define ISAAC_VITA_SETTINGS_SERIALIZED_MAX 343u\n#endif") \
            not in header_code:
        raise AssertionError("SERIALIZED_MAX pins drifted (343 / 365)")
    if "ISAAC_VITA_SETTINGS_MAX_BYTES 4096u" not in header:
        raise AssertionError("port.ini hard cap must stay 4096 bytes")
    if "ISAAC_VITA_SETTINGS_KEY_MAX 31u" not in header or \
            "ISAAC_VITA_SETTINGS_VALUE_MAX 15u" not in header:
        raise AssertionError("key/value length limits drifted from the grammar")
    for key in (
            "schema", "preset", "fps_lock", "display_raster", "render_scale",
            "button.cross", "button.circle", "button.square",
            "button.triangle", "button.l", "button.r", "button.select",
            "chord.start_l", "chord.start_r", "chord.restart_hold",
            "touch.top_left", "touch.top_right", "touch.bottom_right",
            "stick.swap"):
        if f'"{key}"' not in source:
            raise AssertionError(f"{PARSER_SOURCE} lost key {key!r}")


def verify_host_tests_cmake(root: Path) -> None:
    """host_tests/CMakeLists.txt builds and registers the oracle."""
    cmake = (root / "recomp" / "vita" / "host_tests" /
             "CMakeLists.txt").read_text(encoding="utf-8")
    begin = cmake.find("add_executable(host_vita_settings_oracle")
    if begin < 0:
        raise AssertionError("host_tests lost add_executable(host_vita_settings_oracle")
    end = cmake.find("add_test(NAME host_vita_settings_oracle", begin)
    if end < 0:
        raise AssertionError("host_tests lost add_test(NAME host_vita_settings_oracle")
    target = cmake[begin:end]
    for needle in (
            f"../../runtime/{ORACLE_SOURCE}",
            f"../../runtime/{PARSER_SOURCE}",
            "-Wall -Wextra -Werror",
            "target_compile_features(host_vita_settings_oracle PRIVATE c_std_11)"):
        if needle not in target:
            raise AssertionError(f"host_tests oracle target lost: {needle}")
    if "ISAAC_VITA_RUNTIME_SETTINGS" in target:
        raise AssertionError("the parser oracle must build without the knob")
    # add_executable, source path, compile_features, compile_options,
    # include_directories, add_test NAME, add_test COMMAND.
    if cmake.count("host_vita_settings_oracle") != 7:
        raise AssertionError("host_tests oracle target duplicated or truncated")


def _knob_mentions_fenced(text: str, name: str, knob: str = KNOB) -> int:
    """Every mention of the knob sits inside a `#if defined(KNOB)` region
    and never in that region's #else/#elif branch; the directive itself is
    the only accepted test form (-Wundef-proof, no #ifdef, no `#if KNOB`).
    Comments are stripped first (a `#endif /* KNOB */` marker or a prose
    mention is not a code mention).  Returns the number of knob regions."""
    guard = f"#if defined({knob})"
    stack: list[list[bool]] = []  # [is_knob_region, in_else_branch]
    regions = 0
    for number, raw in enumerate(_strip_comments(text).splitlines(), start=1):
        line = raw.strip()
        directive = re.match(r"#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)",
                             line)
        if directive:
            kind, rest = directive.group(1), directive.group(2).strip()
            if kind in ("if", "ifdef", "ifndef"):
                is_knob = knob in rest
                if is_knob and (kind != "if" or rest != f"defined({knob})"):
                    raise AssertionError(
                        f"{name}:{number}: knob test must be exactly "
                        f"{guard}: {raw!r}"
                    )
                regions += is_knob
                stack.append([is_knob, False])
                continue
            if kind == "elif":
                if knob in rest:
                    raise AssertionError(
                        f"{name}:{number}: knob in #elif: {raw!r}"
                    )
                if stack:
                    stack[-1][1] = True
                continue
            if not stack:
                raise AssertionError(
                    f"{name}:{number}: unbalanced #{kind}"
                )
            if kind == "else":
                stack[-1][1] = True
            else:
                stack.pop()
            continue
        if knob in raw and not any(
                is_knob and not in_else for is_knob, in_else in stack):
            raise AssertionError(
                f"{name}:{number}: {knob} outside its {guard} region: "
                f"{raw!r}"
            )
    if stack:
        raise AssertionError(f"{name}: unbalanced preprocessor conditionals")
    return regions


def _between(text: str, begin: str, end: str, name: str) -> str:
    try:
        start = text.index(begin)
        stop = text.index(end, start)
    except ValueError as exc:
        raise AssertionError(f"{name}: lost anchor {exc}") from exc
    return text[start:stop]


def verify_scheduler_duty_contract(root: Path) -> None:
    """kage_vita_fullspeed_scheduler.c/.h: the duty term is fenced, extends
    only the existing non-full skip statement (today's text in #else),
    clear() keeps s_duty_k, the banner suffix follows the pinned substring
    with its argument last, and the header declares the setter only under
    the knob."""
    runtime = root / "recomp" / "runtime"
    source = (runtime / SCHEDULER_SOURCE).read_text(encoding="utf-8")
    header = (runtime / SCHEDULER_HEADER).read_text(encoding="utf-8")
    if _knob_mentions_fenced(source, SCHEDULER_SOURCE) < 3:
        raise AssertionError("scheduler duty regions missing (state, banner, skip)")
    if _knob_mentions_fenced(header, SCHEDULER_HEADER) != 1:
        raise AssertionError("scheduler header must fence the setter once")
    if header.count("void kage_vita_fullspeed_scheduler_set_duty(uint8_t k);") != 1:
        raise AssertionError("scheduler header lost the duty setter")
    for needle in (
            "static uint8_t s_duty_k = 1u;",
            "void kage_vita_fullspeed_scheduler_set_duty(uint8_t k)\n{\n"
            "    s_duty_k = k <= 3u ? k : 1u;\n}",
            "static inline int kage_vita_fullspeed_duty_admits(uint32_t nonfull_phases)\n{\n"
            "    return s_duty_k == 0u ? 0 : (nonfull_phases % s_duty_k) == 0u;\n}",
    ):
        if source.count(needle) != 1:
            raise AssertionError(f"scheduler duty state drifted: {needle!r}")
    plan = _between(
        source, "int kage_vita_fullspeed_scheduler_plan_render(",
        "void kage_vita_fullspeed_scheduler_note_render_entry(void)",
        SCHEDULER_SOURCE,
    )
    if plan.count("interpolation_enabled != s_interpolation_enabled") != 1:
        raise AssertionError("plan_render interpolation gate count changed")
    skip_on = (
        "        if (!interpolation_enabled || another_tick_due ||\n"
        "                !kage_vita_fullspeed_duty_admits(s_counters.nonfull_phases)) {\n"
        "#else\n"
        "        if (!interpolation_enabled || another_tick_due) {\n"
        "#endif\n"
        "            s_render_decision = 0u;\n"
        "            kage_vita_fullspeed_add_u32(&s_counters.render_skips, 1u);\n"
        "            kage_vita_fullspeed_add_u32(&s_counters.nonfull_skips, 1u);\n"
        "            return 0;"
    )
    if plan.count(skip_on) != 1 or plan.count("kage_vita_fullspeed_duty_admits(") != 1:
        raise AssertionError("plan_render duty skip statement drifted")
    if plan.count("if (!interpolation_enabled || another_tick_due") != 2:
        raise AssertionError("plan_render must carry the ON and the OFF skip test")
    increment = plan.index("kage_vita_fullspeed_add_u32(&s_counters.nonfull_phases, 1u);")
    if increment > plan.index("kage_vita_fullspeed_duty_admits("):
        raise AssertionError("duty term must read the counter after its increment")
    clear = _between(source, "static void kage_vita_fullspeed_scheduler_clear(",
                     "\n}\n", SCHEDULER_SOURCE)
    if "s_duty_k" in clear or "memset(&s_counters, 0, sizeof s_counters);" not in clear:
        raise AssertionError("clear() must zero s_counters and leave s_duty_k alone")
    banner = _between(source, '"KAGE VITA CADENCE30: bid=%.32s tick_units=%u "',
                      "void kage_vita_fullspeed_scheduler_note_service(void)",
                      SCHEDULER_SOURCE)
    # Suffix after the pinned substring and after both optional suffixes;
    # argument last; today's argument list byte-identical in #else.
    order = [banner.index(needle) for needle in (
        '"parity=4a264 frame=1a30dc"',
        '" reserve=pre-game"',
        '" head-advance=4500us-experimental"',
        '" render-cost=excludes-present"',
        # ISAAC_VITA_FULLSPEED_FLOOR_DEBT (S5): mode, cap, then the slack.
        '" floor-debt=on"',
        '" floor-debt=observe"',
        '" cap=" KAGE_VITA_STR(ISAAC_VITA_FULLSPEED_FLOOR_DEBT_CAP_US) "us"',
        '" admit-slack=" KAGE_VITA_STR(ISAAC_VITA_FULLSPEED_ADMIT_SLACK_US) "us"',
        f'{KNOB_GUARD}\n            " fps_lock=%u"\n#endif\n            ,',
        f"{KNOB_GUARD}\n"
        "            (unsigned)KAGE_VITA_FULLSPEED_MAX_FULL_RENDER_SKIPS,\n"
        "            kage_vita_fullspeed_duty_fps_lock());\n"
        "#else\n"
        "            (unsigned)KAGE_VITA_FULLSPEED_MAX_FULL_RENDER_SKIPS);\n"
        "#endif\n"
        "    }\n"
        "}\n",
    )]
    if order != sorted(order):
        raise AssertionError("CADENCE30 banner fps_lock suffix/argument order drifted")
    if banner.count("kage_vita_fullspeed_duty_fps_lock()") != 1:
        raise AssertionError("CADENCE30 banner must pass the fps_lock argument once")
    if source.count('" fps_lock=%u"') != 1 or \
            source.count('"parity=4a264 frame=1a30dc"') != 1:
        raise AssertionError("CADENCE30 banner pin or suffix duplicated")


def verify_entry_hook_contract(root: Path) -> None:
    """entry_vita.c: fenced include and one fenced load call between the
    null-PE check and guest_register_all_imports()."""
    text = (root / "recomp" / "runtime" / ENTRY_SOURCE).read_text(encoding="utf-8")
    if _knob_mentions_fenced(text, ENTRY_SOURCE) != 2:
        raise AssertionError("entry_vita.c must fence exactly the include and the call")
    if text.count(f'{KNOB_GUARD}\n#include "{RUNTIME_HEADER}"\n#endif') != 1:
        raise AssertionError("entry_vita.c loader include is not fenced")
    if text.count("isaac_vita_runtime_settings_load();") != 1:
        raise AssertionError("entry_vita.c must call the loader exactly once")
    if not re.search(
            re.escape(KNOB_GUARD) + r"\n(?:    /\*(?:[^*]|\*(?!/))*\*/\n)?"
            r"    isaac_vita_runtime_settings_load\(\);\n#endif\n", text):
        raise AssertionError("entry_vita.c loader call is not fenced")
    order = [text.index(needle) for needle in (
        '"guest boundary begin: PE=%s',
        '"guest setup FAILED: null PE path"',
        "isaac_vita_runtime_settings_load();",
        "guest_register_all_imports();",
    )]
    if order != sorted(order) or text.count("guest_register_all_imports();") != 1:
        raise AssertionError("loader hook is not between the null-PE check and import registration")


def verify_loader_contract(root: Path) -> None:
    """host_vita_runtime_settings.c/.h: everything under the knob, one SDK
    include under the oracle seam, static buffer of 4097 bytes, ENOENT
    literal, exact receipt format literals, fenced scheduler push, no
    allocator / stdio."""
    runtime = root / "recomp" / "runtime"
    source = (runtime / RUNTIME_SOURCE).read_text(encoding="utf-8")
    header = (runtime / RUNTIME_HEADER).read_text(encoding="utf-8")
    end_marker = f"#endif /* {KNOB} */"
    for text, name in ((source, RUNTIME_SOURCE), (header, RUNTIME_HEADER)):
        code = _strip_comments(text)
        if text.count(end_marker) != 1 or text.count(KNOB_GUARD) != 1:
            raise AssertionError(f"{name} lost its knob guard or closing marker")
        outside = _strip_comments(
            text[:text.index(KNOB_GUARD)] +
            text[text.index(end_marker) + len(end_marker):]
        )
        lines = [line.strip() for line in outside.splitlines() if line.strip()]
        allowed = {
            "#ifndef HOST_VITA_RUNTIME_SETTINGS_H",
            "#define HOST_VITA_RUNTIME_SETTINGS_H",
            "#endif",
            f'#include "{RUNTIME_HEADER}"',
        }
        if set(lines) - allowed:
            raise AssertionError(
                f"{name} has text outside the knob: {sorted(set(lines) - allowed)}"
            )
        for token in ALLOCATOR_TOKENS:
            if re.search(rf"\b{token}\s*\(", code):
                raise AssertionError(f"{name} calls poisoned {token}()")
        if re.search(r"#\s*include\s*<std(io|lib)\.h>", code):
            raise AssertionError(f"{name} must not include stdio.h/stdlib.h")
    if "void isaac_vita_runtime_settings_load(void);" not in header:
        raise AssertionError("loader header lost the entry point")
    code = _strip_comments(source)
    if code.count("#include <psp2") != 1 or \
            "#if !defined(ISAAC_VITA_RUNTIME_SETTINGS_ORACLE)\n#include <psp2/io/fcntl.h>\n#endif" not in code:
        raise AssertionError("loader SDK include must sit under the oracle seam only")
    for needle in (
            "#define RUNTIME_SETTINGS_BUFFER_BYTES (ISAAC_VITA_SETTINGS_MAX_BYTES + 1u)",
            "static char s_file[RUNTIME_SETTINGS_BUFFER_BYTES];",
            "#define RUNTIME_SETTINGS_SCE_ENOENT 0x80010002u",
            'ISAAC_VITA_NATIVE_DATA_ROOT "/port.ini"',
            "sceIoOpen(path, SCE_O_RDONLY, 0)",
            '"ISAAC VITA SETTINGS: path=%s status=%s bytes=%u lines=%u "',
            '"rejected=%u unknown=%u dup=%u fps_lock=%u%s raster=%ux%u%s "',
            '"remap=%s%s chords=%s/%s restart_hold=%u stick_swap=%u"',
            '"ISAAC VITA SETTINGS REJECTED: lines=%u,%u,%u,%u "',
            '"unknown=%.15s,%.15s,%.15s,%.15s"',
            '"(ignored:no-fullspeed-scheduler)"',
            '"(ignored:not-compiled)"',
            '"(guard)"',
            "#if defined(ISAAC_VITA_FULLSPEED_SCHEDULER)\n"
            "    kage_vita_fullspeed_scheduler_set_duty(\n"
            "        isaac_vita_settings_fps_lock_duty_k(settings->fps_lock));",
            "isaac_vita_settings_reset_remap(&settings);",
            # By design, display_raster defaults to the build raster
            # the CMake block forwards, substituted when the key is unset.
            "#if defined(ISAAC_VITA_DISPLAY_RASTER_WIDTH)\n"
            "#define RUNTIME_SETTINGS_BUILD_RASTER "
            "((uint16_t)ISAAC_VITA_DISPLAY_RASTER_WIDTH)\n"
            "#else\n"
            "#define RUNTIME_SETTINGS_BUILD_RASTER ((uint16_t)960u)\n"
            "#endif",
            "if ((receipt.assigned & ISAAC_VITA_SETTINGS_ASSIGNED_BIT(\n"
            "             ISAAC_VITA_SETTINGS_KEY_DISPLAY_RASTER)) == 0u) {",
            "settings.display_raster = RUNTIME_SETTINGS_BUILD_RASTER;",
    ):
        if code.count(needle) != 1:
            raise AssertionError(f"loader contract drifted: {needle!r}")
    if code.count("isaac_vita_log(") != 2:
        raise AssertionError("loader must emit exactly the two receipt lines")


def _cmake_runtime_block(cmake: str) -> tuple[str, int]:
    begin = f"    if({KNOB})\n"
    try:
        start = cmake.index(begin)
        stop = cmake.index("\n    endif()\n", start)
    except ValueError as exc:
        raise AssertionError(f"CMake runtime-settings block missing: {exc}") from exc
    return cmake[start:stop], start


def _cmake_manager_block(cmake: str) -> tuple[str, int]:
    """The if(ISAAC_VITA_MANAGER_SETTINGS_UI) ... endif() window that adds
    the manager settings units to isaac_manager."""
    begin = f"if({MANAGER_CMAKE_OPTION})\n"
    try:
        start = cmake.index(begin)
        stop = cmake.index("\nendif()\n", start)
    except ValueError as exc:
        raise AssertionError(f"CMake manager-settings block missing: {exc}") from exc
    return cmake[start:stop], start


def _cmake_owner_names(block: str) -> set[str]:
    owners = _between(
        block, "set_property(SOURCE",
        f"APPEND PROPERTY COMPILE_DEFINITIONS\n          {KNOB}=1)",
        "CMake runtime-settings owners",
    )
    return set(re.findall(r'"\$\{ISAAC_RUNTIME\}/([A-Za-z0-9_]+\.c)"', owners))


def verify_cmake_knob_contract(root: Path) -> None:
    """CMake: option default OFF with the three FATALs; the two new TUs and
    the =1 definition appear only inside if(KNOB) after both pinned windows;
    the nested FULLSPEED definition for the loader sits inside both if()s;
    the owner list is the spec's."""
    cmake = (root / "recomp" / "vita" / "CMakeLists.txt").read_text(encoding="utf-8")
    if not re.search(rf'option\({KNOB}\s+"[^"]+" OFF\)', cmake):
        raise AssertionError("runtime settings option must default OFF")
    for needle in (
            f"if({KNOB} AND NOT ISAAC_VITA_KAGE)",
            f"{KNOB} requires ISAAC_VITA_KAGE=ON",
            f"if({KNOB} AND ISAAC_VITA_SCAFFOLD_ONLY)",
            f"if({KNOB} AND ISAAC_VITA_IO_PROFILE)",
    ):
        if cmake.count(needle) != 1:
            raise AssertionError(f"CMake runtime-settings FATAL lost: {needle}")
    block, start = _cmake_runtime_block(cmake)
    for needle, count in (
            ("list(APPEND ISAAC_VITA_RUNTIME_SOURCES\n"
             f'        "${{ISAAC_RUNTIME}}/{PARSER_SOURCE}"\n'
             f'        "${{ISAAC_RUNTIME}}/{RUNTIME_SOURCE}")', 1),
            (f"{KNOB}=1)", 1),
            ("      if(ISAAC_VITA_FULLSPEED_SCHEDULER)\n"
             "        set_property(SOURCE\n"
             f'          "${{ISAAC_RUNTIME}}/{RUNTIME_SOURCE}"\n'
             "          APPEND PROPERTY COMPILE_DEFINITIONS\n"
             "            ISAAC_VITA_FULLSPEED_SCHEDULER=1)\n"
             "      endif()", 1),
            # The build raster reaches the loader only (the display_raster
            # default); the RASTER_720 gate stays with the graphics owners.
            ("      set_property(SOURCE\n"
             f'        "${{ISAAC_RUNTIME}}/{RUNTIME_SOURCE}"\n'
             "        APPEND PROPERTY COMPILE_DEFINITIONS\n"
             "          ISAAC_VITA_DISPLAY_RASTER_WIDTH="
             "${ISAAC_VITA_DISPLAY_RASTER_WIDTH})", 1),
            ("ISAAC_VITA_DISPLAY_RASTER_720", 0),
            # four in the block itself plus the nested render-scale and
            # debug-overlay owner lists
            (RUNTIME_SOURCE, 6),
            # the source list, the owner list, the nested debug-overlay
            # owner list (the debug_overlay key)
            (PARSER_SOURCE, 3),
    ):
        if block.count(needle) != count:
            raise AssertionError(f"CMake runtime-settings block drifted: {needle!r}")
    # The parser TU is shared with the manager: its only other
    # mention is the target_sources() call inside the manager block.
    manager_block, _ = _cmake_manager_block(cmake)
    for needle, extra in ((RUNTIME_SOURCE, 0), (f"{KNOB}=1", 0),
                          (PARSER_SOURCE, manager_block.count(PARSER_SOURCE))):
        if cmake.count(needle) != block.count(needle) + extra:
            raise AssertionError(
                f"{needle} named outside if({KNOB}) / if({MANAGER_CMAKE_OPTION})"
            )
    if manager_block.count(PARSER_SOURCE) != 1:
        raise AssertionError("the manager block must name the parser TU once")
    if _cmake_owner_names(block) != set(RUNTIME_SETTINGS_OWNERS):
        raise AssertionError("CMake runtime-settings owner list drifted")
    fullspeed = cmake.index("    if(ISAAC_VITA_FULLSPEED_SCHEDULER)\n")
    stable30 = cmake.index("    if(ISAAC_VITA_STABLE_30_PRESENTATION)\n", fullspeed)
    lookup = cmake.index("    if(ISAAC_VITA_GUEST_LOOKUP_CACHE)\n", stable30)
    dispatch = cmake.index("    if(ISAAC_VITA_DISPATCH_TABLE)\n", lookup)
    if not lookup < start < dispatch:
        raise AssertionError("runtime-settings block must follow the lookup-cache block")
    if cmake[fullspeed:lookup].count(KNOB):
        raise AssertionError("runtime-settings text entered a pinned scheduler window")


def verify_build_wrapper_contract(root: Path) -> None:
    wrapper = (root / "tools" / "build_vita.py").read_text(encoding="utf-8")
    for needle, count in (
            # argparse flag, kage_features row, three self-test parses.
            ('"--runtime-settings"', 5),
            ('        "--runtime-settings",\n'
             '        action=argparse.BooleanOptionalAction,\n'
             '        default=False,', 1),
            ("runtime_settings: bool = False,", 2),
            (f'f"-D{KNOB}={{on_off(runtime_settings)}}"', 1),
            (f'"{KNOB}": runtime_settings,', 1),
            ("assert not defaults.runtime_settings", 1),
            ('"--runtime-settings requires --kage"', 2),
            (f'"-D{KNOB}=OFF"', 1),
            (f'"{KNOB}:BOOL=OFF"', 1),
            ("runtime_settings=args.runtime_settings,", 2),
            # Manager UI flag: argparse, kage_features row, two self-test
            # parses; default False; requires --runtime-settings (raise +
            # self-test assertion); both configure call sites forward it.
            ('"--manager-settings-ui"', 4),
            ('        "--manager-settings-ui",\n'
             '        action=argparse.BooleanOptionalAction,\n'
             '        default=False,', 1),
            ("manager_settings_ui: bool = False,", 2),
            (f'f"-D{MANAGER_CMAKE_OPTION}={{on_off(manager_settings_ui)}}"', 1),
            (f'"{MANAGER_CMAKE_OPTION}": manager_settings_ui,', 1),
            ("assert not defaults.manager_settings_ui", 1),
            ('"--manager-settings-ui requires --runtime-settings', 2),
            ("if args.manager_settings_ui and not args.runtime_settings:", 1),
            (f'"-D{MANAGER_CMAKE_OPTION}=OFF"', 1),
            (f'"{MANAGER_CMAKE_OPTION}:BOOL=OFF"', 1),
            ("manager_settings_ui=args.manager_settings_ui,", 2),
    ):
        if wrapper.count(needle) != count:
            raise AssertionError(f"build wrapper contract drifted: {needle!r} x{count}")


def verify_allocator_gate_contract(root: Path) -> None:
    vita = root / "recomp" / "vita"
    gate = (vita / "vita_raw_allocator_gate.py").read_text(encoding="utf-8")
    test = (vita / "test_vita_raw_allocator_gate.py").read_text(encoding="utf-8")
    if f'RUNTIME_SETTINGS_CACHE_KEY = "{KNOB}"' not in gate or \
            "def verify_runtime_settings_compile_scope(" not in gate or \
            "    verify_runtime_settings_compile_scope(records, cache)\n" not in gate:
        raise AssertionError("allocator gate lost the runtime-settings verifier")
    owners = _between(gate, "RUNTIME_SETTINGS_DEFINITION_OWNERS: frozenset[str] = frozenset((",
                      "))", "allocator gate owners")
    if set(re.findall(r'"([A-Za-z0-9_]+\.c)"', owners)) != set(RUNTIME_SETTINGS_OWNERS):
        raise AssertionError("allocator gate owner set differs from the CMake owners")
    if f'"{KNOB}": "OFF",' not in test or "def runtime_settings_scope_tests(" not in test:
        raise AssertionError("allocator gate test lost the runtime-settings fixtures")
    # Manager knob: a cache key the gate reads fail-closed, affecting no
    # eboot source; its definition may reach no compile record.
    for needle in (
            f'MANAGER_SETTINGS_UI_CACHE_KEY = "{MANAGER_CMAKE_OPTION}"',
            f'MANAGER_SETTINGS_UI_DEFINITION = "{MANAGER_KNOB}"',
            "manager_settings_ui = _feature(cache, MANAGER_SETTINGS_UI_CACHE_KEY)",
            "if manager_settings_ui and not runtime_settings:",
            "def verify_manager_settings_ui_compile_scope(",
            "    verify_manager_settings_ui_compile_scope(records, cache)\n",
            "manager settings page definition reached an eboot unit: ",
    ):
        if gate.count(needle) != 1:
            raise AssertionError(f"allocator gate lost the manager-settings knob: {needle!r}")
    if f'"{MANAGER_CMAKE_OPTION}": "OFF",' not in test or \
            "def manager_settings_ui_scope_tests(" not in test or \
            "    manager_settings_ui_scope_tests(root)\n" not in test:
        raise AssertionError("allocator gate test lost the manager-settings fixtures")


def _string_literals(code: str) -> list[str]:
    """Contents of every C string literal in comment-stripped code."""
    return re.findall(r'"((?:[^"\\]|\\.)*)"', code)


def _cmake_mentions_inside(cmake: str, tokens: tuple[str, ...],
                           condition: str, name: str) -> None:
    """Every line naming one of the tokens (as a whole word, so the
    option name ISAAC_VITA_MANAGER_SETTINGS_UI does not count as the
    definition MANAGER_SETTINGS_UI) sits inside an if()/endif() window
    whose if() condition mentions `condition`.  Nesting is tracked on
    if/elseif/else/endif at any indentation; a condition may span lines
    (parentheses balanced)."""
    stack: list[str] = []
    open_kind: str | None = None
    open_text = ""
    open_depth = 0
    for number, raw in enumerate(cmake.splitlines(), start=1):
        line = raw.strip()
        rest: str | None
        if open_kind is None:
            head = re.match(r"(if|elseif|else|endif)\s*\(", line)
            if head:
                open_kind = head.group(1)
                open_text = ""
                open_depth = 0
                rest = line[head.end() - 1:]
            else:
                rest = None
        else:
            rest = line
        if rest is not None:
            open_depth += rest.count("(") - rest.count(")")
            open_text += rest
            if open_depth <= 0:
                kind, open_kind = open_kind, None
                if kind == "if":
                    stack.append(open_text)
                elif kind == "endif":
                    if not stack:
                        raise AssertionError(f"{name}:{number}: unbalanced endif()")
                    stack.pop()
                elif stack:
                    # A sibling branch of a knob block is outside the knob.
                    stack[-1] = open_text if kind == "elseif" else ""
            continue
        if any(re.search(rf"(?<![A-Za-z0-9_]){re.escape(token)}(?![A-Za-z0-9_])",
                         raw) for token in tokens) and not any(
                condition in entry for entry in stack):
            raise AssertionError(
                f"{name}:{number}: manager settings named outside "
                f"if({condition}): {raw!r}"
            )
    if stack or open_kind is not None:
        raise AssertionError(f"{name}: unbalanced if()/endif()")


def verify_manager_core_contract(root: Path) -> None:
    """Manager core: fenced storage primitives, SDK-free settings TU,
    colon-free status literals, host_tests target with the shared fake FS,
    production CMake naming the units only under the manager option."""
    vita = root / "recomp" / "vita"
    tests = vita / "host_tests"
    storage_source = (vita / MANAGER_STORAGE_SOURCE).read_text(encoding="utf-8")
    storage_header = (vita / MANAGER_STORAGE_HEADER).read_text(encoding="utf-8")
    settings_source = (vita / MANAGER_SETTINGS_SOURCE).read_text(encoding="utf-8")
    settings_header = (vita / MANAGER_SETTINGS_HEADER).read_text(encoding="utf-8")

    # 1. manager_storage.c/.h: the knob only inside its #if defined() regions,
    #    at least the primitives block plus the recover() fold, and the
    #    prototypes/enum member in the header.
    if _knob_mentions_fenced(storage_source, MANAGER_STORAGE_SOURCE,
                             MANAGER_KNOB) < 2:
        raise AssertionError(
            f"{MANAGER_STORAGE_SOURCE} must fence the text primitives and "
            "the recover() fold separately"
        )
    if _knob_mentions_fenced(storage_header, MANAGER_STORAGE_HEADER,
                             MANAGER_KNOB) < 2:
        raise AssertionError(
            f"{MANAGER_STORAGE_HEADER} must fence the ABSENT result and the "
            "text prototypes"
        )
    storage_code = _strip_comments(storage_source)
    for needle in (
            "int manager_storage_recover_port(const struct manager_storage_io *io)",
            "int manager_storage_write_text_atomic(",
            "int manager_storage_archive_and_remove(",
            "int manager_storage_find_free_named(",
            "int manager_storage_newest_named(",
            "int manager_storage_read_text(",
            "int manager_storage_read_head(",
            # The slot loop's early return keeps today's text in #else; with
            # the knob the failure is recorded and recover_port still runs
            # (by design, port.ini recovery runs at every start and
            # return).
            "        const int result = manager_storage_recover_slot(io, slot);\n"
            f"{MANAGER_KNOB_GUARD}\n"
            "        if (result < 0) {\n"
            "            if (failure == 0)\n"
            "                failure = result;\n"
            "            continue;\n"
            "        }\n"
            "#else\n"
            "        if (result < 0)\n"
            "            return result;\n"
            "#endif\n",
            f"{MANAGER_KNOB_GUARD}\n"
            "    {\n"
            "        const int result = manager_storage_recover_port(io);\n"
            "        if (result < 0 && failure == 0)\n"
            "            failure = result;\n"
            "        if (result == MANAGER_STORAGE_SYNC_WARNING)\n"
            "            warning = 1;\n"
            "    }\n"
            "    if (failure < 0)\n"
            "        return failure;\n"
            "#endif\n"
            "    return warning ? MANAGER_STORAGE_SYNC_WARNING : MANAGER_STORAGE_OK;\n"
            "}",
    ):
        if storage_code.count(needle) != 1:
            raise AssertionError(f"{MANAGER_STORAGE_SOURCE} lost: {needle!r}")
    if storage_header.count("MANAGER_STORAGE_ABSENT = 2,") != 1 or \
            storage_header.count('#define MANAGER_STORAGE_PORT_INI_NAME "port.ini"') != 1:
        raise AssertionError(f"{MANAGER_STORAGE_HEADER} text contract drifted")
    for text, name in ((storage_source, MANAGER_STORAGE_SOURCE),
                       (storage_header, MANAGER_STORAGE_HEADER)):
        if re.search(r"#\s*include\s*<psp2", text):
            raise AssertionError(f"{name} must stay SDK-free")

    # 2. manager_settings.c/.h: everything under the knob, SDK-free, no
    #    allocator, stdio limited to snprintf.
    end_marker = f"#endif /* {MANAGER_KNOB} */"
    for text, name, allowed in (
            (settings_source, MANAGER_SETTINGS_SOURCE,
             {f'#include "{MANAGER_SETTINGS_HEADER}"'}),
            (settings_header, MANAGER_SETTINGS_HEADER,
             {"#ifndef ISAAC_MANAGER_SETTINGS_H",
              "#define ISAAC_MANAGER_SETTINGS_H", "#endif"})):
        if text.count(end_marker) != 1 or text.count(MANAGER_KNOB_GUARD) != 1:
            raise AssertionError(f"{name} lost its knob guard or closing marker")
        outside = _strip_comments(
            text[:text.index(MANAGER_KNOB_GUARD)] +
            text[text.index(end_marker) + len(end_marker):]
        )
        lines = [line.strip() for line in outside.splitlines() if line.strip()]
        if set(lines) - allowed:
            raise AssertionError(
                f"{name} has text outside the knob: {sorted(set(lines) - allowed)}"
            )
        code = _strip_comments(text)
        if re.search(r"#\s*include\s*<(psp2|vitasdk|vitaGL|taihen)", code):
            raise AssertionError(f"{name} must not include SDK headers")
        for token in ALLOCATOR_TOKENS:
            if re.search(rf"\b{token}\s*\(", code):
                raise AssertionError(f"{name} calls poisoned {token}()")
        if re.search(r"\b(v?s?printf|fprintf|puts|fputs|fopen|fread|fwrite|"
                     r"fclose|vsnprintf|sceClib\w+)\s*\(", code):
            raise AssertionError(f"{name} may use stdio only through snprintf()")
        if re.search(r"#\s*include\s*<stdlib\.h>", code):
            raise AssertionError(f"{name} must not include stdlib.h")
        if re.search(r"\b(alloca)\s*\(", code) or re.search(
                r"\bchar\s+\w+\s*\[\s*[a-z_]\w*\s*\]", code):
            raise AssertionError(f"{name} must not use alloca or a VLA")
    includes = set(re.findall(r"#\s*include\s*[<\"]([^>\"]+)[>\"]",
                              _strip_comments(settings_source)))
    includes |= set(re.findall(r"#\s*include\s*[<\"]([^>\"]+)[>\"]",
                               _strip_comments(settings_header)))
    allowed_includes = {
        MANAGER_SETTINGS_HEADER, "manager_sha256.h", PARSER_HEADER,
        MANAGER_STORAGE_HEADER, "stdio.h", "string.h", "stddef.h", "stdint.h",
    }
    if includes - allowed_includes:
        raise AssertionError(
            f"manager settings TU includes outside the allowed set: "
            f"{sorted(includes - allowed_includes)}"
        )
    for literal in _string_literals(_strip_comments(settings_source)):
        if ":" in literal and literal not in MANAGER_COLON_LITERALS:
            raise AssertionError(
                f"{MANAGER_SETTINGS_SOURCE} status literal carries ':' "
                f"(manager_display_text folds it to '?'): {literal!r}"
            )
    for needle in (
            f'#define MANAGER_SETTINGS_LOG_PATH MANAGER_STORAGE_ROOT "/first-arm-fault.log"',
            "#define MANAGER_SETTINGS_LOG_HEAD_BYTES (16u * 1024u)",
            "#define MANAGER_SETTINGS_FILE_CAPACITY (ISAAC_VITA_SETTINGS_MAX_BYTES + 2u)",
            "int manager_settings_load(",
            "struct manager_settings_result manager_settings_save(",
            "struct manager_settings_result manager_settings_defaults(",
            "struct manager_settings_result manager_settings_undo(",
            "int manager_settings_read_last_boot(",
            "void manager_settings_result_text(",
            "int manager_settings_text_is_narrow(const char *text);",
            "int manager_settings_set_render_scale(struct manager_settings_state *state,",
            "uint8_t manager_settings_render_scale_step(uint8_t current, int direction);",
    ):
        if settings_header.count(needle) != 1:
            raise AssertionError(f"{MANAGER_SETTINGS_HEADER} lost: {needle!r}")
    for needle in ('"build "', '"ISAAC VITA SETTINGS: "', '" fps_lock="',
                   '" remap="', '"(guard)"', '"STOP FTP BEFORE LOCAL CHANGES"',
                   '"FAILED - NO FREE BACKUP SLOT"', '"FAILED - BAD FILE"',
                   '"FAILED - IO ERROR"', '"REFUSED - BAD ARCHIVE"',
                   '"GAME BUILD IGNORES PORT.INI"', '"NO LOG"',
                   "manager_storage_write_text_atomic(",
                   "manager_storage_archive_and_remove(io,",
                   "manager_storage_read_head(io, path, buffer, sizeof(buffer),"):
        if needle not in settings_source:
            raise AssertionError(f"{MANAGER_SETTINGS_SOURCE} lost: {needle!r}")

    # 3. host_tests: the settings target and the shared fake FS.
    cmake = (tests / "CMakeLists.txt").read_text(encoding="utf-8")
    block = _between(cmake, "add_executable(manager_settings_host_test",
                     "add_test(NAME manager_settings_host_test",
                     "host_tests manager_settings target")
    for needle in (
            f"  {MANAGER_SETTINGS_TEST}\n",
            f"  ../{MANAGER_SETTINGS_SOURCE}\n",
            f"  ../{MANAGER_STORAGE_SOURCE}\n",
            f"  ../{MANAGER_SHA256_SOURCE}\n",
            f"  ../../runtime/{PARSER_SOURCE})\n",
            "target_compile_features(manager_settings_host_test PRIVATE c_std_11)",
            "-Wall -Wextra -Werror",
            f"target_compile_definitions(manager_settings_host_test PRIVATE\n"
            f"  {MANAGER_KNOB}=1)",
            "target_include_directories(manager_settings_host_test PRIVATE\n"
            "  .. ../../runtime)",
    ):
        if block.count(needle) != 1:
            raise AssertionError(f"host_tests manager_settings target lost: {needle!r}")
    # add_executable, the test source name, compile_features, compile_options,
    # compile_definitions, include_directories, add_test NAME, COMMAND.
    if cmake.count("manager_settings_host_test") != 8:
        raise AssertionError("host_tests manager_settings target duplicated or truncated")
    fake_fs = (tests / FAKE_FS_HEADER).read_text(encoding="utf-8")
    if "#ifndef FAKE_DATA_CAPACITY\n#define FAKE_DATA_CAPACITY 4096u\n#endif" not in fake_fs:
        raise AssertionError("fake_fs.h must keep the 4 KiB default node and allow an override")
    for test_name in (MANAGER_SETTINGS_TEST, MANAGER_STORAGE_TEST):
        test_text = (tests / test_name).read_text(encoding="utf-8")
        if test_text.count(f'#include "{FAKE_FS_HEADER}"') != 1:
            raise AssertionError(f"{test_name} must include the shared {FAKE_FS_HEADER}")
        if "struct fake_fs {" in test_text:
            raise AssertionError(f"{test_name} re-declares the fake filesystem")
    if (tests / MANAGER_SETTINGS_TEST).read_text(encoding="utf-8").count(
            "#define FAKE_DATA_CAPACITY 17408u\n"
            f'#include "{FAKE_FS_HEADER}"') != 1:
        raise AssertionError("settings test must enlarge the fake nodes for the 16 KiB log")

    # 4. Production CMake: the manager TUs and the definition only under the
    #    manager option (nothing today; the UI work package wires them).
    production = (vita / "CMakeLists.txt").read_text(encoding="utf-8")
    _cmake_mentions_inside(
        production, (MANAGER_SETTINGS_SOURCE, MANAGER_KNOB, "manager_settings_ui.c"),
        MANAGER_CMAKE_OPTION, "recomp/vita/CMakeLists.txt")


def _lines_inside_knob(text: str, knob: str) -> set[int]:
    """1-based numbers of the lines (comments stripped) that sit inside a
    `#if defined(KNOB)` region and not in its #else/#elif branch.  The
    directive lines themselves count as inside.  Directive well-formedness
    is _knob_mentions_fenced's job; this walker only tracks nesting."""
    inside: set[int] = set()
    stack: list[list[bool]] = []
    for number, raw in enumerate(_strip_comments(text).splitlines(), start=1):
        line = raw.strip()
        directive = re.match(r"#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)", line)
        if directive:
            kind, rest = directive.group(1), directive.group(2).strip()
            if kind in ("if", "ifdef", "ifndef"):
                stack.append([knob in rest, False])
                if knob in rest:
                    inside.add(number)
                continue
            was_inside = any(k and not e for k, e in stack)
            if kind in ("elif", "else") and stack:
                stack[-1][1] = True
            elif kind == "endif" and stack:
                stack.pop()
            if was_inside:
                inside.add(number)
            continue
        if any(k and not e for k, e in stack):
            inside.add(number)
    return inside


def _lines_inside_defined(text: str, knob: str) -> set[int]:
    """Like _lines_inside_knob, but only a plain `#if defined(KNOB)` opens a
    region (never `#if !defined(KNOB)`, whose body is the knob-OFF code).
    Used to exempt the manager UI2 includes, which the OFF build
    never sees, from the baseline #include set."""
    inside: set[int] = set()
    stack: list[list[bool]] = []
    for number, raw in enumerate(_strip_comments(text).splitlines(), start=1):
        line = raw.strip()
        directive = re.match(r"#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)", line)
        if directive:
            kind, rest = directive.group(1), directive.group(2).strip()
            if kind in ("if", "ifdef", "ifndef"):
                opens = kind == "if" and rest == f"defined({knob})"
                stack.append([opens, False])
                if opens:
                    inside.add(number)
                continue
            was_inside = any(k and not e for k, e in stack)
            if kind in ("elif", "else") and stack:
                stack[-1][1] = True
            elif kind == "endif" and stack:
                stack.pop()
            if was_inside:
                inside.add(number)
            continue
        if any(k and not e for k, e in stack):
            inside.add(number)
    return inside


def _format_free(literal: str) -> str:
    """A C format string with its printf directives removed (the model only
    uses %c %u %s with optional flags/width/precision and %%)."""
    return re.sub(r"%(%|[-0-9.]*[cus])", "", literal)


def verify_manager_ui_contract(root: Path) -> None:
    """Manager UI: the page model TU/header exist only under the knob,
    SDK-free, allocator-free, snprintf-only, with every emitted literal in
    the wide glyph set and every status literal in the narrow set; every
    manager_main.c edit is fenced and the OFF build sees today's includes;
    production CMake adds the units to isaac_manager only inside
    if(ISAAC_VITA_MANAGER_SETTINGS_UI) after the pinned add_executable and
    target_link_libraries text, with the option OFF and its FATAL; the
    host_tests target and the WSL-aware runner exist."""
    vita = root / "recomp" / "vita"
    tests = vita / "host_tests"
    ui_source = (vita / MANAGER_UI_SOURCE).read_text(encoding="utf-8")
    ui_header = (vita / MANAGER_UI_HEADER).read_text(encoding="utf-8")
    main_source = (vita / MANAGER_MAIN_SOURCE).read_text(encoding="utf-8")

    # 1. manager_settings_ui.c/.h: everything under the knob, SDK-free, no
    #    allocator, stdio limited to snprintf, no VLA, allowed includes.
    end_marker = f"#endif /* {MANAGER_KNOB} */"
    for text, name, allowed in (
            (ui_source, MANAGER_UI_SOURCE, {f'#include "{MANAGER_UI_HEADER}"'}),
            (ui_header, MANAGER_UI_HEADER,
             {"#ifndef ISAAC_MANAGER_SETTINGS_UI_H",
              "#define ISAAC_MANAGER_SETTINGS_UI_H", "#endif"})):
        # The guard as a directive line (the header's comment also quotes
        # it); the closing marker carries its own comment.
        guards = list(re.finditer(rf"^{re.escape(MANAGER_KNOB_GUARD)}[ \t]*$",
                                  text, re.MULTILINE))
        code = _strip_comments(text)
        if text.count(end_marker) != 1 or len(guards) != 1 or \
                code.count(MANAGER_KNOB_GUARD) != 1:
            raise AssertionError(f"{name} lost its knob guard or closing marker")
        outside = _strip_comments(
            text[:guards[0].start()] +
            text[text.index(end_marker) + len(end_marker):]
        )
        lines = [line.strip() for line in outside.splitlines() if line.strip()]
        if set(lines) - allowed:
            raise AssertionError(
                f"{name} has text outside the knob: {sorted(set(lines) - allowed)}"
            )
        if re.search(r"#\s*include\s*<(psp2|vitasdk|vitaGL|taihen)", code):
            raise AssertionError(f"{name} must not include SDK headers")
        for token in ALLOCATOR_TOKENS:
            if re.search(rf"\b{token}\s*\(", code):
                raise AssertionError(f"{name} calls poisoned {token}()")
        if re.search(r"\b(v?s?printf|fprintf|puts|fputs|fopen|fread|fwrite|"
                     r"fclose|vsnprintf|sceClib\w+)\s*\(", code):
            raise AssertionError(f"{name} may use stdio only through snprintf()")
        if re.search(r"#\s*include\s*<stdlib\.h>", code):
            raise AssertionError(f"{name} must not include stdlib.h")
        if re.search(r"\b(alloca)\s*\(", code) or re.search(
                r"\bchar\s+\w+\s*\[\s*[a-z_]\w*\s*\]", code):
            raise AssertionError(f"{name} must not use alloca or a VLA")
        for literal in _string_literals(code):
            if literal.endswith(".h"):
                continue
            stripped = _format_free(literal)
            if set(stripped) - MANAGER_WIDE_GLYPHS:
                raise AssertionError(
                    f"{name} literal outside the manager_draw_text glyph set: "
                    f"{literal!r}"
                )
    includes = set(re.findall(r"#\s*include\s*[<\"]([^>\"]+)[>\"]",
                              _strip_comments(ui_source)))
    includes |= set(re.findall(r"#\s*include\s*[<\"]([^>\"]+)[>\"]",
                               _strip_comments(ui_header)))
    allowed_includes = {
        MANAGER_UI_HEADER, MANAGER_SETTINGS_HEADER, "stdio.h", "string.h",
        "stddef.h", "stdint.h",
    }
    if includes - allowed_includes:
        raise AssertionError(
            f"manager UI TU includes outside the allowed set: "
            f"{sorted(includes - allowed_includes)}"
        )
    ui_code = _strip_comments(ui_source)
    ui_literals = set(_string_literals(ui_code))
    for literal in MANAGER_UI_STATUS_LITERALS:
        if literal not in ui_literals:
            raise AssertionError(f"{MANAGER_UI_SOURCE} lost status literal {literal!r}")
        if set(_format_free(literal)) - MANAGER_NARROW_GLYPHS:
            raise AssertionError(f"status literal is not narrow: {literal!r}")
        if len(_format_free(literal)) > 57 - 4:
            raise AssertionError(f"status literal leaves no room for its value: {literal!r}")
    for literal in MANAGER_UI_ROW_LITERALS + (MANAGER_UI_LEGEND, MANAGER_UI_TITLE):
        if literal not in ui_literals:
            raise AssertionError(f"{MANAGER_UI_SOURCE} lost row literal {literal!r}")
    if len(MANAGER_UI_LEGEND) > 67 or "SQ PRESET" in ui_code or \
            "NOT IMPLEMENTED" in ui_code:
        raise AssertionError("legend must fit one row and name only live keys")
    for needle in (
            f"#define MANAGER_SETTINGS_UI_ROWS 12u",
            "#define MANAGER_SETTINGS_UI_ROW_MAX 67u",
            "#define MANAGER_SETTINGS_UI_STATUS_MAX 57u",
            "MANAGER_SETTINGS_UI_BUTTON_UP = 0x00000010,",
            "MANAGER_SETTINGS_UI_BUTTON_CROSS = 0x00004000,",
            "const char *manager_settings_ui_title(void);",
            "const char *manager_settings_ui_legend(void);",
            "void manager_settings_ui_open(struct manager_settings_ui *ui,",
            "int manager_settings_ui_handle(struct manager_settings_ui *ui,",
            "int manager_settings_ui_text_is_wide(const char *text);",
            "char rows[MANAGER_SETTINGS_UI_ROWS][MANAGER_SETTINGS_UI_ROW_CAPACITY];",
            "    MANAGER_SETTINGS_UI_ROW_RENDER_SCALE,",
    ):
        if ui_header.count(needle) != 1:
            raise AssertionError(f"{MANAGER_UI_HEADER} lost: {needle!r}")
    for needle in ("manager_settings_load(", "manager_settings_fps_lock_step(",
                   "manager_settings_render_scale_step(",
                   "manager_settings_set_render_scale(",
                   "manager_settings_save(", "manager_settings_defaults(",
                   "manager_settings_undo(", "manager_settings_discard(",
                   "manager_settings_result_text(",
                   "manager_settings_read_last_boot(",
                   "manager_settings_last_boot_text(",
                   "manager_settings_port_ini_text("):
        if needle not in ui_code:
            raise AssertionError(f"{MANAGER_UI_SOURCE} no longer calls {needle}")

    # 2. manager_main.c: every mention of the knob and of the UI fenced;
    #    the OFF build's #include set is today's; the wiring is in place.
    regions = _knob_mentions_fenced(main_source, MANAGER_MAIN_SOURCE, MANAGER_KNOB)
    if regions < 8:
        raise AssertionError(
            f"{MANAGER_MAIN_SOURCE} must fence include, page enum, model, "
            f"HOME hint, draw, dispatch, HOME handler and page handler "
            f"separately ({regions} regions)"
        )
    inside = _lines_inside_knob(main_source, MANAGER_KNOB)
    # The manager UI2 includes sit under #if defined(MANAGER_UI2).
    ui2_inside = _lines_inside_defined(main_source, "MANAGER_UI2")
    outside_includes: set[str] = set()
    for number, raw in enumerate(_strip_comments(main_source).splitlines(), start=1):
        if number in inside:
            continue
        stripped = raw.strip()
        if re.match(r"#\s*include\b", stripped) and number not in ui2_inside:
            outside_includes.add(stripped)
        for token in MANAGER_MAIN_UI_TOKENS:
            if token in raw:
                raise AssertionError(
                    f"{MANAGER_MAIN_SOURCE}:{number}: {token} outside the "
                    f"{MANAGER_KNOB_GUARD} regions: {raw!r}"
                )
    if outside_includes != set(MANAGER_MAIN_BASELINE_INCLUDES):
        raise AssertionError(
            f"{MANAGER_MAIN_SOURCE} #include set outside the knob changed: "
            f"{sorted(outside_includes ^ set(MANAGER_MAIN_BASELINE_INCLUDES))}"
        )
    main_code = _strip_comments(main_source)
    for needle in (
            f'{MANAGER_KNOB_GUARD}\n#include "{MANAGER_UI_HEADER}"\n#endif',
            f"{MANAGER_KNOB_GUARD}\n    MANAGER_PAGE_SETTINGS,\n#endif",
            "static struct manager_settings_ui manager_settings_page;",
            "#define MANAGER_SETTINGS_BUTTON_IS(model, sdk) \\\n"
            "    ((unsigned)(model) == (unsigned)(sdk))",
            "_Static_assert(MANAGER_SETTINGS_BUTTON_IS(MANAGER_SETTINGS_UI_BUTTON_UP, SCE_CTRL_UP) &&",
            "MANAGER_SETTINGS_BUTTON_IS(MANAGER_SETTINGS_UI_BUTTON_SELECT, SCE_CTRL_SELECT),",
            "#undef MANAGER_SETTINGS_BUTTON_IS",
            f"{MANAGER_KNOB_GUARD}\n"
            '    manager_draw_text(96u, 436u, 2u, muted, "START: SETTINGS");\n'
            "#endif",
            "static void manager_draw_settings(void)",
            "manager_draw_frame(manager_settings_ui_title());\n"
            "    manager_display_text(status, sizeof(status), manager_status, 58u);\n"
            "    manager_draw_text(88u, 126u, 2u, white, status);",
            "manager_draw_text(88u, 166u + row * 30u, 2u, color,",
            "manager_draw_text(72u, 492u, 2u, white, manager_settings_ui_legend());",
            f"{MANAGER_KNOB_GUARD}\n"
            "    else if (page == MANAGER_PAGE_SETTINGS)\n"
            "        manager_draw_settings();\n"
            "#endif",
            "} else if ((pressed & SCE_CTRL_START) != 0u) {",
            "manager_settings_ui_open(&manager_settings_page,",
            "} else if (page == MANAGER_PAGE_SETTINGS) {",
            "if (manager_settings_ui_handle(&manager_settings_page,",
            "ftp_port != 0u)) {\n"
            "                    page = MANAGER_PAGE_HOME;",
    ):
        if main_code.count(needle) != 1:
            raise AssertionError(f"{MANAGER_MAIN_SOURCE} lost: {needle!r}")
    if main_code.count('"%s", manager_settings_page.status);') != 2:
        raise AssertionError("manager_status must mirror the page status on open and after each edge")

    # 3. Production CMake: option OFF, FATAL without the runtime knob, the
    #    units added to isaac_manager only inside if(option), after the
    #    pinned add_executable/target_link_libraries text.
    production = (vita / "CMakeLists.txt").read_text(encoding="utf-8")
    if not re.search(rf'option\({MANAGER_CMAKE_OPTION}\s+"[^"]+" OFF\)', production):
        raise AssertionError("manager settings option must default OFF")
    fatal = (f"if({MANAGER_CMAKE_OPTION} AND NOT {KNOB})\n"
             "  message(FATAL_ERROR\n"
             f'    "{MANAGER_CMAKE_OPTION} requires {KNOB}=ON (the game must read port.ini)")\n'
             "endif()")
    if production.count(fatal) != 1:
        raise AssertionError("manager settings FATAL lost or reworded")
    block, start = _cmake_manager_block(production)
    expected_block = (
        f"if({MANAGER_CMAKE_OPTION})\n"
        "  target_sources(isaac_manager PRIVATE\n"
        f"    {MANAGER_SETTINGS_SOURCE}\n"
        f"    {MANAGER_UI_SOURCE}\n"
        f'    "${{ISAAC_RUNTIME}}/{PARSER_SOURCE}")\n'
        f"  target_compile_definitions(isaac_manager PRIVATE {MANAGER_KNOB}=1)\n"
        '  target_include_directories(isaac_manager PRIVATE "${ISAAC_RUNTIME}")'
    )
    if not block.startswith(expected_block):
        raise AssertionError(f"CMake manager-settings block drifted:\n{block}")
    add_executable = production.index("add_executable(isaac_manager\n")
    link = production.index("target_link_libraries(isaac_manager PRIVATE", add_executable)
    create_self = production.index("vita_create_self(isaac-manager.bin isaac_manager UNSAFE)")
    if not link < start < create_self:
        raise AssertionError("manager-settings block must follow the pinned manager target text")
    # One target_sources(isaac_manager ...) per manager knob block: this
    # settings block, manager UI2 and the UI2 game skin.
    if production.count("target_sources(isaac_manager PRIVATE") != 3 or \
            production.count(f"{MANAGER_KNOB}=1") != 1 or \
            production.count(MANAGER_UI_SOURCE) != 1 or \
            production.count(MANAGER_SETTINGS_SOURCE) != 1:
        raise AssertionError("manager settings units named more than once in production CMake")
    _cmake_mentions_inside(
        production, (MANAGER_SETTINGS_SOURCE, MANAGER_UI_SOURCE, MANAGER_KNOB),
        MANAGER_CMAKE_OPTION, "recomp/vita/CMakeLists.txt")

    # 4. host_tests: the UI target from the five TUs with the knob; the test
    #    uses the shared fake FS with the enlarged nodes; the runner names it.
    cmake = (tests / "CMakeLists.txt").read_text(encoding="utf-8")
    target = _between(cmake, f"add_executable({MANAGER_UI_TARGET}",
                      f"add_test(NAME {MANAGER_UI_TARGET}",
                      "host_tests manager_settings_ui target")
    for needle in (
            f"  {MANAGER_UI_TEST}\n",
            f"  ../{MANAGER_UI_SOURCE}\n",
            f"  ../{MANAGER_SETTINGS_SOURCE}\n",
            f"  ../{MANAGER_STORAGE_SOURCE}\n",
            f"  ../{MANAGER_SHA256_SOURCE}\n",
            f"  ../../runtime/{PARSER_SOURCE})\n",
            f"target_compile_features({MANAGER_UI_TARGET} PRIVATE c_std_11)",
            "-Wall -Wextra -Werror",
            f"target_compile_definitions({MANAGER_UI_TARGET} PRIVATE\n"
            f"  {MANAGER_KNOB}=1)",
            f"target_include_directories({MANAGER_UI_TARGET} PRIVATE\n"
            "  .. ../../runtime)",
    ):
        if target.count(needle) != 1:
            raise AssertionError(f"host_tests manager_settings_ui target lost: {needle!r}")
    if cmake.count(MANAGER_UI_TARGET) != 8:
        raise AssertionError("host_tests manager_settings_ui target duplicated or truncated")
    test_text = (tests / MANAGER_UI_TEST).read_text(encoding="utf-8")
    if test_text.count("#define FAKE_DATA_CAPACITY 17408u\n"
                       f'#include "{FAKE_FS_HEADER}"') != 1 or \
            "struct fake_fs {" in test_text or \
            f'#include "{MANAGER_UI_HEADER}"' not in test_text:
        raise AssertionError(f"{MANAGER_UI_TEST} must use the shared fake FS and the page header")
    for needle in ("check_invariants(", "manager_settings_ui_text_is_wide(",
                   "manager_settings_text_is_narrow(",
                   "MANAGER_SETTINGS_UI_ROW_MAX", "MANAGER_SETTINGS_UI_STATUS_MAX",
                   '"UNSAVED - O AGAIN TO DISCARD"',
                   '"STOP FTP BEFORE LOCAL CHANGES"',
                   '#define ROW_UNEVEN "  40 45 - UNEVEN CADENCE 30 UPS"',
                   '"UNEVEN CADENCE - X AGAIN TO SAVE 45"',
                   '"ROOM RENDER 960X540 SHARP - X SAVES"',
                   '#define CANON_TAIL "display_raster=960\\n" "render_scale=1\\n" CANON_REST',
                   "manager settings ui host tests: PASS"):
        if needle not in test_text:
            raise AssertionError(f"{MANAGER_UI_TEST} lost: {needle!r}")
    runner = (vita / MANAGER_HOST_RUNNER).read_text(encoding="utf-8")
    for needle in (f'    "{MANAGER_UI_TARGET}",\n', '    "manager_settings_host_test",\n',
                   "def run_in_wsl(", "def run_native(", "ctest",
                   "manager host tests: PASS"):
        if needle not in runner:
            raise AssertionError(f"{MANAGER_HOST_RUNNER} lost: {needle!r}")


def verify_render_scale_contract(root: Path) -> None:
    """options.ini MaxRenderScale normalizer: TU and header under the knob,
    one SDK include under the oracle seam, no allocator / stdio, the exact
    paths and receipt literal, one log call; the loader includes and calls
    it from nested fences after its receipt; CMake option OFF with its
    FATAL and a nested block inside if(KNOB) naming the TU and the loader as
    the only owners; the host_tests target; the release recipe pins it ON
    (owner decision 2026-09-16: MaxRenderScale=1 is the port default) together
    with the runtime-settings knob it requires."""
    runtime = root / "recomp" / "runtime"
    vita = root / "recomp" / "vita"
    source = (runtime / RENDER_SCALE_SOURCE).read_text(encoding="utf-8")
    header = (runtime / RENDER_SCALE_HEADER).read_text(encoding="utf-8")
    end_marker = f"#endif /* {RENDER_SCALE_KNOB} */"
    for text, name in ((source, RENDER_SCALE_SOURCE), (header, RENDER_SCALE_HEADER)):
        code = _strip_comments(text)
        if text.count(end_marker) != 1 or code.count(RENDER_SCALE_GUARD) != 1:
            raise AssertionError(f"{name} lost its knob guard or closing marker")
        outside = _strip_comments(
            text[:text.index(RENDER_SCALE_GUARD)] +
            text[text.index(end_marker) + len(end_marker):]
        )
        lines = [line.strip() for line in outside.splitlines() if line.strip()]
        allowed = {
            "#ifndef HOST_VITA_RENDER_SCALE_H",
            "#define HOST_VITA_RENDER_SCALE_H",
            "#endif",
            f'#include "{RENDER_SCALE_HEADER}"',
        }
        if set(lines) - allowed:
            raise AssertionError(
                f"{name} has text outside the knob: {sorted(set(lines) - allowed)}"
            )
        for token in ALLOCATOR_TOKENS:
            if re.search(rf"\b{token}\s*\(", code):
                raise AssertionError(f"{name} calls poisoned {token}()")
        if re.search(r"#\s*include\s*<std(io|lib)\.h>", code):
            raise AssertionError(f"{name} must not include stdio.h/stdlib.h")
    code = _strip_comments(source)
    if code.count("#include <psp2") != 1 or \
            "#if !defined(ISAAC_VITA_RENDER_SCALE_ORACLE)\n#include <psp2/io/fcntl.h>\n#endif" not in code:
        raise AssertionError("normalizer SDK include must sit under the oracle seam only")
    for needle in (
            'ISAAC_VITA_NATIVE_DATA_ROOT "/Documents/My Games/Binding of Isaac Repentance"',
            '#define RENDER_SCALE_PATH RENDER_SCALE_DIR "/options.ini"',
            '#define RENDER_SCALE_TMP RENDER_SCALE_DIR "/options.ini.rs-tmp"',
            '#define RENDER_SCALE_BAK RENDER_SCALE_DIR "/options.ini.rs-bak"',
            "#define RENDER_SCALE_SCE_ENOENT 0x80010002u",
            "sceIoOpen(path, SCE_O_RDONLY, 0)",
            "sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_EXCL,",
            'static const char k_key[] = "MaxRenderScale";',
            'static const char k_section[] = "[Options]";',
            'return render_scale == 1u ? "1" : "99";',
            '"ISAAC VITA RENDER SCALE: mode=%u target=%s status=%s old=%s "',
            '"bytes=%u rc=0x%08x recovered=%u"',
            "static char s_in[RENDER_SCALE_IN_BYTES];",
            "static char s_out[ISAAC_VITA_RENDER_SCALE_OUT_BYTES];",
    ):
        if code.count(needle) != 1:
            raise AssertionError(f"normalizer contract drifted: {needle!r}")
    if code.count("isaac_vita_log(") != 1:
        raise AssertionError("normalizer must emit exactly one receipt line")
    if "#define ISAAC_VITA_RENDER_SCALE_FILE_MAX 16384u" not in header or \
            "void isaac_vita_render_scale_apply(uint8_t render_scale);" not in header:
        raise AssertionError("normalizer header lost the cap or the entry point")
    # The loader: nested include and one nested call after its receipt.
    loader = (runtime / RUNTIME_SOURCE).read_text(encoding="utf-8")
    if _knob_mentions_fenced(loader, RUNTIME_SOURCE, RENDER_SCALE_KNOB) != 2:
        raise AssertionError("loader must fence exactly the normalizer include and call")
    if loader.count(f'{RENDER_SCALE_GUARD}\n#include "{RENDER_SCALE_HEADER}"\n#endif') != 1:
        raise AssertionError("loader normalizer include is not fenced")
    if loader.count("isaac_vita_render_scale_apply(settings.render_scale);") != 1:
        raise AssertionError("loader must call the normalizer exactly once")
    if not re.search(
            re.escape(RENDER_SCALE_GUARD) + r"\n(?:    /\*(?:[^*]|\*(?!/))*\*/\n)?"
            r"    isaac_vita_render_scale_apply\(settings\.render_scale\);\n#endif\n}",
            loader):
        raise AssertionError("loader normalizer call is not fenced at the end of the load")
    if loader.index("runtime_settings_log(&settings, &receipt);") > \
            loader.index("isaac_vita_render_scale_apply(settings.render_scale);"):
        raise AssertionError("the normalizer must run after the settings receipt")
    # Production CMake.
    cmake = (vita / "CMakeLists.txt").read_text(encoding="utf-8")
    if not re.search(rf'option\({RENDER_SCALE_KNOB}\s+"[^"]+" OFF\)', cmake):
        raise AssertionError("render scale option must default OFF")
    fatal = (f"if({RENDER_SCALE_KNOB} AND NOT {KNOB})\n  message(FATAL_ERROR\n"
             f'    "{RENDER_SCALE_KNOB} requires {KNOB}=ON (the value comes from port.ini)")\n'
             "endif()")
    if cmake.count(fatal) != 1:
        raise AssertionError("render scale FATAL lost or reworded")
    block, _ = _cmake_runtime_block(cmake)
    nested = _between(block, f"      if({RENDER_SCALE_KNOB})\n", "\n      endif()\n",
                      "CMake render-scale block")
    for needle in (
            "        list(APPEND ISAAC_VITA_RUNTIME_SOURCES\n"
            f'          "${{ISAAC_RUNTIME}}/{RENDER_SCALE_SOURCE}")',
            "        set_property(SOURCE\n"
            f'          "${{ISAAC_RUNTIME}}/{RUNTIME_SOURCE}"\n'
            f'          "${{ISAAC_RUNTIME}}/{RENDER_SCALE_SOURCE}"\n'
            "          APPEND PROPERTY COMPILE_DEFINITIONS\n"
            f"            {RENDER_SCALE_KNOB}=1)",
    ):
        if nested.count(needle) != 1:
            raise AssertionError(f"CMake render-scale block drifted: {needle!r}")
    if cmake.count(RENDER_SCALE_SOURCE) != 2 or cmake.count(f"{RENDER_SCALE_KNOB}=1") != 1:
        raise AssertionError("render scale TU or definition named outside its block")
    # host_tests target.
    tests = (vita / "host_tests" / "CMakeLists.txt").read_text(encoding="utf-8")
    target = _between(tests, f"add_executable({RENDER_SCALE_ORACLE_TARGET}",
                      f"add_test(NAME {RENDER_SCALE_ORACLE_TARGET}",
                      "host_tests render-scale oracle target")
    for needle in (
            f"  ../../runtime/{RENDER_SCALE_ORACLE_SOURCE}\n",
            f"  ../../runtime/{RENDER_SCALE_SOURCE})\n",
            "-Wall -Wextra -Werror",
            f"  {RENDER_SCALE_KNOB}=1 ISAAC_VITA_RENDER_SCALE_ORACLE=1)",
    ):
        if target.count(needle) != 1:
            raise AssertionError(f"host_tests render-scale target lost: {needle!r}")
    if tests.count(RENDER_SCALE_ORACLE_TARGET) != 8:
        raise AssertionError("host_tests render-scale target duplicated or truncated")
    # Release recipe: ON (owner decision 2026-09-16: MaxRenderScale=1 is the
    # port default).  The CMake option itself stays OFF (checked above); the
    # recipe must also turn the runtime-settings knob on, or CMake refuses
    # the configure (the FATAL pinned above).
    release = (root / "release" / "v0.1.1-alpha.cmake").read_text(encoding="utf-8")
    if release.count(f'set({RENDER_SCALE_KNOB} ON CACHE BOOL "")') != 1:
        raise AssertionError("release recipe must pin the render scale default ON")
    if release.count(f'set({KNOB} ON CACHE BOOL "")') != 1:
        raise AssertionError(
            f"release recipe must pin {KNOB} ON (the render scale default requires it)")


# Every function takes the repository root and raises AssertionError.
CONTRACT_CHECKS: list[Callable[[Path], None]] = [
    verify_parser_contract,
    verify_host_tests_cmake,
    verify_loader_contract,
    verify_scheduler_duty_contract,
    verify_entry_hook_contract,
    verify_cmake_knob_contract,
    verify_build_wrapper_contract,
    verify_allocator_gate_contract,
    verify_manager_core_contract,
    verify_manager_ui_contract,
    verify_render_scale_contract,
]


def build_and_run_oracle(cc: str, root: Path, output: Path) -> str:
    runtime = root / "recomp" / "runtime"
    common = [cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
              f"-I{runtime}"]
    exe = output / ("settings-oracle.exe" if os.name == "nt"
                    else "settings-oracle")
    run(common + [str(runtime / ORACLE_SOURCE), str(runtime / PARSER_SOURCE),
                  "-o", str(exe)])
    stdout = run([str(exe)])
    if "host_vita_settings_oracle: PASS" not in stdout:
        raise AssertionError(f"oracle did not report PASS:\n{stdout}")
    # The parser TU alone, -Wundef (the manager builds with -Wall -Wextra
    # only, so every knob test must be #if defined()).
    run([cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wundef",
         "-fsyntax-only", str(runtime / PARSER_SOURCE)])
    run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wundef",
         "-fsyntax-only", str(runtime / PARSER_SOURCE)])
    # The header compiles standalone (no hidden include-order dependence).
    run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wundef",
         "-fsyntax-only", "-x", "c", str(runtime / PARSER_HEADER)])
    # The debug_overlay key: parser TU and header with the overlay knob, then
    # its oracle (parser + loader with the oracle I/O seams).
    vita = root / "recomp" / "vita"
    for std in ("-std=c11", "-std=gnu11"):
        run([cc, std, "-Wall", "-Wextra", "-Werror", "-Wundef",
             f"-D{OVERLAY_KNOB}=1", "-fsyntax-only",
             str(runtime / PARSER_SOURCE)])
    run([cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wundef",
         f"-D{OVERLAY_KNOB}=1", "-fsyntax-only", "-x", "c",
         str(runtime / PARSER_HEADER)])
    overlay_exe = output / ("settings-debug-overlay-oracle.exe"
                            if os.name == "nt" else
                            "settings-debug-overlay-oracle")
    run(common + ["-Wundef", f"-I{vita}", f"-D{KNOB}=1", f"-D{KNOB}_ORACLE=1",
                  f"-D{OVERLAY_KNOB}=1", str(runtime / RUNTIME_SOURCE),
                  str(runtime / PARSER_SOURCE),
                  str(runtime / OVERLAY_ORACLE_SOURCE), "-o",
                  str(overlay_exe)])
    overlay_stdout = run([str(overlay_exe)])
    if "host_vita_settings_debug_overlay_oracle: PASS" not in overlay_stdout:
        raise AssertionError(
            f"debug_overlay oracle did not report PASS:\n{overlay_stdout}")
    return stdout + overlay_stdout


def build_and_run_loader_oracle(cc: str, root: Path, output: Path) -> list[str]:
    """Loader receipt goldens in both flavours the production CMake block can
    produce, plus the knob-OFF emptiness of the loader TU and header and the
    -Wundef cleanliness of the scheduler TU with both knobs."""
    runtime = root / "recomp" / "runtime"
    vita = root / "recomp" / "vita"
    strict = [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wundef",
              f"-I{runtime}", f"-I{vita}"]
    lines: list[str] = []
    for variant, flags in (
            ("fullspeed-scheduler", ["-DISAAC_VITA_FULLSPEED_SCHEDULER=1"]),
            ("no-fullspeed-scheduler", []),
            # The CMake block forwards the build raster; a 720x408 eboot
            # must report raster=720x408 for every file without the key.
            ("fullspeed-scheduler-raster720",
             ["-DISAAC_VITA_FULLSPEED_SCHEDULER=1",
              "-DISAAC_VITA_DISPLAY_RASTER_WIDTH=720"])):
        exe = output / (f"runtime-settings-oracle-{variant}.exe"
                        if os.name == "nt" else
                        f"runtime-settings-oracle-{variant}")
        run(strict + ["-O2", f"-D{KNOB}=1", f"-D{KNOB}_ORACLE=1"] + flags + [
            str(runtime / RUNTIME_SOURCE), str(runtime / PARSER_SOURCE),
            str(runtime / RUNTIME_ORACLE_SOURCE), "-o", str(exe)])
        stdout = run([str(exe)])
        if "host_vita_runtime_settings_oracle: PASS" not in stdout or \
                f"variant={variant}" not in stdout:
            raise AssertionError(
                f"loader oracle ({variant}) did not report PASS:\n{stdout}"
            )
        lines.append(stdout.strip())
    # Knob OFF: the loader TU and its header preprocess to nothing.
    run(strict + ["-fsyntax-only", str(runtime / RUNTIME_SOURCE)])
    run(strict + ["-fsyntax-only", "-x", "c", str(runtime / RUNTIME_HEADER)])
    run(strict + [f"-D{KNOB}=1", "-fsyntax-only", "-x", "c",
                  str(runtime / RUNTIME_HEADER)])
    # The scheduler TU stays -Wundef clean with and without the knob.
    run(strict + ["-fsyntax-only", str(runtime / SCHEDULER_SOURCE)])
    run(strict + [f"-D{KNOB}=1", "-DISAAC_VITA_FULLSPEED_SCHEDULER=1",
                  "-fsyntax-only", str(runtime / SCHEDULER_SOURCE)])
    return lines


def build_and_run_render_scale_oracle(cc: str, root: Path, output: Path) -> str:
    """Normalizer oracle (transform, publish protocol, failure seams,
    recovery, receipt goldens); knob OFF the TU and header preprocess to
    nothing; -Wundef clean with and without the knob."""
    runtime = root / "recomp" / "runtime"
    vita = root / "recomp" / "vita"
    strict = [cc, "-std=c11", "-Wall", "-Wextra", "-Werror", "-Wundef",
              f"-I{runtime}", f"-I{vita}"]
    exe = output / ("render-scale-oracle.exe" if os.name == "nt"
                    else "render-scale-oracle")
    run(strict + ["-O2", f"-D{RENDER_SCALE_KNOB}=1",
                  "-DISAAC_VITA_RENDER_SCALE_ORACLE=1",
                  str(runtime / RENDER_SCALE_SOURCE),
                  str(runtime / RENDER_SCALE_ORACLE_SOURCE), "-o", str(exe)])
    stdout = run([str(exe)])
    if "host_vita_render_scale_oracle: PASS" not in stdout:
        raise AssertionError(f"render scale oracle did not report PASS:\n{stdout}")
    run(strict + ["-fsyntax-only", str(runtime / RENDER_SCALE_SOURCE)])
    run(strict + ["-fsyntax-only", "-x", "c", str(runtime / RENDER_SCALE_HEADER)])
    run(strict + [f"-D{RENDER_SCALE_KNOB}=1", "-fsyntax-only", "-x", "c",
                  str(runtime / RENDER_SCALE_HEADER)])
    # The loader with the nested knob stays -Wundef clean (oracle seams on).
    run(strict + [f"-D{KNOB}=1", f"-D{KNOB}_ORACLE=1", f"-D{RENDER_SCALE_KNOB}=1",
                  "-DISAAC_VITA_RENDER_SCALE_ORACLE=1", "-fsyntax-only",
                  str(runtime / RUNTIME_SOURCE)])
    return stdout.strip()


def build_and_run_manager_core_test(cc: str, root: Path,
                                    output: Path) -> list[str]:
    """manager_settings_host_test and the unchanged manager_storage_host_test
    with the host compiler; manager_storage.c -Wundef clean with and without
    the manager knob; manager_settings.c/.h preprocess to nothing without
    it."""
    vita = root / "recomp" / "vita"
    runtime = root / "recomp" / "runtime"
    tests = vita / "host_tests"
    strict = [cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
              f"-I{vita}", f"-I{runtime}", f"-I{tests}"]
    lines: list[str] = []
    exe = output / ("manager-settings-host-test.exe" if os.name == "nt"
                    else "manager-settings-host-test")
    run(strict + [f"-D{MANAGER_KNOB}=1",
                  str(tests / MANAGER_SETTINGS_TEST),
                  str(vita / MANAGER_SETTINGS_SOURCE),
                  str(vita / MANAGER_STORAGE_SOURCE),
                  str(vita / MANAGER_SHA256_SOURCE),
                  str(runtime / PARSER_SOURCE), "-o", str(exe)])
    stdout = run([str(exe)])
    if "manager settings host tests: PASS" not in stdout:
        raise AssertionError(f"manager settings host test did not PASS:\n{stdout}")
    lines.append(stdout.strip())
    exe = output / ("manager-storage-host-test.exe" if os.name == "nt"
                    else "manager-storage-host-test")
    run(strict + [str(tests / MANAGER_STORAGE_TEST),
                  str(vita / MANAGER_STORAGE_SOURCE),
                  str(vita / MANAGER_SHA256_SOURCE), "-o", str(exe)])
    stdout = run([str(exe)])
    if "manager storage host tests: PASS" not in stdout:
        raise AssertionError(f"manager storage host test did not PASS:\n{stdout}")
    lines.append(stdout.strip())
    for flags in ([], [f"-D{MANAGER_KNOB}=1"],
                  ["-DISAAC_VITA_MANAGER_SAVE_GAMESTATE=1"],
                  ["-DISAAC_VITA_MANAGER_SAVE_GAMESTATE=1", f"-D{MANAGER_KNOB}=1"]):
        run(strict + ["-Wundef", "-fsyntax-only"] + flags +
            [str(vita / MANAGER_STORAGE_SOURCE)])
    run(strict + ["-Wundef", "-fsyntax-only", str(vita / MANAGER_SETTINGS_SOURCE)])
    run(strict + ["-Wundef", "-fsyntax-only", "-x", "c",
                  str(vita / MANAGER_SETTINGS_HEADER)])
    run(strict + ["-Wundef", "-fsyntax-only", "-x", "c", f"-D{MANAGER_KNOB}=1",
                  str(vita / MANAGER_SETTINGS_HEADER)])
    preprocessed = run([cc, "-std=c11", f"-I{vita}", f"-I{runtime}", "-E", "-P",
                        str(vita / MANAGER_SETTINGS_SOURCE)])
    if preprocessed.strip():
        raise AssertionError(
            f"{MANAGER_SETTINGS_SOURCE} emits text without {MANAGER_KNOB}:\n"
            f"{preprocessed[:400]}"
        )
    lines.append(
        "manager core: settings + storage host tests PASS with the host "
        "compiler; storage TU -Wundef clean in 4 knob combinations; "
        "settings TU empty without the knob"
    )
    # Manager UI: the page model test with the knob; the UI TU and header
    # -Wundef clean with and without it; the TU preprocesses to nothing
    # without it.
    exe = output / ("manager-settings-ui-host-test.exe" if os.name == "nt"
                    else "manager-settings-ui-host-test")
    run(strict + [f"-D{MANAGER_KNOB}=1",
                  str(tests / MANAGER_UI_TEST),
                  str(vita / MANAGER_UI_SOURCE),
                  str(vita / MANAGER_SETTINGS_SOURCE),
                  str(vita / MANAGER_STORAGE_SOURCE),
                  str(vita / MANAGER_SHA256_SOURCE),
                  str(runtime / PARSER_SOURCE), "-o", str(exe)])
    stdout = run([str(exe)])
    if "manager settings ui host tests: PASS" not in stdout:
        raise AssertionError(f"manager settings ui host test did not PASS:\n{stdout}")
    lines.append(stdout.strip())
    for flags in ([], [f"-D{MANAGER_KNOB}=1"]):
        run(strict + ["-Wundef", "-fsyntax-only"] + flags +
            [str(vita / MANAGER_UI_SOURCE)])
        run(strict + ["-Wundef", "-fsyntax-only", "-x", "c"] + flags +
            [str(vita / MANAGER_UI_HEADER)])
    preprocessed = run([cc, "-std=c11", f"-I{vita}", f"-I{runtime}", "-E", "-P",
                        str(vita / MANAGER_UI_SOURCE)])
    if preprocessed.strip():
        raise AssertionError(
            f"{MANAGER_UI_SOURCE} emits text without {MANAGER_KNOB}:\n"
            f"{preprocessed[:400]}"
        )
    lines.append(
        "manager UI: page model host test PASS with the host compiler; UI "
        "TU and header -Wundef clean with and without the knob; UI TU empty "
        "without the knob"
    )
    return lines


def _nm_tool(prefix: str = "") -> str | None:
    for name in (f"{prefix}nm", "llvm-nm"):
        found = shutil.which(name)
        if found:
            return found
    return None


def _defined_writable_symbols(nm_output: str) -> list[str]:
    """Symbols nm reports in .data/.bss (b B d D) or as common (C)."""
    bad: list[str] = []
    for line in nm_output.splitlines():
        parts = line.split()
        if len(parts) < 2:
            continue
        kind = parts[-2] if len(parts) >= 3 else parts[0]
        if kind in ("b", "B", "d", "D", "C", "g", "G", "s", "S"):
            bad.append(parts[-1])
    return bad


def check_object_has_no_writable_data(cc: str, root: Path,
                                      output: Path) -> str:
    """Compile the parser TU to a host object and prove it defines nothing in
    .data/.bss (gcc's default PIE would put const pointer tables into
    .data.rel.ro and nm reports those as 'd', hence -fno-pie)."""
    runtime = root / "recomp" / "runtime"
    nm = _nm_tool()
    if nm is None:
        return "object-section pin skipped: no nm/llvm-nm in PATH"
    obj = output / "settings-parse-host.o"
    run([cc, "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror", "-fno-pie",
         "-c", str(runtime / PARSER_SOURCE), "-o", str(obj)])
    bad = _defined_writable_symbols(run([nm, str(obj)]))
    if bad:
        raise AssertionError(
            f"{PARSER_SOURCE} defines writable/common symbols: {bad}"
        )
    return "object-section pin: no .data/.bss symbol in the parser object"


def check_arm_compile(root: Path, output: Path) -> str:
    """ARM -Werror gate: compile the parser TU and the oracle
    TU with the production flags of test_kage_vita_input.sh; the parser
    object may import only string.h symbols and __aeabi_* helpers."""
    runtime = root / "recomp" / "runtime"
    sdk = os.environ.get("VITASDK")
    if not sdk:
        return "ARM gate skipped: no VITASDK"
    prefix = Path(sdk) / "bin" / "arm-vita-eabi-"
    gcc = Path(f"{prefix}gcc")
    nm = Path(f"{prefix}nm")
    if not gcc.is_file():
        return f"ARM gate skipped: {gcc} not found"
    parser_obj = output / "settings-parse-arm.o"
    oracle_obj = output / "settings-oracle-arm.o"
    run([str(gcc)] + ARM_COMMON_FLAGS + [f"-I{runtime}", "-c",
         str(runtime / PARSER_SOURCE), "-o", str(parser_obj)])
    run([str(gcc)] + ARM_COMMON_FLAGS + [f"-I{runtime}", "-c",
         str(runtime / ORACLE_SOURCE), "-o", str(oracle_obj)])
    if not nm.is_file():
        raise AssertionError(f"VITASDK set but {nm} is missing")
    undefined = set()
    for line in run([str(nm), "-u", str(parser_obj)]).splitlines():
        parts = line.split()
        if parts:
            undefined.add(parts[-1])
    stray = sorted(
        sym for sym in undefined
        if sym not in PARSER_ALLOWED_UNDEFINED and not sym.startswith("__aeabi_")
    )
    if stray:
        raise AssertionError(
            f"{PARSER_SOURCE} ARM object imports outside string.h: {stray}"
        )
    writable = _defined_writable_symbols(run([str(nm), str(parser_obj)]))
    if writable:
        raise AssertionError(
            f"{PARSER_SOURCE} ARM object defines writable symbols: {writable}"
        )
    # The loader (both flavours) and the scheduler with both knobs; the
    # loader may import only the SDK I/O trio, the log, the parser and the
    # scheduler setter.
    vita = root / "recomp" / "vita"
    for variant, flags in (("fullspeed", ["-DISAAC_VITA_FULLSPEED_SCHEDULER=1"]),
                           ("nofullspeed", []),
                           ("raster720", ["-DISAAC_VITA_FULLSPEED_SCHEDULER=1",
                                          "-DISAAC_VITA_DISPLAY_RASTER_WIDTH=720"])):
        loader_obj = output / f"runtime-settings-arm-{variant}.o"
        run([str(gcc)] + ARM_COMMON_FLAGS + [f"-I{runtime}", f"-I{vita}",
             f"-D{KNOB}=1"] + flags + ["-c", str(runtime / RUNTIME_SOURCE),
             "-o", str(loader_obj)])
        loader_undefined = set()
        for line in run([str(nm), "-u", str(loader_obj)]).splitlines():
            parts = line.split()
            if parts:
                loader_undefined.add(parts[-1])
        stray = sorted(
            sym for sym in loader_undefined
            if sym not in LOADER_ALLOWED_UNDEFINED and
            not sym.startswith("isaac_vita_settings_") and
            not sym.startswith("__aeabi_")
        )
        if stray:
            raise AssertionError(
                f"{RUNTIME_SOURCE} ARM object ({variant}) imports outside "
                f"its contract: {stray}"
            )
        if variant == "nofullspeed" and \
                "kage_vita_fullspeed_scheduler_set_duty" in loader_undefined:
            raise AssertionError(
                "loader without the scheduler definition still calls the setter"
            )
    run([str(gcc)] + ARM_COMMON_FLAGS + [f"-I{runtime}", f"-I{vita}",
         f"-D{KNOB}=1", "-DISAAC_VITA_FULLSPEED_SCHEDULER=1", "-c",
         str(runtime / SCHEDULER_SOURCE), "-o",
         str(output / "scheduler-arm-duty.o")])
    # Manager half under the isaac_manager production flags: the storage TU
    # with and without the manager knob, the settings TU with it (it is
    # never compiled without), both SDK-free so no VitaSDK include path is
    # needed.
    for variant, flags in (("off", []), ("on", [f"-D{MANAGER_KNOB}=1"])):
        run([str(gcc)] + ARM_MANAGER_FLAGS + [f"-I{vita}", f"-I{runtime}"] +
            flags + ["-c", str(vita / MANAGER_STORAGE_SOURCE), "-o",
                     str(output / f"manager-storage-arm-{variant}.o")])
    run([str(gcc)] + ARM_MANAGER_FLAGS + [f"-I{vita}", f"-I{runtime}",
         f"-D{MANAGER_KNOB}=1", "-c", str(vita / MANAGER_SETTINGS_SOURCE),
         "-o", str(output / "manager-settings-arm.o")])
    run([str(gcc)] + ARM_MANAGER_FLAGS + [f"-I{vita}", f"-I{runtime}",
         f"-D{MANAGER_KNOB}=1", "-c", str(vita / MANAGER_UI_SOURCE),
         "-o", str(output / "manager-settings-ui-arm.o")])
    return (
        "ARM gate: parser + oracle + loader (3 flavours) + scheduler duty + "
        "manager storage (2 flavours) + manager settings + manager settings "
        f"UI TUs compile -Werror (softfp thumb); parser imports {sorted(undefined)}"
    )


def main() -> int:
    vita = Path(__file__).resolve().parent
    root = vita.parent.parent
    for check in CONTRACT_CHECKS:
        check(root)
    cc = compiler()
    with tempfile.TemporaryDirectory(prefix="isaac-settings-") as value:
        stdout = build_and_run_oracle(cc, root, Path(value))
        loader_lines = build_and_run_loader_oracle(cc, root, Path(value))
        render_scale_line = build_and_run_render_scale_oracle(cc, root, Path(value))
        manager_lines = build_and_run_manager_core_test(cc, root, Path(value))
        section_line = check_object_has_no_writable_data(cc, root, Path(value))
        arm_line = check_arm_compile(root, Path(value))
    print(stdout.strip())
    for line in loader_lines:
        print(line)
    print(render_scale_line)
    for line in manager_lines:
        print(line)
    print(section_line)
    print(arm_line)
    print(
        "Vita runtime settings contract: PASS "
        f"({len(CONTRACT_CHECKS)} source pins; pure C11 shared TU; "
        "header knob-free; host_tests targets; loader receipt goldens in "
        "both scheduler flavours and with the forwarded 720 build raster; "
        "knob-OFF loader/header empty; render-scale normalizer oracle; scheduler "
        "duty, entry hook, CMake block, build wrapper and allocator gate "
        "pinned; manager core fenced, SDK-free and host-tested; manager "
        "SETTINGS page fenced in manager_main.c, wide/narrow literals, "
        "isaac_manager units only under the manager option)"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
