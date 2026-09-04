# Project plan

Updated 2026-09-04. This file lists the goal, the open problems in priority
order, the performance rules and the definition of done. Build ids and hashes
live in [STATUS.md](STATUS.md); this file names none.

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

## Where things stand (device, 2026-09-04)

- Game speed: 29.85-30.00 updates per second measured on device while the port
  presents up to 60 frames per second. Done; kept as a rule below.
- Menus, light and mid rooms with EID loaded, 960x544: median render time per
  frame 10.2 ms (menu), 7.8 ms (light rooms), 11.5 ms (mid rooms)
  (2026-09-04, STATUS.md performance table).
- Caveat: the profiler counts 120 frames shown per 2-second window as
  "60 FPS", and a morning session had 120 in every window, but that is not a
  smooth 60 FPS. Counted over wall time, rooms ran at a median of 42-49 FPS in
  a 7-minute session and menus at 59.6 in a 20-minute one: room-entry stalls
  and hitches over 33 ms sit inside the windows (2026-09-04, STATUS.md).
- Audio: OpenAL Soft, native Vorbis decoder on CPU 2. No buffer underruns, and
  the native decoder never fell back to the translated one, in any 2026-09-04
  session; a music source runs dry only inside load stalls.
- Mods: EID loads (95 scripts) and draws descriptions; no EID errors were
  logged in either full session. Lua must be compiled in (CMake
  `ISAAC_VITA_LUA=ON`) whenever
  a mod is enabled; a build without it crashes at startup.
- Saves persist; a Continue run was completed; no faults in the 20-minute and
  15-minute sessions. Longest session: 27 minutes, ended by priority 4.

## Current priorities

1. Dense rooms run at 35-37 FPS.
   Problem: rooms with many entities render at 20-22 ms per frame.
   Evidence (2026-09-04, STATUS.md known problem 1): 35,692 translated calls
   and 7,790 mutex lock/unlock pairs per frame versus 10,867 and 700 in a
   normal room; draw count, GL work and Lua unchanged. The pairs come from the
   game's reference counting, which takes a mutex on every AddRef, Release and
   weak-pointer lock.
   Fix: a native replacement for those three helpers with identical behaviour.
   Status: being implemented (design reviewed 2026-09-04); not on device.
2. Laser rooms fall to 30 FPS, the worst room to 6-7 FPS.
   Problem: Circle of Protection alone locks the game to 30 FPS; two Brimstone
   monsters plus Circle of Protection plus Azazel's Brimstone gives 6-7 FPS.
   Evidence (2026-09-04, STATUS.md known problem 2): for Circle of Protection
   alone one profile found the CPU busy in render and a later one found it
   waiting for the GPU. In the worst room, during a burst, the CPU spent 72 ms
   per frame waiting for the GPU inside glClear and at the end of each GXM
   scene (GXM is the Vita's graphics API; vitaGL splits every frame into
   several scenes: 7 clears and 4 scenes per frame, one clear took 138 ms);
   steady state in that room 12.7 ms per frame, a normal room 0.5 ms. Two
   experiments failed: letting vitaGL split the display frame into 8 GXM
   scenes instead of 1 changed nothing; skipping redundant clears of
   off-screen framebuffers saved about 2 ms and left the bursts.
   Fix: the wait is at the end of each offscreen render pass; vitaGL creates
   the offscreen render target with one scene slot per frame, so pass k cannot
   end until pass k-1 has finished on the GPU (frame time = CPU + GPU). Give
   that render target 8 slots (vitaGL patch, build option) and time scene ends
   per render target to confirm. The worst room also spends over 100 ms per
   frame of GPU fill on the laser layers, which this does not fix.
   Status: being implemented (design reviewed 2026-09-04); not on device.
