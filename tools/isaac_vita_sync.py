#!/usr/bin/env python3
"""Safely copy Isaac saves and downloaded Workshop mods between PC and Vita.

The push path never deletes user content: every replaced file is moved into a
unique session backup, uploads are staged under a unique name, and the staged
bytes are read back and SHA-256 checked before the live filename changes.  The
save-pull path sends no FTP mutation commands and creates a new local backup
directory with fsynced, atomically promoted files and a SHA-256 receipt.
"""

from __future__ import annotations

import argparse
from contextlib import contextmanager
from dataclasses import dataclass
from datetime import datetime, timezone
import ftplib
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import stat
import sys
import uuid


APP_ID = "250900"
GAME_DIR = "The Binding of Isaac Rebirth"
REMOTE_ROOT = PurePosixPath("/ux0:/data/isaacr001")
REMOTE_SAVE = REMOTE_ROOT / "Documents/My Games/Binding of Isaac Repentance"
REMOTE_MODS = REMOTE_ROOT / "mods"
REMOTE_BACKUPS = REMOTE_ROOT / ".sync-backups"
SENTINEL_MOD_ROOT = Path(__file__).resolve().parent / "isaac_vita_lua_sentinel"
SENTINEL_MOD_BASENAME = "repentogxm_lua_sentinel"
SENTINEL_MOD_FILES = frozenset(
    (PurePosixPath("main.lua"), PurePosixPath("metadata.xml"))
)
PULL_BACKUP_PARENT = "Isaac Vita Save Backups"
SAVE_NAME = re.compile(
    r"^(?:rep_)?(?P<kind>persistentgamedata)(?P<slot>[123])\.dat$",
    re.IGNORECASE,
)
DATED_SAVE_NAME = re.compile(
    r"^(?P<date>[0-9]{8})\.(?:rep_)?(?P<kind>persistentgamedata)"
    r"(?P<slot>[123])\.dat$",
    re.IGNORECASE,
)
REMOTE_SAVE_SLOTS = tuple(
    (slot, REMOTE_SAVE / f"persistentgamedata{slot}.dat")
    for slot in range(1, 4)
)


class SyncError(RuntimeError):
    pass


class RemoteSaveMissing(SyncError):
    pass


@dataclass(frozen=True)
class Transfer:
    source: Path
    remote: PurePosixPath
    group: str


def sha256_file(path: Path) -> tuple[int, str]:
    digest = hashlib.sha256()
    size = 0
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            size += len(block)
            digest.update(block)
    return size, digest.hexdigest()


def _lexists(path: Path) -> bool:
    return os.path.lexists(os.fspath(path))


def _plain_directory(path: Path, label: str) -> Path:
    if not _lexists(path):
        raise SyncError(f"{label} not found: {path}")
    if _is_link_or_reparse(path):
        raise SyncError(f"refusing symlink/reparse {label}: {path}")
    resolved = path.resolve(strict=True)
    if not resolved.is_dir():
        raise SyncError(f"{label} is not a directory: {path}")
    return resolved


def _fsync_directory(path: Path) -> None:
    if os.name == "nt":
        return
    flags = os.O_RDONLY | getattr(os, "O_DIRECTORY", 0)
    descriptor = os.open(path, flags)
    try:
        os.fsync(descriptor)
    finally:
        os.close(descriptor)


def _promote_no_replace(temp: Path, destination: Path) -> None:
    if _lexists(destination):
        raise SyncError(f"refusing to overwrite local backup file: {destination}")
    try:
        if os.name == "nt":
            # MoveFileEx without MOVEFILE_REPLACE_EXISTING is atomic and fails
            # when another process creates destination after the check above.
            os.rename(temp, destination)
        else:
            # POSIX rename replaces an existing destination.  A same-directory
            # hard link gives us the required atomic no-replace publication.
            os.link(temp, destination)
            temp.unlink()
        _fsync_directory(destination.parent)
    except FileExistsError as exc:
        raise SyncError(
            f"refusing to overwrite local backup file: {destination}"
        ) from exc
    except OSError as exc:
        raise SyncError(
            f"cannot atomically publish local backup {destination}: {exc}"
        ) from exc


def default_pull_output_path(session: str) -> Path:
    if re.fullmatch(r"[0-9A-Za-z_-]{1,64}", session) is None:
        raise SyncError(f"unsafe backup session name: {session!r}")
    return (
        Path.home()
        / "Documents"
        / PULL_BACKUP_PARENT
        / f"isaac-vita-save-backup-{session}"
    )


