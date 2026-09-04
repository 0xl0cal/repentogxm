#!/usr/bin/env python3
"""Host-only behavior oracle for virtual_pad_gameplay_soak.py."""

from __future__ import annotations

import importlib.util
import io
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stderr
from pathlib import Path


SCRIPT = Path(__file__).with_name("virtual_pad_gameplay_soak.py")
SPEC = importlib.util.spec_from_file_location("virtual_pad_gameplay_soak", SCRIPT)
if SPEC is None or SPEC.loader is None:
    raise RuntimeError(f"cannot import {SCRIPT}")
SOAK = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = SOAK
SPEC.loader.exec_module(SOAK)


def append(path: Path, line: str) -> None:
    with path.open("a", encoding="utf-8") as stream:
        stream.write(line + "\n")


class FakePad:
    def __init__(self, console: Path, game: Path, *, ack: bool = True,
                 emit_death: bool = True, emit_restart_room: bool = True) -> None:
        self.console = console
        self.game = game
        self.ack = ack
        self.emit_death = emit_death
        self.emit_restart_room = emit_restart_room
        self.a_releases = 0
        self.actions: list[str] = []
        self.death_emitted = False
        self.closed = False

    def get_index(self) -> int:
        return 7

    def press_a(self) -> None:
        self.actions.append("a-down")
        if self.ack:
            append(self.console, "[input] down=00004000 up=00000000")

    def release_a(self) -> None:
        self.actions.append("a-up")
        self.a_releases += 1
        if self.ack:
            append(self.console, "[input] down=00000000 up=00004000")
        if self.a_releases == SOAK.TITLE_NAVIGATION_PULSES:
            append(self.game, "[INFO] - Room 1.2(Start Room)")
        elif self.a_releases > SOAK.TITLE_NAVIGATION_PULSES:
            if self.emit_restart_room:
                append(self.game, "[INFO] - Room 1.2(Restarted)")

    def set_sticks(self, lx: float, ly: float,
                   rx: float, ry: float) -> None:
        self.actions.append(f"sticks={lx},{ly},{rx},{ry}")
        if self.emit_death and not self.death_emitted:
            self.death_emitted = True
            append(self.game, "[INFO] - Room 1.246(New Room)")
            append(self.game, "[INFO] - Game Over. fake fixture death")

    def neutral(self) -> None:
        self.actions.append("neutral")

    def close(self) -> None:
        self.actions.append("disconnect")
        self.closed = True


