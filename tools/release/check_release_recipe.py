#!/usr/bin/env python3
"""Fail closed unless a release recipe or a build cache turns ON every fix knob.

A release build is configured from ``release/<version>.cmake`` (an initial cache
for ``cmake -C``) and is described afterwards by its ``CMakeCache.txt``.  Every
fix the project shipped after v0.1.1-alpha is a knob that defaults to OFF in
``recomp/vita/CMakeLists.txt``, so a recipe that forgets one silently ships the
bug again.  This checker reads either format and requires the knobs below ON:

* the eight player-facing fix knobs of the 2026-10-26 issue re-check (section
  E2: ``FBO_DEPTH_CARRY`` ... ``CRT_FREAD_COMPLETE``);
* ``GL_RECORD_ADMISSION_CACHE_STANDALONE`` (S1): without it a build with the
  debug overlay OFF loses the GL recorder admission cache, which configure
  itself warns costs 6-15 ms per render;
* the five fixes merged for v0.1.2 on 2026-11-06 (``INTRO_PLAY``,
  ``DISPLAY_CENTER_Y``, ``EFFECTS_PROFILE_KEEP_COLOR_MODIFIER``,
  ``VITAGL_INTENSITY_TEXTURE``, ``LUA_LOADFILE_RETRY``): each was device-gated
  on its own fix build, and a recipe written before their merge silently
  lacked all five.  Their prerequisites are FATAL at configure time.

``CXX_THROW_DIAG`` is advisory: it only runs on the fatal unresolved-import
stop and names the C++ exception that caused it.

Usage::

    check_release_recipe.py FILE                  # a .cmake recipe or a CMakeCache.txt
    check_release_recipe.py RECIPE --against CMakeCache.txt
    check_release_recipe.py --emit-body CMakeCache.txt [--link-id rel:<version>]

``--against`` also requires every entry of RECIPE to equal the cache's value
(BOOL entries compared by truth value; ``ISAAC_VITA_GUEST_LINK_ID`` is reported
but not compared, release candidates override it) and labels each difference
``[value]``, ``[recipe-absent]`` (an ``ISAAC_VITA_*`` key only the cache has)
or ``[cache-absent]`` (a key only the recipe sets), with a count per class.
A recipe is read the way ``cmake -C`` reads it; anything but ``set()``, and any
variable reference (``${}``, ``$ENV{}``, ``$CACHE{}``, quoted or not), is
refused rather than guessed.  ``--emit-body`` prints the
recipe body of a cache: every ``ISAAC_VITA_*`` BOOL and STRING entry except the
per-machine paths, plus ``ISAAC_GUEST_BASE``, ``SOURCE_DATE_EPOCH`` and
``CMAKE_BUILD_TYPE``, sorted, in the ``set(NAME VALUE CACHE TYPE "")`` form of
``release/v0.1.1-alpha.cmake`` (``--link-id`` replaces the cache's link id).
A recipe that sets a per-machine path fails.

Exit status: 0 PASS, 1 FAIL, 2 unreadable or unsupported input.
"""

from __future__ import annotations

import argparse
from pathlib import Path
import re
import sys
from typing import Dict, List, Optional, Tuple


# (knob, issue label).  Labels copied from the 2026-10-26 issue re-check:
# the eight fix knobs and S1; then the five 2026-11-06 fixes (LFR = the Lua
# mod-file read retry).
REQUIRED: Tuple[Tuple[str, str], ...] = (
    ("ISAAC_VITA_FBO_DEPTH_CARRY", "#15"),
    ("ISAAC_VITA_TEXEL_SCRATCH_32M", "#4"),
    ("ISAAC_VITA_VITAGL_SCENE_GUARD", "#19, 4.4"),
    ("ISAAC_VITA_BACKDROP_WATERGFX_GUARD", "5.4/5.17a"),
    ("ISAAC_VITA_LUA_CALL_ERROR_CATCH", "#17b"),
    ("ISAAC_VITA_EFFECTS_PROFILE_WATER", "5.16"),
    ("ISAAC_VITA_AUDIO_STARVATION_SHIELD", "5.1"),
    ("ISAAC_VITA_CRT_FREAD_COMPLETE", "AF-c"),
    ("ISAAC_VITA_GL_RECORD_ADMISSION_CACHE_STANDALONE", "S1, 3b"),
    ("ISAAC_VITA_INTRO_PLAY", "#14"),
    ("ISAAC_VITA_DISPLAY_CENTER_Y", "#5, 5.8b"),
    ("ISAAC_VITA_EFFECTS_PROFILE_KEEP_COLOR_MODIFIER", "#15, 15-i"),
    ("ISAAC_VITA_VITAGL_INTENSITY_TEXTURE", "#4, 4.2"),
    ("ISAAC_VITA_LUA_LOADFILE_RETRY", "LFR"),
)

