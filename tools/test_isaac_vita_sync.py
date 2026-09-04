from __future__ import annotations

import ftplib
import importlib.util
import json
from datetime import datetime, timezone
from pathlib import Path
from pathlib import PurePosixPath
import sys
import tempfile
import unittest
import xml.etree.ElementTree as ET


HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location(
    "isaac_vita_sync", HERE / "isaac_vita_sync.py"
)
assert SPEC is not None and SPEC.loader is not None
sync = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = sync
SPEC.loader.exec_module(sync)


class FakeFTP:
    def __init__(self):
        self.directories = {
            "/",
            "/ux0:",
            "/ux0:/data",
            "/ux0:/data/isaacr001",
        }
        self.files: dict[str, bytes] = {}
        self.current = "/"
        self.fail_store_before_write = False
        self.fail_install = False
        self.ambiguous_install = False
        self.interrupt_backup = False
        self.fail_retr_after_data = False
        self.retr_commands: list[str] = []
        self.mutation_commands: list[str] = []

    def connect(self, *_args, **_kwargs):
        return None

    def login(self, *_args, **_kwargs):
        return None

    def voidcmd(self, *_args, **_kwargs):
        return "200 OK"

    def quit(self):
        return None

    def close(self):
        return None

    def pwd(self):
        return self.current

    def cwd(self, path):
        if path not in self.directories:
            raise ftplib.error_perm("550 missing")
        self.current = path

    def mkd(self, path):
        self.mutation_commands.append(f"MKD {path}")
        if path in self.directories:
            raise ftplib.error_perm("550 exists")
        parent = str(PurePosixPath(path).parent)
        if parent not in self.directories:
            raise ftplib.error_perm("550 missing parent")
        self.directories.add(path)

    def mlsd(self, path):
        if path not in self.directories:
            raise ftplib.error_perm("550 missing")
        prefix = path.rstrip("/") + "/"
        rows = []
        for directory in self.directories:
            if directory.startswith(prefix):
                remainder = directory[len(prefix) :]
                if remainder and "/" not in remainder:
                    rows.append((remainder, {"type": "dir"}))
        for name, payload in self.files.items():
            if name.startswith(prefix):
                remainder = name[len(prefix) :]
                if remainder and "/" not in remainder:
                    rows.append(
                        (remainder, {"type": "file", "size": str(len(payload))})
                    )
        return iter(rows)

    def size(self, path):
        if path not in self.files:
            raise ftplib.error_perm("550 missing")
        return len(self.files[path])

    def storbinary(self, command, stream, blocksize=8192):
        unused_blocksize = blocksize
        self.mutation_commands.append(command)
        if self.fail_store_before_write:
            raise ftplib.error_temp("426 injected store failure")
        path = command.split(" ", 1)[1]
        self.files[path] = stream.read()

    def retrbinary(self, command, callback, blocksize=8192):
        unused_blocksize = blocksize
        self.retr_commands.append(command)
        path = command.split(" ", 1)[1]
        if path not in self.files:
            raise ftplib.error_perm("550 missing")
        payload = self.files[path]
        for offset in range(0, len(payload), 3):
            callback(payload[offset : offset + 3])
            if self.fail_retr_after_data:
                raise ftplib.error_temp("426 injected retrieve failure")

    def rename(self, source, destination):
        self.mutation_commands.append(f"RNFR/RNTO {source} {destination}")
        if source not in self.files:
            raise ftplib.error_perm("550 missing")
        if self.fail_install and ".tmp" in source:
            raise ftplib.error_perm("550 injected install failure")
        if destination in self.files or destination in self.directories:
            raise ftplib.error_perm("550 destination exists")
        self.files[destination] = self.files.pop(source)
        if self.interrupt_backup and "/.sync-backups/" in destination:
            raise KeyboardInterrupt
        if self.ambiguous_install and ".tmp" in source:
            raise ftplib.error_temp("426 injected lost acknowledgement")

    def delete(self, path):
        self.mutation_commands.append(f"DELE {path}")
        if path not in self.files:
            raise ftplib.error_perm("550 missing")
        del self.files[path]


