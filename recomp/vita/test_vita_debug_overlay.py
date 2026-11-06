#!/usr/bin/env python3
"""Host gate for the on-screen debug overlay (ISAAC_VITA_DEBUG_OVERLAY).

  * builds recomp/runtime/host_vita_debug_overlay_oracle.c with the overlay TU
    (-std=gnu11 -O2 -Wall -Wextra -Werror, every owner feature on) and runs
    it: formatting, memblock/thread ledger through the --wrap entry points,
    boot probe, per-domain reconciliation, frame/cpu/thread statistics,
    compact and full page goldens, redzone drawing at three rasters, off path;
  * syntax-checks the overlay TU with no owner feature and preprocesses it and
    its header to nothing without the knob (-Wundef);
  * syntax-checks the manager units that include the shared parser with the
    knob (manager_settings.c / manager_settings_ui.c with MANAGER_SETTINGS_UI)
    -- the ON manager must still compile;
  * pins the font copy (digits and letters equal kage_vita_loading.c), the
    fences in every pre-existing file the overlay touches, the CMake option /
    FATALs / --wrap set / release pin and the raw allocator gate registration;
  * with --base <commit>: the host half of the OFF identity proof.  Every
    runtime TU the change touches, directly or through a touched header, is
    preprocessed (-E -P, missing SDK headers stubbed) at <commit> and here,
    with no definitions and with the release recipe's definitions, and the
    token streams must be equal; the pure parser TU is also compiled with
    WSL gcc in both trees and the objects must be byte-identical.  The ARM
    whole-tree comparison stays on the Linux build host.
  * with --mockup <dir>: writes overlay-compact.png / overlay-full.png
    rendered by the oracle over a synthetic background (synthetic values).
"""

from __future__ import annotations

import argparse
import hashlib
import io
import os
import re
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
RUNTIME = ROOT / "recomp" / "runtime"
VITA = ROOT / "recomp" / "vita"
sys.path.insert(0, str(VITA))
import test_vita_runtime_settings as trs  # noqa: E402

KNOB = "ISAAC_VITA_DEBUG_OVERLAY"
GUARD = f"#if defined({KNOB})"
SOURCE = "host_vita_debug_overlay.c"
HEADER = "host_vita_debug_overlay.h"
ORACLE = "host_vita_debug_overlay_oracle.c"
FEATURES = (
    f"-D{KNOB}=1", f"-D{KNOB}_ORACLE=1",
    f"-D{KNOB}_HEAP_TELEMETRY=1",
    f"-D{KNOB}_OPENAL_POOL=1", f"-D{KNOB}_LUA_ARENA=1",
    f"-D{KNOB}_SCHEDULER=1",
)
# Pre-existing files the overlay edits; every knob mention must be fenced.
TOUCHED_FILES = (
    "recomp/runtime/host_vita_settings_parse.c",
    "recomp/runtime/host_vita_settings_parse.h",
    "recomp/runtime/host_vita_runtime_settings.c",
    "recomp/runtime/host_vita_runtime_settings.h",
    "recomp/runtime/kage_vita_backend.c",
    "recomp/runtime/kage_vita_fullspeed_scheduler.c",
    "recomp/runtime/kage_vita_fullspeed_scheduler.h",
    "recomp/runtime/host_vita_lua.c",
    "recomp/runtime/host_vita_lua_gc.h",
)
# host_vita_lua.c / host_vita_lua_gc.h widen an existing OR-ed export guard
# by one term on the same physical line.
OR_GUARD = ("#if defined(ISAAC_VITA_LUA_GC_PROFILE) || "
            "defined(ISAAC_VITA_HEAP_CENSUS) || defined(ISAAC_VITA_DEBUG_OVERLAY)")
WRAPPED = (
    "sceKernelAllocMemBlock", "kuKernelAllocMemBlock", "sceKernelFreeMemBlock",
    "sceKernelCreateThread", "sceKernelDeleteThread",
    "sceKernelExitDeleteThread",
)


