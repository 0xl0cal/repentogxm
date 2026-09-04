#!/usr/bin/env python3
"""Executable fixture tests for tools/release_audit.py."""

from __future__ import annotations

import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile


AUDIT = Path(__file__).with_name("release_audit.py").resolve()
PROJECT_ROOT = AUDIT.parent.parent


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def run(command: list[str], cwd: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        command,
        cwd=cwd,
        check=False,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        encoding="utf-8",
        errors="replace",
    )


def git(root: Path, *args: str) -> None:
    completed = run(["git", *args], root)
    check(
        completed.returncode == 0,
        f"git {' '.join(args)} failed:\n{completed.stdout}{completed.stderr}",
    )


def write(root: Path, rel: str, data: bytes | str) -> None:
    path = root / Path(*rel.split("/"))
    path.parent.mkdir(parents=True, exist_ok=True)
    if isinstance(data, str):
        path.write_text(data, encoding="utf-8")
    else:
        path.write_bytes(data)


class Fixture:
    def __init__(
        self,
        *,
        with_license: bool = True,
        with_distribution: bool = True,
        provenance: str | None = "complete",
    ) -> None:
        self._temporary = tempfile.TemporaryDirectory(prefix="release-audit-")
        self.root = Path(self._temporary.name)
        git(self.root, "init", "--quiet")
        git(self.root, "config", "user.name", "Release Audit Fixture")
        git(self.root, "config", "user.email", "fixture@example.invalid")
        if with_license:
            write(self.root, "LICENSE", "Fixture-only test licence.\n")
        if with_distribution:
            if provenance is None:
                body = "# Distribution policy\n\nNo status field here.\n"
            else:
                body = (
                    "# Distribution policy\n\n"
                    f"Provenance-Status: {provenance}\n"
                )
            write(self.root, "DISTRIBUTION.md", body)
        write(self.root, "safe.txt", "fixture\n")
        self.commit("baseline")

    def commit(self, message: str) -> None:
        git(self.root, "add", "-A")
        git(self.root, "commit", "--quiet", "-m", message)

    def audit(self, *, include_untracked: bool = True) -> subprocess.CompletedProcess[str]:
        command = [sys.executable, str(AUDIT), "--strict"]
        if include_untracked:
            command.append("--include-untracked")
        return run(command, self.root)

    def close(self) -> None:
        self._temporary.cleanup()

    def __enter__(self) -> "Fixture":
        return self

    def __exit__(self, *unused: object) -> None:
        self.close()


def output(completed: subprocess.CompletedProcess[str]) -> str:
    return completed.stdout + completed.stderr


def expect_blocker(
    completed: subprocess.CompletedProcess[str], marker: str
) -> None:
    combined = output(completed)
    check(completed.returncode == 1, f"expected NO-GO, got:\n{combined}")
    check(marker in combined, f"missing {marker!r} in:\n{combined}")
    check("release audit: NO-GO" in combined, f"missing NO-GO in:\n{combined}")


def test_clean_fixture() -> None:
    with Fixture() as fixture:
        completed = fixture.audit()
        combined = output(completed)
        check(completed.returncode == 0, f"clean fixture failed:\n{combined}")
        check("release audit: GO" in combined, f"clean fixture did not say GO:\n{combined}")


def test_license_and_provenance_gates() -> None:
    with Fixture(with_license=False) as fixture:
        expect_blocker(fixture.audit(), "missing required root LICENSE")
    with Fixture(with_distribution=False) as fixture:
        expect_blocker(fixture.audit(), "missing required root DISTRIBUTION.md")
    with Fixture(provenance=None) as fixture:
        expect_blocker(fixture.audit(), "exactly one Provenance-Status")
    with Fixture(provenance="incomplete") as fixture:
        expect_blocker(fixture.audit(), "provenance is not complete: incomplete")


def test_new_binary_suffixes() -> None:
    suffixes = (".velf", ".sfo", ".suprx", ".sprx", ".skprx")
    with Fixture() as fixture:
        for suffix in suffixes:
            write(fixture.root, f"candidate/artifact{suffix}", "not a binary\n")
        completed = fixture.audit()
        for suffix in suffixes:
            expect_blocker(completed, f"candidate/artifact{suffix}")


def test_generated_guest_code() -> None:
    with Fixture() as fixture:
        write(fixture.root, "generated/guest_0123.c", "void translated(void) {}\n")
        expect_blocker(fixture.audit(), "generated translated guest code in current files")