class DiscoveryTests(unittest.TestCase):
    def test_save_mapping_ignores_dated_backups_and_preserves_vita_settings(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "rep_persistentgamedata1.dat").write_bytes(b"persistent")
            (root / "rep_gamestate1.dat").write_bytes(b"run")
            (root / "20260830.rep_persistentgamedata1.dat").write_bytes(b"old")
            (root / "options.ini").write_text("pc-only", encoding="utf-8")
            transfers = sync.discover_saves(root)
        self.assertEqual(
            [item.remote.name for item in transfers],
            ["persistentgamedata1.dat"],
        )
        self.assertEqual({item.group for item in transfers}, {"save"})

    def test_receipt_is_exclusive_and_checkpoints_progress(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "receipt.json"
            receipt = {"schema": 1, "state": "in-progress", "files": []}
            with sync._create_receipt(path, receipt) as stream:
                receipt["files"].append({"remote": "one"})
                sync._checkpoint_receipt(stream, receipt)
            self.assertEqual(json.loads(path.read_text(encoding="utf-8")), receipt)
            with self.assertRaisesRegex(sync.SyncError, "refusing to overwrite"):
                sync._create_receipt(path, receipt)
            self.assertEqual(json.loads(path.read_text(encoding="utf-8")), receipt)

    def test_dated_backup_is_not_silently_promoted(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "20260830.rep_persistentgamedata1.dat").write_bytes(b"old")
            with self.assertRaisesRegex(sync.SyncError, "latest-save-backups"):
                sync.discover_saves(root)
            transfers = sync.discover_saves(root, latest_backups=True)
        self.assertEqual([item.remote.name for item in transfers], ["persistentgamedata1.dat"])
        self.assertEqual(transfers[0].source.name, "20260830.rep_persistentgamedata1.dat")

    def test_symlinked_active_save_is_rejected_instead_of_using_backup(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            target = root / "outside.dat"
            target.write_bytes(b"current")
            (root / "rep_persistentgamedata1.dat").symlink_to(target)
            (root / "20260830.rep_persistentgamedata1.dat").write_bytes(b"old")
            with self.assertRaisesRegex(sync.SyncError, "symlinked PC save"):
                sync.discover_saves(root, latest_backups=True)

    def test_library_vdf_and_workshop_mod_mapping(self):
        with tempfile.TemporaryDirectory() as directory:
            base = Path(directory)
            steam = base / "Steam"
            library = base / "Library"
            (steam / "steamapps").mkdir(parents=True)
            (steam / "steamapps" / "libraryfolders.vdf").write_text(
                '"libraryfolders"\n{\n  "1"\n  {\n    "path" "'
                + str(library).replace("\\", "\\\\")
                + '"\n  }\n}\n',
                encoding="utf-8",
            )
            mod = library / "steamapps" / "workshop" / "content" / "250900" / "42"
            (mod / "resources" / "gfx").mkdir(parents=True)
            (mod / "metadata.xml").write_text("<metadata/>", encoding="utf-8")
            (mod / "main.lua").write_text("return 1", encoding="utf-8")
            (mod / "resources" / "gfx" / "x.png").write_bytes(b"png")
            libraries = sync.steam_libraries([steam])
            transfers = sync.discover_mod_transfers(libraries, ["42"], False)
        self.assertIn(library.resolve(), libraries)
        self.assertEqual(len(transfers), 3)
        self.assertTrue(
            all(str(item.remote).startswith("/ux0:/data/isaacr001/mods/42/") for item in transfers)
        )


class PreparedModPlanTests(unittest.TestCase):
    @staticmethod
    def _write_mod(root: Path, main: bytes = b"return 1") -> None:
        (root / "resources" / "gfx").mkdir(parents=True)
        (root / "metadata.xml").write_bytes(b"<metadata/>")
        (root / "main.lua").write_bytes(main)
        (root / "resources" / "gfx" / "x.png").write_bytes(b"png")

    @staticmethod
    def _workshop_root(library: Path, item: str = "42") -> Path:
        return library / "steamapps" / "workshop" / "content" / "250900" / item

    @staticmethod
    def _installed_root(library: Path, basename: str) -> Path:
        return (
            library
            / "steamapps"
            / "common"
            / "The Binding of Isaac Rebirth"
            / "mods"
            / basename
        )

    def test_identical_prepared_copy_preserves_its_basename(self):
        with tempfile.TemporaryDirectory() as directory:
            library = Path(directory)
            workshop = self._workshop_root(library)
            prepared = self._installed_root(library, "external_item_descriptions_42")
            self._write_mod(workshop)
            self._write_mod(prepared)
            transfers = sync.discover_mod_transfers([library], ["42"], False)
        self.assertEqual(len(transfers), 3)
        self.assertTrue(all(prepared.resolve() in item.source.parents for item in transfers))
        self.assertTrue(
            all(
                str(item.remote).startswith(
                    "/ux0:/data/isaacr001/mods/external_item_descriptions_42/"
                )
                for item in transfers
            )
        )

    def test_changed_prepared_copy_falls_back_to_numeric_workshop_root(self):
        with tempfile.TemporaryDirectory() as directory:
            library = Path(directory)
            workshop = self._workshop_root(library)
            prepared = self._installed_root(library, "example_42")
            self._write_mod(workshop, b"return 1")
            self._write_mod(prepared, b"return 2")
            transfers = sync.discover_mod_transfers([library], ["42"], False)
        self.assertTrue(all(workshop.resolve() in item.source.parents for item in transfers))
        self.assertTrue(
            all(
                str(item.remote).startswith("/ux0:/data/isaacr001/mods/42/")
                for item in transfers
            )
        )

    def test_duplicate_prepared_copies_are_rejected_as_ambiguous(self):
        with tempfile.TemporaryDirectory() as directory:
            library = Path(directory)
            self._write_mod(self._workshop_root(library))
            self._write_mod(self._installed_root(library, "first_42"))
            self._write_mod(self._installed_root(library, "second_42"))
            with self.assertRaisesRegex(sync.SyncError, "ambiguous prepared mod copies"):
                sync.discover_mod_transfers([library], ["42"], False)

    def test_symlinked_prepared_copy_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            library = Path(directory)
            self._write_mod(self._workshop_root(library))
            outside = library / "outside"
            self._write_mod(outside)
            installed = self._installed_root(library, "linked_42")
            installed.parent.mkdir(parents=True)
            installed.symlink_to(outside, target_is_directory=True)
            with self.assertRaisesRegex(sync.SyncError, "symlink/reparse prepared"):
                sync.discover_mod_transfers([library], ["42"], False)

    def test_case_colliding_paths_are_rejected(self):
        seen: dict[str, str] = {}
        sync._check_case_collision(seen, PurePosixPath("resources/gfx/Icon.png"))
        with self.assertRaisesRegex(sync.SyncError, "case-colliding paths"):
            sync._check_case_collision(
                seen, PurePosixPath("resources/gfx/icon.png")
            )


class LuaSentinelTests(unittest.TestCase):
    def test_exact_two_file_sentinel_maps_without_steam(self):
        transfers = sync.discover_sentinel_transfers()
        self.assertEqual(
            [item.source.name for item in transfers],
            ["main.lua", "metadata.xml"],
        )
        self.assertEqual({item.group for item in transfers}, {"mod:lua-sentinel"})
        self.assertTrue(
            all(
                str(item.remote).startswith(
                    "/ux0:/data/isaacr001/mods/repentogxm_lua_sentinel/"
                )
                for item in transfers
            )
        )
        main = next(item.source for item in transfers if item.source.name == "main.lua")
        text = main.read_text(encoding="utf-8")
        self.assertIn("REPENTOGXM LUA SENTINEL TOPLEVEL", text)
        self.assertIn("ModCallbacks.MC_POST_GAME_STARTED", text)
        self.assertIn("ModCallbacks.MC_POST_UPDATE", text)
        self.assertIn("ModCallbacks.MC_POST_RENDER", text)

        metadata = next(
            item.source for item in transfers if item.source.name == "metadata.xml"
        )
        root = ET.parse(metadata).getroot()
        self.assertEqual(root.tag, "metadata")
        self.assertEqual(root.findtext("name"), "repentogxm Lua Sentinel")
        self.assertEqual(root.findtext("directory"), sync.SENTINEL_MOD_BASENAME)
        self.assertIsNone(root.find("id"))
        self.assertEqual(root.findtext("visibility"), "Private")
        self.assertEqual([item.get("id") for item in root.findall("tag")], ["Lua"])

    def test_changed_sentinel_inventory_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "main.lua").write_text("return 1", encoding="utf-8")
            (root / "metadata.xml").write_text("<metadata/>", encoding="utf-8")
            (root / "disable.it").write_bytes(b"")
            with self.assertRaisesRegex(sync.SyncError, "inventory changed"):
                sync.discover_sentinel_transfers(root)

    def test_cli_selection_does_not_require_steam_or_saves(self):
        args = sync.parser().parse_args(["plan", "--lua-sentinel"])
        transfers = sync.build_transfers(args)
        self.assertEqual(len(transfers), 2)
        self.assertEqual({item.group for item in transfers}, {"mod:lua-sentinel"})


class UploadTests(unittest.TestCase):
    def test_atomic_upload_hashes_before_replacing_and_retains_backup(self):
        fake = FakeFTP()
        target = "/ux0:/data/isaacr001/Documents/My Games/Binding of Isaac Repentance/persistentgamedata1.dat"
        fake.directories.update(
            {
                "/ux0:/data/isaacr001/Documents",
                "/ux0:/data/isaacr001/Documents/My Games",
                "/ux0:/data/isaacr001/Documents/My Games/Binding of Isaac Repentance",
            }
        )
        fake.files[target] = b"old"
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "rep_persistentgamedata1.dat"
            source.write_bytes(b"new-save")
            transfer = sync.Transfer(source, sync.PurePosixPath(target), "save")
            transport = sync.VitaFTP("vita", 1337, 1)
            record = transport.upload_atomic(fake, transfer, "session", "token")
        self.assertEqual(fake.files[target], b"new-save")
        self.assertEqual(
            fake.files[
                "/ux0:/data/isaacr001/.sync-backups/session/Documents/My Games/Binding of Isaac Repentance/persistentgamedata1.dat"
            ],
            b"old",
        )
        self.assertEqual(record["verified"], "staged-sha256+installed-size")
        self.assertNotIn(".tmp", "\n".join(fake.files))

    def test_failed_install_restores_old_file_and_removes_owned_temp(self):
        fake = FakeFTP()
        target = "/ux0:/data/isaacr001/mods/42/main.lua"
        fake.directories.update(
            {
                "/ux0:/data/isaacr001/mods",
                "/ux0:/data/isaacr001/mods/42",
            }
        )
        fake.files[target] = b"old"
        fake.fail_install = True
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "main.lua"
            source.write_bytes(b"new")
            transfer = sync.Transfer(source, sync.PurePosixPath(target), "mod:42")
            transport = sync.VitaFTP("vita", 1337, 1)
            with self.assertRaisesRegex(sync.SyncError, "upload failed"):
                transport.upload_atomic(fake, transfer, "session", "token")
        self.assertEqual(fake.files, {target: b"old"})

    def test_failed_store_does_not_mistake_same_size_old_file_for_new(self):
        fake = FakeFTP()
        target = "/ux0:/data/isaacr001/mods/42/main.lua"
        fake.directories.update(
            {
                "/ux0:/data/isaacr001/mods",
                "/ux0:/data/isaacr001/mods/42",
            }
        )
        fake.files[target] = b"old"
        fake.fail_store_before_write = True
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "main.lua"
            source.write_bytes(b"new")
            transfer = sync.Transfer(source, sync.PurePosixPath(target), "mod:42")
            transport = sync.VitaFTP("vita", 1337, 1)
            with self.assertRaisesRegex(sync.SyncError, "upload failed"):
                transport.upload_atomic(fake, transfer, "session", "token")
        self.assertEqual(fake.files, {target: b"old"})

    def test_interrupted_backup_rename_restores_old_file_and_removes_temp(self):
        fake = FakeFTP()
        target = "/ux0:/data/isaacr001/mods/42/main.lua"
        fake.directories.update(
            {
                "/ux0:/data/isaacr001/mods",
                "/ux0:/data/isaacr001/mods/42",
            }
        )
        fake.files[target] = b"old"
        fake.interrupt_backup = True
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "main.lua"
            source.write_bytes(b"new")
            transfer = sync.Transfer(source, sync.PurePosixPath(target), "mod:42")
            transport = sync.VitaFTP("vita", 1337, 1)
            with self.assertRaisesRegex(sync.SyncError, "upload failed"):
                transport.upload_atomic(fake, transfer, "session", "token")
        self.assertEqual(fake.files, {target: b"old"})

    def test_upload_rejects_local_symlink_before_following_it(self):
        fake = FakeFTP()
        target = "/ux0:/data/isaacr001/mods/42/main.lua"
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            real_source = root / "outside.lua"
            real_source.write_bytes(b"outside")
            source = root / "main.lua"
            source.symlink_to(real_source)
            transfer = sync.Transfer(source, sync.PurePosixPath(target), "mod:42")
            transport = sync.VitaFTP("vita", 1337, 1)
            with self.assertRaisesRegex(sync.SyncError, "symlinked local file"):
                transport.upload_atomic(fake, transfer, "session", "token")
        self.assertEqual(fake.files, {})

    def test_remote_scope_rejects_app_partition(self):
        transport = sync.VitaFTP("vita", 1337, 1)
        with self.assertRaisesRegex(sync.SyncError, "escaped"):
            transport._remote("ux0:/app/ISAACR001/eboot.bin")

    def test_remote_scope_rejects_dotdot_and_sibling_prefix(self):
        transport = sync.VitaFTP("vita", 1337, 1)
        unsafe = (
            "/ux0:/data/isaacr001/../other/file",
            "/ux0:/data/isaacr001-other/file",
        )
        for path in unsafe:
            with self.subTest(path=path):
                with self.assertRaises(sync.SyncError):
                    transport._remote(path)

    def test_remote_directory_is_never_replaced_by_a_file(self):
        fake = FakeFTP()
        target = "/ux0:/data/isaacr001/mods/42/main.lua"
        fake.directories.update(
            {
                "/ux0:/data/isaacr001/mods",
                "/ux0:/data/isaacr001/mods/42",
                target,
            }
        )
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "main.lua"
            source.write_bytes(b"new")
            transfer = sync.Transfer(source, sync.PurePosixPath(target), "mod:42")
            transport = sync.VitaFTP("vita", 1337, 1)
            with self.assertRaisesRegex(sync.SyncError, "refusing to replace remote dir"):
                transport.upload_atomic(fake, transfer, "session", "token")
        self.assertEqual(fake.files, {})

    def test_ambiguous_install_is_preserved_and_old_file_is_restored(self):
        fake = FakeFTP()
        target = "/ux0:/data/isaacr001/mods/42/main.lua"
        fake.directories.update(
            {
                "/ux0:/data/isaacr001/mods",
                "/ux0:/data/isaacr001/mods/42",
            }
        )
        fake.files[target] = b"old"
        fake.ambiguous_install = True
        with tempfile.TemporaryDirectory() as directory:
            source = Path(directory) / "main.lua"
            source.write_bytes(b"new")
            transfer = sync.Transfer(source, sync.PurePosixPath(target), "mod:42")
            transport = sync.VitaFTP("vita", 1337, 1)
            with self.assertRaisesRegex(sync.SyncError, "upload failed"):
                transport.upload_atomic(fake, transfer, "session", "token")
        self.assertEqual(fake.files[target], b"old")
        self.assertEqual(fake.files[target + ".isaac-sync-token.failed"], b"new")


class PullTests(unittest.TestCase):
    @staticmethod
    def _fake_with_slots(*slots: int) -> FakeFTP:
        fake = FakeFTP()
        for slot in slots:
            remote = str(sync.REMOTE_SAVE / f"persistentgamedata{slot}.dat")
            fake.files[remote] = f"vita-save-{slot}".encode("ascii")
        return fake

    def test_pull_is_retr_only_and_writes_verified_complete_receipt(self):
        fake = self._fake_with_slots(1, 2, 3)
        transport = sync.VitaFTP("vita", 1337, 1, ftp_factory=lambda: fake)
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "new-backup"
            output.mkdir()
            receipt_path = output / "receipt.json"
            receipt = sync.pull_save_slots(
                transport,
                output,
                receipt_path,
                "session",
                datetime(2026, 8, 31, tzinfo=timezone.utc),
            )
            loaded = json.loads(receipt_path.read_text(encoding="utf-8"))
            for slot in (1, 2, 3):
                self.assertEqual(
                    (output / f"persistentgamedata{slot}.dat").read_bytes(),
                    f"vita-save-{slot}".encode("ascii"),
                )

        self.assertEqual(receipt, loaded)
        self.assertEqual(receipt["state"], "complete")
        self.assertEqual([record["slot"] for record in receipt["files"]], [1, 2, 3])
        self.assertEqual(receipt["missing"], [])
        self.assertEqual(
            fake.retr_commands,
            [f"RETR {remote}" for unused_slot, remote in sync.REMOTE_SAVE_SLOTS],
        )
        self.assertEqual(fake.mutation_commands, [])
        self.assertTrue(
            all(
                record["verified"]
                == "retr-sha256+fsync+atomic-no-replace+local-readback-sha256"
                for record in receipt["files"]
            )
        )

    def test_prepare_pull_output_creates_only_a_new_exclusive_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            destination = Path(directory) / "backup"
            prepared = sync.prepare_pull_output(destination, "session")
            self.assertEqual(prepared, destination.resolve())
            self.assertTrue(prepared.is_dir())
            with self.assertRaisesRegex(sync.SyncError, "refusing to reuse"):
                sync.prepare_pull_output(destination, "session")

    def test_prepare_pull_output_rejects_symlinked_parent(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            real_parent = root / "real"
            real_parent.mkdir()
            linked_parent = root / "linked"
            linked_parent.symlink_to(real_parent, target_is_directory=True)
            with self.assertRaisesRegex(sync.SyncError, "symlink/reparse"):
                sync.prepare_pull_output(linked_parent / "backup", "session")

    def test_default_pull_output_rejects_session_path_segments(self):
        with self.assertRaisesRegex(sync.SyncError, "unsafe backup session"):
            sync.default_pull_output_path("../escape")

    def test_missing_slots_are_recorded_without_blocking_present_slot(self):
        fake = self._fake_with_slots(2)
        transport = sync.VitaFTP("vita", 1337, 1, ftp_factory=lambda: fake)
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "new-backup"
            output.mkdir()
            receipt = sync.pull_save_slots(
                transport,
                output,
                output / "receipt.json",
                "session",
                datetime(2026, 8, 31, tzinfo=timezone.utc),
            )
        self.assertEqual([record["slot"] for record in receipt["files"]], [2])
        self.assertEqual([record["slot"] for record in receipt["missing"]], [1, 3])
        self.assertEqual(receipt["state"], "complete")
        self.assertEqual(fake.mutation_commands, [])

    def test_all_missing_slots_fail_but_leave_checkpoint_receipt(self):
        fake = self._fake_with_slots()
        transport = sync.VitaFTP("vita", 1337, 1, ftp_factory=lambda: fake)
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "new-backup"
            output.mkdir()
            receipt_path = output / "receipt.json"
            with self.assertRaisesRegex(sync.SyncError, "no persistent Vita save"):
                sync.pull_save_slots(
                    transport,
                    output,
                    receipt_path,
                    "session",
                    datetime(2026, 8, 31, tzinfo=timezone.utc),
                )
            receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
        self.assertEqual(receipt["state"], "failed")
        self.assertEqual(len(receipt["missing"]), 3)
        self.assertEqual(fake.mutation_commands, [])

    def test_failed_retr_cleans_owned_temp_and_records_failure(self):
        fake = self._fake_with_slots(1)
        fake.fail_retr_after_data = True
        transport = sync.VitaFTP("vita", 1337, 1, ftp_factory=lambda: fake)
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "new-backup"
            output.mkdir()
            receipt_path = output / "receipt.json"
            with self.assertRaisesRegex(sync.SyncError, "download failed"):
                sync.pull_save_slots(
                    transport,
                    output,
                    receipt_path,
                    "session",
                    datetime(2026, 8, 31, tzinfo=timezone.utc),
                )
            self.assertEqual(
                sorted(path.name for path in output.iterdir()), ["receipt.json"]
            )
            receipt = json.loads(receipt_path.read_text(encoding="utf-8"))
        self.assertEqual(receipt["state"], "failed")
        self.assertEqual(fake.mutation_commands, [])

    def test_existing_local_save_is_never_overwritten(self):
        fake = self._fake_with_slots(1)
        transport = sync.VitaFTP("vita", 1337, 1)
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            destination = output / "persistentgamedata1.dat"
            destination.write_bytes(b"keep-me")
            with self.assertRaisesRegex(sync.SyncError, "refusing to overwrite"):
                transport.download_save_atomic(
                    fake, sync.REMOTE_SAVE_SLOTS[0][1], output, "token"
                )
            self.assertEqual(destination.read_bytes(), b"keep-me")
        self.assertEqual(fake.retr_commands, [])

    def test_pull_rejects_any_remote_path_outside_exact_save_slots(self):
        fake = self._fake_with_slots()
        transport = sync.VitaFTP("vita", 1337, 1)
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(sync.SyncError, "not a persistent save slot"):
                transport.download_save_atomic(
                    fake, sync.REMOTE_MODS / "42/main.lua", Path(directory), "token"
                )
        self.assertEqual(fake.retr_commands, [])

    def test_pull_rejects_staging_token_with_path_separator(self):
        fake = self._fake_with_slots(1)
        transport = sync.VitaFTP("vita", 1337, 1)
        with tempfile.TemporaryDirectory() as directory:
            with self.assertRaisesRegex(sync.SyncError, "unsafe local staging token"):
                transport.download_save_atomic(
                    fake,
                    sync.REMOTE_SAVE_SLOTS[0][1],
                    Path(directory),
                    "../escape",
                )
        self.assertEqual(fake.retr_commands, [])

    def test_no_replace_promotion_keeps_preexisting_destination(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            temp = root / ".save.tmp"
            destination = root / "save.dat"
            temp.write_bytes(b"new")
            destination.write_bytes(b"old")
            with self.assertRaisesRegex(sync.SyncError, "refusing to overwrite"):
                sync._promote_no_replace(temp, destination)
            self.assertEqual(destination.read_bytes(), b"old")
            self.assertEqual(temp.read_bytes(), b"new")


if __name__ == "__main__":
    unittest.main()
