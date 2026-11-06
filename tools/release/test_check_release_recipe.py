#!/usr/bin/env python3
"""Executable fixture tests for tools/release/check_release_recipe.py."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile


CHECKER = Path(__file__).with_name("check_release_recipe.py").resolve()
PROJECT_ROOT = CHECKER.parents[2]
RELEASE_DIR = PROJECT_ROOT / "release"
CMAKELISTS = PROJECT_ROOT / "recomp" / "vita" / "CMakeLists.txt"

spec = importlib.util.spec_from_file_location("check_release_recipe", CHECKER)
assert spec and spec.loader
crr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(crr)  # type: ignore[union-attr]

REQUIRED = [name for name, _ in crr.REQUIRED]


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def run(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(CHECKER), *args],
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
    )


def last_line(completed: subprocess.CompletedProcess[str]) -> str:
    lines = completed.stdout.strip().splitlines()
    return lines[-1] if lines else ""


def full_recipe(**overrides: str) -> str:
    values = {name: "ON" for name in REQUIRED}
    values["ISAAC_VITA_GL_THREAD_WORKER"] = "ON"
    values.update(overrides)
    return "".join(f'set({k} {v} CACHE BOOL "")\n' for k, v in values.items())


def test_truth() -> None:
    for value in ("ON", "on", "1", "YES", "true", "Y", "2", "0.5", "-1"):
        check(crr.truth(value) is True, f"{value!r} must be true")
    for value in ("OFF", "0", "no", "FALSE", "N", "IGNORE", "NOTFOUND", "", "X-NOTFOUND", "0.0"):
        check(crr.truth(value) is False, f"{value!r} must be false")
    for value in ("enabled", "OBSERVE", "master"):
        check(crr.truth(value) is None, f"{value!r} is no CMake constant")


def test_parse_cache() -> None:
    text = (
        "# This is the CMakeCache file.\n"
        "//Doc line\n"
        "ISAAC_VITA_FBO_DEPTH_CARRY:BOOL=ON\n"
        "ISAAC_VITA_GUEST_LINK_ID:STRING=rel:v0.1.2-alpha-rc1\n"
        "ISAAC_VITA_LUA_BINDING_REPLAY_FAMILIES:STRING=scalar;getdata\n"
        "ISAAC_VITA_FBO_DEPTH_CARRY-ADVANCED:INTERNAL=1\n"
        "\n"
    )
    entries = crr.parse_cache(text)
    check(entries["ISAAC_VITA_FBO_DEPTH_CARRY"] == ("ON", "BOOL"), str(entries))
    check(entries["ISAAC_VITA_GUEST_LINK_ID"][0] == "rel:v0.1.2-alpha-rc1", str(entries))
    check(entries["ISAAC_VITA_LUA_BINDING_REPLAY_FAMILIES"][0] == "scalar;getdata", str(entries))
    try:
        crr.parse_cache("not a cache line\n")
    except crr.RecipeError:
        pass
    else:
        raise AssertionError("a non-cache line must be rejected")


def test_parse_recipe_semantics() -> None:
    text = (
        "# comment\n"
        'set(A ON CACHE BOOL "")\n'
        'set(A OFF CACHE BOOL "")          # no FORCE: the first set wins\n'
        'set(B OFF CACHE BOOL "")\n'
        'set(B ON CACHE BOOL "" FORCE)     # FORCE replaces\n'
        'set(C "two words" CACHE STRING "doc (with parens)")\n'
        'set(D x;y CACHE STRING "")\n'
        'set(E "a\\"b" CACHE STRING "")\n'
        "set(F\n    ON CACHE BOOL\n    \"multi-line\")\n"
        "set(G ON)                         # no CACHE: never reaches the cache\n"
        'set(H "a\\${B}" CACHE STRING "")   # an escaped $ is literal\n'
    )
    entries, notes = crr.parse_recipe(text)
    check(entries["A"][0] == "ON", f"A: {entries['A']}")
    check(entries["B"][0] == "ON", f"B: {entries['B']}")
    check(entries["C"][0] == "two words", f"C: {entries['C']}")
    check(entries["D"][0] == "x;y", f"D: {entries['D']}")
    check(entries["E"][0] == 'a"b', f"E: {entries['E']}")
    check(entries["F"] == ("ON", "BOOL"), f"F: {entries['F']}")
    check("G" not in entries, "set() without CACHE must not create an entry")
    check(entries["H"][0] == "a${B}", f"H: {entries['H']}")
    check(any("A=OFF ignored" in n for n in notes), f"notes: {notes}")
    check(any("without CACHE" in n for n in notes), f"notes: {notes}")
    for bad in ('if(X)\nendif()\n', 'set(A ${B} CACHE BOOL "")\n', 'set(A ON CACHE BOOL ""\n',
                'include(other.cmake)\n', 'set(A "x${B}" CACHE STRING "")\n',
                'set(A "$ENV{HOME}" CACHE STRING "")\n', 'set(A "$CACHE{B}" CACHE STRING "")\n',
                'set(A $CACHE{B} CACHE STRING "")\n'):
        try:
            crr.parse_recipe(bad)
        except crr.RecipeError:
            continue
        raise AssertionError(f"must fail closed on {bad!r}")


def test_required_and_dependency() -> None:
    entries, _ = crr.parse_recipe(full_recipe())
    _, missing = crr.check(entries)
    check(missing == [], f"full recipe: {missing}")
    entries, _ = crr.parse_recipe(full_recipe(ISAAC_VITA_CRT_FREAD_COMPLETE="OFF"))
    _, missing = crr.check(entries)
    check(missing == ["ISAAC_VITA_CRT_FREAD_COMPLETE"], f"one OFF: {missing}")
    entries, _ = crr.parse_recipe(full_recipe(ISAAC_VITA_GL_THREAD_WORKER="OFF"))
    _, missing = crr.check(entries)
    check(missing == ["ISAAC_VITA_GL_THREAD_WORKER"], f"S1 without the GL worker: {missing}")
    entries, _ = crr.parse_recipe(full_recipe(ISAAC_VITA_TEXEL_SCRATCH_32M="enabled"))
    _, missing = crr.check(entries)
    check(missing == ["ISAAC_VITA_TEXEL_SCRATCH_32M"], f"a non-constant is not ON: {missing}")


def test_cli_on_fixtures() -> None:
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        good = root / "good.cmake"
        good.write_text(full_recipe(ISAAC_VITA_CXX_THROW_DIAG="OFF"), encoding="utf-8")
        done = run(str(good))
        check(done.returncode == 0, done.stdout)
        check(last_line(done) == f"release recipe check: PASS ({len(REQUIRED)}/{len(REQUIRED)} required ON)",
              done.stdout)
        check("advisory ISAAC_VITA_CXX_THROW_DIAG=OFF" in done.stdout, "advisory must be reported, not enforced")

        cache = root / "CMakeCache.txt"
        cache.write_text(
            "".join(f"{k}:BOOL=ON\n" for k in REQUIRED)
            + "ISAAC_VITA_GL_THREAD_WORKER:BOOL=ON\n"
            + "ISAAC_VITA_GUEST_LINK_ID:STRING=rel:v9-rc1\n"
            + "ISAAC_VITA_HEAP_MB:STRING=81\n"
            + "ISAAC_VITA_LATER_KNOB:BOOL=OFF\n"
            + "ISAAC_VITA_DOLLAR_PROBE:STRING=a${B} c\n"                     # emitted as "a\${B} c"
            + "ISAAC_VITA_TROPHY_PACK_DIR:PATH=/stage/trophy-packs-v2\n"
            + "ISAAC_VITA_THEORADEC_PREFIX:STRING=/stage/theora/prefix\n"   # a command-line -D caches paths as STRING
            + "ISAAC_GUEST_BASE:STRING=0x98000000\n"
            + "CMAKE_BUILD_TYPE:STRING=Release\n"
            + "CMAKE_AR:FILEPATH=/opt/ar\n",
            encoding="utf-8",
        )
        done = run(str(cache))
        check(done.returncode == 0 and "(CMakeCache," in done.stdout, done.stdout)

        body = run("--emit-body", str(cache))
        check(body.returncode == 0, body.stdout)
        check("TROPHY_PACK_DIR" not in body.stdout and "CMAKE_AR" not in body.stdout
              and "THEORADEC_PREFIX" not in body.stdout and "/stage" not in body.stdout,
              "per-machine paths must not be emitted, whatever their cache type")
        check('set(CMAKE_BUILD_TYPE Release CACHE STRING "")' in body.stdout, body.stdout)
        check('set(ISAAC_VITA_GUEST_LINK_ID rel:v9-rc1 CACHE STRING "")' in body.stdout, body.stdout)
        check('set(ISAAC_VITA_DOLLAR_PROBE "a\\${B} c" CACHE STRING "")' in body.stdout, body.stdout)
        relabelled = run("--emit-body", str(cache), "--link-id", "rel:v9")
        check('set(ISAAC_VITA_GUEST_LINK_ID rel:v9 CACHE STRING "")' in relabelled.stdout, relabelled.stdout)
        recipe = root / "from-cache.cmake"
        recipe.write_text(body.stdout, encoding="utf-8")
        done = run(str(recipe), "--against", str(cache))
        check(done.returncode == 0 and
              "0 difference(s) (value=0 recipe-absent=0 cache-absent=0)" in done.stdout, done.stdout)

        drift = root / "drift.cmake"
        drift.write_text("".join(
            line + "\n" for line in body.stdout.replace("ISAAC_VITA_HEAP_MB 81", "ISAAC_VITA_HEAP_MB 64")
            .splitlines() if "ISAAC_VITA_LATER_KNOB" not in line)
            + 'set(ISAAC_VITA_NEW_KNOB OFF CACHE BOOL "")\n', encoding="utf-8")
        done = run(str(drift), "--against", str(cache))
        check(done.returncode == 1, done.stdout)
        check("3 difference(s) (value=1 recipe-absent=1 cache-absent=1)" in done.stdout, done.stdout)
        check("[value] ISAAC_VITA_HEAP_MB: recipe 64, cache 81" in done.stdout, done.stdout)
        check("[recipe-absent] ISAAC_VITA_LATER_KNOB: recipe <absent>, cache OFF" in done.stdout, done.stdout)
        check("[cache-absent] ISAAC_VITA_NEW_KNOB: recipe OFF, cache <absent>" in done.stdout, done.stdout)
        tagged = [l.split("]")[0].strip() + "]" for l in done.stdout.splitlines() if l.startswith("    [")]
        check(tagged == ["[value]", "[recipe-absent]", "[cache-absent]"], f"class order: {tagged}")
        check(last_line(done) == "release recipe check: FAIL differs=3", done.stdout)

        pathy = root / "pathy.cmake"
        pathy.write_text(body.stdout + 'set(ISAAC_VITA_TROPHY_PACK_DIR /stage/trophy-packs-v2 CACHE PATH "")\n',
                         encoding="utf-8")
        done = run(str(pathy))
        check(done.returncode == 1, done.stdout)
        check(last_line(done) == "release recipe check: FAIL per-machine=ISAAC_VITA_TROPHY_PACK_DIR", done.stdout)

        bad = root / "bad.cmake"
        bad.write_text('if(X)\nendif()\n', encoding="utf-8")
        done = run(str(bad))
        check(done.returncode == 2 and last_line(done).startswith("release recipe check: ERROR"), done.stdout)


def test_repository_recipes() -> None:
    old = run(str(RELEASE_DIR / "v0.1.1-alpha.cmake"))
    check(old.returncode == 1, old.stdout)
    check(last_line(old) == "release recipe check: FAIL missing=" + ",".join(REQUIRED), old.stdout)

    recipes = sorted(p for p in RELEASE_DIR.glob("*.cmake") if p.name != "v0.1.1-alpha.cmake")
    check(any(p.name == "v0.1.2-alpha.cmake" for p in recipes), f"recipes: {recipes}")
    declared = set(re.findall(r"\boption\((ISAAC_VITA_\w+)", CMAKELISTS.read_text(encoding="utf-8")))
    declared |= set(re.findall(r"\bset\((ISAAC_VITA_\w+)\s+(?:\"[^\"]*\"|\S+)\s+CACHE\b",
                               CMAKELISTS.read_text(encoding="utf-8")))
    for path in recipes:
        done = run(str(path))
        check(done.returncode == 0, f"{path.name}:\n{done.stdout}")
        entries, notes = crr.parse_recipe(path.read_text(encoding="utf-8"))
        check(notes == [], f"{path.name}: {notes}")
        stale = sorted(k for k in entries if k.startswith("ISAAC_VITA_") and k not in declared)
        check(stale == [], f"{path.name} sets keys recomp/vita/CMakeLists.txt no longer declares: {stale}")
        for key in ("ISAAC_GUEST_BASE", "SOURCE_DATE_EPOCH", "CMAKE_BUILD_TYPE", "ISAAC_VITA_GUEST_LINK_ID"):
            check(key in entries, f"{path.name} lacks {key}")
        check(entries["CMAKE_BUILD_TYPE"][0] == "Release", f"{path.name}: CMAKE_BUILD_TYPE")
        check(entries["ISAAC_VITA_GUEST_LINK_ID"][0] == "rel:" + path.stem, f"{path.name}: GUEST_LINK_ID")


def test_real_cmake_agrees() -> None:
    """Configure a NONE-language project with -C and compare CMake's cache with the parser."""
    cmake = shutil.which("cmake")
    if cmake is None:
        print("SKIP: test_real_cmake_agrees (no cmake on PATH)")
        return
    fixture = (
        'set(A ON CACHE BOOL "")\nset(A OFF CACHE BOOL "")\n'
        'set(B OFF CACHE BOOL "")\nset(B ON CACHE BOOL "" FORCE)\n'
        'set(C "two words" CACHE STRING "doc (x)")\nset(D x;y CACHE STRING "")\n'
        "set(F\n  ON CACHE BOOL\n  \"multi-line\")\n"
        'set(V expanded CACHE STRING "")\nset(H "a\\${V} c" CACHE STRING "")\n'   # H stays literal
    )
    with tempfile.TemporaryDirectory() as tmp:
        root = Path(tmp)
        for name, text in (("fixture", fixture),
                           ("v0.1.2", (RELEASE_DIR / "v0.1.2-alpha.cmake").read_text(encoding="utf-8"))):
            src, build = root / f"src-{name}", root / f"build-{name}"
            src.mkdir()
            (src / "CMakeLists.txt").write_text("cmake_minimum_required(VERSION 3.16)\nproject(x NONE)\n",
                                                encoding="utf-8")
            init = root / f"{name}.cmake"
            init.write_text(text, encoding="utf-8")
            done = subprocess.run([cmake, "-C", str(init), "-S", str(src), "-B", str(build)],
                                  check=False, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  text=True, encoding="utf-8", errors="replace")
            check(done.returncode == 0, done.stdout)
            parsed, _ = crr.parse_recipe(text)
            real = crr.parse_cache((build / "CMakeCache.txt").read_text(encoding="utf-8"))
            wrong = sorted(k for k in parsed if real.get(k, ("<absent>",))[0] != parsed[k][0])
            check(wrong == [], f"{name}: parser and CMake disagree on {wrong}")
            print(f"  cmake -C agrees with the parser on {len(parsed)} entries ({name})")


def main() -> int:
    tests = (
        test_truth,
        test_parse_cache,
        test_parse_recipe_semantics,
        test_required_and_dependency,
        test_cli_on_fixtures,
        test_repository_recipes,
        test_real_cmake_agrees,
    )
    for test in tests:
        test()
        print(f"PASS: {test.__name__}")
    print(f"release recipe checker tests: PASS ({len(tests)} groups)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
