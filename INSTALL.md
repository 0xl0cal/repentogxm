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
[release/v0.1.1-alpha.cmake](release/v0.1.1-alpha.cmake), a CMake initial
cache; its header gives the three commands that reproduce the release build
(corpus generation with `--generate-only`, `cmake -C`, `ninja`). The numbers
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

## Save / Mod Manager

The `SAVE / MOD MANAGER` button on the LiveArea page opens a small separate
app. It can:

- start and stop an FTP server (Circle) on port 1337 whose root is
  `ux0:data/isaacr001`;
- enable or disable installed mods (Triangle, then X on a mod);
- back up and restore the three save slots (Square).

The FTP server has no password. Anyone on your network can change files under
`ux0:data/isaacr001` while it runs. Turn it on only for the transfer.

The Manager has not been tested on a real Vita; its code passes host tests
only. Use VitaShell for backups.

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