def prepare_pull_output(path: Path | None, session: str) -> Path:
    candidate = (
        default_pull_output_path(session) if path is None else path.expanduser()
    )
    if path is None:
        documents = _plain_directory(Path.home() / "Documents", "Documents directory")
        backup_parent = documents / PULL_BACKUP_PARENT
        if _lexists(backup_parent):
            backup_parent = _plain_directory(backup_parent, "backup parent")
        else:
            try:
                backup_parent.mkdir()
                _fsync_directory(documents)
            except OSError as exc:
                raise SyncError(
                    f"cannot create backup parent {backup_parent}: {exc}"
                ) from exc
        candidate = backup_parent / candidate.name
    else:
        parent = _plain_directory(candidate.parent, "backup parent")
        candidate = parent / candidate.name

    if not candidate.name or candidate.name in (".", ".."):
        raise SyncError(f"unsafe local backup directory: {candidate}")
    if _lexists(candidate):
        raise SyncError(f"refusing to reuse local backup directory: {candidate}")
    try:
        candidate.mkdir()
        _fsync_directory(candidate.parent)
    except OSError as exc:
        raise SyncError(
            f"cannot create local backup directory {candidate}: {exc}"
        ) from exc
    if _is_link_or_reparse(candidate):
        raise SyncError(f"refusing symlink/reparse backup directory: {candidate}")
    return candidate.resolve(strict=True)


def _is_link_or_reparse(path: Path) -> bool:
    info = path.lstat()
    attributes = getattr(info, "st_file_attributes", 0)
    reparse_flag = getattr(stat, "FILE_ATTRIBUTE_REPARSE_POINT", 0)
    return stat.S_ISLNK(info.st_mode) or bool(attributes & reparse_flag)


def _check_case_collision(seen: dict[str, str], relative: PurePosixPath) -> None:
    spelling = relative.as_posix()
    folded = spelling.casefold()
    previous = seen.get(folded)
    if previous is not None and previous != spelling:
        raise SyncError(
            f"case-colliding paths cannot be copied safely: {previous}, {spelling}"
        )
    seen[folded] = spelling


def _safe_files(root: Path) -> list[Path]:
    if not os.path.lexists(root):
        raise SyncError(f"sync root not found: {root}")
    if _is_link_or_reparse(root):
        raise SyncError(f"refusing symlink/reparse sync root: {root}")
    root = root.resolve(strict=True)
    if not root.is_dir():
        raise SyncError(f"sync root is not a directory: {root}")

    files: list[Path] = []
    seen: dict[str, str] = {}
    pending = [root]
    while pending:
        directory = pending.pop()
        children = sorted(directory.iterdir(), key=lambda path: path.name)
        child_directories: list[Path] = []
        for candidate in children:
            if _is_link_or_reparse(candidate):
                raise SyncError(f"refusing symlink/reparse inside sync root: {candidate}")
            info = candidate.lstat()
            relative = PurePosixPath(*candidate.relative_to(root).parts)
            _check_case_collision(seen, relative)
            if stat.S_ISDIR(info.st_mode):
                child_directories.append(candidate)
                continue
            if not stat.S_ISREG(info.st_mode):
                raise SyncError(f"refusing non-regular path inside sync root: {candidate}")
            resolved = candidate.resolve(strict=True)
            if root not in resolved.parents:
                raise SyncError(f"file escaped sync root: {candidate}")
            files.append(resolved)
        pending.extend(reversed(child_directories))
    return sorted(files, key=lambda path: path.relative_to(root).parts)


def discover_save_dir(explicit: Path | None) -> Path:
    if explicit is not None:
        path = explicit.expanduser().resolve()
    else:
        path = (
            Path.home()
            / "Documents"
            / "My Games"
            / "Binding of Isaac Repentance"
        )
    if not path.is_dir():
        raise SyncError(f"Isaac PC save directory not found: {path}")
    return path


