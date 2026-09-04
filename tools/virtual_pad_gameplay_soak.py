#!/usr/bin/env python3
"""Drive a Vita3K Isaac run through a ViGEm X360 pad.

The console log is used as the acknowledgement channel for every A press.  The
game log is tailed from its current end so records left by an older run cannot
trigger a room/death decision.  This module deliberately imports ``vgamepad``
only when the real adapter is constructed; the behavior oracle uses a fake pad.
"""

from __future__ import annotations

import argparse
import ctypes
import gc
import re
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Optional, Protocol


FIRST_PRESENT = "[kage-vita] first present complete"
A_DOWN_ACK = "down=00004000"
A_UP_ACK = "up=00004000"
GAME_OVER = "Game Over."
ROOM_RE = re.compile(r"\[INFO\]\s*-\s*Room\s+(.+?)\s*$")
TITLE_NAVIGATION_PULSES = 4
TERMINAL_CONSOLE_MARKERS = (
    "KAGE VITA GUEST STOP:",
    "KAGE BOUNDARY FAIL:",
    "KAGE VITA PROCESS EXIT HANDOFF:",
    "Game closed",
)
WATCHED_CONSOLE_FRAGMENTS = (FIRST_PRESENT, A_DOWN_ACK, A_UP_ACK)


class OwnerExited(RuntimeError):
    """The exact process object that owns this diagnostic run has exited."""


class GuestEnded(RuntimeError):
    """The current guest run reached a terminal console boundary."""


class WindowsProcessLiveness:
    """Hold a SYNCHRONIZE handle so PID reuse cannot extend pad ownership."""

    SYNCHRONIZE = 0x00100000
    PROCESS_QUERY_LIMITED_INFORMATION = 0x00001000
    WAIT_OBJECT_0 = 0x00000000
    WAIT_TIMEOUT = 0x00000102

    def __init__(self, pid: int) -> None:
        if sys.platform != "win32":
            raise RuntimeError("--owner-pid is supported only on Windows")
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.OpenProcess.argtypes = (
            ctypes.c_uint32,
            ctypes.c_int,
            ctypes.c_uint32,
        )
        kernel32.OpenProcess.restype = ctypes.c_void_p
        kernel32.WaitForSingleObject.argtypes = (ctypes.c_void_p,
                                                  ctypes.c_uint32)
        kernel32.WaitForSingleObject.restype = ctypes.c_uint32
        kernel32.CloseHandle.argtypes = (ctypes.c_void_p,)
        kernel32.CloseHandle.restype = ctypes.c_int
        handle = kernel32.OpenProcess(
            self.SYNCHRONIZE | self.PROCESS_QUERY_LIMITED_INFORMATION,
            0,
            pid,
        )
        if not handle:
            error = ctypes.get_last_error()
            raise OSError(error, f"OpenProcess({pid}) failed")
        self.pid = pid
        self._kernel32 = kernel32
        self._handle: Optional[int] = handle

    def is_alive(self) -> bool:
        if self._handle is None:
            return False
        result = self._kernel32.WaitForSingleObject(self._handle, 0)
        if result == self.WAIT_TIMEOUT:
            return True
        if result == self.WAIT_OBJECT_0:
            return False
        error = ctypes.get_last_error()
        raise OSError(error, f"WaitForSingleObject({self.pid}) failed")

    def close(self) -> None:
        if self._handle is not None:
            self._kernel32.CloseHandle(self._handle)
            self._handle = None


class Pad(Protocol):
    def get_index(self) -> int: ...
    def press_a(self) -> None: ...
    def release_a(self) -> None: ...
    def set_sticks(self, lx: float, ly: float, rx: float, ry: float) -> None: ...
    def neutral(self) -> None: ...
    def close(self) -> None: ...


class Trace:
    def __init__(self, path: Path) -> None:
        self.path = path
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("x", encoding="utf-8"):
            pass

    def write(self, message: str) -> None:
        with self.path.open("a", encoding="utf-8") as stream:
            stream.write(f"{time.time():.6f} {message}\n")


