#!/usr/bin/env python3
"""Fixtures for vita_theoradec_import_audit.py (nm -u parsing, the verdict and
the CLI path CMake runs: --nm subprocess, exit codes, --delete-on-failure)."""

from __future__ import annotations

import contextlib
import io
import sys
import tempfile
from pathlib import Path

import vita_theoradec_import_audit as audit


GOOD = """
apiwrapper.o:
                 U isaac_vita_theora_free
                 U isaac_vita_theora_malloc
                 U memset

bitpack.o:

decode.o:
                 U __aeabi_idiv
                 U isaac_vita_theora_calloc
                 U isaac_vita_theora_free
                 U isaac_vita_theora_malloc
                 U memcpy
                 U memmove
                 w __gnu_Unwind_Find_exidx

info.o:
                 U isaac_vita_theora_realloc
                 U isaac_vita_theora_free
"""

RAW = GOOD + """
armstate.o:
                 U malloc
                 U posix_memalign
"""

OGG = GOOD + """
decinfo.o:
                 U oggpackB_read
                 U ogg_stream_init
"""

UNRENAMED = """
decode.o:
                 U calloc
                 U free
                 U malloc
                 U memcpy
                 U realloc
"""


def run_cli(archive: Path, nm: str, delete: bool) -> tuple[int, str, str]:
    """main() as CMake runs it; (exit status, stdout, stderr)."""
    argv = [str(audit.__file__), "--nm", nm]
    if delete:
        argv.append("--delete-on-failure")
    argv.append(str(archive))
    out, err = io.StringIO(), io.StringIO()
    saved = sys.argv
    sys.argv = argv
    try:
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            status = audit.main()
    finally:
        sys.argv = saved
    return status, out.getvalue(), err.getvalue()


def fake_nm_archive(directory: Path, name: str, nm_output: str, exit_status: int = 0) -> Path:
    """An 'archive' that is a python script printing canned `nm -u` output.

    main() runs `<--nm> -u <archive>`; with --nm = sys.executable that is
    `python -u <archive>`, so the real subprocess path is exercised on every
    host without a platform-specific stub executable."""
    archive = directory / name
    archive.write_text("import sys\n"
                       f"sys.stdout.write({nm_output!r})\n"
                       f"sys.exit({exit_status})\n", encoding="utf-8")
    return archive


def check_cli() -> None:
    python = sys.executable
    with tempfile.TemporaryDirectory() as scratch:
        directory = Path(scratch)

        # A passing archive: exit 0, the import list and OK on stdout, kept.
        good = fake_nm_archive(directory, "good.a", GOOD)
        status, out, err = run_cli(good, python, delete=True)
        assert status == 0 and good.is_file(), (status, out, err)
        assert "good.a imports 9 symbols: __aeabi_idiv __gnu_Unwind_Find_exidx" in out, out
        assert "theoradec import audit: OK (pool allocator imports present" in out, out
        assert err == "", err

        # A raw allocator import: exit 1, FAIL on stderr; the copy is removed
        # only with --delete-on-failure.
        raw = fake_nm_archive(directory, "raw.a", RAW)
        status, out, err = run_cli(raw, python, delete=False)
        assert status == 1 and raw.is_file(), (status, out, err)
        assert "FAIL: raw allocator imports remain: malloc posix_memalign" in err, err
        assert "removed" not in err, err
        status, out, err = run_cli(raw, python, delete=True)
        assert status == 1 and not raw.exists(), (status, out, err)
        assert "FAIL: raw allocator imports remain: malloc posix_memalign" in err, err
        assert f"theoradec import audit: removed {raw}" in err, err

        # An un-renamed archive fails on both counts, in that order.
        unrenamed = fake_nm_archive(directory, "unrenamed.a", UNRENAMED)
        status, out, err = run_cli(unrenamed, python, delete=True)
        assert status == 1 and not unrenamed.exists(), (status, out, err)
        assert err.index("FAIL: pool allocator imports missing") < \
            err.index("FAIL: raw allocator imports remain: calloc free malloc realloc"), err

        # nm itself failing (non-zero exit): exit 1, the nm error quoted, the
        # copy removed with --delete-on-failure (an unaudited copy must not link).
        broken = fake_nm_archive(directory, "broken.a", "", exit_status=3)
        status, out, err = run_cli(broken, python, delete=False)
        assert status == 1 and broken.is_file(), (status, out, err)
        assert f"-u {broken} failed (3)" in err, err
        status, out, err = run_cli(broken, python, delete=True)
        assert status == 1 and not broken.exists(), (status, out, err)

        # nm not runnable at all (OSError): exit 1, the copy removed.
        missing_nm = str(directory / "no-such-nm")
        gone = fake_nm_archive(directory, "gone.a", GOOD)
        status, out, err = run_cli(gone, missing_nm, delete=True)
        assert status == 1 and not gone.exists() and "theoradec import audit:" in err, (status, out, err)

        # A missing archive is exit 1 without touching nm.
        status, out, err = run_cli(directory / "absent.a", missing_nm, delete=True)
        assert status == 1 and "no such archive" in err, (status, out, err)


def main() -> int:
    check_cli()
    good = audit.parse_undefined(GOOD)
    assert good == {"isaac_vita_theora_free", "isaac_vita_theora_malloc", "memset",
                    "__aeabi_idiv", "isaac_vita_theora_calloc", "memcpy", "memmove",
                    "__gnu_Unwind_Find_exidx", "isaac_vita_theora_realloc"}, good
    assert audit.audit(good) == [], audit.audit(good)

    raw = audit.audit(audit.parse_undefined(RAW))
    assert len(raw) == 1 and raw[0] == "raw allocator imports remain: malloc posix_memalign", raw

    ogg = audit.audit(audit.parse_undefined(OGG))
    assert len(ogg) == 1 and ogg[0] == "libogg imports: ogg_stream_init oggpackB_read", ogg

    unrenamed = audit.audit(audit.parse_undefined(UNRENAMED))
    assert len(unrenamed) == 2, unrenamed
    assert unrenamed[0].startswith("pool allocator imports missing") and \
        unrenamed[0].endswith("isaac_vita_theora_malloc isaac_vita_theora_calloc "
                              "isaac_vita_theora_realloc isaac_vita_theora_free"), unrenamed
    assert unrenamed[1] == "raw allocator imports remain: calloc free malloc realloc", unrenamed

    # An empty archive (no undefined symbols at all) fails on the missing pool imports.
    empty = audit.audit(audit.parse_undefined("member.o:\n\n"))
    assert len(empty) == 1 and empty[0].startswith("pool allocator imports missing"), empty

    # nm variants: address column present, tabs, CRLF.
    variant = audit.parse_undefined("x.o:\r\n00000000 U foo\r\n\tw bar\r\n         U isaac_vita_theora_free\r\n")
    assert variant == {"foo", "bar", "isaac_vita_theora_free"}, variant

    # The forbidden list covers the plan's set exactly plus the newlib spellings.
    for name in ("malloc", "calloc", "realloc", "free", "memalign", "posix_memalign",
                 "aligned_alloc", "strdup", "valloc", "pvalloc", "_malloc_r"):
        assert name in audit.FORBIDDEN, name
    print("PASS test_vita_theoradec_import_audit")
    return 0


if __name__ == "__main__":
    sys.exit(main())