def discover_saves(root: Path, latest_backups: bool = False) -> list[Transfer]:
    by_destination: dict[str, Path] = {}
    backups: dict[str, tuple[str, Path]] = {}
    for candidate in sorted(root.iterdir()):
        match = SAVE_NAME.fullmatch(candidate.name)
        backup_match = (
            None if match is not None else DATED_SAVE_NAME.fullmatch(candidate.name)
        )
        if match is None and backup_match is None:
            continue
        if candidate.is_symlink():
            raise SyncError(f"refusing symlinked PC save: {candidate}")
        if not candidate.is_file():
            raise SyncError(f"refusing non-regular PC save: {candidate}")
        if match is None:
            assert backup_match is not None
            destination = (
                f"{backup_match.group('kind').lower()}"
                f"{backup_match.group('slot')}.dat"
            )
            value = (backup_match.group("date"), candidate.resolve(strict=True))
            if destination not in backups or value[0] > backups[destination][0]:
                backups[destination] = value
            continue
        destination = f"{match.group('kind').lower()}{match.group('slot')}.dat"
        previous = by_destination.get(destination)
        if previous is not None:
            raise SyncError(
                f"two active PC saves map to {destination}: {previous.name}, "
                f"{candidate.name}"
            )
        by_destination[destination] = candidate.resolve(strict=True)
    if latest_backups:
        for destination, date_and_source in sorted(backups.items()):
            by_destination.setdefault(destination, date_and_source[1])
    if not any(name.startswith("persistentgamedata") for name in by_destination):
        raise SyncError(
            f"no active rep_persistentgamedata[1-3].dat files in {root}; "
            "pass --latest-save-backups to select the newest dated backup per slot"
        )
    return [
        Transfer(source, REMOTE_SAVE / destination, "save")
        for destination, source in sorted(by_destination.items())
    ]


def _steam_registry_root() -> Path | None:
    if sys.platform != "win32":
        return None
    try:
        import winreg

        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam") as key:
            value, unused_kind = winreg.QueryValueEx(key, "SteamPath")
        return Path(value)
    except (ImportError, FileNotFoundError, OSError):
        return None


def steam_libraries(explicit: list[Path]) -> list[Path]:
    roots = [path.expanduser().resolve() for path in explicit]
    registry_root = _steam_registry_root()
    if not roots and registry_root is not None:
        roots.append(registry_root.resolve())
    discovered: list[Path] = []
    for root in roots:
        if root not in discovered:
            discovered.append(root)
        manifest = root / "steamapps" / "libraryfolders.vdf"
        if not manifest.is_file():
            continue
        text = manifest.read_text(encoding="utf-8", errors="replace")
        for raw in re.findall(r'^\s*"path"\s+"([^"]+)"', text, re.MULTILINE):
            path = Path(raw.replace(r"\\", "\\")).expanduser().resolve()
            if path not in discovered:
                discovered.append(path)
    if not discovered:
        raise SyncError("Steam root not found; pass --steam-root")
    return discovered


def discover_workshop_mods(libraries: list[Path]) -> dict[str, Path]:
    mods: dict[str, Path] = {}
    for library in libraries:
        root = library / "steamapps" / "workshop" / "content" / APP_ID
        if not os.path.lexists(root):
            continue
        if _is_link_or_reparse(root):
            raise SyncError(f"refusing symlink/reparse Workshop root: {root}")
        if not root.is_dir():
            raise SyncError(f"Workshop root is not a directory: {root}")
        for candidate in sorted(root.iterdir()):
            if _is_link_or_reparse(candidate):
                raise SyncError(f"refusing symlink/reparse Workshop item: {candidate}")
            if not candidate.is_dir() or not candidate.name.isdecimal():
                continue
            if not (candidate / "metadata.xml").is_file():
                continue
            previous = mods.get(candidate.name)
            if previous is not None and previous.resolve() != candidate.resolve():
                raise SyncError(
                    f"Workshop item {candidate.name} exists in two libraries: "
                    f"{previous}, {candidate}"
                )
            mods[candidate.name] = candidate.resolve()
    return mods


def _tree_files_by_relative(root: Path, files: list[Path]) -> dict[PurePosixPath, Path]:
    return {
        PurePosixPath(*source.relative_to(root).parts): source
        for source in files
    }


def _same_tree_contents(
    first_root: Path,
    first_files: list[Path],
    second_root: Path,
    second_files: list[Path],
) -> bool:
    first = _tree_files_by_relative(first_root, first_files)
    second = _tree_files_by_relative(second_root, second_files)
    if first.keys() != second.keys():
        return False
    for relative, first_path in first.items():
        second_path = second[relative]
        if first_path.stat().st_size != second_path.stat().st_size:
            return False
        if sha256_file(first_path) != sha256_file(second_path):
            return False
    return True