class AppendedLineReader:
    """Read complete lines while tolerating create, append, rotate and truncate."""

    def __init__(self, path: Path, *, start_at_end: bool) -> None:
        self.path = path
        self._offset = 0
        self._identity: Optional[tuple[int, int]] = None
        self._partial = b""
        try:
            stat = path.stat()
        except FileNotFoundError:
            return
        self._identity = (stat.st_dev, stat.st_ino)
        if start_at_end:
            self._offset = stat.st_size

    def read_lines(self) -> list[str]:
        try:
            stat = self.path.stat()
        except FileNotFoundError:
            return []

        identity = (stat.st_dev, stat.st_ino)
        if self._identity is None:
            self._identity = identity
        elif identity != self._identity or stat.st_size < self._offset:
            self._identity = identity
            self._offset = 0
            self._partial = b""

        with self.path.open("rb") as stream:
            stream.seek(self._offset)
            new_bytes = stream.read()
            self._offset = stream.tell()
        if not new_bytes:
            return []

        chunks = (self._partial + new_bytes).splitlines(keepends=True)
        self._partial = b""
        if chunks and not chunks[-1].endswith((b"\n", b"\r")):
            self._partial = chunks.pop()
        return [chunk.decode("utf-8", errors="replace").rstrip("\r\n")
                for chunk in chunks]


@dataclass
class Counters:
    rooms: int = 0
    game_overs: int = 0
    restart_attempts: int = 0
    restarts: int = 0
    exercise_cycles: int = 0


@dataclass(frozen=True)
class SoakConfig:
    first_present_timeout: float = 240.0
    ack_timeout: float = 3.0
    menu_settle: float = 5.0
    menu_gap: float = 4.0
    gameplay_start_timeout: float = 30.0
    exercise_seconds: float = 1800.0
    active_seconds: float = 0.85
    neutral_seconds: float = 0.15
    poll_interval: float = 0.05
    restart_room_timeout: float = 12.0
    restart_retry_gap: float = 2.0
    restart_max_attempts: int = 4
    resume_gameplay: bool = False


class GameLog:
    def __init__(self, reader: AppendedLineReader, trace: Trace,
                 counters: Counters) -> None:
        self.reader = reader
        self.trace = trace
        self.counters = counters

    def poll(self) -> None:
        for line in self.reader.read_lines():
            room = ROOM_RE.search(line)
            if room:
                self.counters.rooms += 1
                self.trace.write(
                    f"room={self.counters.rooms} label={room.group(1).strip()}")
            if GAME_OVER in line:
                self.counters.game_overs += 1
                self.trace.write(f"game-over={self.counters.game_overs}")


