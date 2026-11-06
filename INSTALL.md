# Installing on a PS Vita

Take the VPK from the project's GitHub Releases page, or build it yourself
from your own Windows copy of *The Binding of Isaac: Repentance* (section 3).
Either way the game's resources come from that Windows copy; they are not
distributed with the port, and without them the app does not start.

The title ID is `ISAACR001`. Resources, settings, logs and saves live under
`ux0:data/isaacr001/`, outside the application directory. Reinstalling the app
does not touch them.

This is an alpha. [STATUS.md](STATUS.md) lists what works and what does not.

## Requirements

- A PS Vita with homebrew (taiHEN) and VitaShell. PS TV has not been tested.
- `kubridge.skprx` loaded as a kernel plugin (section 1).
- Your own copy of `libshacccg.suprx` at `ur0:data/libshacccg.suprx`
  (section 1).
- About 1.5 GiB free on `ux0:`. The game resources alone are about 1 GiB.
- A PC installation of Repentance v1.7.9b, for the resources (version check
  below).
- Only if you build the VPK yourself: a PC with Python 3.10 or newer and the
  soft-float VitaSDK (section 3).

If you build yourself, the build accepts exactly one executable:

```text
isaac-ng.exe.unpacked.exe
size:   8,650,240 bytes
sha256: 31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404
```

Any other file is rejected with `unsupported unpacked PE`.

The port is generated from Repentance v1.7.9b (build J835), so the resources
must come from that version. To check yours, look at two files:

```text
resources/packed/animations.a
size:   660,301 bytes
sha256: 182e071934fc2d4600506bb326bb8d3946861bfa7ce3c4404d524d68ada8abf5
resources/packed/repentance.a
size:   385,003,320 bytes
```

`resources/packed` holds 22 archives (`*.a`), about 1.06 GB in total.
Resources of another game version are untested.

## 1. Install the platform prerequisites

### kubridge

The game executable is built to load at one fixed memory address. kubridge
provides that mapping. Without it the app stops right after launch.

Copy `kubridge.skprx` to your taiHEN plugin folder (normally `ur0:tai/`) and
add it under `*KERNEL` in the config file the console actually loads:

```text
*KERNEL
ur0:tai/kubridge.skprx
```

Reboot after changing kernel plugins. If you have both `ux0:tai/config.txt`
and `ur0:tai/config.txt`, make sure you edited the one the system loads.

### libshacccg.suprx

vitaGL compiles shaders on the console at run time and needs Sony's shader
compiler module for that. Put your own copy at:

```text
ur0:data/libshacccg.suprx
```

The module is proprietary. It is not in this repository and not in any build
output. Without it vitaGL cannot compile shaders and nothing is drawn.

## 2. Copy your resources

Copy the complete `resources` directory from your PC installation to:

```text
ux0:data/isaacr001/resources/
```

The file you checked above must then be at:

```text
ux0:data/isaacr001/resources/packed/animations.a
```

Do not put resources under `ux0:app/ISAACR001`; installing a new VPK replaces
that directory. Do not copy the PC executable, Steam DLLs or PC mods into the
Vita data directory.

### Optional: repack the archives for faster loading

`tools/isaac_vita_repack.py` (Python 3.10 or newer, standard library only)
re-encodes the compressed pieces of the MiniZ archives (`afterbirthp.a`,
`repentance.a` and the other mode-2 files) as static Huffman, so the game's
inflater no longer builds a Huffman table for every kilobyte it decodes. It
always writes new files. Run it on the PC and copy the output directory to
`ux0:data/isaacr001/resources/packed` instead of the original one:

```text
python tools/isaac_vita_repack.py repack-dir "<steam>/The Binding of Isaac Rebirth/resources/packed" <output dir>
```

Before the tool reports success it decodes every entry of every repacked
archive with its own decoder and byte-compares it with the original; keys,
sizes, mount checksums and entry order are unchanged. `afterbirth.a` and
`animations.a` are copied unchanged (the Vita build validates those two
against a receipt of the stock files), as is every non-MiniZ archive. The
repacked archives are 0.9-3.9 % smaller (measured on the shipped
`repentance.a` and `afterbirthp.a`). This step is optional: the stock
archives keep working exactly as before.

If you also use the smaller sound catalogue (see below), repack first and
halve the repacked copy: point the halving tool's `tree` command at the
directory that holds the repacked `packed/`, then copy its output archives
and their `.receipt` files over the repacked ones. The halving tool copies
every entry it does not rewrite byte for byte, so the static pieces survive,
and its `.receipt` sidecar then pins the final bytes. The other order leaves
that sidecar stale: `repack-dir` does not copy a `.receipt` whose archive it
changed and says so; write a new one with
`python tools/isaac_vita_halve_sfx.py receipt <repacked archive>`.