def _prepared_mod_copy(
    libraries: list[Path], item: str
) -> tuple[Path, list[Path]] | None:
    suffix = f"_{item}"
    candidates: list[tuple[Path, list[Path]]] = []
    seen_roots: set[Path] = set()
    for library in libraries:
        root = library / "steamapps" / "common" / GAME_DIR / "mods"
        if not os.path.lexists(root):
            continue
        if _is_link_or_reparse(root):
            raise SyncError(f"refusing symlink/reparse installed mods root: {root}")
        if not root.is_dir():
            raise SyncError(f"installed mods root is not a directory: {root}")
        for candidate in sorted(root.iterdir(), key=lambda path: path.name):
            if not candidate.name.endswith(suffix):
                continue
            if _is_link_or_reparse(candidate):
                raise SyncError(
                    f"refusing symlink/reparse prepared mod copy: {candidate}"
                )
            if not candidate.is_dir():
                raise SyncError(f"prepared mod copy is not a directory: {candidate}")
            resolved = candidate.resolve(strict=True)
            if resolved in seen_roots:
                continue
            seen_roots.add(resolved)
            candidates.append((resolved, _safe_files(candidate)))
    if len(candidates) > 1:
        paths = ", ".join(str(root) for root, unused_files in candidates)
        raise SyncError(f"ambiguous prepared mod copies for Workshop {item}: {paths}")
    return candidates[0] if candidates else None


def discover_mod_transfers(
    libraries: list[Path], requested: list[str], all_mods: bool
) -> list[Transfer]:
    mods = discover_workshop_mods(libraries)
    selected = sorted(mods) if all_mods else sorted(set(requested))
    if not selected:
        raise SyncError("no Workshop mods selected; pass --mod ID or --all-mods")
    if any(not item.isdecimal() for item in selected):
        raise SyncError("Workshop IDs must contain decimal digits only")
    missing = [item for item in selected if item not in mods]
    if missing:
        raise SyncError(f"Workshop mods not found: {', '.join(missing)}")
    transfers: list[Transfer] = []
    for item in selected:
        workshop_root = mods[item]
        workshop_files = _safe_files(workshop_root)
        root = workshop_root
        files = workshop_files
        remote_basename = item
        prepared = _prepared_mod_copy(libraries, item)
        if prepared is not None:
            prepared_root, prepared_files = prepared
            if _same_tree_contents(
                workshop_root,
                workshop_files,
                prepared_root,
                prepared_files,
            ):
                if any(character in prepared_root.name for character in ("/", "\\")):
                    raise SyncError(
                        f"unsafe prepared mod basename: {prepared_root.name!r}"
                    )
                root = prepared_root
                files = prepared_files
                remote_basename = prepared_root.name
        for source in files:
            relative = PurePosixPath(*source.relative_to(root).parts)
            transfers.append(
                Transfer(
                    source,
                    REMOTE_MODS / remote_basename / relative,
                    f"mod:{item}",
                )
            )
    return transfers


def discover_sentinel_transfers(root: Path = SENTINEL_MOD_ROOT) -> list[Transfer]:
    """Return the exact project-owned Lua/callback smoke-test mod.

    This is deliberately independent of Steam discovery.  The closed two-file
    inventory prevents an accidental local build artefact or disable marker
    from becoming part of a diagnostic whose only job is to distinguish Lua
    startup from the first engine-dispatched callbacks.
    """

    files = _safe_files(root)
    by_relative = _tree_files_by_relative(root.resolve(strict=True), files)
    if frozenset(by_relative) != SENTINEL_MOD_FILES:
        actual = ", ".join(sorted(path.as_posix() for path in by_relative))
        raise SyncError(f"Lua sentinel source inventory changed: {actual}")
    return [
        Transfer(
            by_relative[relative],
            REMOTE_MODS / SENTINEL_MOD_BASENAME / relative,
            "mod:lua-sentinel",
        )
        for relative in sorted(SENTINEL_MOD_FILES, key=lambda path: path.parts)
    ]


