#!/usr/bin/env python3
"""Import audit of the pooled libtheoradec copy (ISAAC_VITA_NATIVE_THEORA).

The CMake knob block copies the selected libtheoradec.a with objcopy, renaming
malloc/calloc/realloc/free to the seam's isaac_vita_theora_* pool allocator,
and links only that copy.  The raw allocator gate scans the target's objects
and the Lua archive, not LINK_LIBRARIES archives, so the rename is the only
thing standing between the decoder and the poisoned newlib heap.  This script
runs `nm -u` on the copy and fails unless

  * every one of isaac_vita_theora_malloc/calloc/realloc/free is imported
    (the rename happened and the decoder really allocates through the pool),
  * none of the raw allocator names in FORBIDDEN remains (malloc calloc
    realloc free memalign posix_memalign aligned_alloc strdup valloc pvalloc
    _malloc_r and the newlib reentrant/BSD spellings), and
  * no symbol whose name starts with "ogg" is imported (ogg_*, oggpack_*,
    oggpackB_*: libtheoradec has its own bit reader; a libogg import would
    mean a different archive than expected).

It prints the whole undefined set so the configure/build log records what the
archive really imports (expected beyond the four: memcpy memmove memset and
libgcc's __aeabi_* helpers).  With --delete-on-failure the audited archive is
removed when the audit fails, so a failing build cannot link it.

usage: vita_theoradec_import_audit.py --nm <arm-vita-eabi-nm> [--delete-on-failure] <archive>
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path


REQUIRED = (
    "isaac_vita_theora_malloc",
    "isaac_vita_theora_calloc",
    "isaac_vita_theora_realloc",
    "isaac_vita_theora_free",
)
FORBIDDEN = (
    "malloc", "calloc", "realloc", "free",
    "memalign", "posix_memalign", "aligned_alloc", "strdup", "strndup",
    "valloc", "pvalloc", "reallocf", "reallocarray",
    "_malloc_r", "_calloc_r", "_realloc_r", "_free_r", "_memalign_r",
)
FORBIDDEN_PATTERN = re.compile(r"^ogg")
# nm -u lines: optional blank value column, a one-letter type (U for undefined,
# w/v for weak undefined), the symbol; "member.o:" lines and blanks between.
SYMBOL_LINE = re.compile(r"^\s*(?:[0-9a-fA-F]+\s+)?([UwvA-Za-z])\s+(\S+)\s*$")


def parse_undefined(nm_output: str) -> set[str]:
    """The set of symbol names in `nm -u` output (archive member headers skipped)."""
    names: set[str] = set()
    for line in nm_output.splitlines():
        stripped = line.strip()
        if not stripped or stripped.endswith(":"):
            continue
        match = SYMBOL_LINE.match(line)
        if match is None:
            continue
        names.add(match.group(2))
    return names


def audit(undefined: set[str]) -> list[str]:
    """Problems with an undefined-symbol set; empty means the archive passes."""
    problems: list[str] = []
    missing = [name for name in REQUIRED if name not in undefined]
    if missing:
        problems.append("pool allocator imports missing (objcopy rename did not "
                        f"happen or the decoder does not allocate): {' '.join(missing)}")
    raw = sorted(name for name in undefined if name in FORBIDDEN)
    if raw:
        problems.append(f"raw allocator imports remain: {' '.join(raw)}")
    ogg = sorted(name for name in undefined if FORBIDDEN_PATTERN.match(name))
    if ogg:
        problems.append(f"libogg imports: {' '.join(ogg)}")
    return problems


def run_nm(nm: str, archive: Path) -> str:
    completed = subprocess.run([nm, "-u", str(archive)], capture_output=True,
                               text=True, check=False)
    if completed.returncode != 0:
        raise RuntimeError(f"{nm} -u {archive} failed ({completed.returncode}): "
                           f"{completed.stderr.strip()}")
    return completed.stdout


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Import audit of the pooled libtheoradec copy (nm -u)")
    parser.add_argument("--nm", required=True, help="arm-vita-eabi-nm (CMAKE_NM)")
    parser.add_argument("--delete-on-failure", action="store_true",
                        help="remove the archive when the audit fails (build products only)")
    parser.add_argument("archive", type=Path, help="the pooled libtheoradec copy")
    arguments = parser.parse_args()
    if not arguments.archive.is_file():
        print(f"theoradec import audit: no such archive: {arguments.archive}", file=sys.stderr)
        return 1
    try:
        undefined = parse_undefined(run_nm(arguments.nm, arguments.archive))
    except (OSError, RuntimeError) as exc:
        print(f"theoradec import audit: {exc}", file=sys.stderr)
        if arguments.delete_on_failure:
            arguments.archive.unlink(missing_ok=True)
        return 1
    print(f"theoradec import audit: {arguments.archive.name} imports {len(undefined)} "
          f"symbols: {' '.join(sorted(undefined))}")
    problems = audit(undefined)
    if problems:
        for problem in problems:
            print(f"theoradec import audit: FAIL: {problem}", file=sys.stderr)
        if arguments.delete_on_failure:
            arguments.archive.unlink(missing_ok=True)
            print(f"theoradec import audit: removed {arguments.archive}", file=sys.stderr)
        return 1
    print("theoradec import audit: OK (pool allocator imports present, no raw "
          "allocator or libogg import)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