## 3. Build the VPK (optional)

Skip this section if you use the VPK from the Releases page.

You need Python 3.10 or newer, the packages in `requirements.txt`, and the soft-float
VitaSDK build (`vitasdk-softfp`). The standard VitaSDK is hard-float and will
not link; the build script checks this first and stops.

```bash
python -m pip install -r requirements.txt
python tools/build_vita.py \
  --pe /path/to/isaac-ng.exe.unpacked.exe \
  --vitasdk /path/to/vitasdk-softfp
```

The VPK is written to the build directory (default `build/vita-release/`) as
`the-binding-of-isaac-repentance.vpk`.

`python tools/build_vita.py --self-test` runs the host-side checks only; it
needs neither the executable nor the VitaSDK.

Generated C code goes to `../repentogxm-build/vita-generated`, next to the
checkout, by default. The build directory, the generated code and the
executable are ignored by Git.

`python tools/build_vita.py --help` lists all options. Two matter for most
people:

- `--lua --lua-source /abs/path/to/lua-5.3.3` enables the Lua bridge. Mods
  need it. A build without Lua crashes at start if any mod is enabled. The
  path must point at an unmodified, extracted `lua-5.3.3` source tree.
- `--heap-mb` sets the game heap. Leave it at the default 81.

This script builds a plain configuration: Lua off unless you pass `--lua`,
the 60 FPS output mode off, none of the performance options of the released
VPK. The complete option set of the release is
[release/v0.1.1-alpha.cmake](release/v0.1.1-alpha.cmake) as checked out at
the `v0.1.1-alpha` tag, a CMake initial cache; its header gives the three
commands that reproduce the release build (corpus generation with
`--generate-only`, `cmake -C`, `ninja`). On later revisions the same file
also pins knobs added after the release, three of them ON, so it no longer
reproduces the shipped VPK exactly. The numbers
in [STATUS.md](STATUS.md) do not apply to a build made with the defaults
above.

## 4. Install and launch

Copy the VPK (from the Releases page or your own build) to the console and
install it with VitaShell. On the LiveArea
page the app is named `The Binding of Isaac: Repentance`. The same page has a
second button, `SAVE / MOD MANAGER` (see below).

The first launch creates:

```text
ux0:data/isaacr001/first-arm-fault.log
ux0:data/isaacr001/Documents/My Games/Binding of Isaac Repentance/
```

The first file is the port's own log. The folder holds the game's saves and the
game's own `log.txt`.

Loading takes time. Measured on the development console at native 960x544
with the EID mod loaded:

- process start to the main menu: about 4 s
- Continue from the main menu: about 7 s
- changing floor: 1.5 to 4.3 s
- entering a room: 0.9 to 1.1 s when resources load; ordinary revisits far less

If the loading display stays much longer than this, see
[TROUBLESHOOTING.md](TROUBLESHOOTING.md).

## Mods

Mods need a build with Lua enabled (section 3). A Workshop mod goes to:

```text
ux0:data/isaacr001/mods/<workshop id>/
```

For example External Item Descriptions (EID) is
`ux0:data/isaacr001/mods/836319872/`. A file named `disable.it` inside a mod
folder disables that mod. Delete the file to enable the mod again.

The PC tool `tools/isaac_vita_sync.py` can copy a mod you already have in Steam
to the Vita over FTP:

```text
python tools/isaac_vita_sync.py plan --mod 836319872
python tools/isaac_vita_sync.py push --host <vita ip> --mod 836319872 --yes
```

`plan` only prints what would be copied and needs no Vita; add
`--steam-root <path>` if the tool does not find your Steam library. `push`
needs the Manager's FTP server running (next section).

The game refuses to continue a saved run if a mod was enabled or disabled
after the save was made. `log.txt` then says
`Cannot continue a game that doesn't have the same modding state`. That is the
game's own rule. Restore the same mod state or start a new run.

EID (95 scripts, 19 MB) loads and draws item descriptions and was enabled in
every measured session. It is the only mod that has been tested.

## Optional: a smaller sound catalogue

The engine keeps every loaded sound effect as raw PCM in its 81 MiB heap and
reads the whole WAV from the archive, synchronously, when a sound is first
needed (about 170 sounds at boot, the rest on first use in a room; each load
is a 13 ms average, 235 ms worst-case stall on the Vita). About two thirds of
the catalogue carries no audible content above 11 kHz, so those samples can
be stored at half their sample rate without changing what they sound like.
`tools/isaac_vita_halve_sfx.py` does that offline, on a copy, never in place
(needs `numpy`):