def run(command: list[str], cwd: Path | None = None) -> str:
    done = subprocess.run(command, cwd=cwd, check=False, text=True,
                          capture_output=True)
    if done.returncode != 0:
        raise AssertionError(
            f"command failed ({done.returncode}): {command!r}\n"
            f"stdout:\n{done.stdout}\nstderr:\n{done.stderr}")
    return done.stdout


def exe_name(output: Path, stem: str) -> Path:
    return output / (stem + ".exe" if os.name == "nt" else stem)


# ---- oracle ---------------------------------------------------------------

def build_and_run_oracle(cc: str, output: Path,
                         mockup: Path | None = None) -> str:
    exe = exe_name(output, "debug-overlay-oracle")
    run([cc, "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror", "-Wundef",
         f"-I{RUNTIME}", *FEATURES, str(RUNTIME / SOURCE),
         str(RUNTIME / ORACLE), "-o", str(exe)])
    args = [str(exe)]
    if mockup is not None:
        args.append(str(mockup))
    stdout = run(args)
    if "host_vita_debug_overlay_oracle: PASS" not in stdout:
        raise AssertionError(f"overlay oracle did not report PASS:\n{stdout}")
    strict = [cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wundef",
              f"-I{RUNTIME}", "-fsyntax-only"]
    # no owner feature, oracle seams only
    run(strict + [f"-D{KNOB}=1", f"-D{KNOB}_ORACLE=1", str(RUNTIME / SOURCE)])
    # the production poison boundary (isaac_vita_raw_allocator_poison.h):
    # libc headers first, then the seven identifiers poisoned
    poison = output / "poison.h"
    poison.write_text(
        "#include <malloc.h>\n#include <stdlib.h>\n#include <string.h>\n"
        "#pragma GCC poison malloc calloc realloc free memalign "
        "aligned_alloc strdup\n", encoding="utf-8")
    run(strict + ["-include", str(poison), *FEATURES, str(RUNTIME / SOURCE)])
    # knob OFF: TU and header are empty
    run(strict + [str(RUNTIME / SOURCE)])
    run(strict + ["-x", "c", str(RUNTIME / HEADER)])
    run(strict + [f"-D{KNOB}=1", "-x", "c", str(RUNTIME / HEADER)])
    pre = run([cc, "-E", "-P", f"-I{RUNTIME}", str(RUNTIME / SOURCE)])
    if pre.strip():
        raise AssertionError("overlay TU is not empty without the knob")
    return stdout


def write_png(path: Path, width: int, height: int, rgba: bytes) -> None:
    rows = b"".join(b"\x00" + rgba[y * width * 4:(y + 1) * width * 4]
                    for y in range(height))

    def chunk(kind: bytes, data: bytes) -> bytes:
        return (struct.pack(">I", len(data)) + kind + data +
                struct.pack(">I", zlib.crc32(kind + data) & 0xffffffff))

    png = (b"\x89PNG\r\n\x1a\n" +
           chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)) +
           chunk(b"IDAT", zlib.compress(rows, 9)) + chunk(b"IEND", b""))
    path.write_bytes(png)


def convert_mockups(directory: Path) -> list[Path]:
    made = []
    for name in ("overlay-compact", "overlay-full"):
        raw = directory / f"{name}.rgba"
        data = raw.read_bytes()
        if len(data) != 960 * 544 * 4:
            raise AssertionError(f"{raw}: unexpected size {len(data)}")
        target = directory / f"{name}.png"
        write_png(target, 960, 544, data)
        raw.unlink()
        made.append(target)
    return made


# ---- manager with the knob --------------------------------------------------

def check_manager_compiles(cc: str) -> None:
    strict = [cc, "-std=gnu11", "-Wall", "-Wextra", "-Werror", "-Wundef",
              f"-I{RUNTIME}", f"-I{VITA}", "-DMANAGER_SETTINGS_UI=1",
              f"-D{KNOB}=1", "-fsyntax-only"]
    for unit in ("manager_settings.c", "manager_settings_ui.c"):
        run(strict + [str(VITA / unit)])


