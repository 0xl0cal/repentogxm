# ⚠️ !! WARNING !! ⚠️

## THE MOST VIBESLOP SHIT YOU HAVE EVER SEEN

> This repository contains industrial quantities of AI-assisted reverse
> engineering, vibe coding, ugly experiments, suspiciously effective slop,
> and code that was repeatedly bullied into running on a 2011 handheld.
>
> **If you are anti-AI, please close this repository now. Continued exposure
> may be harmful to your blood pressure, sleep schedule, and faith in software
> engineering. Yes, it is AI slop. You have been warned.**

# repentogxm

`repentogxm` is an experimental static-recompilation port of **The Binding of
Isaac: Repentance** to the PlayStation Vita. The PC executable is translated
ahead of time into C and compiled for the Vita's ARM CPU. There is no emulator.
This repository contains no game code, resources or binaries; you supply your
own PC copy. It is not affiliated with or endorsed by Edmund McMillen, Nicalis,
Valve, Sony, or the maintainers of any dependency named below.

## Does it work?

Yes: title screen, menus, rooms, bosses, pause, Continue, Exit to menu, Vita
controls, music and effects, saves. It is early and not smooth everywhere.
Output is the Vita's native 960x544 with no resolution downgrade. Game logic
runs at its normal 30 updates per second (measured 29.85 to 30.00); the port
shows up to 60 frames per second.