def load_audit_module():
    spec = importlib.util.spec_from_file_location("release_audit_under_test", AUDIT)
    check(spec is not None and spec.loader is not None, "cannot load release_audit.py")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def test_suspicious_media_extensions() -> None:
    module = load_audit_module()
    expected_allowlist = frozenset({
        "recomp/vita/assets/sce_sys/icon0.png",
        "recomp/vita/assets/sce_sys/pic0.png",
        "recomp/vita/assets/sce_sys/livearea/contents/bg0.png",
        "recomp/vita/assets/sce_sys/livearea/contents/nicalis.png",
        "recomp/vita/assets/sce_sys/livearea/contents/startup.png",
    })
    check(
        module.ALLOWED_MEDIA_ASSETS == expected_allowlist,
        "media allowlist must contain only the pinned LiveArea image paths",
    )

    with Fixture() as fixture:
        for rel in sorted(expected_allowlist):
            write(fixture.root, rel, b"fixture reviewed package image\n")
        completed = fixture.audit()
        combined = output(completed)
        check(completed.returncode == 0, f"allowlisted media failed:\n{combined}")
        write(
            fixture.root,
            "recomp/vita/assets/sce_sys/livearea/contents/unreviewed.png",
            b"fixture unreviewed package image\n",
        )
        expect_blocker(
            fixture.audit(),
            "sce_sys/livearea/contents/unreviewed.png",
        )

    with Fixture() as fixture:
        for suffix in sorted(module.SUSPICIOUS_MEDIA_SUFFIXES):
            write(fixture.root, f"docs/unreviewed_asset{suffix}", b"fixture media\n")
        completed = fixture.audit()
        for suffix in sorted(module.SUSPICIOUS_MEDIA_SUFFIXES):
            expect_blocker(completed, f"docs/unreviewed_asset{suffix}")


def test_current_magic() -> None:
    samples = {
        "ELF": b"\x7fELFfixture",
        "SELF": b"SCE\x00fixture",
        "ZIP-local": b"PK\x03\x04fixture",
        "ZIP-empty": b"PK\x05\x06fixture",
        "ZIP-spanned": b"PK\x07\x08fixture",
    }
    with Fixture() as fixture:
        for name, data in samples.items():
            write(fixture.root, f"payload/{name}.dat", data)
        completed = fixture.audit()
        expect_blocker(completed, "ELF magic in current file: payload/ELF.dat")
        expect_blocker(completed, "SELF magic in current file: payload/SELF.dat")
        for name in ("ZIP-local", "ZIP-empty", "ZIP-spanned"):
            expect_blocker(completed, f"ZIP magic in current file: payload/{name}.dat")


def test_historical_magic_after_head_deletion() -> None:
    samples = {
        "old_elf.dat": b"\x7fELFfixture",
        "old_self.dat": b"SCE\x00fixture",
        "old_zip.dat": b"PK\x03\x04fixture",
    }
    with Fixture() as fixture:
        for name, data in samples.items():
            write(fixture.root, f"retired/{name}", data)
        fixture.commit("add forbidden historical blobs")
        git(fixture.root, "rm", "-q", "--", *[f"retired/{name}" for name in samples])
        fixture.commit("delete forbidden blobs from head")
        completed = fixture.audit()
        combined = output(completed)
        for kind, name in (
            ("ELF", "old_elf.dat"),
            ("SELF", "old_self.dat"),
            ("ZIP", "old_zip.dat"),
        ):
            expect_blocker(completed, f"{kind} magic in Git history: retired/{name}")
            check(
                f"{kind} magic in current file: retired/{name}" not in combined,
                f"deleted history fixture was incorrectly treated as current:\n{combined}",
            )


def test_distribution_policy_text() -> None:
    text = (PROJECT_ROOT / "DISTRIBUTION.md").read_text(encoding="utf-8")
    for required in ("ELF", "VELF", "eboot.bin", "VPK", "translated game code"):
        check(required in text, f"DISTRIBUTION.md does not explicitly forbid {required}")


def main() -> int:
    tests = (
        test_clean_fixture,
        test_license_and_provenance_gates,
        test_new_binary_suffixes,
        test_generated_guest_code,
        test_suspicious_media_extensions,
        test_current_magic,
        test_historical_magic_after_head_deletion,
        test_distribution_policy_text,
    )
    for test in tests:
        test()
        print(f"PASS: {test.__name__}")
    print(f"release audit fixture tests: PASS ({len(tests)} groups)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