# ---- source pins ------------------------------------------------------------

def _font_rows(text: str, name: str) -> list[list[int]]:
    match = re.search(rf"static const uint8_t {name}\[\d+\]\[7\] = \{{(.*?)\n\}};",
                      text, re.S)
    if not match:
        raise AssertionError(f"font table {name} not found")
    rows = []
    for body in re.findall(r"\{([^}]*)\}", match.group(1)):
        rows.append([int(v.strip().rstrip("u"), 16)
                     for v in body.split(",") if v.strip()])
    return rows


def verify_font() -> None:
    loading = (RUNTIME / "kage_vita_loading.c").read_text(encoding="utf-8")
    overlay = (RUNTIME / SOURCE).read_text(encoding="utf-8")
    digits = _font_rows(loading, "s_loading_digits")
    letters = _font_rows(loading, "s_loading_letters")
    font = _font_rows(overlay, "k_dov_font")
    if len(font) != 64:
        raise AssertionError("overlay font must cover 0x20..0x5f")
    for i, rows in enumerate(digits):
        if font[0x30 - 0x20 + i] != rows:
            raise AssertionError(f"digit {i} differs from kage_vita_loading.c")
    for i, rows in enumerate(letters):
        if font[0x41 - 0x20 + i] != rows:
            raise AssertionError(
                f"letter {chr(65 + i)} differs from kage_vita_loading.c")
    for i, rows in enumerate(font):
        if len(rows) != 7 or any(v > 0x1f for v in rows):
            raise AssertionError(f"glyph 0x{0x20 + i:02x} is not 5x7")


def verify_fences() -> None:
    for relative in TOUCHED_FILES:
        text = (ROOT / relative).read_text(encoding="utf-8")
        name = Path(relative).name
        if name in ("host_vita_lua.c", "host_vita_lua_gc.h"):
            code = trs._strip_comments(text)
            mentions = [line for line in code.splitlines() if KNOB in line]
            if mentions != [OR_GUARD]:
                raise AssertionError(f"{name}: unexpected knob mentions {mentions}")
            continue
        regions = trs._knob_mentions_fenced(text, name, KNOB)
        # host_vita_runtime_settings.h only documents the receipt word.
        if regions == 0 and name != "host_vita_runtime_settings.h":
            raise AssertionError(f"{name}: no {GUARD} region")
    source = (RUNTIME / SOURCE).read_text(encoding="utf-8")
    code = re.sub(r'"(?:[^"\\\n]|\\.)*"', '""', trs._strip_comments(source))
    for token in trs.ALLOCATOR_TOKENS:
        if re.search(rf"\b{token}\s*\(", code):
            raise AssertionError(f"{SOURCE} calls poisoned {token}()")
    if re.search(r"\b(s?n?printf|fprintf|puts)\s*\(", code):
        raise AssertionError(f"{SOURCE} must not use stdio formatting")
    for function in WRAPPED:
        if f"__wrap_{function}(" not in code or f"__real_{function}(" not in code:
            raise AssertionError(f"{SOURCE} lost the {function} wrapper")


FREE_WRAP = "sceKernelFreeMemBlock"
OVERLAY_BLOCK_OPEN = f"if({KNOB})"
OVERLAY_OWNER_SET = "set(ISAAC_GL_FREE_WRAP_BY_OVERLAY ON)"
RECORDER_OWNER_GUARD = "AND NOT ISAAC_GL_FREE_WRAP_BY_OVERLAY"


def enclosing_if(cmake: str, offset: int) -> str:
    """The `if(` line of the innermost CMake block that contains `offset`."""
    depth = 0
    lines = cmake[:offset].splitlines()
    for line in reversed(lines):
        stripped = line.strip()
        if re.match(r"endif\s*\(", stripped):
            depth += 1
        elif re.match(r"if\s*\(", stripped):
            if depth == 0:
                return stripped
            depth -= 1
    return ""