class SoakRunner:
    DIRECTIONS = (
        (0.0, 1.0, 1.0, 0.0),
        (1.0, 0.0, 0.0, -1.0),
        (0.0, -1.0, -1.0, 0.0),
        (-1.0, 0.0, 0.0, 1.0),
    )

    def __init__(self, pad: Pad, console: AppendedLineReader,
                 game: GameLog, trace: Trace, config: SoakConfig,
                 *, clock: Callable[[], float] = time.monotonic,
                 sleep: Callable[[float], None] = time.sleep,
                 owner_alive: Callable[[], bool] = lambda: True) -> None:
        self.pad = pad
        self.console = console
        self.game = game
        self.trace = trace
        self.config = config
        self.clock = clock
        self.sleep = sleep
        self.owner_alive = owner_alive
        self.counters = game.counters
        self._handled_game_overs = 0
        self._console_events: list[str] = []

    def _poll_console(self) -> None:
        for line in self.console.read_lines():
            marker = next((candidate for candidate in TERMINAL_CONSOLE_MARKERS
                           if candidate in line), None)
            if marker is not None:
                raise GuestEnded(f"guest run ended at {marker.rstrip(':')}")
            if any(fragment in line
                   for fragment in WATCHED_CONSOLE_FRAGMENTS):
                self._console_events.append(line)
        # ACK traffic is bounded in normal use, but a malformed producer must
        # not turn this safety helper into an unbounded log accumulator.
        if len(self._console_events) > 64:
            del self._console_events[:-64]

    def _consume_console(self, fragment: str) -> bool:
        self._poll_console()
        for index, line in enumerate(self._console_events):
            if fragment in line:
                del self._console_events[:index + 1]
                return True
        return False

    def _discard_console_events(self) -> None:
        self._poll_console()
        self._console_events.clear()

    def _require_owner(self) -> None:
        if not self.owner_alive():
            raise OwnerExited("owner process exited")
        self._poll_console()

    def _wait(self, predicate: Callable[[], bool], timeout: float,
              description: str) -> None:
        deadline = self.clock() + timeout
        while True:
            self._require_owner()
            if predicate():
                return
            remaining = deadline - self.clock()
            if remaining <= 0:
                raise TimeoutError(f"timeout waiting for {description}")
            self.sleep(min(self.config.poll_interval, remaining))

    def _wait_console(self, fragment: str, timeout: float,
                      description: str) -> None:
        self._wait(
            lambda: self._consume_console(fragment),
            timeout,
            description,
        )

    def _pulse_a(self, role: str) -> None:
        # Discard records that predate this physical transition.
        self._discard_console_events()
        self.pad.press_a()
        self.trace.write(f"a-pulse role={role} state=down")
        try:
            self._wait_console(A_DOWN_ACK, self.config.ack_timeout,
                               f"{role} A-down guest ACK")
            self.trace.write(f"a-pulse role={role} guest-down-ack")
        finally:
            self.pad.release_a()
            self.trace.write(f"a-pulse role={role} state=up")
        self._wait_console(A_UP_ACK, self.config.ack_timeout,
                           f"{role} A-up guest ACK")
        self.trace.write(f"a-pulse role={role} guest-up-ack")

    def _wait_for_room_after(self, baseline: int, timeout: float,
                             description: str) -> None:
        def observed() -> bool:
            self.game.poll()
            return self.counters.rooms > baseline

        self._wait(observed, timeout, description)

    def _new_game_over(self) -> bool:
        self.game.poll()
        return self.counters.game_overs > self._handled_game_overs

    def _restart_after_game_over(self) -> None:
        # One response per observed death, even if a log writes duplicate lines.
        self._handled_game_overs = self.counters.game_overs
        self.pad.neutral()
        baseline_rooms = self.counters.rooms
        death = self._handled_game_overs
        self.trace.write(f"restart-begin game-over={death} rooms={baseline_rooms}")

        for attempt in range(1, self.config.restart_max_attempts + 1):
            # Check whether a delayed room record arrived before another action.
            self.game.poll()
            if self.counters.rooms > baseline_rooms:
                self.counters.restarts += 1
                self.trace.write(
                    f"restart={self.counters.restarts} game-over={death} "
                    f"confirmed-before-attempt={attempt}")
                self._handled_game_overs = self.counters.game_overs
                return

            self.counters.restart_attempts += 1
            self.trace.write(
                f"restart-attempt={self.counters.restart_attempts} "
                f"game-over={death} local-attempt={attempt}")
            self._pulse_a(f"restart-{death}-{attempt}")
            try:
                self._wait_for_room_after(
                    baseline_rooms,
                    self.config.restart_room_timeout,
                    f"new Room after game-over {death}, attempt {attempt}",
                )
            except TimeoutError:
                self.trace.write(
                    f"restart-wait-timeout game-over={death} attempt={attempt}")
                if attempt == self.config.restart_max_attempts:
                    raise
                try:
                    self._wait_for_room_after(
                        baseline_rooms,
                        self.config.restart_retry_gap,
                        f"delayed Room after game-over {death}, attempt {attempt}",
                    )
                except TimeoutError:
                    continue

            self.counters.restarts += 1
            self.trace.write(
                f"restart={self.counters.restarts} game-over={death} "
                f"confirmed-room={self.counters.rooms} attempt={attempt}")
            self._handled_game_overs = self.counters.game_overs
            return

        raise AssertionError("restart attempt loop exhausted without a result")

    def _phase_saw_game_over(self, seconds: float,
                             exercise_deadline: float) -> bool:
        phase_deadline = min(exercise_deadline, self.clock() + seconds)
        while True:
            self._require_owner()
            if self._new_game_over():
                return True
            remaining = phase_deadline - self.clock()
            if remaining <= 0:
                return False
            self.sleep(min(self.config.poll_interval, remaining))

    def _run_body(self) -> Counters:
        self._wait_console(FIRST_PRESENT, self.config.first_present_timeout,
                           "current-run first-present marker")
        self.trace.write("observed first-present")
        if self.config.resume_gameplay:
            # This is an explicit recovery seam for an already-running room.
            # The caller must independently capture/verify the emulator window;
            # no menu input is issued and the trace makes the bypass visible.
            self.game.poll()
            self.trace.write(
                f"gameplay-resume externally-confirmed rooms={self.counters.rooms}")
        else:
            if self.config.menu_settle:
                self.sleep(self.config.menu_settle)

            initial_rooms = self.counters.rooms
            for ordinal in range(1, TITLE_NAVIGATION_PULSES + 1):
                self._pulse_a(f"title-{ordinal}")
                if ordinal != TITLE_NAVIGATION_PULSES and self.config.menu_gap:
                    self.sleep(self.config.menu_gap)

            self._wait_for_room_after(
                initial_rooms,
                self.config.gameplay_start_timeout,
                "first gameplay Room after four title pulses",
            )
            self.trace.write(f"gameplay-confirmed room={self.counters.rooms}")

        exercise_deadline = self.clock() + self.config.exercise_seconds
        self.trace.write("exercise-start")
        while self.clock() < exercise_deadline:
            self._require_owner()
            if self._new_game_over():
                self._restart_after_game_over()
                continue

            lx, ly, rx, ry = self.DIRECTIONS[
                self.counters.exercise_cycles % len(self.DIRECTIONS)]
            self.pad.set_sticks(lx, ly, rx, ry)
            cycle = self.counters.exercise_cycles
            if cycle < 8 or cycle % 32 == 0:
                self.trace.write(
                    f"exercise-cycle={cycle} left={lx},{ly} right={rx},{ry} "
                    f"rooms={self.counters.rooms} "
                    f"game-overs={self.counters.game_overs} "
                    f"restarts={self.counters.restarts}")
            if self._phase_saw_game_over(self.config.active_seconds,
                                         exercise_deadline):
                self.pad.neutral()
                self._restart_after_game_over()
                continue

            self.pad.neutral()
            if self._phase_saw_game_over(self.config.neutral_seconds,
                                         exercise_deadline):
                self._restart_after_game_over()
            self.counters.exercise_cycles += 1

        self.game.poll()
        self.trace.write(f"exercise-end cycles={self.counters.exercise_cycles}")
        return self.counters

    def run(self) -> Counters:
        failed = False
        try:
            return self._run_body()
        except BaseException as error:
            failed = True
            self.trace.write(f"error type={type(error).__name__} message={error}")
            raise
        finally:
            cleanup_errors: list[BaseException] = []
            try:
                self.pad.neutral()
                self.trace.write("neutral")
            except BaseException as error:
                cleanup_errors.append(error)
                self.trace.write(
                    f"cleanup-error phase=neutral type={type(error).__name__}")
            try:
                self.pad.close()
                self.trace.write("disconnected")
            except BaseException as error:
                cleanup_errors.append(error)
                self.trace.write(
                    f"cleanup-error phase=disconnect type={type(error).__name__}")
            self.trace.write(
                "summary "
                f"rooms={self.counters.rooms} "
                f"game-overs={self.counters.game_overs} "
                f"restart-attempts={self.counters.restart_attempts} "
                f"restarts={self.counters.restarts} "
                f"cycles={self.counters.exercise_cycles}")
            if cleanup_errors and not failed:
                raise RuntimeError("pad cleanup failed") from cleanup_errors[0]


