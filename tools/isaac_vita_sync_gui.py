#!/usr/bin/env python3
"""Small Windows UI for the audited Isaac PC/Vita sync CLI.

The window deliberately delegates discovery and every write to
``isaac_vita_sync.py``.  It first runs the CLI's read-only plan command, shows
that exact output, and only starts ``push --yes`` after explicit confirmation.
The separate save-backup action invokes the CLI's RETR-only pull contract.
"""

from __future__ import annotations

from dataclasses import dataclass, replace
from datetime import datetime, timezone
import os
from pathlib import Path
import queue
import subprocess
import sys
import threading
import uuid
import xml.etree.ElementTree as ET

import isaac_vita_sync as sync
import isaac_workshop_preview as workshop


SCRIPT = Path(__file__).with_name("isaac_vita_sync.py").resolve()


class GuiInputError(ValueError):
    pass


@dataclass(frozen=True)
class GuiMod:
    workshop_id: str
    title: str
    path: Path
    preview_url: str = ""
    preview_path: Path | None = None

    @property
    def label(self) -> str:
        return f"{self.title}  [{self.workshop_id}]"


@dataclass(frozen=True)
class GuiSelection:
    saves: bool
    latest_save_backups: bool
    mods: tuple[str, ...]
    steam_root: str = ""
    lua_sentinel: bool = False


def metadata_title(path: Path, workshop_id: str) -> str:
    """Return a compact display title without trusting metadata as a path."""

    try:
        root = ET.parse(path).getroot()
        node = root.find("name")
        if node is None:
            node = root.find("title")
        value = "" if node is None or node.text is None else " ".join(node.text.split())
    except (ET.ParseError, OSError):
        value = ""
    value = "".join(character for character in value if character >= " " and character != "\x7f")
    return value[:90] or f"Workshop mod {workshop_id}"


def discover_gui_mods(explicit_root: str = "") -> list[GuiMod]:
    roots = [Path(explicit_root)] if explicit_root.strip() else []
    libraries = sync.steam_libraries(roots)
    found = sync.discover_workshop_mods(libraries)
    cached, _online = workshop.refresh_workshop_metadata(found)
    result: list[GuiMod] = []
    for item, root in sorted(found.items(), key=lambda pair: int(pair[0])):
        entry = cached.get(item)
        result.append(
            GuiMod(
                item,
                entry.title if entry else metadata_title(root / "metadata.xml", item),
                root,
                "" if entry is None else entry.preview_url,
                None if entry is None else entry.preview_path,
            )
        )
    return result


def build_cli_command(
    action: str,
    selection: GuiSelection,
    *,
    host: str = "",
    port: int = 1337,
    receipt: Path | None = None,
) -> list[str]:
    if action not in ("plan", "push"):
        raise GuiInputError(f"unsupported action: {action}")
    if not selection.saves and not selection.mods and not selection.lua_sentinel:
        raise GuiInputError(
            "select PC saves, at least one Workshop mod, or the Lua sentinel"
        )
    if port < 1 or port > 65535:
        raise GuiInputError("FTP port must be between 1 and 65535")
    if action == "push" and not host.strip():
        raise GuiInputError("enter the IP address shown by the Vita Manager")

    command = [sys.executable, "-u", str(SCRIPT), action]
    if selection.saves:
        command.append("--saves")
        if selection.latest_save_backups:
            command.append("--latest-save-backups")
    for item in sorted(set(selection.mods)):
        if not item.isdecimal():
            raise GuiInputError(f"invalid Workshop ID: {item}")
        command.extend(("--mod", item))
    if selection.steam_root.strip():
        command.extend(("--steam-root", selection.steam_root.strip()))
    if selection.lua_sentinel:
        command.append("--lua-sentinel")
    if action == "push":
        if receipt is None:
            raise GuiInputError("push requires a receipt path")
        command.extend(
            (
                "--host",
                host.strip(),
                "--port",
                str(port),
                "--yes",
                "--receipt",
                str(receipt),
            )
        )
    return command


