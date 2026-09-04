from __future__ import annotations

import importlib.util
from pathlib import Path
import sys
import tempfile
import unittest


HERE = Path(__file__).resolve().parent
SYNC_SPEC = importlib.util.spec_from_file_location(
    "isaac_vita_sync", HERE / "isaac_vita_sync.py"
)
assert SYNC_SPEC is not None and SYNC_SPEC.loader is not None
sync = importlib.util.module_from_spec(SYNC_SPEC)
sys.modules[SYNC_SPEC.name] = sync
SYNC_SPEC.loader.exec_module(sync)

GUI_SPEC = importlib.util.spec_from_file_location(
    "isaac_vita_sync_gui", HERE / "isaac_vita_sync_gui.py"
)
assert GUI_SPEC is not None and GUI_SPEC.loader is not None
gui = importlib.util.module_from_spec(GUI_SPEC)
sys.modules[GUI_SPEC.name] = gui
GUI_SPEC.loader.exec_module(gui)


class SyncGuiLogicTests(unittest.TestCase):
    def test_plan_command_is_read_only_and_deduplicates_mods(self) -> None:
        selection = gui.GuiSelection(True, True, ("836319872", "836319872"), "E:/Steam")
        command = gui.build_cli_command("plan", selection, port=1337)
        self.assertEqual(command[3], "plan")
        self.assertIn("--saves", command)
        self.assertIn("--latest-save-backups", command)
        self.assertEqual(command.count("--mod"), 1)
        self.assertNotIn("--yes", command)
        self.assertNotIn("--host", command)

    def test_sentinel_only_plan_needs_no_steam_selection(self) -> None:
        selection = gui.GuiSelection(False, False, (), lua_sentinel=True)
        command = gui.build_cli_command("plan", selection)
        self.assertIn("--lua-sentinel", command)
        self.assertNotIn("--steam-root", command)
        self.assertNotIn("--mod", command)

    def test_push_requires_host_and_receipt(self) -> None:
        selection = gui.GuiSelection(False, False, ("836319872",))
        with self.assertRaises(gui.GuiInputError):
            gui.build_cli_command("push", selection, receipt=Path("receipt.json"))
        with self.assertRaises(gui.GuiInputError):
            gui.build_cli_command("push", selection, host="192.168.1.5")

    def test_push_uses_exact_cli_confirmation_boundary(self) -> None:
        selection = gui.GuiSelection(False, False, ("836319872",))
        receipt = Path("C:/receipts/session.json")
        command = gui.build_cli_command(
            "push", selection, host="192.168.1.5", port=1337, receipt=receipt
        )
        self.assertEqual(command[3], "push")
        self.assertIn("--yes", command)
        self.assertEqual(command[command.index("--host") + 1], "192.168.1.5")
        self.assertEqual(command[command.index("--receipt") + 1], str(receipt))

    def test_pull_command_is_save_only_and_has_no_write_confirmation(self) -> None:
        command = gui.build_pull_command(" 192.168.1.5 ", 1337)
        self.assertEqual(command[3], "pull")
        self.assertIn("--saves", command)
        self.assertEqual(command[command.index("--host") + 1], "192.168.1.5")
        self.assertNotIn("--yes", command)
        self.assertNotIn("--mod", command)
        self.assertNotIn("--all-mods", command)

    def test_pull_requires_host_and_valid_port(self) -> None:
        with self.assertRaises(gui.GuiInputError):
            gui.build_pull_command("")
        with self.assertRaises(gui.GuiInputError):
            gui.build_pull_command("192.168.1.5", 0)

    def test_invalid_selection_is_rejected_before_cli(self) -> None:
        with self.assertRaises(gui.GuiInputError):
            gui.build_cli_command("plan", gui.GuiSelection(False, False, ()))
        with self.assertRaises(gui.GuiInputError):
            gui.build_cli_command("plan", gui.GuiSelection(False, False, ("not-an-id",)))
        with self.assertRaises(gui.GuiInputError):
            gui.build_cli_command("plan", gui.GuiSelection(True, False, ()), port=70000)

    def test_metadata_title_is_display_only_and_bounded(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            metadata = Path(directory) / "metadata.xml"
            metadata.write_text(
                "<metadata><name>  External\n Item  Descriptions  </name></metadata>",
                encoding="utf-8",
            )
            self.assertEqual(
                gui.metadata_title(metadata, "836319872"), "External Item Descriptions"
            )
            metadata.write_text("<broken", encoding="utf-8")
            self.assertEqual(
                gui.metadata_title(metadata, "836319872"), "Workshop mod 836319872"
            )


if __name__ == "__main__":
    unittest.main()