```text
python tools/isaac_vita_halve_sfx.py tree <copy of resources> <output dir> --dry-run
python tools/isaac_vita_halve_sfx.py tree <copy of resources> <output dir>
```

If you also repack the archives (section 2), repack first and run this tool
on the repacked copy, so its `.receipt` sidecars pin the final bytes.

The dry run prints what would change. The second command writes only the
archives that changed into `<output dir>/packed/`, each with a 256-byte
`<archive>.receipt` next to it (measured on v1.7.9b: `afterbirthp.a`,
`repentance.a` and `sfx.a`; 591 of their 922 catalogued samples halved,
185.5 MiB of PCM to 110.9 MiB, 544.5 MB of archives to 469.3 MB). Copy the
archives and their `.receipt` files over
`ux0:data/isaacr001/resources/packed/` and keep the originals. Files whose
treble matters (coins, chains, splatter) are left byte for byte alone; every
other file in the archives is copied unchanged. What one session actually
loads is much less than the catalogue: pass `--game-log <log.txt>` (the game
log from `Documents/My Games/Binding of Isaac Repentance/`) and the tool
prints the saving for exactly those sounds. Measured against two Vita logs:
a boot plus a modded run had 31.7 MiB of sound PCM resident, of which the
rewritten archives hold 19.2 MiB, 11.7 MiB after (7.5 MiB saved; 11.5 MiB
with `--include-afterbirth`); a plain boot 22.6 MiB resident, 13.0 to
7.6 MiB (5.4 MiB saved; 8.1 MiB with `afterbirth.a`). The heap is 81 MiB.

Boot time, read this before copying. At mount the engine decodes and
checksums every entry of every archive. The port skips that for entries whose
checksum matches a table baked into the executable and, for `afterbirth.a`
and `animations.a`, for files whose exact identity its validation receipt
recorded (`ISAAC_VITA_ARCHIVE_VALIDATION_SKIP` and `_RECEIPT`, both ON in the
release build). Rewritten entries have new checksums, so a released eboot
validates them the original way on every launch. Measured on the Vita as a
proxy: the shipped `afterbirth.a` validated that way, entry by entry, on a
perf build before its receipt existed costs 63-66 s per launch
(`[archive-validation-receipt] archive=9 baked=0 receipt=0 original=2647` from
10.4 s to 75.5 s in the debug log); a rewritten `afterbirth.a` has the same
2647 entries and would pay the same, which is why the tool leaves it alone
unless told `--include-afterbirth`.
The 591 rewritten entries of the other three archives (74.7 MiB decoded) are
not measured; at the 2.5 MiB/s of that afterbirth.a pass they would add up to
about 30 s per launch, probably less because they are stored, not deflated.
An eboot built with `-DISAAC_VITA_ARCHIVE_VALIDATION_SIDECAR=ON` (default
OFF, not part of v0.1.1-alpha) removes that cost: it reads the `.receipt`
next to each rewritten archive, checks the file against it (size, ARCH000
header, SHA-256 of the index table, SHA-256 of 64 sampled 4 KiB windows) and
skips the loop; the debug log then shows `[archive-validation-sidecar] ...
archive=repentance.a trusted=1`. With such a build `--include-afterbirth`
(149 more samples, 12.0 MiB of PCM) is fine too. Without it, expect the
slower launch or do not copy the archives; the sound itself is unaffected
either way. The tool writes a sidecar only after re-reading the archive it
just wrote and verifying every entry against its index checksum, the same
test the engine applies.

Halved sounds go through OpenAL Soft's resampler at play time (1.19.1 on the
Vita, default `linear` per its `alsoftrc.sample`), which slightly softens the
top octave (8-11 kHz); nothing else about the sound changes. This has not yet
been played on a Vita; the first run after copying is a test.

## Save / Mod Manager

The `SAVE / MOD MANAGER` button on the LiveArea page opens a small separate
app. It can:

- start and stop an FTP server (Circle) on port 1337 whose root is
  `ux0:data/isaacr001`;
- enable or disable installed mods (Triangle, then X on a mod);
- back up and restore the three save slots (Square);
- with the two settings options described below: edit the port's FPS lock
  (Start opens the `ISAAC SETTINGS` page).