class VitaFTP:
    def __init__(self, host: str, port: int, timeout: float, ftp_factory=ftplib.FTP):
        self.host = host
        self.port = port
        self.timeout = timeout
        self.ftp_factory = ftp_factory

    @contextmanager
    def connect(self):
        ftp = self.ftp_factory()
        try:
            ftp.connect(self.host, self.port, timeout=self.timeout)
            ftp.login("anonymous", "isaac-sync@localhost")
            ftp.voidcmd("TYPE I")
            yield ftp
        except (OSError, EOFError, ftplib.Error) as exc:
            raise SyncError(f"FTP {self.host}:{self.port} failed: {exc}") from exc
        finally:
            try:
                ftp.quit()
            except (AttributeError, OSError, EOFError, ftplib.Error):
                try:
                    ftp.close()
                except (AttributeError, OSError):
                    pass

    @staticmethod
    def _remote(path: PurePosixPath | str) -> PurePosixPath:
        value = str(path).replace("\\", "/")
        if not value.startswith("/"):
            value = "/" + value
        if any(part in (".", "..") for part in value.split("/")):
            raise SyncError(f"unsafe remote path: {path}")
        normalized = PurePosixPath(value)
        try:
            normalized.relative_to(REMOTE_ROOT)
        except ValueError as exc:
            raise SyncError(f"remote path escaped Isaac data root: {path}") from exc
        return normalized

    @classmethod
    def _ensure_dir(cls, ftp, path: PurePosixPath | str) -> None:
        target = cls._remote(path)
        current = REMOTE_ROOT
        for part in target.relative_to(REMOTE_ROOT).parts:
            current /= part
            try:
                ftp.mkd(str(current))
            except ftplib.error_perm as exc:
                if not str(exc).startswith("550"):
                    raise
                kind = cls._kind(ftp, current)
                if kind is None or kind[0] != "dir":
                    raise SyncError(f"remote path component is not a directory: {current}")

    @classmethod
    def _kind(cls, ftp, path: PurePosixPath | str) -> tuple[str, int | None] | None:
        target = cls._remote(path)
        try:
            rows = ftp.mlsd(str(target.parent))
            for name, facts in rows:
                if name != target.name:
                    continue
                size = facts.get("size", "")
                return facts.get("type", "unknown"), int(size) if size.isdigit() else None
            return None
        except (AttributeError, ftplib.error_perm) as exc:
            if isinstance(exc, ftplib.error_perm) and not str(exc).startswith(
                ("500", "501", "502", "504", "550")
            ):
                raise
        try:
            size = ftp.size(str(target))
            return "file", int(size)
        except ftplib.error_perm:
            pass
        original = ftp.pwd()
        try:
            ftp.cwd(str(target))
            return "dir", None
        except ftplib.error_perm:
            return None
        finally:
            ftp.cwd(original)

    @staticmethod
    def _remote_sha256(ftp, path: PurePosixPath) -> tuple[int, str]:
        digest = hashlib.sha256()
        size = 0

        def consume(block: bytes) -> None:
            nonlocal size
            size += len(block)
            digest.update(block)

        ftp.retrbinary(f"RETR {path}", consume, blocksize=256 * 1024)
        return size, digest.hexdigest()

    @classmethod
    def _remote_save_slot(cls, path: PurePosixPath | str) -> tuple[int, PurePosixPath]:
        target = cls._remote(path)
        for slot, expected in REMOTE_SAVE_SLOTS:
            if target == expected:
                return slot, target
        raise SyncError(f"remote path is not a persistent save slot: {path}")

    def download_save_atomic(
        self,
        ftp,
        remote: PurePosixPath | str,
        output_dir: Path,
        token: str,
    ) -> dict:
        slot, source = self._remote_save_slot(remote)
        if re.fullmatch(r"[0-9A-Za-z_-]{1,64}", token) is None:
            raise SyncError(f"unsafe local staging token: {token!r}")
        root = _plain_directory(output_dir, "backup directory")
        destination = root / source.name
        temp = root / f".{source.name}.isaac-pull-{token}.tmp"
        if destination.parent != root or temp.parent != root:
            raise SyncError(f"local save path escaped backup directory: {destination}")
        if _lexists(destination):
            raise SyncError(f"refusing to overwrite local backup file: {destination}")
        if _lexists(temp):
            raise SyncError(f"local staging name collision: {temp}")

        digest = hashlib.sha256()
        size = 0
        promoted = False
        try:
            with temp.open("xb") as stream:
                def consume(block: bytes) -> None:
                    nonlocal size
                    if not isinstance(block, bytes):
                        raise SyncError(f"FTP returned non-byte save data for slot {slot}")
                    stream.write(block)
                    size += len(block)
                    digest.update(block)

                ftp.retrbinary(f"RETR {source}", consume, blocksize=256 * 1024)
                stream.flush()
                os.fsync(stream.fileno())
            _promote_no_replace(temp, destination)
            promoted = True
            saved_size, saved_sha = sha256_file(destination)
            received_sha = digest.hexdigest()
            if (saved_size, saved_sha) != (size, received_sha):
                raise SyncError(f"local SHA-256 readback mismatch for save slot {slot}")
        except BaseException as exc:
            cleanup_error = None
            if not promoted and _lexists(temp):
                try:
                    temp.unlink()
                    _fsync_directory(root)
                except OSError as cleanup_exc:
                    cleanup_error = cleanup_exc
            if isinstance(exc, RemoteSaveMissing):
                raise
            if isinstance(exc, ftplib.error_perm) and str(exc).startswith("550"):
                detail = (
                    f"; temp cleanup failed: {cleanup_error}"
                    if cleanup_error
                    else ""
                )
                raise RemoteSaveMissing(f"Vita save slot {slot} is absent{detail}") from exc
            if isinstance(exc, (KeyboardInterrupt, SystemExit)) and cleanup_error is None:
                raise
            detail = f"; temp cleanup failed: {cleanup_error}" if cleanup_error else ""
            if isinstance(exc, SyncError):
                raise SyncError(f"{exc}{detail}") from exc
            raise SyncError(f"download failed for save slot {slot}: {exc}{detail}") from exc

        return {
            "slot": slot,
            "remote": str(source)[1:],
            "local": destination.name,
            "size": size,
            "sha256": digest.hexdigest(),
            "verified": "retr-sha256+fsync+atomic-no-replace+local-readback-sha256",
        }

    def upload_atomic(
        self,
        ftp,
        transfer: Transfer,
        session: str,
        token: str,
    ) -> dict:
        source_path = transfer.source
        if source_path.is_symlink():
            raise SyncError(f"refusing symlinked local file: {source_path}")
        source = source_path.resolve(strict=True)
        if not source.is_file():
            raise SyncError(f"refusing non-regular local file: {source}")
        target = self._remote(transfer.remote)
        relative = target.relative_to(REMOTE_ROOT)
        backup = self._remote(REMOTE_BACKUPS / session / relative)
        temp = self._remote(f"{target}.isaac-sync-{token}.tmp")
        failed = self._remote(f"{target}.isaac-sync-{token}.failed")
        local_size, local_sha = sha256_file(source)
        old = self._kind(ftp, target)
        if old is not None and old[0] != "file":
            raise SyncError(f"refusing to replace remote {old[0]}: {target}")
        if self._kind(ftp, temp) is not None or self._kind(ftp, failed) is not None:
            raise SyncError(f"session staging name collision for {target}")
        if old is not None and self._kind(ftp, backup) is not None:
            raise SyncError(f"session backup already exists: {backup}")

        moved_old = False
        installed = False
        promotion_attempted = False
        self._ensure_dir(ftp, target.parent)
        try:
            with source.open("rb") as stream:
                ftp.storbinary(f"STOR {temp}", stream, blocksize=256 * 1024)
            staged = self._kind(ftp, temp)
            if staged != ("file", local_size):
                raise SyncError(f"staged size mismatch for {target}: {staged}")
            staged_size, staged_sha = self._remote_sha256(ftp, temp)
            if (staged_size, staged_sha) != (local_size, local_sha):
                raise SyncError(f"staged SHA-256 mismatch for {target}")
            if old is not None:
                self._ensure_dir(ftp, backup.parent)
                ftp.rename(str(target), str(backup))
                moved_old = True
            promotion_attempted = True
            ftp.rename(str(temp), str(target))
            installed = True
            if self._kind(ftp, target) != ("file", local_size):
                raise SyncError(f"installed size mismatch for {target}")
        except (Exception, KeyboardInterrupt) as exc:
            rollback: list[str] = []
            try:
                target_now = self._kind(ftp, target)
                backup_now = self._kind(ftp, backup) if old is not None else None
                temp_now = self._kind(ftp, temp)
                if old is not None and backup_now is not None:
                    moved_old = True
                if (
                    promotion_attempted
                    and target_now == ("file", local_size)
                    and temp_now is None
                ):
                    installed = True
            except (OSError, EOFError, ftplib.Error, SyncError) as probe_exc:
                rollback.append(f"probe ambiguous upload: {probe_exc}")
            try:
                if installed and self._kind(ftp, target) is not None:
                    ftp.rename(str(target), str(failed))
            except (OSError, EOFError, ftplib.Error) as rollback_exc:
                rollback.append(f"preserve failed target: {rollback_exc}")
            try:
                if moved_old and self._kind(ftp, backup) is not None:
                    ftp.rename(str(backup), str(target))
            except (OSError, EOFError, ftplib.Error) as rollback_exc:
                rollback.append(f"restore backup: {rollback_exc}")
            try:
                if self._kind(ftp, temp) is not None:
                    ftp.delete(str(temp))
            except (OSError, EOFError, ftplib.Error) as rollback_exc:
                rollback.append(f"remove owned staging file: {rollback_exc}")
            detail = f"; rollback incomplete: {'; '.join(rollback)}" if rollback else ""
            raise SyncError(f"upload failed for {target}: {exc}{detail}") from exc

        return {
            "group": transfer.group,
            "source": str(source),
            "remote": str(target)[1:],
            "size": local_size,
            "sha256": local_sha,
            "backup": str(backup)[1:] if moved_old else None,
            "verified": "staged-sha256+installed-size",
        }


