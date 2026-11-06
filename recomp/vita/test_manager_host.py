#!/usr/bin/env python3
"""Configure, build and run every target of recomp/vita/host_tests, then
prove the manager's knob-OFF object identity against a base commit.

The host_tests tree is a plain CMake project (manager FTP/storage/settings
tests, the port.ini parser and loader oracles, the profile and FBO policy
tests).  This wrapper is the documented way to run all of it:

  * native: `cmake -S recomp/vita/host_tests -B <build>`, `cmake --build`,
    `ctest --output-on-failure`; needs cmake, ctest and a C compiler (env CC,
    then cc / gcc / clang on PATH);
  * Windows without a C compiler (the usual state of a Windows checkout:
    CMake installed, no gcc/clang): the script re-runs itself inside WSL
    (`wsl -d <distro> -- bash -lc 'cd /mnt/<drive>/<path> && python3 ...'`)
    with the repository path translated to the WSL mount; the distro comes
    from ISAAC_WSL_DISTRO (default Ubuntu-22.04).  `--wsl` forces that route,
    `--native` forbids it (the run then fails with a clear message when no
    compiler is present).

It never skips silently: a missing toolchain is a failure.  Besides ctest's
own verdict it pins that the manager targets exist in the ctest list
(manager_ftp_host_test, manager_storage_host_test,
manager_storage_gamestate_host_test, manager_settings_host_test,
manager_settings_ui_host_test, and the game-skin decoders:
manager_gameskin_fixtures, manager_arch_host_test,
manager_png_host_test, manager_bmfont_host_test, plus the skin itself,
manager_skin_game_host_test), so a CMakeLists.txt edit that drops one of
them fails here rather than passing with fewer tests.
manager_gameskin_fixtures is a ctest fixture-setup step that regenerates the
synthetic archives, PNGs and the synthetic game install of the skin test
(tools/isaac_vita_repack.py's encoder, stdlib zlib) on every run with the
interpreter running this script (passed to CMake as Python3_EXECUTABLE).

OFF-object identity: with the manager
knobs (MANAGER_SETTINGS_UI, ISAAC_VITA_MANAGER_SAVE_GAMESTATE, MANAGER_UI2)
undefined,
every isaac_manager translation unit must compile to the same bytes as at
the base commit.  The gate extracts recomp/vita/*.c and *.h of the base
commit into a scratch tree, compiles each manager TU there and in the
checkout with the production isaac_manager flags (basename on the command
line from the TU's own directory, so __FILE__ and the .file symbol agree)
and cmp's the two objects.  Two flavours:

  * host (always, with the host compiler; -mcpu/-mthumb dropped): the
    SDK-free TUs manager_ftp_lifecycle.c, manager_ftp_path.c,
    manager_ftp_protocol.c, manager_sha256.c, manager_storage.c;
  * ARM (VITASDK set and arm-vita-eabi-gcc present): all eight TUs of the
    add_executable(isaac_manager ...) list under the exact production flags,
    plus a -Werror compile of every TU with each knob ON.  Without the SDK
    the ARM half prints a loud skip line; it does not fail.  The stripped
    isaac-manager.bin comparison needs the full CMake tree (release cache,
    generated corpus) and stays with the release build procedure.

The base commit comes from --base, else ISAAC_MANAGER_IDENTITY_BASE, else
`git merge-base HEAD origin/main` / `main`; when none resolves the identity
gate prints a loud skip.

Options: --build-dir (default: a fresh temporary directory), --jobs,
--keep (do not delete the temporary build directory), --base <commit>.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parents[1]
HOST_TESTS = HERE / "host_tests"
DEFAULT_WSL_DISTRO = "Ubuntu-22.04"
# ctest names that must be present (the manager's host tests).
REQUIRED_TESTS = (
    "manager_ftp_host_test",
    "manager_storage_host_test",
    "manager_storage_gamestate_host_test",
    "manager_settings_host_test",
    "manager_settings_ui_host_test",
    # Manager UI2 (knob ISAAC_VITA_MANAGER_UI2).
    "manager_font_host_test",
    "manager_strings_host_test",
    "manager_ini_host_test",
    "manager_mods_meta_host_test",
    "manager_ui_host_test",
    "manager_ui_render_host_test",
    # Manager UI2 game skin (ISAAC_VITA_MANAGER_UI2_GAME_SKIN).
    "manager_gameskin_fixtures",
    "manager_arch_host_test",
    "manager_png_host_test",
    "manager_bmfont_host_test",
    # The skin itself over the fixtures' synthetic install.
    "manager_skin_game_host_test",
    # The ACHIEVEMENTS trophy import row, the SETTINGS debug overlay row and
    # their strings (ISAAC_VITA_MANAGER_TROPHY_IMPORT,
    # ISAAC_VITA_MANAGER_DEBUG_OVERLAY_ROW).
    "manager_ui_trophy_host_test",
    "manager_ui_overlay_host_test",
    "manager_strings_knobs_host_test",
)
# add_executable(isaac_manager ...) of recomp/vita/CMakeLists.txt (pinned
# verbatim by test_livearea_package.py); the knob units are added with
# target_sources() only when ISAAC_VITA_MANAGER_SETTINGS_UI is ON.
MANAGER_SOURCES = (
    "manager_main.c",
    "manager_ftp_lifecycle.c",
    "manager_ftp_path.c",
    "manager_ftp_protocol.c",
    "manager_ftp_server.c",
    "manager_sha256.c",
    "manager_storage.c",
    "manager_storage_vita.c",
)
# The subset host_tests/CMakeLists.txt compiles with a host compiler (no
# psp2 headers), hence provable on any host.
SDK_FREE_MANAGER_SOURCES = (
    "manager_ftp_lifecycle.c",
    "manager_ftp_path.c",
    "manager_ftp_protocol.c",
    "manager_sha256.c",
    "manager_storage.c",
)
# Units the manager knobs add; compiled ON only (never linked OFF).
MANAGER_KNOB_UNITS = ("manager_settings.c", "manager_settings_ui.c")
# Manager UI2 (ISAAC_VITA_MANAGER_UI2 -> MANAGER_UI2=1, which
# CMake allows only together with MANAGER_SETTINGS_UI=1): its SDK-free units
# (host and ARM) and its sceIo font loader (ARM only, needs psp2 headers).
MANAGER_UI2_UNITS = (
    "manager_font.c", "manager_canvas.c", "manager_theme.c",
    "manager_strings.c", "manager_ui.c", "manager_ui_draw.c",
    "manager_mods_meta.c", "manager_ini.c",
)
MANAGER_UI2_SDK_UNITS = ("manager_assets_vita.c",)
# Each ON variant is the set of definitions one knob configuration gives
# isaac_manager (UI2 always with the settings page, as its FATAL demands).
MANAGER_KNOBS = (
    ("MANAGER_SETTINGS_UI",),
    ("ISAAC_VITA_MANAGER_SAVE_GAMESTATE",),
    ("MANAGER_SETTINGS_UI", "MANAGER_UI2"),
    ("MANAGER_SETTINGS_UI", "MANAGER_UI2",
     "ISAAC_VITA_MANAGER_SAVE_GAMESTATE"),
    # The game skin needs UI2 (CMake FATAL): manager_main.c and the UI2
    # units see MANAGER_UI2_GAME_SKIN (manager_theme.c's skin hook).
    ("MANAGER_SETTINGS_UI", "MANAGER_UI2", "MANAGER_UI2_GAME_SKIN"),
    # The trophy import and debug overlay rows need UI2 (CMake FATAL); they
    # add no unit.  Every manager knob at once as well.
    ("MANAGER_SETTINGS_UI", "MANAGER_UI2", "MANAGER_TROPHY_IMPORT"),
    ("MANAGER_SETTINGS_UI", "MANAGER_UI2", "MANAGER_DEBUG_OVERLAY_ROW"),
    ("MANAGER_SETTINGS_UI", "MANAGER_UI2", "MANAGER_UI2_GAME_SKIN",
     "MANAGER_TROPHY_IMPORT", "MANAGER_DEBUG_OVERLAY_ROW",
     "ISAAC_VITA_MANAGER_SAVE_GAMESTATE"),
)
# Units ISAAC_VITA_MANAGER_UI2_GAME_SKIN adds (target_sources() in its knob
# block); new files, so there is no OFF object to compare - they are compiled
# ON only, with the defines a game-skin manager build carries (UI2 needs the
# settings UI, the game skin needs UI2).  manager_blob_vita.c includes psp2
# headers and is compiled in the ARM half only.
GAMESKIN_UNITS = ("manager_skin_game.c", "manager_arch.c", "manager_png.c",
                  "manager_bmfont.c", "manager_tinfl.c")
GAMESKIN_SDK_UNITS = ("manager_blob_vita.c",)
GAMESKIN_DEFINES = ("MANAGER_SETTINGS_UI", "MANAGER_UI2",
                    "MANAGER_UI2_GAME_SKIN")
# target_compile_options(isaac_manager ...) plus -Werror for the gate.
ARM_MANAGER_FLAGS = (
    "-std=gnu11", "-O2",
    "-mcpu=cortex-a9", "-mfpu=neon", "-mfloat-abi=softfp", "-mthumb",
    "-ffunction-sections", "-fdata-sections",
    "-Wall", "-Wextra", "-Werror",
)
HOST_MANAGER_FLAGS = tuple(
    flag for flag in ARM_MANAGER_FLAGS if not flag.startswith("-m")
)
BASE_ENVIRONMENT = "ISAAC_MANAGER_IDENTITY_BASE"


class HostTestError(RuntimeError):
    pass


def find_compiler() -> str | None:
    override = os.environ.get("CC")
    if override:
        found = shutil.which(override)
        if found:
            return found
        if Path(override).is_file():
            return override
        raise HostTestError(f"CC={override!r} is not an executable")
    for name in ("cc", "gcc", "clang"):
        found = shutil.which(name)
        if found:
            return found
    return None


def run(command: list[str], cwd: Path | None = None) -> str:
    completed = subprocess.run(
        command, cwd=None if cwd is None else str(cwd), check=False,
        text=True, capture_output=True,
    )
    if completed.returncode != 0:
        raise HostTestError(
            f"command failed ({completed.returncode}): {command!r}\n"
            f"stdout:\n{completed.stdout}\nstderr:\n{completed.stderr}"
        )
    return completed.stdout


def windows_to_wsl_path(path: Path) -> str:
    """D:\\a\\b -> /mnt/d/a/b (drive letters only; UNC paths are refused)."""
    resolved = path.resolve()
    drive = resolved.drive
    if not re.fullmatch(r"[A-Za-z]:", drive):
        raise HostTestError(f"cannot map {resolved} into WSL (no drive letter)")
    tail = resolved.as_posix()[len(drive):]
    return f"/mnt/{drive[0].lower()}{tail}"


def run_in_wsl(jobs: int | None, base: str | None) -> int:
    """Re-run this script natively inside WSL; the child's output streams
    through, its exit code is ours.  A Windows worktree's .git file carries
    a Windows gitdir path that WSL git cannot follow, so the base commit is
    resolved and its recomp/vita sources are extracted here, with Windows
    git, and handed to the child as --base-tree."""
    if os.name != "nt":
        raise HostTestError("--wsl is only meaningful on Windows")
    wsl = shutil.which("wsl")
    if wsl is None:
        raise HostTestError("no C compiler on PATH and wsl.exe not found")
    distro = os.environ.get("ISAAC_WSL_DISTRO", DEFAULT_WSL_DISTRO)
    script = REPO / "recomp" / "vita" / "test_manager_host.py"
    inner = (
        f"cd {windows_to_wsl_path(REPO)} && python3 "
        f"{windows_to_wsl_path(script)} --native"
    )
    if jobs is not None:
        inner += f" --jobs {int(jobs)}"
    resolved = resolve_base(base)
    base_tree: str | None = None
    if resolved is not None:
        base_tree = tempfile.mkdtemp(prefix="isaac-manager-base-")
        extract_base_vita(resolved, Path(base_tree))
        inner += (f" --base-tree {windows_to_wsl_path(Path(base_tree))}"
                  f" --base-label {resolved}")
    print(f"no host C compiler on PATH; running inside WSL ({distro})",
          flush=True)
    try:
        completed = subprocess.run(
            [wsl, "-d", distro, "--", "bash", "-lc", inner], check=False,
        )
    finally:
        if base_tree is not None:
            shutil.rmtree(base_tree, ignore_errors=True)
    return completed.returncode


def ctest_names(build: Path, ctest: str) -> list[str]:
    listing = run([ctest, "-N"], cwd=build)
    return re.findall(r"^\s*Test\s+#\d+:\s+(\S+)", listing, flags=re.MULTILINE)


# ---- OFF-object identity --------------------------------------------------


def git(*arguments: str) -> str:
    return run(["git", "-C", str(REPO), *arguments])


def resolve_base(requested: str | None) -> str | None:
    """Commit the OFF objects are compared against, or None (loud skip)."""
    candidates: list[list[str]] = []
    explicit = requested or os.environ.get(BASE_ENVIRONMENT)
    if explicit:
        candidates.append(["rev-parse", "--verify", "-q", f"{explicit}^{{commit}}"])
    else:
        candidates.append(["merge-base", "HEAD", "origin/main"])
        candidates.append(["merge-base", "HEAD", "main"])
    for candidate in candidates:
        completed = subprocess.run(
            ["git", "-C", str(REPO), *candidate], check=False, text=True,
            capture_output=True,
        )
        if completed.returncode == 0 and completed.stdout.strip():
            return completed.stdout.strip()
    if explicit:
        raise HostTestError(f"identity base {explicit!r} is not a commit")
    return None


def extract_base_vita(base: str, output: Path) -> Path:
    """recomp/vita/*.c and *.h (top level) of `base` into output/recomp/vita
    so base TUs compile against base headers."""
    listing = git("ls-tree", "--name-only", base, "recomp/vita/")
    target = output / "recomp" / "vita"
    target.mkdir(parents=True, exist_ok=True)
    count = 0
    for line in listing.splitlines():
        name = line.strip()
        if not (name.endswith(".c") or name.endswith(".h")):
            continue
        blob = subprocess.run(
            ["git", "-C", str(REPO), "show", f"{base}:{name}"], check=False,
            capture_output=True,
        )
        if blob.returncode != 0:
            raise HostTestError(f"git show {base}:{name} failed")
        (target / Path(name).name).write_bytes(blob.stdout)
        count += 1
    if count == 0:
        raise HostTestError(f"{base} has no recomp/vita sources")
    return target


def compile_unit(compiler: str, flags: tuple[str, ...], source_dir: Path,
                 name: str, output: Path, extra: list[str]) -> Path:
    """Compile source_dir/name with the basename on the command line from
    its own directory (identical __FILE__ / .file for base and HEAD)."""
    output.mkdir(parents=True, exist_ok=True)
    obj = output / f"{name}.o"
    run([compiler, *flags, *extra, "-c", name, "-o", str(obj)], cwd=source_dir)
    return obj


def prove_off_identity(compiler: str, flags: tuple[str, ...],
                       sources: tuple[str, ...], base: str, base_vita: Path,
                       output: Path, label: str) -> str:
    """Knobs undefined: every object of `sources` byte-identical to base."""
    head_dir = output / f"{label}-head"
    base_dir = output / f"{label}-base"
    head_dir.mkdir(parents=True, exist_ok=True)
    base_dir.mkdir(parents=True, exist_ok=True)
    differing: list[str] = []
    for name in sources:
        if not (base_vita / name).is_file():
            raise HostTestError(f"{name} does not exist at base {base}")
        head_obj = compile_unit(compiler, flags, HERE, name, head_dir, [])
        base_obj = compile_unit(compiler, flags, base_vita, name, base_dir, [])
        if head_obj.read_bytes() != base_obj.read_bytes():
            differing.append(name)
    if differing:
        raise HostTestError(
            f"{label} OFF objects differ from base {base[:12]}: {differing} "
            "(a manager edit escaped its #if defined(<knob>) fence)"
        )
    return (f"{label} OFF identity: {len(sources)} manager objects "
            f"byte-identical to {base[:12]}")


def compile_on_variants(compiler: str, flags: tuple[str, ...],
                        sources: tuple[str, ...], output: Path,
                        label: str, sdk: bool = False) -> str:
    """Every knob configuration ON compiles -Werror (the knob units with the
    settings knob and the runtime include directory; the UI2 units under
    each UI2 configuration, the sceIo loader only with the SDK)."""
    on_dir = output / f"{label}-on"
    on_dir.mkdir(parents=True, exist_ok=True)
    runtime = str(REPO / "recomp" / "runtime")
    count = 0
    ui2_units = MANAGER_UI2_UNITS + (MANAGER_UI2_SDK_UNITS if sdk else ())
    for variant in MANAGER_KNOBS:
        extra = [f"-D{knob}=1" for knob in variant] + [f"-I{runtime}"]
        variant_dir = on_dir / "+".join(variant)
        for name in sources:
            compile_unit(compiler, flags, HERE, name, variant_dir, extra)
            count += 1
        if "MANAGER_UI2" in variant:
            for name in ui2_units:
                compile_unit(compiler, flags, HERE, name,
                             variant_dir / "ui2", extra)
                count += 1
    for name in MANAGER_KNOB_UNITS:
        compile_unit(compiler, flags, HERE, name, on_dir / "units",
                     ["-DMANAGER_SETTINGS_UI=1", f"-I{runtime}"])
        count += 1
    count += compile_gameskin_units(compiler, flags, on_dir, label)
    return f"{label} ON compile: {count} objects -Werror clean"


def compile_gameskin_units(compiler: str, flags: tuple[str, ...],
                           on_dir: Path, label: str) -> int:
    """The game-skin units with every define of that build, -Werror."""
    runtime = str(REPO / "recomp" / "runtime")
    defines = [f"-D{name}=1" for name in GAMESKIN_DEFINES]
    units = GAMESKIN_UNITS + (GAMESKIN_SDK_UNITS if label == "arm" else ())
    for name in units:
        compile_unit(compiler, flags, HERE, name, on_dir / "gameskin",
                     [*defines, f"-I{runtime}"])
    return len(units)


def arm_compiler() -> str | None:
    sdk = os.environ.get("VITASDK")
    if not sdk:
        return None
    gcc = Path(sdk) / "bin" / "arm-vita-eabi-gcc"
    return str(gcc) if gcc.is_file() else None


def run_identity(cc: str, base: str | None, base_tree: Path | None,
                 output: Path) -> list[str]:
    """base_tree (recomp/vita of the base already extracted, WSL route) or
    an extraction from `base` with git; None for both = loud skip."""
    lines: list[str] = []
    if base is None:
        lines.append(
            "OFF identity SKIPPED: no base commit (pass --base <commit> or set "
            f"{BASE_ENVIRONMENT}; origin/main and main did not resolve)"
        )
        return lines
    if base_tree is not None:
        base_vita = base_tree / "recomp" / "vita"
        if not base_vita.is_dir():
            raise HostTestError(f"--base-tree {base_tree} has no recomp/vita")
    else:
        base_vita = extract_base_vita(base, output / "base-tree")
    lines.append(prove_off_identity(cc, HOST_MANAGER_FLAGS,
                                    SDK_FREE_MANAGER_SOURCES, base, base_vita,
                                    output, "host"))
    lines.append(compile_on_variants(cc, HOST_MANAGER_FLAGS,
                                     SDK_FREE_MANAGER_SOURCES, output, "host"))
    arm = arm_compiler()
    if arm is None:
        lines.append(
            "ARM OFF identity SKIPPED: VITASDK unset or arm-vita-eabi-gcc "
            "missing (the host half above covers the SDK-free TUs only)"
        )
        return lines
    lines.append(prove_off_identity(arm, ARM_MANAGER_FLAGS, MANAGER_SOURCES,
                                    base, base_vita, output, "arm"))
    lines.append(compile_on_variants(arm, ARM_MANAGER_FLAGS, MANAGER_SOURCES,
                                     output, "arm", sdk=True))
    return lines


# ---- host_tests -----------------------------------------------------------


def run_native(build: Path, jobs: int | None, base: str | None,
               base_tree: Path | None = None,
               base_label: str | None = None) -> int:
    cmake = shutil.which("cmake")
    ctest = shutil.which("ctest")
    cc = find_compiler()
    missing = [name for name, found in (("cmake", cmake), ("ctest", ctest),
                                        ("a C compiler (CC/cc/gcc/clang)", cc))
               if found is None]
    if missing or cmake is None or ctest is None or cc is None:
        raise HostTestError(
            "host toolchain incomplete: missing " + ", ".join(missing) +
            ("; on Windows run without --native to use WSL" if os.name == "nt"
             else "")
        )
    configure = [cmake, "-S", str(HOST_TESTS), "-B", str(build),
                 "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_C_COMPILER={cc}",
                 f"-DPython3_EXECUTABLE={sys.executable}"]
    print("== configure ==", flush=True)
    run(configure)
    print("== build ==", flush=True)
    build_command = [cmake, "--build", str(build)]
    if jobs is not None:
        build_command += ["--parallel", str(int(jobs))]
    else:
        build_command += ["--parallel"]
    run(build_command)
    names = ctest_names(build, ctest)
    missing_tests = [name for name in REQUIRED_TESTS if name not in names]
    if missing_tests:
        raise HostTestError(
            f"host_tests lost ctest targets: {missing_tests}; present: {names}"
        )
    print(f"== ctest ({len(names)} tests) ==", flush=True)
    completed = subprocess.run([ctest, "--output-on-failure"], cwd=str(build),
                               check=False)
    if completed.returncode != 0:
        raise HostTestError(f"ctest failed ({completed.returncode})")
    print("== manager OFF-object identity ==", flush=True)
    if base_tree is not None:
        resolved: str | None = base_label or "base-tree"
    else:
        resolved = resolve_base(base)
    identity_lines = run_identity(cc, resolved, base_tree, build / "identity")
    for line in identity_lines:
        print(line, flush=True)
    print(
        f"manager host tests: PASS ({len(names)} ctest targets; manager: "
        + ", ".join(REQUIRED_TESTS) + f"; compiler {cc}; "
        + "; ".join(identity_lines) + ")"
    )
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=(__doc__ or "").split("\n\n")[0])
    parser.add_argument("--build-dir", type=Path, default=None,
                        help="reuse/create this build directory (default: temp)")
    parser.add_argument("--jobs", type=int, default=None,
                        help="parallel build jobs (default: cmake's choice)")
    parser.add_argument("--keep", action="store_true",
                        help="keep the temporary build directory")
    parser.add_argument("--base", default=None,
                        help=("commit the knob-OFF manager objects are compared "
                              f"against (default: {BASE_ENVIRONMENT}, then "
                              "merge-base with origin/main or main)"))
    parser.add_argument("--base-tree", type=Path, default=None,
                        help=("directory holding recomp/vita/*.c|*.h of the base "
                              "commit (the WSL route extracts it with Windows "
                              "git and passes it here)"))
    parser.add_argument("--base-label", default=None,
                        help="commit id reported for --base-tree")
    route = parser.add_mutually_exclusive_group()
    route.add_argument("--wsl", action="store_true",
                       help="force the WSL route (Windows only)")
    route.add_argument("--native", action="store_true",
                       help="never fall back to WSL")
    args = parser.parse_args()
    base_tree: Path | None = args.base_tree
    base_label: str | None = args.base_label
    try:
        if args.wsl or (os.name == "nt" and not args.native and
                        find_compiler() is None):
            return run_in_wsl(args.jobs, args.base)
        if args.build_dir is not None:
            args.build_dir.mkdir(parents=True, exist_ok=True)
            return run_native(args.build_dir.resolve(), args.jobs, args.base,
                              base_tree, base_label)
        temporary = tempfile.mkdtemp(prefix="isaac-manager-host-")
        try:
            return run_native(Path(temporary), args.jobs, args.base,
                              base_tree, base_label)
        finally:
            if args.keep:
                print(f"build directory kept: {temporary}")
            else:
                shutil.rmtree(temporary, ignore_errors=True)
    except HostTestError as exc:
        print(f"manager host tests: FAIL: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