def build_pull_command(host: str, port: int = 1337) -> list[str]:
    if not host.strip():
        raise GuiInputError("enter the IP address shown by the Vita Manager")
    if port < 1 or port > 65535:
        raise GuiInputError("FTP port must be between 1 and 65535")
    return [
        sys.executable,
        "-u",
        str(SCRIPT),
        "pull",
        "--saves",
        "--host",
        host.strip(),
        "--port",
        str(port),
    ]


def default_receipt_path() -> Path:
    root = Path.home() / "Documents" / "Isaac Vita Sync Receipts"
    stamp = datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")
    return root / f"isaac-vita-sync-{stamp}-{uuid.uuid4().hex[:8]}.json"


class SyncWindow:
    def __init__(self, root, tk, ttk, messagebox) -> None:
        self.root = root
        self.tk = tk
        self.ttk = ttk
        self.messagebox = messagebox
        self.events: queue.Queue[tuple] = queue.Queue()
        self.mods: list[GuiMod] = []
        self.busy = False
        self.preview_photo = None
        self.preview_loading: set[str] = set()
        self.preview_failed: set[str] = set()

        root.title("Isaac Vita Sync")
        root.geometry("920x680")
        root.minsize(760, 560)

        self.host = tk.StringVar(value=os.environ.get("ISAAC_VITA_HOST", ""))
        self.port = tk.StringVar(value="1337")
        self.steam_root = tk.StringVar(value="")
        self.saves = tk.BooleanVar(value=True)
        self.latest = tk.BooleanVar(value=True)
        self.lua_sentinel = tk.BooleanVar(value=False)
        self.status = tk.StringVar(value="Ready")

        outer = ttk.Frame(root, padding=12)
        outer.pack(fill="both", expand=True)
        connection = ttk.LabelFrame(outer, text="Vita Manager FTP", padding=8)
        connection.pack(fill="x")
        ttk.Label(connection, text="IP address").grid(row=0, column=0, sticky="w")
        ttk.Entry(connection, textvariable=self.host, width=28).grid(
            row=0, column=1, sticky="ew", padx=(8, 18)
        )
        ttk.Label(connection, text="Port").grid(row=0, column=2, sticky="w")
        ttk.Entry(connection, textvariable=self.port, width=8).grid(
            row=0, column=3, sticky="w", padx=(8, 0)
        )
        connection.columnconfigure(1, weight=1)
        ttk.Label(
            connection,
            text="Open SAVE / MOD MANAGER on Vita, press Circle, then enter the shown IP.",
        ).grid(row=1, column=0, columnspan=4, sticky="w", pady=(7, 0))

        source = ttk.LabelFrame(outer, text="PC content", padding=8)
        source.pack(fill="both", expand=True, pady=(10, 0))
        ttk.Label(source, text="Steam root (optional)").grid(row=0, column=0, sticky="w")
        ttk.Entry(source, textvariable=self.steam_root).grid(
            row=0, column=1, sticky="ew", padx=(8, 8)
        )
        self.refresh_button = ttk.Button(source, text="Refresh", command=self.refresh)
        self.refresh_button.grid(row=0, column=2, sticky="e")
        source.columnconfigure(1, weight=1)

        ttk.Checkbutton(source, text="Copy active PC save slots", variable=self.saves).grid(
            row=1, column=0, columnspan=2, sticky="w", pady=(8, 0)
        )
        ttk.Checkbutton(
            source,
            text="Use newest dated backup when an active save slot is missing",
            variable=self.latest,
        ).grid(row=2, column=0, columnspan=2, sticky="w")
        ttk.Checkbutton(
            source,
            text="Install repentogxm Lua/callback sentinel mod",
            variable=self.lua_sentinel,
        ).grid(row=3, column=0, columnspan=2, sticky="w")
        ttk.Label(source, text="Downloaded Workshop mods (Ctrl/Shift selects several)").grid(
            row=4, column=0, columnspan=3, sticky="w", pady=(10, 3)
        )
        list_frame = ttk.Frame(source)
        list_frame.grid(row=5, column=0, columnspan=3, sticky="nsew")
        self.mod_list = tk.Listbox(list_frame, selectmode="extended", exportselection=False)
        scrollbar = ttk.Scrollbar(list_frame, orient="vertical", command=self.mod_list.yview)
        self.mod_list.configure(yscrollcommand=scrollbar.set)
        preview_panel = ttk.Frame(list_frame, width=250, padding=(12, 0, 0, 0))
        preview_panel.pack(side="right", fill="y")
        preview_panel.pack_propagate(False)
        self.preview_image = ttk.Label(
            preview_panel,
            text="Select one mod to see its Steam Workshop preview.",
            anchor="center",
            justify="center",
            wraplength=225,
        )
        self.preview_image.pack(fill="both", expand=True)
        self.preview_title = ttk.Label(
            preview_panel, text="", anchor="center", justify="center", wraplength=225
        )
        self.preview_title.pack(fill="x", pady=(8, 0))
        scrollbar.pack(side="right", fill="y")
        self.mod_list.pack(side="left", fill="both", expand=True)
        self.mod_list.bind("<<ListboxSelect>>", self.show_selected_preview)
        source.rowconfigure(5, weight=1)

        buttons = ttk.Frame(outer)
        buttons.pack(fill="x", pady=(10, 0))
        self.plan_button = ttk.Button(buttons, text="Show safe plan", command=self.plan)
        self.plan_button.pack(side="left")
        self.send_button = ttk.Button(
            buttons, text="Plan and send to Vita", command=self.prepare_push
        )
        self.send_button.pack(side="left", padx=(8, 0))
        self.backup_button = ttk.Button(
            buttons, text="Back up Vita saves to PC", command=self.pull_saves
        )
        self.backup_button.pack(side="left", padx=(8, 0))
        ttk.Label(buttons, textvariable=self.status).pack(side="right")

        self.output = tk.Text(outer, height=12, wrap="word", state="disabled")
        self.output.pack(fill="both", expand=True, pady=(10, 0))
        root.protocol("WM_DELETE_WINDOW", self.close)
        root.after(80, self.poll)
        root.after(100, self.refresh)

    def write(self, message: str, *, clear: bool = False) -> None:
        self.output.configure(state="normal")
        if clear:
            self.output.delete("1.0", "end")
        self.output.insert("end", message)
        self.output.see("end")
        self.output.configure(state="disabled")

    def set_busy(self, value: bool, status: str) -> None:
        self.busy = value
        state = "disabled" if value else "normal"
        self.refresh_button.configure(state=state)
        self.plan_button.configure(state=state)
        self.send_button.configure(state=state)
        self.backup_button.configure(state=state)
        self.status.set(status)

    def selection(self) -> GuiSelection:
        selected = tuple(self.mods[index].workshop_id for index in self.mod_list.curselection())
        return GuiSelection(
            saves=bool(self.saves.get()),
            latest_save_backups=bool(self.latest.get()),
            mods=selected,
            steam_root=self.steam_root.get(),
            lua_sentinel=bool(self.lua_sentinel.get()),
        )

    def parsed_port(self) -> int:
        try:
            return int(self.port.get(), 10)
        except ValueError as exc:
            raise GuiInputError("FTP port must be a number") from exc

    def refresh(self) -> None:
        if self.busy:
            return
        self.set_busy(True, "Discovering Steam mods...")
        explicit = self.steam_root.get()

        def worker() -> None:
            try:
                mods = discover_gui_mods(explicit)
                self.events.put(("mods", mods))
            except Exception as exc:  # surfaced verbatim in the UI
                self.events.put(("error", str(exc)))

        threading.Thread(target=worker, daemon=True).start()

    def _selected_mod(self) -> GuiMod | None:
        selected = self.mod_list.curselection()
        if len(selected) != 1:
            return None
        index = int(selected[0])
        return self.mods[index] if 0 <= index < len(self.mods) else None

    def _render_preview(self, item: GuiMod) -> None:
        self.preview_photo = None
        self.preview_title.configure(text=item.label)
        if item.preview_path is None:
            self.preview_image.configure(image="", text="Preview unavailable offline.")
            return
        try:
            from PIL import Image, ImageTk
        except ImportError:
            self.preview_image.configure(
                image="", text="Preview cached, but Pillow is not installed."
            )
            return
        try:
            with Image.open(item.preview_path) as source:
                width, height = source.size
                if (
                    width < 1
                    or height < 1
                    or width > 4096
                    or height > 4096
                    or width * height > 4_000_000
                ):
                    raise ValueError("preview dimensions exceed safety limit")
                image = source.convert("RGBA")
                image.thumbnail((225, 180), Image.Resampling.LANCZOS)
        except (OSError, ValueError, Image.DecompressionBombError):
            try:
                workshop.invalidate_cached_preview(
                    item.workshop_id, expected_path=item.preview_path
                )
            except (OSError, workshop.WorkshopPreviewError):
                pass
            self.preview_failed.add(item.workshop_id)
            for index, candidate in enumerate(self.mods):
                if candidate.workshop_id == item.workshop_id:
                    self.mods[index] = replace(candidate, preview_path=None)
                    break
            self.preview_image.configure(
                image="", text="The cached preview image is invalid or too large."
            )
            return
        try:
            self.preview_photo = ImageTk.PhotoImage(image)
            self.preview_image.configure(image=self.preview_photo, text="")
        except self.tk.TclError:
            self.preview_image.configure(
                image="", text="The preview could not be displayed by this desktop."
            )

    def show_selected_preview(self, unused_event=None) -> None:
        item = self._selected_mod()
        if item is None:
            self.preview_photo = None
            self.preview_title.configure(text="")
            self.preview_image.configure(
                image="", text="Select one mod to see its Steam Workshop preview."
            )
            return
        if item.preview_path is not None:
            self._render_preview(item)
            return
        self.preview_title.configure(text=item.label)
        if not item.preview_url or item.workshop_id in self.preview_failed:
            self.preview_image.configure(image="", text="Preview unavailable offline.")
            return
        self.preview_image.configure(image="", text="Loading Steam Workshop preview...")
        if item.workshop_id in self.preview_loading:
            return
        self.preview_loading.add(item.workshop_id)

        def worker() -> None:
            try:
                entry = workshop.fetch_and_cache_preview(item.workshop_id)
                self.events.put(("preview", item.workshop_id, entry))
            except (OSError, workshop.WorkshopPreviewError) as exc:
                self.events.put(("preview_error", item.workshop_id, str(exc)))

        threading.Thread(target=worker, daemon=True).start()

    def start_command(
        self,
        command: list[str],
        purpose: str,
        selection: GuiSelection | None,
    ) -> None:
        status = {
            "push": "Transferring...",
            "pull": "Backing up saves...",
        }.get(purpose, "Planning...")
        self.set_busy(True, status)
        self.write("$ " + subprocess.list2cmdline(command) + "\n", clear=True)

        def worker() -> None:
            creationflags = subprocess.CREATE_NO_WINDOW if sys.platform == "win32" else 0
            lines: list[str] = []
            try:
                process = subprocess.Popen(
                    command,
                    cwd=SCRIPT.parent.parent,
                    stdout=subprocess.PIPE,
                    stderr=subprocess.STDOUT,
                    text=True,
                    encoding="utf-8",
                    errors="replace",
                    bufsize=1,
                    creationflags=creationflags,
                )
                assert process.stdout is not None
                for line in process.stdout:
                    lines.append(line)
                    self.events.put(("line", line))
                returncode = process.wait()
            except OSError as exc:
                self.events.put(("error", str(exc)))
                return
            self.events.put(("command_done", purpose, returncode, "".join(lines), selection))

        threading.Thread(target=worker, daemon=True).start()

    def plan(self) -> None:
        try:
            selection = self.selection()
            command = build_cli_command("plan", selection, port=self.parsed_port())
        except GuiInputError as exc:
            self.messagebox.showerror("Isaac Vita Sync", str(exc))
            return
        self.start_command(command, "plan", selection)

    def prepare_push(self) -> None:
        try:
            selection = self.selection()
            port = self.parsed_port()
            command = build_cli_command("plan", selection, port=port)
            if not self.host.get().strip():
                raise GuiInputError("enter the IP address shown by the Vita Manager")
        except GuiInputError as exc:
            self.messagebox.showerror("Isaac Vita Sync", str(exc))
            return
        self.start_command(command, "confirm_push", selection)

    def pull_saves(self) -> None:
        try:
            command = build_pull_command(self.host.get(), self.parsed_port())
        except GuiInputError as exc:
            self.messagebox.showerror("Isaac Vita Sync", str(exc))
            return
        self.start_command(command, "pull", None)

    def confirm_push(self, plan: str, selection: GuiSelection) -> None:
        if not self.messagebox.askyesno(
            "Write to Vita?",
            "The read-only plan succeeded. Transfer these files now?\n\n" + plan[-3000:],
        ):
            self.status.set("Cancelled before writing")
            return
        receipt = default_receipt_path()
        try:
            receipt.parent.mkdir(parents=True, exist_ok=True)
            command = build_cli_command(
                "push",
                selection,
                host=self.host.get(),
                port=self.parsed_port(),
                receipt=receipt,
            )
        except (GuiInputError, OSError) as exc:
            self.messagebox.showerror("Isaac Vita Sync", str(exc))
            return
        self.start_command(command, "push", selection)

    def poll(self) -> None:
        try:
            while True:
                event = self.events.get_nowait()
                if event[0] == "mods":
                    self.mods = event[1]
                    self.preview_loading.clear()
                    self.preview_failed.clear()
                    self.mod_list.delete(0, "end")
                    for item in self.mods:
                        self.mod_list.insert("end", item.label)
                    self.set_busy(False, f"Found {len(self.mods)} Workshop mods")
                    self.show_selected_preview()
                elif event[0] == "preview":
                    unused, item_id, entry = event
                    self.preview_loading.discard(item_id)
                    for index, item in enumerate(self.mods):
                        if item.workshop_id == item_id:
                            selected_before = index in self.mod_list.curselection()
                            self.mods[index] = replace(
                                item,
                                title=entry.title,
                                preview_url=entry.preview_url,
                                preview_path=entry.preview_path,
                            )
                            self.mod_list.delete(index)
                            self.mod_list.insert(index, self.mods[index].label)
                            if selected_before:
                                self.mod_list.selection_set(index)
                            break
                    selected = self._selected_mod()
                    if selected is not None and selected.workshop_id == item_id:
                        self._render_preview(selected)
                elif event[0] == "preview_error":
                    unused, item_id, unused_message = event
                    self.preview_loading.discard(item_id)
                    self.preview_failed.add(item_id)
                    selected = self._selected_mod()
                    if selected is not None and selected.workshop_id == item_id:
                        self.preview_image.configure(
                            image="", text="Preview unavailable offline."
                        )
                elif event[0] == "line":
                    self.write(event[1])
                elif event[0] == "error":
                    self.set_busy(False, "Failed")
                    self.messagebox.showerror("Isaac Vita Sync", event[1])
                elif event[0] == "command_done":
                    unused, purpose, returncode, output, selection = event
                    self.set_busy(False, "Complete" if returncode == 0 else "Failed")
                    if returncode != 0:
                        self.messagebox.showerror(
                            "Isaac Vita Sync", f"Command failed with exit code {returncode}."
                        )
                    elif purpose == "confirm_push":
                        self.confirm_push(output, selection)
                    elif purpose == "push":
                        self.messagebox.showinfo(
                            "Isaac Vita Sync",
                            "Transfer complete. Stop FTP on the Vita before returning to the game.",
                        )
                    elif purpose == "pull":
                        self.messagebox.showinfo(
                            "Isaac Vita Sync",
                            "Save backup complete. The Vita was not modified. "
                            "The backup folder and SHA-256 receipt are shown in the log.",
                        )
        except queue.Empty:
            pass
        self.root.after(80, self.poll)

    def close(self) -> None:
        if self.busy:
            self.messagebox.showwarning(
                "Isaac Vita Sync", "Wait for the current plan or transfer to finish."
            )
            return
        self.root.destroy()


def main() -> int:
    try:
        import tkinter as tk
        from tkinter import messagebox, ttk
    except ImportError as exc:
        print(f"error: Python Tk support is unavailable: {exc}", file=sys.stderr)
        return 2
    root = tk.Tk()
    SyncWindow(root, tk, ttk, messagebox)
    root.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