ADVISORY: Tuple[Tuple[str, str], ...] = (
    ("ISAAC_VITA_CXX_THROW_DIAG", "17c, fatal-path diagnostic"),
)

# A required knob whose configure step is FATAL without another one
# (recomp/vita/CMakeLists.txt: STANDALONE needs GL_THREAD_WORKER).
DEPENDS: Dict[str, Tuple[str, ...]] = {
    "ISAAC_VITA_GL_RECORD_ADMISSION_CACHE_STANDALONE": ("ISAAC_VITA_GL_THREAD_WORKER",),
}

NOT_COMPARED = {"ISAAC_VITA_GUEST_LINK_ID"}

# Per-machine paths: given on the command line, never part of a recipe.  A
# Linux build host passes some of them as -D, which caches them as STRING, so they are
# excluded by name, not by type.  (recomp/vita/CMakeLists.txt declares each as
# PATH or FILEPATH; REAL_PACK_VPK comes from deterministic_vpk.cmake.)
PER_MACHINE = {
    "ISAAC_VITA_LAYOUT_HUB_ORDER",
    "ISAAC_VITA_OPENAL_OVERLAY_DIR",
    "ISAAC_VITA_REAL_PACK_VPK",
    "ISAAC_VITA_THEORADEC_PREFIX",
    "ISAAC_VITA_TRANSLATED_CPU_HOT_SET",
    "ISAAC_VITA_TROPHY_PACK_DIR",
    "ISAAC_VITA_VITAGL_OVERLAY_DIR",
    "ISAAC_VITA_VITAGL_STOCK_REFERENCE_DIR",
}
EMIT_EXTRA = ("ISAAC_GUEST_BASE", "SOURCE_DATE_EPOCH", "CMAKE_BUILD_TYPE")

TRUE_WORDS = {"1", "ON", "YES", "TRUE", "Y"}
FALSE_WORDS = {"0", "OFF", "NO", "FALSE", "N", "IGNORE", "NOTFOUND", ""}

CACHE_LINE = re.compile(r"^([A-Za-z_][A-Za-z0-9_.+-]*):([A-Z]+)=(.*)$")
# The variable references CMake expands in quoted and unquoted arguments.
VARIABLE_REF = re.compile(r"\$(ENV|CACHE)?\{")


class RecipeError(Exception):
    """Input this checker cannot read without guessing."""


Entry = Tuple[str, str]          # (value, type)


def truth(value: str) -> Optional[bool]:
    """CMake if(<constant>) truth for a cache value; None when it is no constant."""
    word = value.strip().upper()
    if word in TRUE_WORDS:
        return True
    if word in FALSE_WORDS or word.endswith("-NOTFOUND"):
        return False
    if re.fullmatch(r"[+-]?(\d+(\.\d*)?|\.\d+)", word):
        return float(word) != 0.0
    return None


def parse_cache(text: str) -> Dict[str, Entry]:
    entries: Dict[str, Entry] = {}
    for raw in text.splitlines():
        line = raw.rstrip("\r")
        if not line or line.startswith("#") or line.startswith("//"):
            continue
        match = CACHE_LINE.match(line)
        if not match:
            raise RecipeError(f"not a CMakeCache line: {line[:80]!r}")
        name, typ, value = match.groups()
        entries[name] = (value, typ)
    return entries


def _tokens(text: str) -> List[Tuple[str, bool, int]]:
    """Split CMake script text into (token, quoted, line) with '(' and ')' as tokens."""
    out: List[Tuple[str, bool, int]] = []
    i, line, n = 0, 1, len(text)
    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
        elif c in " \t\r":
            i += 1
        elif c == "#":
            if text.startswith("#[", i):
                raise RecipeError(f"line {line}: bracket comments are not supported")
            while i < n and text[i] != "\n":
                i += 1
        elif c in "()":
            out.append((c, False, line))
            i += 1
        elif c == '"':
            j, buf = i + 1, []
            while True:
                if j >= n:
                    raise RecipeError(f"line {line}: unterminated quoted argument")
                ch = text[j]
                if ch == "\\" and j + 1 < n:
                    nxt = text[j + 1]
                    buf.append({"n": "\n", "t": "\t", "r": "\r"}.get(nxt, nxt))
                    j += 2
                    continue
                if ch == "$" and VARIABLE_REF.match(text, j):
                    # CMake expands ${...}, $ENV{...} and $CACHE{...} inside a
                    # quoted argument too; only an escaped \$ is literal.
                    raise RecipeError(f"line {line}: variable reference in a quoted argument is not supported")
                if ch == '"':
                    break
                if ch == "\n":
                    line += 1
                buf.append(ch)
                j += 1
            out.append(("".join(buf), True, line))
            i = j + 1
        else:
            j = i
            while j < n and text[j] not in ' \t\r\n()#"':
                j += 1
            word = text[i:j]
            if VARIABLE_REF.search(word):
                raise RecipeError(f"line {line}: variable reference {word!r} is not supported")
            out.append((word, False, line))
            i = j
    return out