def build_transfers(args: argparse.Namespace) -> list[Transfer]:
    transfers: list[Transfer] = []
    if args.saves:
        transfers.extend(
            discover_saves(
                discover_save_dir(args.save_dir),
                latest_backups=args.latest_save_backups,
            )
        )
    if args.mod or args.all_mods:
        transfers.extend(
            discover_mod_transfers(
                steam_libraries(args.steam_root), args.mod, args.all_mods
            )
        )
    if args.lua_sentinel:
        transfers.extend(discover_sentinel_transfers())
    if not transfers:
        raise SyncError(
            "nothing selected; pass --saves, --mod ID, --all-mods, or "
            "--lua-sentinel"
        )
    destinations = [str(item.remote).lower() for item in transfers]
    if len(destinations) != len(set(destinations)):
        raise SyncError("two local files map to the same Vita path")
    return transfers


def print_plan(transfers: list[Transfer]) -> None:
    total = sum(item.source.stat().st_size for item in transfers)
    groups: dict[str, int] = {}
    for item in transfers:
        groups[item.group] = groups.get(item.group, 0) + 1
    print(f"files={len(transfers)} bytes={total}")
    for group, count in sorted(groups.items()):
        print(f"  {group}: {count} files")
    for item in transfers[:20]:
        print(f"  {item.source} -> {str(item.remote)[1:]}")
    if len(transfers) > 20:
        print(f"  ... {len(transfers) - 20} more files")


