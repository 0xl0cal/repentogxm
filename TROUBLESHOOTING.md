# Troubleshooting on a PS Vita

## Before you change anything

Collect these four things first. Without them a report cannot be checked.

- `ux0:data/isaacr001/first-arm-fault.log` and
  `ux0:data/isaacr001/first-arm-fault.prev.log` (the port's own log; send
  both). Each launch renames the log of the launch before it to `.prev.log`
  and starts a fresh `first-arm-fault.log`. So after a crash and one
  relaunch, the crash is in `.prev.log` and `first-arm-fault.log` describes
  the current launch; a second relaunch deletes the crash log. Copy both
  files before launching again. The second line of the log,
  `KAGE VITA LOG ROTATE: rotate=ok|none|rename_fail:0x...`, says whether the
  rename worked (`none` = there was no log to keep, first launch).
- `ux0:data/isaacr001/Documents/My Games/Binding of Isaac Repentance/log.txt`
  (the game's own log)
- the SHA-256 of `ux0:app/ISAACR001/eboot.bin` (called "eboot hash" below;
  copy the file to a PC and run `certutil -hashfile eboot.bin SHA256` on
  Windows or `sha256sum eboot.bin` elsewhere)
- what you were doing: room, items, menu action, how long you had been playing

A log from an older eboot says nothing about a newer build.

On builds up to v0.1.1-alpha every launch empties `first-arm-fault.log` and
starts it again. After a crash, copy both logs to a PC before you launch the
game again.

## Black screen right after launch

What it means: the app did not reach the loading screen.

What to do:

1. Check that `ur0:data/libshacccg.suprx` exists. vitaGL cannot compile
   shaders without it.
2. Check that `kubridge.skprx` is listed under `*KERNEL` in the taiHEN config
   the console loads, and that you rebooted after adding it (next entry).
3. Check the eboot hash. Reinstalling a VPK and launching an old `eboot.bin`
   are different things; make sure the installed file is the build you meant
   to test.

## App stops at start; log says `kuKernelAllocMemBlock ... failed`

What it means: the executable is built to load at one fixed address
(`0x98000000`). kubridge provides that mapping. Without kubridge the app cannot
start.

What to do: install kubridge as in [INSTALL.md](INSTALL.md) section 1, reboot,
launch again. Do not remove other plugins just to make the error go away.

## Game stops during loading after a fresh resource copy

What it means: the port started but the game could not use the resources.
The recompiled code is Repentance v1.7.9b (build J835). Resources of another
game version, a copy that ended early, or loose files next to `packed/` can
stop the game before the title screen.

What to do:

1. Compare `ux0:data/isaacr001/resources/packed/` with your PC copy: 22
   archives, `animations.a` 660,301 bytes, `repentance.a` 385,003,320 bytes
   for v1.7.9b. A transfer that ended early leaves smaller files; copy again
   and compare sizes. Copy the PC installation's `resources` folder only,
   without files added by mods or unpacking tools.
2. Move `ux0:data/isaacr001/mods/` aside for one launch. A mod written for
   another game version or for a PC-only extension can stop the game at start.
3. Read the game's `log.txt`. It lists the Lua scripts it ran from
   `resources/scripts/`, then `Binding of Isaac: Repentance v1.7.9b.J835`,
   `load archives: ... milliseconds`, one `Initialize ... Shader` line per
   shader and `Setting PersistentGameData ReadOnly to False`. The last line
   present tells which step failed. Report it together with the end of
   `first-arm-fault.log`.

## Loading never reaches the title screen

Normal on the development console: about 4 s from process start to the main
menu.

What to do:

1. Check that `ux0:data/isaacr001/resources/packed/animations.a` exists and
   matches the size and hash in [INSTALL.md](INSTALL.md). If not, the resource
   copy is incomplete or from a different game version.
2. Read the end of `first-arm-fault.log`. Find the last line starting with
   `guest stop:`, `guest: FAULT`, `guest: PE load failed`, or
   `bad_alloc diagnostic:` and put it in the report.
3. If the Vita's screen went off or the console was locked during loading, the
   system has moved the app to the background. It looks frozen. Unlock the
   console. This is not a crash and not a build problem.
4. A save file larger than 64 KiB once hung the boot and has not been
   reproduced since. If the console is unlocked and the app still sits on the
   loading display, move the `gamestate*.dat` files out of the save folder
   (keep them), launch again, and report the file sizes.

## Game stops after the PS button or a console lock

Symptom: you pressed the PS button, took a screenshot, or the console locked
while the game was open; shortly after you come back the app is gone and
`first-arm-fault.log` ends with `ArchivedFile block header is invalid`.

What it means: the Vita kernel invalidates the game's open file handles across
a suspend. Builds before v0.1.1-alpha stopped on the next music-stream read.
v0.1.1-alpha reopens the handle and continues; the log then shows one
`arcdiag v1 tag=descriptor-recover-v1` line per recovered handle. Saves
written before that point are intact either way.

What to do: on v0.1.1-alpha or later this should not happen; if it does,
report it with the four items above and say what interrupted the game. On
an older build, launch again and Continue.

## Game stops with Vita error C2-12828-1

Symptom: the game disappears and the console shows an error screen with the
code `C2-12828-1`.

What it means: this code is the console's generic report of an application
exception: the process itself faulted and the system ended it. It is not a
system or memory-card error. Unless core dumps are disabled in the HENkaku
settings, the console writes a dump of the process to
`ux0:data/psp2core-*.psp2dmp` at that moment. The port's log may end without
a `guest stop:` line, because the process was stopped before it could write
one.

What to do:

1. Do not launch the game again yet. On builds up to v0.1.1-alpha every launch
   empties `first-arm-fault.log`. Copy it and the game's `log.txt` to a PC
   first.
2. Copy the newest `psp2core-*.psp2dmp` from `ux0:data/` (VitaShell or FTP).
   The file is large; compress it before uploading.
3. Report the dump, the two logs, the eboot hash and what you were doing when
   the game stopped: room, boss, item used, how long you had played.

## Low frame rate in some rooms (known)

Measured on the development console, native 960x544, EID mod loaded. These
are known; report them only if you see something different.

- Menus: 60 FPS.
- Rooms with enemies: 55 to 59 FPS; dense rooms: 33 to 34 FPS. Cause: the CPU
  render phase (translated game code, vitaGL draws, Lua) takes 10 to 16 ms
  per frame.
- Laser-heavy rooms (Circle of Protection, Brimstone): lower. The last
  measurement (30 FPS, 6 to 7 FPS in the worst room) predates the offscreen
  render target fix of 2026-09-05 and has not been repeated; the GPU fill cost
  of the laser layers remains.
- An open EID description box adds 8 to 10 ms of Lua per frame.

The game itself simulates at 30 Hz. "60 FPS" means 60 presented frames per
second.

## Stalls when entering a room or changing floor (known)

Entering a room with resource loads takes 0.9 to 1.1 s, Continue from the
main menu about 7 s, changing floor 1.5 to 4.3 s. Cause: resource loads on
entry plus the game's room state reset (73 ms) and room snapshot (40 ms),
sometimes on two consecutive frames.

The music may stop for a moment during such a stall, and `first-arm-fault.log`
shows `KAGE VITA AUDIO REPLAY`. That is the same stall, not an audio fault.
If the music does not come back afterwards (the track's loop never starts and
the log shows `music stopped playing`), the game dropped a music slot whose
OpenAL source ran dry during the stall; a build with
`ISAAC_VITA_AUDIO_STARVATION_SHIELD=ON` keeps the slot and restarts the source
(`KAGE VITA AUDIO SHIELD` lines), and `ISAAC_VITA_AUDIO_MUSIC_DROP_RECEIPT=ON`
logs each drop as `KAGE VITA AUDIO MUSIC DROP`.

## Crash on "Exit game" after a long session (known)

Symptom: after a long session (once, after 27 minutes), choosing Exit game
drops you back to LiveArea. `first-arm-fault.log` ends with lines like:

```text
heapovf: e=pool-failure ... req=307200 ...
guest stop: ... fault=VCRUNTIME140.dll!_CxxThrowException ...
bad_alloc diagnostic: request=307200 ... arena=84930560 allocated=... free=... chunks=...
```

What it means: the 81 MiB game heap was full when the menu music was opened.
The game threw `std::bad_alloc`, which the recompiled code cannot handle, so
the process stopped on purpose. The saves had already been written.

What to do: launch again. Your save is intact. If you report it, include the
`bad_alloc diagnostic:` line and how long you played.

## Out of memory anywhere else

What to do: include the `bad_alloc diagnostic:` and `heapovf:` lines from
`first-arm-fault.log`, the room and the session length. Changing `--heap-mb`
is not a fix.

## Log shows a `guest: FAULT` line naming a `DLL!function` or an `unsupported GL backend symbol`

What it means: the game called a Windows or OpenGL function the port does not
implement. The port stops on purpose instead of faking success. The same name
appears after `fault=` in the `guest stop:` line.

What to do: report the symbol name from that log line and the eboot hash.

## Cannot continue a saved run

Symptom: Continue does not resume the run, and `log.txt` says
`Cannot continue a game that doesn't have the same modding state`.

What it means: a mod was enabled or disabled after that run was saved. The
game refuses by design; this is not a port bug.

What to do: restore the same mod state (add or remove `disable.it` in the mod
folder) or start a new run.

## A button does nothing; cannot pick tainted characters

Symptom: an action the game binds to a controller button does not happen. The
common case: the character select does not flip to the tainted characters,
which the game binds to the right trigger.

What it means: the Vita has no right trigger. The port presents the pad as an
Xbox controller and the game keeps its default bindings; every Xbox element
exists, but some are reached differently. Select is the right trigger: it
drops (tap swaps pocket or Schoolbag items, hold drops) and flips the
character select to the tainted characters. R is the left trigger (active
item) and L is the left shoulder (bomb). The right shoulder (pill/card) is
Start + L; Back (map) is Start + R. While Start is held, Start, L and R are
withheld from the game until the chord is resolved, so L or R pressed
together with Start does not bomb or use the active item. A short Start tap
pauses when Start is released; Start held for 0.8 s or longer becomes the
game's keyboard R (restart). The full table is in
[README.md](README.md#controls).

What to do: press Select where the game asks for the right trigger; use
Start + L and Start + R for pill/card and map. If a button still does nothing,
report which button, which screen and the eboot hash. The front-touch zones
(active item, minimap, pocket item) have not been tested on hardware; report
them separately.

## Daily challenge shows a paper and does not continue

What it means: the daily challenge comes from Steam. The port has no Steam
client, so the game shows the paper and nothing follows. This cannot be fixed
in the port.

What to do: leave the screen and start a normal run. Do not report this.

## A mod does not load

What to do:

1. Look for `disable.it` in `ux0:data/isaacr001/mods/<workshop id>/`. Delete
   it to enable the mod.
2. Make sure the build has Lua enabled ([INSTALL.md](INSTALL.md) section 3).
   A build without Lua crashes at start when a mod is enabled.
3. Read `log.txt` for errors printed by the mod itself.

## Custom builds only

These two do not happen in the released build, which has `ISAAC_VITA_LUA=ON`
and `ISAAC_VITA_FULLSPEED_EXIT_ZERO_FRAME_SITES=ON`. They can happen if you
build with other options.

### `guest: FAULT ... Lua5.3.3r.dll!lua_close` at exit

Happens with `ISAAC_VITA_LUA=OFF`. The game calls `lua_close` at shutdown, no
Lua exists, and the missing import faults after the saves were written. The app
exits with an error code; saves are not damaged. Build with
`ISAAC_VITA_LUA_OFF_CLOSE_NOOP=ON` to avoid it. Any other `FAULT` before the
exit is a real report.

### `KAGE VITA CADENCE30 DISABLE ... viol=0/0/1/0/0/0/0`

Happens with `ISAAC_VITA_FULLSPEED_SCHEDULER=ON` and
`ISAAC_VITA_FULLSPEED_EXIT_ZERO_FRAME_SITES=OFF` when you leave a run for the
menu. The 60 Hz presentation switches itself off for the rest of the session
and the game runs on its original frame limiter. Build with
`ISAAC_VITA_FULLSPEED_EXIT_ZERO_FRAME_SITES=ON`. If the numbers after `viol=`
differ from `0/0/1/0/0/0/0`, report the line.

### The game ignores my settings (`port.ini`)

Only builds with `ISAAC_VITA_RUNTIME_SETTINGS=ON` read
`ux0:/data/isaacr001/port.ini`, once, at game start. Open
`ux0:/data/isaacr001/first-arm-fault.log` (rewritten at every game start) and
find the line that begins `ISAAC VITA SETTINGS:`, a few lines after
`guest boundary begin`.

- No such line: this eboot was built without the option and never reads the
  file. Nothing in `port.ini` can change it.
- `status=absent`: no file at that path. The name is lowercase.
- `status=schema`: `schema=1` must appear on some accepted line (any
  position). A UTF-8 BOM, a `CR CR LF` line ending or any stray byte on that
  line rejects it, and then the whole file is ignored. `status=garbage` is a
  NUL byte in the file,
  `status=too-large` more than 4096 bytes, `status=io-error(0x........)` an
  open or read failure with that SCE code. In every one of these cases the
  compile-time defaults apply.
- `status=ok` but a value did not stick: read the second line,
  `ISAAC VITA SETTINGS REJECTED: lines=... unknown=...`. It lists the 1-based
  numbers of the first four rejected lines (bad key, value outside the
  allowed set, non-ASCII byte, blanks around `=`) and the first four unknown
  keys. Keys and values are lowercase, `key=value`, `fps_lock` takes only
  `60`, `45`, `40` or `30`. `dup=N` counts accepted repeats of a key (the
  last one wins); a repeat whose value is rejected is counted under
  `rejected=` instead and the earlier accepted value stands.
- `fps_lock=45(ignored:no-fullspeed-scheduler)`: this build has no
  `ISAAC_VITA_FULLSPEED_SCHEDULER` (a `STABLE_30` build, for example), so
  `fps_lock` has nothing to act on. The `KAGE VITA CADENCE30:` banner of a
  build that does apply it ends with ` fps_lock=N`.
- `fps_lock` accepted, banner shows it, but the lock is gone mid-session:
  look for a `KAGE VITA CADENCE30 DISABLE` line (entry above). The
  scheduler switched itself off and the lock with it; the game runs on its
  original limiter until the next start.
- `raster=...(ignored:not-compiled)` and `remap=...` are expected in this
  build: the resolution and remap keys are parsed and reported but not yet
  applied. `remap=default(guard)` means the remap section was dropped
  because no button maps to `a` or none to `b`.
- `render_scale=1|2` (the Manager's `ROOM RENDER` row) is applied by a
  build with `ISAAC_VITA_RENDER_SCALE_DEFAULT=ON`, which writes it into
  the game's `options.ini` (`MaxRenderScale=1` for `480X270 FAST`,
  `MaxRenderScale=99` for `960X540 SHARP`) right after the settings
  receipt and logs `ISAAC VITA RENDER SCALE: mode=N target=1|99
  status=... old=... bytes=N rc=0x........ recovered=0|1`. `status=`
  `unchanged` (the line already had the value), `rewritten`, `appended`
  (the key was missing and was added at the end of `[Options]`) mean
  the game saw the value; `absent` means there was no `options.ini` yet
  (the game writes its defaults on that run; the next start
  normalizes); `duplicate`, `no-section`, `binary`, `empty`,
  `too-large` (over 16 KiB) mean the file was left alone because it
  does not look like the game's own; `read-error`, `write-error`,
  `rename-error` carry the SCE code in `rc=` and always leave the
  previous `options.ini` in place (a stranded `options.ini.rs-bak` is
  moved back at the next start, `recovered=1`). No such line: the
  eboot was built without the option and the row does nothing; the
  game then uses whatever `MaxRenderScale` `options.ini` holds (`99`
  when the game created the file itself). The game's own log
  (`ux0:/data/isaacr001/Documents/My Games/Binding of Isaac
  Repentance/log.txt`) confirms the result with `Render Width: 480` /
  `Point Scale: 1.000000` for `1` and `Render Width: 960` for `99`.
- Black square (the "dirt"/"Hole" layer) under burrowing enemies, sometimes
  turning normal later (issue #10): the game tints that layer with a colour
  it reads back from its pre-rendered floor surface with `glReadPixels`.
  Stock vitaGL reads framebuffer textures with the display's row flip (row
  height-1-y) although it renders them already flipped, so the read lands
  in the surface's unused padding below the room and returns zeros; rooms
  that fill the surface read real texels, hence "sometimes normal". A build
  with `ISAAC_VITA_FBO_READBACK_FIX=ON` serves such reads from the texture
  memory at the rows the game drew (after draining the GPU when the surface
  was drawn since the last drain) and logs
  `KAGE VITA FBO READBACK: reads=N display=N finishes=N unknown=N
  settled=N skipped=N copied=N wait_us=N max_us=N` every 256 offscreen
  reads and at exit (`copied` = reads served from the texture, `finishes` =
  drains issued, `settled` = reads that needed none, `unknown` = drains
  forced by writes to a target whose texture the port could not identify,
  `max_us` = longest drain). The first eight offscreen reads are logged in
  detail: `[kage-vita] fbo-readback: fb= tex= at=x,y WxH sync= wait_us=
  rgba= copy=` plus a `fbo-readback-tex` line (the texture's descriptor)
  and a `fbo-readback-mem` line (a census of its memory: non-zero texels,
  first/last non-zero rows, the texel at the read position `at=` and at the
  row stock vitaGL would copy `flip=`). No such lines: the eboot was built
  without the option.
- A Manager built with `ISAAC_VITA_MANAGER_SETTINGS_UI` shows the same
  verdict on the `LAST BOOT` row of its `ISAAC SETTINGS` page: `FPS N REMAP
  ...` repeats the receipt of the last game start (`status=ok` or `absent`);
  `FPS N IGNORED REMAP ...` is a receipt whose `fps_lock` carried
  `(ignored:no-fullspeed-scheduler)`, i.e. that game build could not apply
  the lock; `FILE SCHEMA | GARBAGE | TOO-LARGE | IO-ERROR - DEFAULTS USED`
  is any other `status=`, i.e. that start ran on the built-in values
  whatever the file said; `GAME BUILD IGNORES PORT.INI` means that start
  logged its `build` line but no receipt (an `eboot.bin` built without
  `ISAAC_VITA_RUNTIME_SETTINGS`, or a start that faulted before the receipt
  was written); `NO LOG` means no log, an unreadable log, or a log without a
  `build` line. Its `PORT.INI` row (`ABSENT`, `OK`, `OK - N LINES IGNORED`,
  `INVALID - DEFAULTS SHOWN`, `IO ERROR`) is the Manager's own read of the
  file; X on the page rewrites an invalid file in canonical form, Triangle
  twice removes it (the copy goes to `.manager-backups/port-NNNN.ini`). A
  `RECOVERY` status on the Manager's home screen means its start-up repair
  of the save slots or of `port.ini` reported a failure (typically a
  directory sitting where a `.manager-rollback-*` or `port.ini.rollback`
  sidecar belongs); the settings page stays usable, but `RETURN TO GAME`
  repeats the repair and refuses to start the game until it succeeds.

## Restoring saves

Saves live in:

```text
ux0:data/isaacr001/Documents/My Games/Binding of Isaac Repentance/
```

Exit the game first. Move the damaged folder aside, copy your backup into
place, and keep both until you have loaded and saved once. Reinstalling the
VPK does not erase this folder.

## Not tested

No device evidence exists for: PS TV, language switching, runs longer than 27
minutes, beating a full run. If you try one of these, report the result with
the four items from the top of this page.

What is being worked on is listed in [PLAN.md](PLAN.md); measured device
results are in [STATUS.md](STATUS.md).