def verify_free_wrap_owners(cmake: str) -> None:
    """sceKernelFreeMemBlock has two possible owners, never both in one link
    (by design): (a) exactly one wrap inside the overlay
    block, before it records ISAAC_GL_FREE_WRAP_BY_OVERLAY; (b) two in the
    whole file; (c) the other one inside a block whose if() line excludes the
    overlay owner."""
    flag = f"-Wl,--wrap={FREE_WRAP}"
    if cmake.count(flag) != 2:
        raise AssertionError(f"CMake must wrap {FREE_WRAP} exactly twice (overlay, recorder)")
    if cmake.count(OVERLAY_BLOCK_OPEN) != 1 or cmake.count(OVERLAY_OWNER_SET) != 1:
        raise AssertionError("debug overlay block / free-wrap owner flag lost")
    start = cmake.index(OVERLAY_BLOCK_OPEN)
    end = cmake.index(OVERLAY_OWNER_SET)
    if end < start or cmake[start:end].count(flag) != 1:
        raise AssertionError(f"the overlay must wrap {FREE_WRAP} exactly once in its block")
    others = [m.start() for m in re.finditer(re.escape(flag), cmake)
              if not start <= m.start() < end]
    if len(others) != 1:
        raise AssertionError(f"exactly one {FREE_WRAP} wrap must lie outside the overlay block")
    guard = enclosing_if(cmake, others[0])
    if RECORDER_OWNER_GUARD not in guard:
        raise AssertionError(
            f"the second {FREE_WRAP} wrap must sit in a block guarded by "
            f"'{RECORDER_OWNER_GUARD}', found {guard!r}")


def verify_cmake() -> None:
    cmake = (VITA / "CMakeLists.txt").read_text(encoding="utf-8")
    release = (ROOT / "release" / "v0.1.1-alpha.cmake").read_text(encoding="utf-8")
    if not re.search(rf'option\({KNOB}\s+"[^"]+" OFF\)', cmake):
        raise AssertionError("debug overlay option must default OFF")
    for needle in (
            f"if({KNOB} AND NOT ISAAC_VITA_KAGE)",
            f"if({KNOB} AND NOT ISAAC_VITA_RUNTIME_SETTINGS)",
            f"if({KNOB} AND ISAAC_VITA_CONTINUE_OVERLAY)"):
        if cmake.count(needle) != 1:
            raise AssertionError(f"CMake debug-overlay FATAL lost: {needle}")
    for function in WRAPPED:
        if function == FREE_WRAP:
            continue
        if cmake.count(f"-Wl,--wrap={function}") != 1:
            raise AssertionError(f"CMake must wrap {function} exactly once")
    verify_free_wrap_owners(cmake)
    if f'set({KNOB} OFF CACHE BOOL "")' not in release:
        raise AssertionError("release recipe must pin the debug overlay OFF")
    gate = (VITA / "vita_raw_allocator_gate.py").read_text(encoding="utf-8")
    test = (VITA / "test_vita_raw_allocator_gate.py").read_text(encoding="utf-8")
    for needle in (f'DEBUG_OVERLAY_CACHE_KEY = "{KNOB}"',
                   f'DEBUG_OVERLAY_SOURCE = "{SOURCE}"'):
        if gate.count(needle) != 1:
            raise AssertionError(f"allocator gate lost {needle}")
    if f'"{KNOB}": "OFF",' not in test:
        raise AssertionError("allocator gate fixture cache lost the knob")


# ---- OFF identity, host half -------------------------------------------------

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.M)