A slot is `persistentgamedataN.dat` (unlocks and progress) plus the optional
`gamestateN.dat` (the run in progress). A build configured with
`-DISAAC_VITA_MANAGER_SAVE_GAMESTATE=ON` (`tools/build_vita.py
--manager-save-gamestate`; default OFF, and the v0.1.1-alpha profile pins it
OFF, so the release manager handles only `persistentgamedataN.dat` - an
opt-in until it has run on a Vita) backs both up together: a backup of
slot N writes `.manager-backups/slotN-NNNN.dat` and, when the slot had a run,
`.manager-backups/slotN-NNNN.gamestate.dat` under one index. The SAVES page
shows `RUN <bytes>` or `NO RUN` next to each slot and `WITH RUN` after every
backup that carries a run; the status line ends in `RUN STATE SAVED` /
`NO RUN STATE` after a backup and `RUN STATE RESTORED` / `RUN STATE KEPT`
after a restore. Restoring a backup without a run (an older manager backup, the
game's own dated `YYYYMMDD.persistentgamedataN.dat`, or a PC sync copy) leaves
the live `gamestateN.dat` untouched. A restore replaces both files under one
marker; if the Vita loses power midway, the next Manager start puts the
previous pair back and moves the half-written copies to
`.manager-backups/recovered-slotN-NNNN[.gamestate].dat`. The pair the restore
replaced is kept as `slotN-NNNN[.gamestate].dat`, so a restore can be undone
by restoring that entry. One window is the exception: if power is lost after
the replaced pair has been archived but before the restore's marker is
removed (the last step), the next Manager start treats the restore as
unfinished and undoes it too - the previous pair comes back from that archive
and the restored content lands in `recovered-slotN-NNNN[.gamestate].dat`.
Nothing is lost in either case; the old data wins.

The FTP server has no password. Anyone on your network can change files under
`ux0:data/isaacr001` while it runs. Turn it on only for the transfer.

The Manager has not been tested on a real Vita; its code passes host tests
only. Use VitaShell for backups.

### Settings page (`port.ini`)

A build configured with `-DISAAC_VITA_RUNTIME_SETTINGS=ON
-DISAAC_VITA_MANAGER_SETTINGS_UI=ON` (`tools/build_vita.py --runtime-settings
--manager-settings-ui`; both default OFF; `release/v0.1.1-alpha.cmake` turns
`ISAAC_VITA_RUNTIME_SETTINGS` on for the game, since the render-scale default
needs it, and leaves the Manager page OFF) adds an `ISAAC SETTINGS` page to
the Manager, opened with Start on its home screen. The page edits one file,
`ux0:/data/isaacr001/port.ini`
(the spelling the game's receipt line uses; the two forms name the same
file), which the game reads once when it starts: a change takes effect at the next
game start, never in a running game. The page has two rows:

- `FPS LOCK` `60 | 45 | 40 | 30` (Left/Right; default 60). The game keeps
  simulating at 30 updates per second; the lock only caps how many of the
  interpolated in-between frames are presented: 60 shows every one (today's
  behaviour), 45 every second, 40 every third, 30 none. 45 and 40 are an
  uneven cadence (frame intervals alternate between 16.7 and 33.3 ms), so
  the page marks them `40 45 - UNEVEN CADENCE 30 UPS` and asks for X twice
  before saving. The lock acts through the release build's 60 Hz scheduler
  (`ISAAC_VITA_FULLSPEED_SCHEDULER`); a game built without it logs the value
  as ignored.
- `ROOM RENDER` `480X270 FAST | 960X540 SHARP` (Left/Right; default
  `480X270 FAST`). This is the game's own `MaxRenderScale` option
  (`options.ini`, no in-game menu): `FAST` draws the rooms at the 480x270
  world raster and upscales once, as the PC game does in a small window;
  `SHARP` is the game's stock unbounded value (2x at the Vita's 960x540
  window), sharper sprites for a higher CPU and fill cost. A game built
  with `ISAAC_VITA_RENDER_SCALE_DEFAULT` rewrites the `MaxRenderScale`
  line of `ux0:/data/isaacr001/Documents/My Games/Binding of Isaac
  Repentance/options.ini` to the chosen value at every start (`1` or
  `99`), leaving every other byte of the file alone, and logs an
  `ISAAC VITA RENDER SCALE:` receipt; if the file does not exist yet the
  game writes its own defaults on that first run and the next start
  normalizes them. Edits made by hand to that line do not survive a
  start; the port.ini setting is the single source of truth.

Keys: Up/Down row, Left/Right value, X save, Triangle twice `DEFAULTS`, O
back (with unsaved changes O asks once more, then discards them). `SAVE`
writes the file through a temporary copy that is hashed before it replaces
the old one; the previous version is kept as
`ux0:/data/isaacr001/.manager-backups/port-NNNN.ini`, and while such a copy
differs from the live file an `UNDO` row offers to restore it (UNDO twice =
redo whenever the undone SAVE or UNDO replaced a live file; after `DEFAULTS`
+ `UNDO` nothing was replaced, the row hides and Triangle twice is the way
back; the Manager never deletes these copies). `DEFAULTS` moves `port.ini`
into that directory and removes it, so the game starts with the built-in
values; deleting `port.ini` by hand (VitaShell, FTP) has the same effect.
Every write is refused while the Manager's FTP server is running.