class ViGEmPad:
    def __init__(self) -> None:
        try:
            import vgamepad as vg
        except ImportError as error:
            raise RuntimeError(
                "vgamepad is unavailable; set PYTHONPATH to the pinned "
                "vgamepad source used by the Vita3K tooling") from error
        self._vg = vg
        self._raw = vg.VX360Gamepad()

    def get_index(self) -> int:
        return int(self._raw.get_index())

    def press_a(self) -> None:
        self._raw.press_button(
            button=self._vg.XUSB_BUTTON.XUSB_GAMEPAD_A)
        self._raw.update()

    def release_a(self) -> None:
        self._raw.release_button(
            button=self._vg.XUSB_BUTTON.XUSB_GAMEPAD_A)
        self._raw.update()

    def set_sticks(self, lx: float, ly: float,
                   rx: float, ry: float) -> None:
        self._raw.left_joystick_float(x_value_float=lx, y_value_float=ly)
        self._raw.right_joystick_float(x_value_float=rx, y_value_float=ry)
        self._raw.update()

    def neutral(self) -> None:
        if self._raw is None:
            return
        self._raw.reset()
        self._raw.update()

    def close(self) -> None:
        raw, self._raw = self._raw, None
        if raw is None:
            return
        close = getattr(raw, "close", None)
        if callable(close):
            close()
        del raw
        # vgamepad 0.1.0 disconnects the target in VGamepad.__del__.
        gc.collect()


def positive_float(value: str) -> float:
    result = float(value)
    if result <= 0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return result