def touched_units(root: Path) -> list[str]:
    """Runtime TUs whose OFF preprocessing could change: the touched .c
    files plus every runtime .c including a touched header (one level of
    runtime headers is followed transitively)."""
    headers = {Path(p).name for p in TOUCHED_FILES if p.endswith(".h")}
    runtime = root / "recomp" / "runtime"
    header_map: dict[str, set[str]] = {}
    for path in runtime.glob("*.h"):
        header_map[path.name] = set(INCLUDE_RE.findall(
            path.read_text(encoding="utf-8", errors="replace")))
    closure = set(headers)
    changed = True
    while changed:
        changed = False
        for name, includes in header_map.items():
            if name not in closure and includes & closure:
                closure.add(name)
                changed = True
    units = {Path(p).name for p in TOUCHED_FILES if p.endswith(".c")}
    for path in runtime.glob("*.c"):
        if "oracle" in path.name or path.name.startswith("test_"):
            continue
        includes = set(INCLUDE_RE.findall(
            path.read_text(encoding="utf-8", errors="replace")))
        if includes & closure:
            units.add(path.name)
    return sorted(units)


def release_definitions() -> list[str]:
    text = (ROOT / "release" / "v0.1.1-alpha.cmake").read_text(encoding="utf-8")
    defs = []
    for name, value in re.findall(r'set\((ISAAC_VITA_\w+) (\S+) CACHE', text):
        if value == "ON":
            defs.append(f"-D{name}=1")
        elif re.fullmatch(r"\d+", value):
            defs.append(f"-D{name}={value}")
    return defs


TOKEN_RE = re.compile(
    r'"(?:[^"\\\n]|\\.)*"|\'(?:[^\'\\\n]|\\.)*\'|[A-Za-z0-9_.]+|\S')


def tokens(text: str) -> list[str]:
    """Token stream of preprocessed C: whitespace and line breaks (which an
    empty macro expansion moves around) are not part of it; literals are
    kept whole."""
    return TOKEN_RE.findall(text)


def extract_base(base: str, target: Path) -> None:
    archive = subprocess.run(["git", "-C", str(ROOT), "archive", base, "recomp"],
                             check=True, capture_output=True).stdout
    with tarfile.open(fileobj=io.BytesIO(archive)) as tar:
        tar.extractall(target)


def preprocess(cc: str, tree: Path, unit: str, defs: list[str],
               stubs: Path) -> str | None:
    runtime = tree / "recomp" / "runtime"
    vita = tree / "recomp" / "vita"
    for _ in range(80):
        done = subprocess.run(
            [cc, "-E", "-P", "-w", f"-I{runtime}", f"-I{vita}", f"-I{stubs}",
             "-DGUEST_IMAGE_BASE=0x98000000u", *defs, str(runtime / unit)],
            check=False, text=True, capture_output=True)
        if done.returncode == 0:
            return done.stdout
        missing = re.search(r"'([^']+)' file not found", done.stderr)
        if not missing:
            return None
        stub = stubs / missing.group(1)
        stub.parent.mkdir(parents=True, exist_ok=True)
        stub.write_text("", encoding="utf-8")
    return None