The page's status rows say what the game will see. `PORT.INI ABSENT | OK |
OK - N LINES IGNORED | INVALID - DEFAULTS SHOWN | IO ERROR`: an invalid or
unreadable file means built-in values, and X rewrites it. `LAST BOOT`
repeats the settings receipt of the last game start (`FPS 45 REMAP
DEFAULT`; `FPS 45 IGNORED REMAP DEFAULT` when that game build could not
apply the lock; `FILE SCHEMA - DEFAULTS USED` and the like when that start
rejected the file as a whole), or says `GAME BUILD IGNORES PORT.INI` when
that game build was made without `ISAAC_VITA_RUNTIME_SETTINGS` (an
`eboot.bin` swapped in on its own, for example), or `NO LOG`. The file is plain text (`schema=1`,
`fps_lock=45`, `render_scale=2`, one lowercase `key=value` per line); TROUBLESHOOTING.md
explains the receipt line. The page has not run on a real Vita yet; its
logic passes host tests only.

### New Manager interface (opt-in)

A build configured with `-DISAAC_VITA_RUNTIME_SETTINGS=ON
-DISAAC_VITA_MANAGER_SETTINGS_UI=ON -DISAAC_VITA_MANAGER_UI2=ON`
(`tools/build_vita.py --runtime-settings --manager-settings-ui
--manager-ui2`; default OFF, and the v0.1.1-alpha profile pins it OFF)
replaces the Manager's screens with a list interface: pages HOME, MODS,
SAVES, SETTINGS, ACHIEVEMENTS, CONTROLS and ABOUT (L/R switch pages), in the
DejaVu font with English and Russian text. The VPK then also carries
`manager/font-*.imf` (DejaVu, licence in `licenses/dejavu.txt`). The
Manager's own choices are kept in `ux0:data/isaacr001/manager.ini` and edited
on the SETTINGS page: `lang=auto|en|ru` (`auto` follows the system
language), `theme=game|dark|light` (a build without the game skin shows
`game` as `light`) and `autolaunch=off|5|10|30` (seconds before the game
starts on its own). It has not run on a real Vita yet; it passes host tests
only.

## Updating without losing saves

Saves live in:

```text
ux0:data/isaacr001/Documents/My Games/Binding of Isaac Repentance/
  gamestate*.dat
  persistentgamedata1.dat .. persistentgamedata3.dat
```

1. Exit the game from its menu.
2. Copy the whole `Documents` folder to your PC with VitaShell. If the
   Manager's FTP is on, the PC tool can copy the three
   `persistentgamedata` files instead:

   ```text
   python tools/isaac_vita_sync.py pull --host <vita ip> --saves
   ```

   It writes them into a new folder under `Documents/Isaac Vita Save Backups`
   on the PC and never overwrites an existing backup. Untested against a real
   Vita.
3. Note the SHA-256 of the installed `ux0:app/ISAACR001/eboot.bin` (the
   "eboot hash" in reports): copy the file to a PC and run
   `certutil -hashfile eboot.bin SHA256` (Windows) or `sha256sum eboot.bin`.
4. Install the new VPK with VitaShell, or copy only the new `eboot.bin` over
   the old one and keep a copy of the old one.
5. Do not delete `ux0:data/isaacr001/`.
6. Keep the old `eboot.bin` and your save backup until the new build has
   loaded, entered a room, and saved and loaded once.

## Uninstalling

Back up saves first. Deleting the `ISAACR001` app from LiveArea removes the
app but leaves `ux0:data/isaacr001/` in place. Delete that folder by hand only
if you also want to remove resources, settings, logs and saves. Do not remove
kubridge or `libshacccg.suprx` if other homebrew uses them.

## Vita3K

Vita3K (a PS Vita emulator for PC) is useful to reproduce crashes, not to
judge performance. Its default Windows paths are:

```text
%APPDATA%\Vita3K\Vita3K\ur0\data\libshacccg.suprx
%APPDATA%\Vita3K\Vita3K\ux0\data\isaacr001\resources\
```

Read [TROUBLESHOOTING.md](TROUBLESHOOTING.md) before replacing files at random.
Current device evidence is in [STATUS.md](STATUS.md); build internals are in
[recomp/vita/README.md](recomp/vita/README.md).
