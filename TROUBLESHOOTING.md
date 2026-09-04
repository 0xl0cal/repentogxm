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

## Loading never reaches the title screen

Normal on the development console (2026-09-03): about 26 s from launch to the
title screen.

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
4. One boot hang on 2026-09-03 was traced to a save file larger than 64 KiB.
   Whether current builds still have it is not confirmed. If the console is
   unlocked and the app still sits on the loading display, move the
   `gamestate*.dat` files out of the save folder (keep them), launch again,
   and report the file sizes.

## Low frame rate in some rooms (known)

Measured on the development console on 2026-09-04, native 960x544, EID mod
loaded. All of these are known and not fixed yet. Do not report them again
unless you see something different.

- Menus and ordinary rooms: 60 FPS.
- Rooms full of enemies and effects: 35 to 37 FPS. Cause: per-object
  reference counting under a lock in the translated code. Fix in progress.
- Boss fights with Brimstone: about 50 FPS with visible hitches. Cause not
  fully attributed.
- Circle of Protection (laser ring item): about 30 FPS. Both CPU render cost
  and waits for the GPU were measured; under investigation.
- Brimstone enemies plus Circle of Protection plus Azazel's Brimstone: 6 to
  7 FPS. Cause: the CPU waits for the GPU to finish drawing at buffer clears
  inside vitaGL. Fix in progress.

The game itself simulates at 30 Hz. "60 FPS" means 60 presented frames per
second.

## Stalls when entering a room or changing floor (known)

Measured on the development console on 2026-09-04: entering a room 0.9 to
1.7 s, changing floor 1.5 to 4.3 s, starting a run or Continue 5.3 to 5.5 s.
Cause: sprite sheets and sound effects are decoded and archives re-read on
every entry. Fix in progress.

The music may stop for a moment during such a stall, and `first-arm-fault.log`
shows `KAGE VITA AUDIO REPLAY`. That is the same stall, not an audio fault.

## Crash on "Exit game" after a long session (known)

Symptom: after about 27 minutes of play (one occurrence, 2026-09-04), choosing
Exit game drops you back to
LiveArea. `first-arm-fault.log` ends with lines like:

```text
heapovf: e=pool-failure ... req=307200 ...
guest stop: ... fault=VCRUNTIME140.dll!_CxxThrowException ...
bad_alloc diagnostic: request=307200 ... arena=84930560 allocated=... free=... chunks=...
```

What it means: the 81 MiB game heap was full when the menu music was opened.
The game threw `std::bad_alloc`, which the recompiled code cannot handle, so
the process stopped on purpose. The saves had already been written before that
point (the 2026-09-04 log shows both save files written successfully before
the stop).

What to do: launch again. Your save is intact. The cause (heap use grows during
play) is being investigated. If you report it, include the
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

These two do not happen in the builds measured in STATUS.md, which are built
with `ISAAC_VITA_LUA=ON` and `ISAAC_VITA_FULLSPEED_EXIT_ZERO_FRAME_SITES=ON`.
They can happen if you build with other options.

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

No device evidence exists for: PS TV, suspend/resume, resuming the game after
the PS button, language switching, runs longer than 27 minutes, beating a full
run. If you try one of these, report the result with the four items from the
top of this page.

What is being worked on is listed in [PLAN.md](PLAN.md); measured device
results are in [STATUS.md](STATUS.md).