def parse_recipe(text: str) -> Tuple[Dict[str, Entry], List[str]]:
    """Replay a cmake -C initial-cache script made of set(... CACHE ...) commands.

    The first set() of a name creates its entry; a later one replaces it only
    with FORCE, as in CMake.  A set() without CACHE never reaches the cache.
    """
    toks = _tokens(text)
    entries: Dict[str, Entry] = {}
    notes: List[str] = []
    i = 0
    while i < len(toks):
        word, quoted, line = toks[i]
        if quoted or word in "()" or i + 1 >= len(toks) or toks[i + 1][0] != "(":
            raise RecipeError(f"line {line}: expected a command, found {word!r}")
        depth, j, args = 1, i + 2, []
        while depth:
            if j >= len(toks):
                raise RecipeError(f"line {line}: unterminated {word}(")
            tok = toks[j]
            if tok[0] == "(" and not tok[1]:
                depth += 1
            elif tok[0] == ")" and not tok[1]:
                depth -= 1
                if not depth:
                    break
            args.append(tok)
            j += 1
        i = j + 1
        if word.lower() != "set":
            raise RecipeError(f"line {line}: only set() is supported in a release recipe, found {word}()")
        names = [a[0] for a in args]
        if "CACHE" not in names[1:] or any(a[1] for a in args if a[0] == "CACHE"):
            notes.append(f"line {line}: set({names[0] if names else ''}) without CACHE does not reach the cache")
            continue
        at = names.index("CACHE", 1)
        if at + 2 >= len(names):
            raise RecipeError(f"line {line}: set({names[0]} ... CACHE) needs a type and a docstring")
        name, value, typ = names[0], ";".join(names[1:at]), names[at + 1]
        force = len(names) > at + 3 and names[at + 3] == "FORCE" and not args[at + 3][1]
        if name in entries and not force:
            if entries[name][0] != value:
                notes.append(f"line {line}: {name}={value} ignored (no FORCE; first set wins: {entries[name][0]})")
            continue
        entries[name] = (value, typ)
    return entries, notes


def load(path: Path) -> Tuple[str, Dict[str, Entry], List[str]]:
    try:
        text = path.read_text(encoding="utf-8")
    except (OSError, UnicodeDecodeError) as exc:
        raise RecipeError(f"cannot read {path}: {exc}") from exc
    body = [l for l in text.splitlines() if l.strip() and not l.lstrip().startswith(("#", "//"))]
    if body and all(CACHE_LINE.match(l.rstrip("\r")) for l in body):
        return "CMakeCache", parse_cache(text), []
    entries, notes = parse_recipe(text)
    return "cmake recipe", entries, notes


def check(entries: Dict[str, Entry]) -> Tuple[List[str], List[str]]:
    lines: List[str] = []
    missing: List[str] = []
    for name, label in REQUIRED:
        if name not in entries:
            lines.append(f"  MISSING  {name}  ({label})")
            missing.append(name)
            continue
        value = entries[name][0]
        on = truth(value) is True
        lines.append(f"  {'ON     ' if on else 'NOT ON '}  {name}={value}  ({label})")
        if not on:
            missing.append(name)
            continue
        for dep in DEPENDS.get(name, ()):
            if dep not in entries or truth(entries[dep][0]) is not True:
                lines.append(f"  NOT ON   {dep}={entries.get(dep, ('<absent>',))[0]}  (needed by {name})")
                missing.append(dep)
    for name, label in ADVISORY:
        value = entries.get(name, ("<absent>",))[0]
        state = "ON" if truth(value) is True else "not ON (recommended ON)"
        lines.append(f"  advisory {name}={value}  ({label}): {state}")
    return lines, missing


# Difference classes of --against, in report order:
#   value          both sides set the key, to different values;
#   recipe-absent  the cache has an ISAAC_VITA_* key the recipe never sets
#                  (typically a knob declared after the recipe was written);
#   cache-absent   the recipe sets a key the cache lacks (typically a knob the
#                  cache's revision did not declare yet, e.g. a later pin).
DIFF_CLASSES = ("value", "recipe-absent", "cache-absent")