3. Loading stalls.
   Problem: room entries stop the game for 0.9-1.7 s, floor changes for
   1.5-4.3 s, run start and Continue for about 5.5 s (2026-09-04).
   Evidence (profiler, 2026-09-04): 45-50 % of a room entry is PNG sprite-sheet
   decode plus the archive reads under it (sprites are decoded and uploaded
   again on every entry); 10 % is a full Lua garbage collection at level init.
   Already replaced by native code or otherwise optimized when measured, so
   not the cause: PNG row decode, zlib inflate, and the file-size seek loop of
   floor loading (answered from a cache). The limit on that collection's work
   per floor (CMake `ISAAC_VITA_LUA_GCCOLLECT_CLAMP`) is in the latest build,
   unmeasured.
   Fix: keep decoded sprites across room entries. Status: implemented on a
   branch, not merged, not measured on device.
4. Guest heap runs out in long sessions.
   Problem: after 27 minutes of play, "Exit game" crashed.
   Evidence (2026-09-04): a 307,200 byte allocation for a menu music stream
   failed with 80.5 MiB of the game's 81 MiB heap in use and 505 KB free in 3,294
   fragments. The same exit worked early in the session, so heap use grows
   with play time. Saves were written before the crash.
   Fix: log heap usage periodically during play, then find what grows.
   Status: in progress.
5. An open EID description breaks 60 FPS.
   Problem: Lua costs about 2.7 ms per frame in rooms and 8-10 ms per frame
   while an EID description box is on screen (profiler, 2026-09-04).
   Fix: native replacements for the two hottest calls of the Lua bridge (the
   layer that exposes game objects to Lua), `__index` and `getClass`, are in
   the latest build with verification on; one false
   verification alarm needs a comparator fix before verification is turned off.
   Then time the EID callbacks themselves. Status: on device, unmeasured.
6. Controls.
   Today: L = bomb, R = active item, Select = drop (tap swaps pocket items, as
   the PC right trigger does), Start = pause, Start+L = pill/card, Start+R =
   map, hold Start = restart; touch zones over the active-item icon, minimap
   and pocket icon (not re-verified on the 2026-09-04 builds). Full table:
   recomp/vita/README.md.
   Open: remapping UI, PS TV profiles, audit of two active items, Schoolbag
   and characters whose pocket slot holds an active item. Status: not started.
7. Mods beyond EID.
   Open: callback/API compatibility report, dependency and load-order display,
   a recovery boot that disables the last enabled mod. A tiny test mod
   (`tools/isaac_vita_sync.py --lua-sentinel`) exists; not yet run on device.
   Rule: Workshop content comes from the user's own Steam installation; the
   project will not download paid content or bypass Steam. Status: not started.
8. Companion tools.
   Present: one VPK and Title ID; a second button on the game's LiveArea page
   opens the on-device
   Manager (mod enable/disable via the game's own `disable.it` file, SHA-256
   checked save backup and restore, FTP on port 1337 rooted at the data
   directory); a PC command-line tool and a small Windows GUI around it. Host
   tests pass (2026-09-04); Manager UI and transfers have not been used on
   hardware.
   Open: package the GUI as a Windows executable; test the Manager on a Vita;
   a one-time FTP code (any password is accepted today). Status: built;
   nothing tested on hardware; open items not started.
9. Long-run correctness and lifecycle.
   Untested: a run played end to end (several floors to an ending); Utero II
   rendering (an earlier report of that floor rendering black has not been
   re-tested); suspend/resume; the PS button;
   language switching; update and rollback; PS TV. Status: not started.
10. Releases.
   Present: source on GitHub; the VPK is attached to GitHub Releases. The
   game's resources are never published.
   Licence: GPL-2.0-or-later. Open: keep THIRD_PARTY.md current and
   `tools/release_audit.py --strict` at GO for every release. Status: done for
   the source; the per-release steps are in RELEASE_CHECKLIST.md.

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
  and are not a fix. Skipping clears of off-screen framebuffers (CMake
  `ISAAC_VITA_FBO_CLEAR_ELISION`) stays off: combined with framebuffer
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