def _checkpoint_receipt(stream, receipt: dict) -> None:
    stream.seek(0)
    stream.truncate(0)
    json.dump(receipt, stream, indent=2, sort_keys=True)
    stream.write("\n")
    stream.flush()
    os.fsync(stream.fileno())


def _create_receipt(path: Path, receipt: dict):
    path = path.expanduser()
    parent = _plain_directory(path.parent, "receipt directory")
    path = parent / path.name
    if not path.name or path.name in (".", ".."):
        raise SyncError(f"unsafe receipt path: {path}")
    if _lexists(path):
        raise SyncError(f"refusing to overwrite receipt: {path}")
    try:
        stream = path.open("x+", encoding="utf-8", newline="\n")
    except OSError as exc:
        raise SyncError(f"cannot create receipt {path}: {exc}") from exc
    try:
        _checkpoint_receipt(stream, receipt)
    except BaseException:
        stream.close()
        raise
    return stream


def pull_save_slots(
    transport: VitaFTP,
    output_dir: Path,
    receipt_path: Path,
    session: str,
    created_utc: datetime,
) -> dict:
    output = _plain_directory(output_dir, "backup directory")
    files: list[dict] = []
    missing: list[dict] = []
    receipt = {
        "schema": 1,
        "operation": "vita-save-pull",
        "session": session,
        "created_utc": created_utc.isoformat(),
        "host": transport.host,
        "port": transport.port,
        "output_dir": str(output),
        "state": "in-progress",
        "files": files,
        "missing": missing,
    }
    with _create_receipt(receipt_path, receipt) as receipt_stream:
        try:
            with transport.connect() as ftp:
                for slot, remote in REMOTE_SAVE_SLOTS:
                    try:
                        record = transport.download_save_atomic(
                            ftp, remote, output, uuid.uuid4().hex
                        )
                    except RemoteSaveMissing as exc:
                        missing.append(
                            {
                                "slot": slot,
                                "remote": str(remote)[1:],
                                "reason": str(exc),
                            }
                        )
                    else:
                        files.append(record)
                    _checkpoint_receipt(receipt_stream, receipt)
            if not files:
                raise SyncError("no persistent Vita save slots were found")
        except BaseException as exc:
            receipt["state"] = "failed"
            receipt["error"] = f"{type(exc).__name__}: {exc}"
            _checkpoint_receipt(receipt_stream, receipt)
            raise
        receipt["state"] = "complete"
        _checkpoint_receipt(receipt_stream, receipt)
    return receipt