class SoakFixture(unittest.TestCase):
    @unittest.skipUnless(sys.platform == "win32", "Windows handle oracle")
    def test_windows_owner_handle_stays_bound_after_process_exit(self) -> None:
        flags = getattr(subprocess, "CREATE_NO_WINDOW", 0)
        process = subprocess.Popen(
            [sys.executable, "-c", "import time; time.sleep(30)"],
            creationflags=flags,
        )
        owner = None
        try:
            owner = SOAK.WindowsProcessLiveness(process.pid)
            self.assertTrue(owner.is_alive())
            process.terminate()
            process.wait(timeout=10)
            self.assertFalse(owner.is_alive())
            owner.close()
            self.assertFalse(owner.is_alive())
            owner.close()
        finally:
            if owner is not None:
                owner.close()
            if process.poll() is None:
                process.kill()
                process.wait(timeout=10)

    def make_runner(self, root: Path, pad: FakePad, **changes):
        console = root / "console.log"
        game = root / "game.log"
        trace = SOAK.Trace(root / "trace.log")
        counters = SOAK.Counters()
        values = dict(
            first_present_timeout=0.03,
            ack_timeout=0.03,
            menu_settle=0.0,
            menu_gap=0.0,
            gameplay_start_timeout=0.03,
            exercise_seconds=0.02,
            active_seconds=0.002,
            neutral_seconds=0.001,
            poll_interval=0.001,
            restart_room_timeout=0.02,
            restart_retry_gap=0.005,
            restart_max_attempts=1,
        )
        values.update(changes)
        runner = SOAK.SoakRunner(
            pad,
            SOAK.AppendedLineReader(console, start_at_end=False),
            SOAK.GameLog(SOAK.AppendedLineReader(game, start_at_end=True),
                         trace, counters),
            trace,
            SOAK.SoakConfig(**values),
        )
        return runner, counters, trace.path

    def test_four_pulses_death_restart_and_cleanup(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            console, game = root / "console.log", root / "game.log"
            append(console, SOAK.FIRST_PRESENT)
            # This old record must be ignored by the fresh game-log cursor.
            append(game, "[INFO] - Game Over. stale previous run")
            pad = FakePad(console, game)
            runner, counters, trace_path = self.make_runner(root, pad)

            result = runner.run()

            self.assertIs(result, counters)
            self.assertEqual(pad.a_releases, 5)
            self.assertEqual(counters.rooms, 3)
            self.assertEqual(counters.game_overs, 1)
            self.assertEqual(counters.restart_attempts, 1)
            self.assertEqual(counters.restarts, 1)
            self.assertGreater(counters.exercise_cycles, 0)
            self.assertTrue(pad.closed)
            self.assertEqual(pad.actions[-2:], ["neutral", "disconnect"])
            trace = trace_path.read_text(encoding="utf-8")
            self.assertIn("restart=1 game-over=1 confirmed-room=3 attempt=1", trace)
            self.assertIn("summary rooms=3 game-overs=1 restart-attempts=1", trace)

    def test_first_present_timeout_is_bounded_and_cleans_up(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            console, game = root / "console.log", root / "game.log"
            console.touch()
            game.touch()
            pad = FakePad(console, game, emit_death=False)
            runner, _, trace_path = self.make_runner(root, pad,
                                                      first_present_timeout=0.005)

            with self.assertRaisesRegex(TimeoutError, "first-present"):
                runner.run()

            self.assertEqual(pad.a_releases, 0)
            self.assertEqual(pad.actions[-2:], ["neutral", "disconnect"])
            self.assertIn("error type=TimeoutError", trace_path.read_text())

    def test_missing_ack_releases_a_then_cleans_up(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            console, game = root / "console.log", root / "game.log"
            append(console, SOAK.FIRST_PRESENT)
            game.touch()
            pad = FakePad(console, game, ack=False, emit_death=False)
            runner, _, _ = self.make_runner(root, pad, ack_timeout=0.005)

            with self.assertRaisesRegex(TimeoutError, "A-down guest ACK"):
                runner.run()

            self.assertEqual(pad.actions[:2], ["a-down", "a-up"])
            self.assertEqual(pad.actions[-2:], ["neutral", "disconnect"])

    def test_restart_room_timeout_is_bounded_and_cleans_up(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            console, game = root / "console.log", root / "game.log"
            append(console, SOAK.FIRST_PRESENT)
            game.touch()
            pad = FakePad(console, game, emit_restart_room=False)
            runner, counters, _ = self.make_runner(
                root, pad, restart_room_timeout=0.005,
                restart_max_attempts=1)

            with self.assertRaisesRegex(TimeoutError, "new Room"):
                runner.run()

            self.assertEqual(counters.game_overs, 1)
            self.assertEqual(counters.restart_attempts, 1)
            self.assertEqual(counters.restarts, 0)
            self.assertEqual(pad.actions[-2:], ["neutral", "disconnect"])

    def test_resume_gameplay_skips_menu_and_initial_room_wait(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            console, game = root / "console.log", root / "game.log"
            append(console, SOAK.FIRST_PRESENT)
            game.touch()
            pad = FakePad(console, game, emit_death=False)
            runner, counters, trace_path = self.make_runner(
                root, pad, resume_gameplay=True)

            result = runner.run()

            self.assertIs(result, counters)
            self.assertEqual(pad.a_releases, 0)
            self.assertGreater(counters.exercise_cycles, 0)
            self.assertTrue(any(action.startswith("sticks=")
                                for action in pad.actions))
            self.assertEqual(pad.actions[-2:], ["neutral", "disconnect"])
            trace = trace_path.read_text(encoding="utf-8")
            self.assertIn("gameplay-resume externally-confirmed rooms=0", trace)
            self.assertNotIn("a-pulse role=title-", trace)

    def test_owner_exit_stops_and_cleans_up_global_pad(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            console, game = root / "console.log", root / "game.log"
            append(console, SOAK.FIRST_PRESENT)
            game.touch()
            pad = FakePad(console, game, emit_death=False)
            checks = 0

            def owner_alive() -> bool:
                nonlocal checks
                checks += 1
                return checks < 8

            runner, _, trace_path = self.make_runner(
                root, pad, resume_gameplay=True, exercise_seconds=1.0)
            runner.owner_alive = owner_alive

            with self.assertRaisesRegex(SOAK.OwnerExited,
                                        "owner process exited"):
                runner.run()

            self.assertGreaterEqual(checks, 8)
            self.assertEqual(pad.actions[-2:], ["neutral", "disconnect"])
            trace = trace_path.read_text(encoding="utf-8")
            self.assertIn("error type=OwnerExited", trace)
            self.assertIn("neutral\n", trace)
            self.assertIn("disconnected", trace)

    def test_guest_terminal_marker_stops_and_cleans_up_global_pad(self) -> None:
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            console, game = root / "console.log", root / "game.log"
            append(console, SOAK.FIRST_PRESENT)
            game.touch()

            class TerminalPad(FakePad):
                def set_sticks(self, lx: float, ly: float,
                               rx: float, ry: float) -> None:
                    super().set_sticks(lx, ly, rx, ry)
                    append(self.console,
                           "KAGE VITA GUEST STOP: fixture terminal boundary")

            pad = TerminalPad(console, game, emit_death=False)
            runner, _, trace_path = self.make_runner(
                root, pad, resume_gameplay=True, exercise_seconds=1.0)

            with self.assertRaisesRegex(SOAK.GuestEnded,
                                        "KAGE VITA GUEST STOP"):
                runner.run()

            self.assertEqual(pad.actions[-2:], ["neutral", "disconnect"])
            trace = trace_path.read_text(encoding="utf-8")
            self.assertIn("error type=GuestEnded", trace)
            self.assertIn("neutral\n", trace)
            self.assertIn("disconnected", trace)

    def test_owner_pid_is_required_and_positive(self) -> None:
        parser = SOAK.build_parser()
        common = ["--console-log", "console.log",
                  "--game-log", "game.log",
                  "--trace", "trace.log"]
        with redirect_stderr(io.StringIO()):
            with self.assertRaises(SystemExit):
                parser.parse_args(common)
            with self.assertRaises(SystemExit):
                parser.parse_args(common + ["--owner-pid", "0"])
        parsed = parser.parse_args(common + ["--owner-pid", "123"])
        self.assertEqual(parsed.owner_pid, 123)


if __name__ == "__main__":
    unittest.main(verbosity=2)
