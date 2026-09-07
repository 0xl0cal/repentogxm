# Project plan

Updated 2026-09-07. Goal, open problems in priority order, performance rules
and the definition of done. Build ids, hashes and per-build numbers live in
[STATUS.md](STATUS.md).

## Goal

A playable, beatable, stable port of The Binding of Isaac: Repentance on a
PS Vita:

- 60 FPS at the native 960x544 panel. Lowering the display resolution is not
  an accepted fix. The game world is drawn at its own 480x270 and upscaled
  once, as the PC game does (the game's HQX and colour-correction passes are
  bypassed); that is faithful output, not a downgrade.
- Game logic runs 30 times per second, as on PC. Never faster to hide a stall.
- Lua mods, External Item Descriptions (EID) first, behave as they do on PC.
- Audio runs off the game core: the OpenAL mixer and the music decoder on CPU 2.
- Zero regressions: saves and mods that worked keep working.
- Loading pauses short enough not to interrupt play.

## Where things stand

v0.1.1-alpha: menus 60 FPS, rooms with enemies 55-59 FPS, dense rooms 33-34
FPS, game at 30 updates per second, EID working, audio on CPU 2 without
underruns, saves and Continue working. Door transitions with resource loads
stall about a second, Continue about 7 s. Longest session 27 minutes, ended
by the heap (priority 5).

## Current priorities

1. Rooms below 60 FPS.
   Cause: the CPU render phase, 9.9-15.9 ms median per frame: about 5.7 ms of
   translated game code and 4.3 ms of native code called from it (vitaGL draw
   bodies, scene switches, Lua callbacks and sprite attribute state each under
   1 ms).
   Done: neutral ColorOffset fragment path, laser ring shadow skip,
   reference-count seam, host replay of the sprite quad builder
   `Image::PushQuad` for every quad (room render median 11.7-13.1 ms down to
   9.9-10.3 ms).
   Next: re-attribute the room render phase on the current build, then the
   remaining translated render work.
2. Laser rooms fall to 30 FPS, the worst room found to 6-7 FPS.
   Cause: the CPU waited for the GPU at the end of each offscreen render pass,
   because vitaGL created the offscreen render target with one scene slot, so
   pass k could not end until pass k-1 had finished on the GPU; plus over
   100 ms per frame of GPU fill on the laser layers, which has no fix.
   Done: the 8-slot render target (`ISAAC_VITA_STOCK_FBO_RT_SCENES`) and the
   omission of the sampled/ring laser shadow pass
   (`ISAAC_VITA_LASER_RING_SHADOW_SKIP`).
   Next: measure a laser room on the current build.
3. Loading stalls: door transitions with resource loads 0.9-1.1 s, Continue
   about 7 s, floor changes 1.5-4.3 s.
   Cause: the room state reset (`00520160`, 73 ms per call, 53 ms its own
   work) and the room snapshot (`00314fc0`, 40 ms), sometimes on two
   consecutive frames; PNG sprite-sheet decode plus the archive reads under it
   (45-50 % of a room entry) and a full Lua garbage collection at level init
   (10 %).
   Done: the PNG fast paths and the per-floor GC clamp.
   Next: the room state reset's own 53 ms. Sprite retention across rooms
   (`ISAAC_VITA_IMAGE_RETAIN`) is in the tree, OFF, unmeasured.
4. Crash after the PS button or a console lock: `ArchivedFile block header is
   invalid`.
   Cause: the Vita kernel invalidates the process's open `ux0:` file handles
   across an app suspend; the next music-stream read failed and the archive
   reader stopped the game.
   Done: the CRT reopens the handle from its retained path at the same cursor
   and redoes the read (`ISAAC_VITA_CRT_DESCRIPTOR_RECOVER`); verified on
   device with the PS button at the main menu.
   Next: the same check with a run open and after standby.
5. Guest heap runs out in long sessions.
   Cause: after 27 minutes a 307,200-byte allocation for the menu music
   stream failed with 80.5 MiB of the 81 MiB heap in use and 505 KB free in
   3,294 fragments; the same exit worked early in the session, so heap use
   grows with play time. Saves were written before the crash.
   Done: a one-slot emergency allocation for that stream queue
   (`ISAAC_VITA_OGG_QUEUE_EMERGENCY`); not a fragmentation cure.
   Next: log heap usage periodically during play, find what grows.