def parser() -> argparse.ArgumentParser:
    result = argparse.ArgumentParser(description=__doc__)
    result.add_argument("command", choices=("plan", "push", "pull"))
    result.add_argument("--saves", action="store_true", help="copy active save slots")
    result.add_argument("--save-dir", type=Path)
    result.add_argument(
        "--latest-save-backups",
        action="store_true",
        help="when an active slot is absent, use its newest YYYYMMDD backup",
    )
    result.add_argument("--mod", action="append", default=[], metavar="WORKSHOP_ID")
    result.add_argument("--all-mods", action="store_true")
    result.add_argument(
        "--lua-sentinel",
        action="store_true",
        help="install the project-owned two-file Lua/callback smoke-test mod",
    )
    result.add_argument("--steam-root", action="append", type=Path, default=[])
    result.add_argument("--host", help="Vita IP/host for push or pull")
    result.add_argument("--port", type=int, default=1337)
    result.add_argument("--timeout", type=float, default=15.0)
    result.add_argument("--yes", action="store_true", help="confirm writes for push")
    result.add_argument("--receipt", type=Path)
    result.add_argument(
        "--output-dir",
        type=Path,
        help="new, non-existing directory for a Vita-to-PC save backup",
    )
    return result


def main(argv: list[str] | None = None) -> int:
    args = parser().parse_args(argv)
    try:
        if args.port < 1 or args.port > 65535:
            raise SyncError("FTP port must be between 1 and 65535")
        if args.timeout <= 0:
            raise SyncError("FTP timeout must be positive")

        if args.command == "pull":
            if not args.saves:
                raise SyncError("pull requires --saves")
            if (
                args.save_dir is not None
                or args.latest_save_backups
                or args.mod
                or args.all_mods
                or args.lua_sentinel
                or args.steam_root
                or args.yes
            ):
                raise SyncError(
                    "pull accepts only Vita save slots; PC save/mod and --yes "
                    "options are not valid"
                )
            if not args.host:
                raise SyncError("pull requires --host")

            now = datetime.now(timezone.utc)
            session = now.strftime("%Y%m%dT%H%M%SZ") + "-" + uuid.uuid4().hex[:8]
            output = prepare_pull_output(args.output_dir, session)
            receipt_path = args.receipt or output / "receipt.json"
            print(f"operation=vita-save-pull files=3 output={output}")
            for slot, remote in REMOTE_SAVE_SLOTS:
                print(f"  slot {slot}: {str(remote)[1:]} -> {output / remote.name}")
            transport = VitaFTP(args.host, args.port, args.timeout)
            receipt = pull_save_slots(
                transport, output, receipt_path, session, now
            )
            for record in receipt["files"]:
                print(
                    f"  saved slot {record['slot']}: {record['local']} "
                    f"bytes={record['size']} sha256={record['sha256']}"
                )
            for record in receipt["missing"]:
                print(f"  missing slot {record['slot']}")
            print(
                f"receipt={Path(receipt_path).expanduser().resolve()} "
                f"state=complete output={output}"
            )
            return 0

        if args.output_dir is not None:
            raise SyncError("--output-dir is valid only for pull")
        transfers = build_transfers(args)
        print_plan(transfers)
        if args.command == "plan":
            return 0
        if not args.host:
            raise SyncError("push requires --host")
        if not args.yes:
            raise SyncError("push writes Vita data; inspect plan and pass --yes")

        now = datetime.now(timezone.utc)
        session = now.strftime("%Y%m%dT%H%M%SZ") + "-" + uuid.uuid4().hex[:8]
        records: list[dict] = []
        receipt = {
            "schema": 1,
            "session": session,
            "created_utc": now.isoformat(),
            "host": args.host,
            "port": args.port,
            "state": "in-progress",
            "files": records,
        }
        receipt_path = args.receipt or Path(f"isaac-vita-sync-{session}.json")
        transport = VitaFTP(args.host, args.port, args.timeout)
        with _create_receipt(receipt_path, receipt) as receipt_stream:
            print(f"receipt={receipt_path.resolve()} state=in-progress")
            try:
                with transport.connect() as ftp:
                    for index, transfer in enumerate(transfers, 1):
                        token = uuid.uuid4().hex
                        record = transport.upload_atomic(
                            ftp, transfer, session, token
                        )
                        records.append(record)
                        _checkpoint_receipt(receipt_stream, receipt)
                        print(f"[{index}/{len(transfers)}] {record['remote']}")
            except BaseException as exc:
                receipt["state"] = "failed"
                receipt["error"] = f"{type(exc).__name__}: {exc}"
                _checkpoint_receipt(receipt_stream, receipt)
                raise
            receipt["state"] = "complete"
            _checkpoint_receipt(receipt_stream, receipt)
        print(f"receipt={receipt_path.resolve()} state=complete")
        return 0
    except (OSError, SyncError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