[Video of v0.1.0-alpha](https://www.youtube.com/watch?v=R6Imi5NVfwI)

Measured on one PS Vita on 2026-09-04 with the External Item Descriptions
(EID) mod loaded:

| Scene | Result | Cause / status |
|---|---|---|
| Menus | 59-60 FPS, render about 10 ms per frame | fine |
| Ordinary rooms | mostly 60 FPS (render 8-12 ms per frame); on a morning build almost every 2-second stretch had one frame over 33 ms (a visible hitch), so the average over such a stretch was 42-51 FPS | hitches and load stalls, see below |
| Dense rooms (many enemies and effects) | 35-37 FPS | per-object reference counting under a lock in translated code; fix in progress |
| Boss fights with Brimstone | about 50 FPS with hitches | CPU cost in update and render plus waits for the GPU; not fully attributed |
| Laser rooms (Circle of Protection) | about 30 FPS | the CPU waits for the GPU at the end of every offscreen render pass, most likely because vitaGL gives the offscreen render target one scene slot per frame; a fix is being built and is not yet measured |
| Worst room found (two Brimstone monsters + Circle of Protection + Azazel's Brimstone) | 6-7 FPS | the same wait plus more than 100 ms of GPU fill per frame from the laser layers; the fill cost needs its own fix |
| Room entry | 0.9-1.7 s stall | sprites and sounds are reloaded on every entry; fix in progress |
| Floor change; run start or Continue | 1.5-4.3 s; 5.3-5.5 s | asset reload plus a full Lua garbage collection per floor |
| Long sessions | a 27-minute session filled the game's 81 MiB heap and the game stopped on "Exit game"; saves were written first | heap use grows over play time; fix in progress |

Audio is OpenAL Soft; music is decoded by a native Vorbis decoder on CPU core
2. No audio underruns were logged in the measured sessions, but the music
stream can run dry and restart during a load stall. EID loads and shows item
descriptions; measured before the latest build's Lua changes, it cost about
2.7 ms per frame in rooms and 8-10 ms while a description box is on screen,
which breaks 60 FPS. No other mod has been tested.

Not tested on the current builds: PS TV, suspend/resume, language switching,
front-touch zones, a full run to a final boss, and the Utero II floor (an
earlier report of that floor rendering black has not been re-tested). The
longest measured sessions were 20-27 minutes. [STATUS.md](STATUS.md) has the
per-build tables; [PLAN.md](PLAN.md) the priorities.

## How it works

`recomp/gen_all.py` translates the functions of the unpacked PC executable
into C. `recomp/vita/` compiles that C with the soft-float VitaSDK together
with a host runtime and loads it at a fixed address (`0x98000000`) through
kubridge. The game's OpenGL calls go to vitaGL (an OpenGL layer over the
Vita's native GXM graphics API), its audio to OpenAL Soft, and its Lua 5.3.3
is built from a source tree you supply. Selected hot functions are replaced by
native code; each replacement falls back to the translated code when its
preconditions are not met.

## Why this exists

Rebirth is the only Isaac the Vita ever got, and it stayed that way: no Afterbirth, no Afterbirth+, no Repentance. I wanted to play Repentance on my Vita and I was bored. And, I guess, nobody else was going to do it. So I decided to burn a frankly stupid amount of LLM tokens on the problem and see whether the PC exe could be statically recompiled into something that runs on a 2011 handheld. Turns out it can, mostly.

It's a fun project, it runs well enough that I actually play it, and now you can too.

## You must supply the game

This repository contains **no game executable, generated C code, gameplay
resources, saves, keys, Sony modules or firmware**. The only tracked artwork
is the LiveArea images in `recomp/vita/assets/sce_sys/`. Ready-made VPKs are on
the Releases page; they contain the recompiled game code but no resources.
Everything else comes from your own legally obtained PC copy of Repentance.
Building from source additionally needs exactly one unpacked 32-bit
executable; any other file is rejected (how to unpack it is not described
here):

```text
size:   8,650,240 bytes
sha256: 31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404
```

## Installing on a Vita

You need:

- a homebrew-enabled PS Vita;
- `kubridge.skprx` listed under `*KERNEL` in the taiHEN config the console
  actually loads (normally `ur0:tai/config.txt`); reboot after adding it;
- your own `libshacccg.suprx` at `ur0:data/libshacccg.suprx` (vitaGL compiles
  shaders at run time);
- about 1.5 GiB free on `ux0:` (the packed resources alone are about 1 GiB);
- the complete `resources` directory of your PC installation, copied to
  `ux0:data/isaacr001/resources/`.

The title ID is `ISAACR001`. The app appears in LiveArea as `The Binding of
Isaac: Repentance` with a second button, `SAVE / MOD MANAGER` (not yet tested
on real hardware). Everything the port writes lives under `ux0:data/isaacr001/`,
outside the app directory:

- saves and the game's `log.txt`:
  `ux0:data/isaacr001/Documents/My Games/Binding of Isaac Repentance/`;
- the port's own log: `ux0:data/isaacr001/first-arm-fault.log`;
- mods: `ux0:data/isaacr001/mods/<workshop id>/`; a file named `disable.it`
  inside a mod directory turns that mod off.

Back up the save directory before replacing a build, and keep the old
`eboot.bin` until the new one has loaded, entered a room and completed one
save/load cycle. Removing the app does not remove `ux0:data/isaacr001/`. A run
saved without a mod cannot be continued after enabling one; that is the game's
own rule, not a port bug. When reporting a problem, include the SHA-256 of the
installed `ux0:app/ISAACR001/eboot.bin`, the port log and the game's `log.txt`.

VPKs are on the Releases page. Full install, update and Vita3K instructions are in
[INSTALL.md](INSTALL.md); known failures in [TROUBLESHOOTING.md](TROUBLESHOOTING.md).

## Building from source

Host requirements: Python 3.10 or newer (the pinned Pillow release needs it),
the packages in `requirements.txt`, Git, CMake (Ninja for `--release`), and
the **soft-float** VitaSDK (`vitasdk-softfp`), not the default hard-float one.

```bash
python -m pip install -r requirements.txt
python tools/build_vita.py \
  --pe /path/to/isaac-ng.exe.unpacked.exe \
  --vitasdk /path/to/vitasdk-softfp
```

To run Lua mods add `--lua --lua-source /abs/path/to/lua-5.3.3` (a pristine
Lua 5.3.3 source tree); a build without Lua crashes at startup if a mod is
enabled. `--heap-mb` sets the game heap (default 81 MiB; see `--help`).
`python tools/build_vita.py --help` lists the rest. The script checks the
executable hash and the generated code and stops on any mismatch.

`build_vita.py` builds a baseline configuration. The builds measured above
were configured with CMake directly and turn on options the script does not
expose: translated-code CPU optimizations, native Vorbis and PNG decoders,
asynchronous logging and others. Expect the baseline build to be slower; on a
2026-09-02 build without the native decoder (720x408, Lua off), audio alone
dropped game updates from 30 to about 20 per second. The option list and build
details are in [recomp/vita/README.md](recomp/vita/README.md).

## Broken or in progress

- dense rooms drop to 35-37 FPS (per-object locking in translated code); boss
  fights with Brimstone to about 50 FPS (cause not fully attributed);
- laser rooms drop to 30 FPS (the CPU waits for the GPU between render
  passes; fix being built) and the worst room to 6-7 FPS (GPU fill cost of the
  laser layers; no fix yet);
- room entry stalls about 1 s, floor changes 1.5-4 s (asset reload);
- the game's heap fills up over a long session and the game stops on exit;
- the on-device Save/Mod Manager and the PC sync tool are untested on hardware;
- no controller remapping UI;
- provenance and licensing review for a public release is unfinished.

## Repository layout

- `recomp/` -- the translator (`gen_all.py`), its tests and the portable runtime;
- `recomp/vita/` -- the Vita CMake project, host runtime, vitaGL and OpenAL
  patches, the Manager, tests;
- `tools/` -- `build_vita.py`, the PC/Vita sync tool, release audit helpers;
- `release/` -- release notes (text only; no binaries, no checksums yet).

## PC/Vita sync

`tools/isaac_vita_sync.py` copies saves and downloaded Steam Workshop mods
between PC and Vita over FTP (the Manager's server or VitaShell). `plan` needs
no Vita. `push` never deletes: replaced files are backed up on the Vita and every
upload is SHA-256 verified. `pull` only downloads the three save files into a
new dated folder under `Documents/Isaac Vita Save Backups`. In the examples,
`836319872` is EID's Steam Workshop id.

```text
python tools/isaac_vita_sync.py plan --saves --latest-save-backups
python tools/isaac_vita_sync.py push --host 192.168.1.50 --mod 836319872 --yes
python tools/isaac_vita_sync.py pull --host 192.168.1.50 --saves
```

`tools/launch_isaac_vita_sync.cmd` opens a Windows GUI around the same commands.
The Manager's FTP server has no password: enable it only for the transfer.
Neither the Manager nor the tool has been tested end to end on real hardware.

## Contributing and licence

AI-written, human-written and mixed contributions are welcome. Preserve exact
game behaviour, state what you measured, and keep the translated fallback for
cases you did not prove. Never commit game executables, resources, generated
`guest_####.c` files, saves, keys, firmware, Sony modules, build outputs,
dumps or credentials; `.gitignore` covers them.

The project's own code is licensed under the GNU GPL, version 2 or (at your
option) any later version ([LICENSE](LICENSE)): fork it, change it, ship it,
keep it open. Third-party material keeps its own terms
([THIRD_PARTY.md](THIRD_PARTY.md)). The game and its resources belong to
Nicalis and Edmund McMillen and are not distributed here. Releases carry the
VPK only; see [DISTRIBUTION.md](DISTRIBUTION.md) and
[CONTRIBUTING.md](CONTRIBUTING.md).

## Credits

The biggest external sources of code, formats, fixes and ideas:

- [Team REPENTOGON](https://github.com/TeamREPENTOGON/REPENTOGON) and libzhl --
  most of the useful Isaac function names, signatures, layouts and several
  exact gameplay/render bug fixes;
- [Rinnegatamante](https://github.com/Rinnegatamante/vitaGL) -- vitaGL, the
  entire OpenGL/GXM foundation, and
  [lpp-vita](https://github.com/Rinnegatamante/lpp-vita) as prior art for the
  on-device manager/FTP workflow;
- [VitaSDK](https://github.com/vitasdk/vita-toolchain) and
  [vitasdk-softfp](https://github.com/vitasdk-softfp) -- the Vita ABI,
  headers, linker/package pipeline and the soft-float toolchain;
- [TheOfficialFloW/kubridge](https://github.com/TheOfficialFloW/kubridge) and
  [fgsfdsfgs/max_vita](https://github.com/fgsfdsfgs/max_vita) -- loading the
  game at a fixed memory address and the loader patterns behind it;
- [Chris Robinson and OpenAL Soft](https://github.com/kcat/openal-soft), plus
  [isage's Vita port](https://github.com/isage/openal-soft) -- the audio
  backend and its Vita adaptation;
- [Rich Geldreich/miniz](https://github.com/tessel/miniz/blob/dee3e1992f0abbe42c1871590f6f7246b517db46/miniz.c),
  [Jean-loup Gailly and Mark Adler/zlib](https://github.com/madler/zlib/tree/v1.1.4),
  and the [libpng contributors](https://github.com/pnggroup/libpng/tree/v1.4.20)
  -- the archive and PNG algorithms adapted into native code;
- Sean Barrett's stb_vorbis and [xerpi's libftpvita](https://github.com/xerpi/libftpvita)
  -- the music decoder and the Manager's FTP server;
- [Rick Gibbed/Gibbed.Rebirth](https://github.com/gibbed/Gibbed.Rebirth/tree/8454c449cc60d680d57e3edb27ea9e56009c024a)
  and [Basement Renovator](https://github.com/Basement-Renovator/Basement-Renovator)
  -- archive encryption/framing and Isaac's `.stb` room-file format;
- the [Vita3K team](https://github.com/Vita3K/Vita3K) and
  [AnimMouse/SceShaccCg](https://github.com/AnimMouse/SceShaccCg) -- the main
  bring-up environment and the shader-compiler path that made it usable;
- [max_vita](https://github.com/fgsfdsfgs/max_vita),
  [gtasa_vita](https://github.com/TheOfficialFloW/gtasa_vita),
  [baba-is-you-vita](https://github.com/v-atamanenko/baba-is-you-vita), and
  [pkgj](https://github.com/blastrock/pkgj) -- proven Vita I/O, cache, libc and
  input patterns reused instead of reinventing them;
- [M-HT/SR](https://github.com/M-HT/SR),
  [XenonRecomp](https://github.com/hedge-dev/XenonRecomp), and
  [xboxrecomp](https://github.com/sp00nznet/xboxrecomp) -- the main static-
  recompilation architecture references;
- the [Khronos OpenGL Registry](https://github.com/KhronosGroup/OpenGL-Registry)
  and [Wine's Isaac fix](https://gitlab.winehq.org/wine/wine/-/merge_requests/4145)
  -- the generated GL surface and the clue for an animation-file crash;
- [Specialist Dance](https://steamcommunity.com/sharedfiles/filedetails/?id=2575911103)
  by Jiftoo, Devector and SetoKeino -- the loading-screen animation (converted
  frames in `recomp/runtime/kage_vita_loading_specialist.inc`, rebuilt from
  the mod with `tools/build_specialist_loader.py`).

Also adapted or linked: OpenAL Soft 1.19.1, stb_vorbis v1.04, miniz v1.15,
zlib 1.1.4, libpng 1.4.20, musl 1.2.5 (decimal float formatting), Lua 5.3.3
(from your own source tree), capstone and pefile (PC translator tooling),
Pillow (PC GUI only). kubridge and `libshacccg.suprx` are not included. Exact
revisions and licences are in [THIRD_PARTY.md](THIRD_PARTY.md) and
[NOTICE.md](NOTICE.md).

Thanks also to Edmund McMillen, Nicalis and everyone who made Repentance; to
wofsauge and the External Item Descriptions contributors, whose mod is the
Lua-compatibility benchmark here; to Team Molecule, yifanlu and TheOfficialFloW
for HENkaku, taiHEN and VitaShell, without which nothing runs; to Rinnegatamante
for vitaShaRK and years of Vita ports to learn from; to the Lua team at
PUC-Rio; to the capstone, pefile, Pillow, CMake, Ninja and GCC maintainers;
and to everyone in the Vita homebrew scene who answers questions.