6. An open EID description box breaks 60 FPS: Lua costs about 0.8 ms per frame
   with the box closed and 8-10 ms with it open.
   Done: native `__index` and `getClass` seams.
   Next: fix the `__index` VERIFY comparator false alarm (STATUS.md problem 7),
   then time the EID callbacks themselves.
7. Controls.
   Today: L = bomb, R = active item, Select = drop (tap swaps pocket items, as
   the PC right trigger does), Start = pause, Start+L = pill/card, Start+R =
   map, hold Start = restart; touch zones over the active-item icon, minimap
   and pocket icon. Full table: recomp/vita/README.md.
   Open: remapping UI, PS TV profiles, an audit of two active items, Schoolbag
   and characters whose pocket slot holds an active item.
8. Mods beyond EID.
   Open: callback/API compatibility report, dependency and load-order display,
   a recovery boot that disables the last enabled mod. A tiny test mod
   (`tools/isaac_vita_sync.py --lua-sentinel`) exists, not yet run on device.
   Rule: Workshop content comes from the user's own Steam installation; the
   project will not download paid content or bypass Steam.
9. Companion tools.
   Present: a second button on the game's LiveArea page opens the on-device
   Manager (mod enable/disable via the game's own `disable.it` file, SHA-256
   checked save backup and restore, FTP on port 1337 rooted at the data
   directory); a PC command-line tool and a small Windows GUI around it. Host
   tests pass; nothing has been used on hardware.
   Open: test the Manager on a Vita; package the GUI as a Windows executable;
   a one-time FTP code (any password is accepted today).
10. Long-run correctness and lifecycle.
    Untested: a run played end to end; standby with a run open; language
    switching; update and rollback; PS TV.
11. Releases.
    Source on GitHub; the v0.1.0-alpha and v0.1.1-alpha VPKs are attached to
    GitHub Releases. The game's resources are never published. Licence
    GPL-2.0-or-later. Keep THIRD_PARTY.md current and
    `tools/release_audit.py --strict` at GO for every release; per-release
    steps in release/README.md.

## Performance rules

- Measure on the Vita. Instruction counts and host benchmarks do not count.
- Report the maximum and 95th-percentile frame time per phase (update, render,
  other) and window wall time, never the frame count alone: 120 frames per 2 s
  window prints "60 FPS" even when frames take 30-70 ms or a load stalls for
  seconds.
- Frames presented, frames rendered and game updates are three numbers.
- A native replacement for translated code must check that it sees exactly the
  expected code and data, and otherwise run the original translated code.
- Fix a measured seconds-scale stall before any micro-optimization.
- Never hide a stall by running the simulation faster than 30 updates/s.
- Keep native 960x544 output. The 720x408 and 480x272 display options exist
  and are not a fix. Skipping clears of off-screen framebuffers
  (`ISAAC_VITA_FBO_CLEAR_ELISION`) stays off: combined with framebuffer
  down-scaling it lost shading layers on device; alone it saved about 2 ms and
  did not remove the laser-room bursts.
- The menu is vsync-bound; menu savings buy no FPS. Rank work by room and
  fight milliseconds.

## Acceptance checklist

- boots repeatedly on real hardware with the exact build listed in STATUS.md;
- menus, controls, audio, rooms, combat, projectiles and effects are correct;
- active items and the map have usable default bindings and remapping;
- music and sound effects survive transitions and suspend/resume;
- a run can be completed through floors, bosses and an ending;
- no crash, no out-of-memory, no save corruption, no unrecoverable audio stop;
- save/load survives an `eboot.bin` update and rollback;
- room, Continue and Exit transitions meet a latency the owner accepts;
- frame times are measured on the Vita in steady and dense scenes, with
  60 FPS reached where the hardware allows and the simulation at 30 updates/s;
- suspend/resume and clean shutdown pass;
- PC-to-Vita save and mod transfer is recoverable, checksum-verified and never
  overwrites the only copy; the Manager can disable a broken mod;
- a clean Windows machine installs from the release VPK and manager EXE
  without Python or a terminal; the manager lists only Steam-downloaded mods
  and shows their names and preview images on PC and Vita;
- provenance, notices, licence, release audit and a repeatable build from
  source are accepted before publication.