def compare(recipe: Dict[str, Entry], cache: Dict[str, Entry]) -> List[Tuple[str, str]]:
    """(class, description) for every entry on which RECIPE and CACHE disagree."""
    diffs: List[Tuple[str, str]] = []
    for name in sorted(recipe):
        if name in NOT_COMPARED or name in PER_MACHINE:
            continue
        rv, rt = recipe[name]
        if name not in cache:
            diffs.append(("cache-absent", f"{name}: recipe {rv}, cache <absent>"))
            continue
        cv, ct = cache[name]
        if "BOOL" in (rt, ct) and truth(rv) is not None and truth(cv) is not None:
            same = truth(rv) == truth(cv)
        else:
            same = rv == cv
        if not same:
            diffs.append(("value", f"{name}: recipe {rv}, cache {cv}"))
    for name in sorted(cache):
        if name.startswith("ISAAC_VITA_") and cache[name][1] in ("BOOL", "STRING") \
                and name not in recipe and name not in NOT_COMPARED and name not in PER_MACHINE:
            diffs.append(("recipe-absent", f"{name}: recipe <absent>, cache {cache[name][0]}"))
    order = {c: i for i, c in enumerate(DIFF_CLASSES)}
    return sorted(diffs, key=lambda d: order[d[0]])


def emit_body(cache: Dict[str, Entry], link_id: Optional[str] = None) -> List[str]:
    out = []
    for name in sorted(cache):
        value, typ = cache[name]
        wanted = (name.startswith("ISAAC_VITA_") and typ in ("BOOL", "STRING")) or name in EMIT_EXTRA
        if not wanted or name in PER_MACHINE:
            continue
        if name == "ISAAC_VITA_GUEST_LINK_ID" and link_id is not None:
            value = link_id
        typ = typ if typ in ("BOOL", "STRING") else "STRING"
        plain = value and not any(c in value for c in ' ;"()#\\$')
        shown = value if plain else \
            '"' + value.replace("\\", "\\\\").replace('"', '\\"').replace("$", "\\$") + '"'
        out.append(f'set({name} {shown} CACHE {typ} "")')
    return out


def main(argv: Optional[List[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("file", nargs="?", type=Path, help="a release .cmake recipe or a CMakeCache.txt")
    parser.add_argument("--against", type=Path, metavar="CMakeCache.txt",
                        help="also require FILE to agree with this build cache")
    parser.add_argument("--emit-body", type=Path, metavar="CMakeCache.txt",
                        help="print the recipe body of a build cache and exit")
    parser.add_argument("--link-id", metavar="rel:<version>",
                        help="with --emit-body: the ISAAC_VITA_GUEST_LINK_ID to write")
    args = parser.parse_args(argv)
    try:
        if args.emit_body is not None:
            kind, entries, _ = load(args.emit_body)
            if kind != "CMakeCache":
                raise RecipeError(f"{args.emit_body} is not a CMakeCache.txt")
            print("\n".join(emit_body(entries, args.link_id)))
            return 0
        if args.file is None:
            parser.error("FILE is required unless --emit-body is given")
        kind, entries, notes = load(args.file)
        against = load(args.against) if args.against is not None else None
    except RecipeError as exc:
        print(f"release recipe check: ERROR {exc}")
        return 2

    print(f"release recipe check: {args.file} ({kind}, {len(entries)} entries)")
    for note in notes:
        print(f"  note: {note}")
    lines, missing = check(entries)
    print("\n".join(lines))
    machine = sorted(k for k in entries if k in PER_MACHINE) if kind == "cmake recipe" else []
    for name in machine:
        print(f"  PER-MACHINE {name}={entries[name][0]}  (a path belongs on the command line)")
    diffs: List[Tuple[str, str]] = []
    if against is not None:
        if against[0] != "CMakeCache":
            print(f"release recipe check: ERROR {args.against} is not a CMakeCache.txt")
            return 2
        diffs = compare(entries, against[1])
        counts = " ".join(f"{c}={sum(1 for d in diffs if d[0] == c)}" for c in DIFF_CLASSES)
        print(f"  against {args.against}: {len(diffs)} difference(s) ({counts})")
        for kind, diff in diffs:
            print(f"    [{kind}] {diff}")
    if missing or diffs or machine:
        parts = []
        if missing:
            parts.append("missing=" + ",".join(missing))
        if machine:
            parts.append("per-machine=" + ",".join(machine))
        if diffs:
            parts.append(f"differs={len(diffs)}")
        print("release recipe check: FAIL " + " ".join(parts))
        return 1
    print(f"release recipe check: PASS ({len(REQUIRED)}/{len(REQUIRED)} required ON)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