def nonnegative_float(value: str) -> float:
    result = float(value)
    if result < 0:
        raise argparse.ArgumentTypeError("must not be negative")
    return result


def positive_int(value: str) -> int:
    result = int(value, 10)
    if result <= 0:
        raise argparse.ArgumentTypeError("must be greater than zero")
    return result


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--console-log", required=True, type=Path,
                        help="fresh per-run Vita3K stdout log")
    parser.add_argument("--game-log", required=True, type=Path,
                        help="live Isaac log.txt; existing bytes are ignored")
    parser.add_argument("--trace", required=True, type=Path,
                        help="new trace path (must not already exist)")
    parser.add_argument(
        "--owner-pid",
        required=True,
        type=positive_int,
        help=("exact supervising-process PID; an open handle bounds all global "
              "ViGEm input and prevents PID reuse from extending the run; "
              "prefer the supervising diagnostic-runner PID when available"),
    )
    parser.add_argument("--first-present-timeout", type=positive_float,
                        default=240.0)
    parser.add_argument("--ack-timeout", type=positive_float, default=3.0)
    parser.add_argument("--menu-settle", type=nonnegative_float, default=5.0)
    parser.add_argument("--menu-gap", type=nonnegative_float, default=4.0)
    parser.add_argument("--gameplay-start-timeout", type=positive_float,
                        default=30.0)
    parser.add_argument("--exercise-seconds", type=positive_float,
                        default=1800.0)
    parser.add_argument("--active-seconds", type=positive_float, default=0.85)
    parser.add_argument("--neutral-seconds", type=nonnegative_float,
                        default=0.15)
    parser.add_argument("--poll-interval", type=positive_float, default=0.05)
    parser.add_argument("--restart-room-timeout", type=positive_float,
                        default=12.0)
    parser.add_argument("--restart-retry-gap", type=positive_float,
                        default=2.0)
    parser.add_argument("--restart-max-attempts", type=int, default=4)
    parser.add_argument(
        "--resume-gameplay",
        action="store_true",
        help=("skip title/menu pulses and the initial Room-log wait because "
              "the current emulator window was independently verified to be "
              "inside gameplay"),
    )
    return parser


def main(argv: Optional[list[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    if args.restart_max_attempts <= 0:
        raise SystemExit("--restart-max-attempts must be greater than zero")

    trace = Trace(args.trace)
    try:
        owner = WindowsProcessLiveness(args.owner_pid)
        if not owner.is_alive():
            raise OwnerExited("owner process already exited")
    except BaseException as error:
        trace.write(
            f"owner-error pid={args.owner_pid} "
            f"type={type(error).__name__} message={error}")
        print(error, file=sys.stderr)
        return 1
    trace.write(f"owner-bound pid={args.owner_pid}")
    try:
        pad = ViGEmPad()
    except BaseException as error:
        owner.close()
        trace.write(f"connect-error type={type(error).__name__} message={error}")
        print(error, file=sys.stderr)
        return 1

    trace.write(f"connected index={pad.get_index()}")
    counters = Counters()
    runner = SoakRunner(
        pad,
        AppendedLineReader(args.console_log, start_at_end=False),
        GameLog(AppendedLineReader(args.game_log, start_at_end=True),
                trace, counters),
        trace,
        SoakConfig(
            first_present_timeout=args.first_present_timeout,
            ack_timeout=args.ack_timeout,
            menu_settle=args.menu_settle,
            menu_gap=args.menu_gap,
            gameplay_start_timeout=args.gameplay_start_timeout,
            exercise_seconds=args.exercise_seconds,
            active_seconds=args.active_seconds,
            neutral_seconds=args.neutral_seconds,
            poll_interval=args.poll_interval,
            restart_room_timeout=args.restart_room_timeout,
            restart_retry_gap=args.restart_retry_gap,
            restart_max_attempts=args.restart_max_attempts,
            resume_gameplay=args.resume_gameplay,
        ),
        owner_alive=owner.is_alive,
    )
    try:
        runner.run()
    except OwnerExited as error:
        print(error, file=sys.stderr)
        return 3
    except GuestEnded as error:
        print(error, file=sys.stderr)
        return 3
    except TimeoutError as error:
        print(error, file=sys.stderr)
        return 2
    except BaseException as error:
        print(f"virtual-pad soak failed: {error}", file=sys.stderr)
        return 1
    finally:
        owner.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