def off_identity(cc: str, base: str, output: Path) -> list[str]:
    lines = []
    base_tree = output / "base-tree"
    if base_tree.exists():
        shutil.rmtree(base_tree)
    base_tree.mkdir()
    extract_base(base, base_tree)
    stubs = output / "stubs"
    stubs.mkdir(exist_ok=True)
    units = touched_units(ROOT)
    configs = (("none", []), ("release", release_definitions()))
    compared = 0
    new_units = [unit for unit in units
                 if not (base_tree / "recomp" / "runtime" / unit).exists()]
    if new_units != [SOURCE]:
        raise AssertionError(f"unexpected new runtime units: {new_units}")
    units = [unit for unit in units if unit not in new_units]
    for unit in units:
        for label, defs in configs:
            head = preprocess(cc, ROOT, unit, defs, stubs)
            old = preprocess(cc, base_tree, unit, defs, stubs)
            if head is None or old is None:
                if (head is None) != (old is None):
                    raise AssertionError(
                        f"{unit} [{label}] preprocesses in one tree only")
                lines.append(f"  {unit} [{label}]: preprocessor error in both "
                             "trees (config not applicable)")
                continue
            if tokens(head) != tokens(old):
                raise AssertionError(
                    f"{unit} [{label}]: OFF token stream differs from {base}")
            compared += 1
    # Teeth: with the knob defined, every directly edited unit must differ,
    # or the comparison above could not have seen a fenced edit leak.
    sensitive = []
    for unit in units:
        if f"recomp/runtime/{unit}" not in TOUCHED_FILES:
            continue
        defs = release_definitions() + [f"-D{KNOB}=1"]
        on = preprocess(cc, ROOT, unit, defs, stubs)
        off = preprocess(cc, ROOT, unit, release_definitions(), stubs)
        if on is None or off is None or tokens(on) == tokens(off):
            raise AssertionError(
                f"{unit}: the knob changes nothing in the token stream; the "
                "OFF comparison has no teeth for it")
        sensitive.append(unit)
    lines.insert(0, f"OFF identity (-E -P, knob undefined): {compared} "
                    f"unit/config pairs equal to {base}; units: "
                    + ", ".join(units))
    lines.insert(1, "  comparator teeth: knob ON changes the token stream of "
                    + ", ".join(sensitive))
    # pure parser TU: object identity with WSL gcc from the same relative path
    wsl = shutil.which("wsl")
    if wsl:
        def wsl_path(path: Path) -> str:
            drive = path.drive.rstrip(":").lower()
            return "/mnt/" + drive + path.as_posix()[2:]
        objects = []
        for tree in (base_tree, ROOT):
            for label, defs in (("none", []), ("release", release_definitions())):
                obj = f"/tmp/dov-parse-{hashlib.sha1(str(tree).encode()).hexdigest()[:8]}-{label}.o"
                script = (f"cd {wsl_path(tree)} && gcc -std=gnu11 -O2 -c "
                          f"{' '.join(defs)} recomp/runtime/host_vita_settings_parse.c "
                          f"-o {obj} && sha256sum {obj}")
                done = subprocess.run([wsl, "-d", "Ubuntu-22.04", "--", "bash", "-c",
                                       script], check=False, text=True,
                                      capture_output=True)
                if done.returncode != 0:
                    raise AssertionError(f"WSL gcc failed: {done.stderr}")
                objects.append((label, done.stdout.split()[0]))
        base_objs = dict(objects[:2])
        head_objs = dict(objects[2:])
        if base_objs != head_objs:
            raise AssertionError(f"parser object differs: {base_objs} vs {head_objs}")
        lines.append("OFF identity (WSL gcc 11 objects): host_vita_settings_parse.c "
                     f"none={head_objs['none'][:16]} release="
                     f"{head_objs['release'][:16]} equal to {base}")
    else:
        lines.append("WSL gcc object check skipped: no wsl")
    return lines


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--base", help="commit for the OFF identity half")
    parser.add_argument("--mockup", type=Path, help="write PNG mock-ups here")
    args = parser.parse_args()
    cc = trs.compiler()
    verify_font()
    verify_fences()
    verify_cmake()
    with tempfile.TemporaryDirectory() as temp:
        output = Path(temp)
        mock_dir = args.mockup.resolve() if args.mockup else None
        if mock_dir:
            mock_dir.mkdir(parents=True, exist_ok=True)
        stdout = build_and_run_oracle(cc, output, mock_dir)
        print(stdout.rstrip())
        if mock_dir:
            for path in convert_mockups(mock_dir):
                print(f"mockup png: {path}")
        check_manager_compiles(cc)
        print("manager units compile with MANAGER_SETTINGS_UI + the overlay knob")
        if args.base:
            for line in off_identity(cc, args.base, output):
                print(line)
        else:
            print("OFF identity half skipped: pass --base <commit>")
    print("Vita debug overlay contract: PASS (oracle, knob-off empty TU, "
          "manager compiles with the knob, font copy, fences, CMake option/"
          "FATALs/--wrap/release pin, allocator gate registration)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
