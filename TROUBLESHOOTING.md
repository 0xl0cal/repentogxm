# Troubleshooting on a PS Vita

## Before you change anything

Collect these four things first. Without them a report cannot be checked.

- `ux0:data/isaacr001/first-arm-fault.log` (the port's own log)
- `ux0:data/isaacr001/Documents/My Games/Binding of Isaac Repentance/log.txt`
  (the game's own log)
- the SHA-256 of `ux0:app/ISAACR001/eboot.bin` (called "eboot hash" below;
  copy the file to a PC and run `certutil -hashfile eboot.bin SHA256` on
  Windows or `sha256sum eboot.bin` elsewhere)
- what you were doing: room, items, menu action, how long you had been playing

A log from an older eboot says nothing about a newer build.

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
