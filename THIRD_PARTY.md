# Third-party material

This repository does not contain *The Binding of Isaac: Repentance* executable,
generated guest code or gameplay data. Users must provide their own legally
obtained PC copy. Five pinned Vita presentation images remain under
`recomp/vita/assets/sce_sys/`; their inclusion is separate from the user-supplied
runtime resources.

This file records the external code, assets, interfaces and research used by
the source tree. Add an entry when you bring something in.

## Source adaptations included in this tree

### libftpvita

The manager's passive FTP server is a reduced, altered adaptation of
[`xerpi/libftpvita`](https://github.com/xerpi/libftpvita) pinned at commit
`77a1d39c1d6f11a55fe65a6ed9fe707bff5ede4b`. Upstream is MIT licensed.
Copyright notices from both the upstream source header and repository licence
are retained: Copyright (c) 2015-2016 Sergi Granell (xerpi), and Copyright (c)
2019 Sergi Granell.

The adaptation replaces upstream's mount-point model and unchecked absolute
paths with one lexical mapper rooted at `ux0:/data/isaacr001`, omits active mode
and custom commands, and joins the worker before network teardown. It does not
claim to contain pre-existing native symbolic links or mount aliases. The exact
upstream source/licence hashes, alterations and unmodified MIT text are in
[`recomp/vita/third_party/libftpvita/PROVENANCE.md`](recomp/vita/third_party/libftpvita/PROVENANCE.md)
and
[`recomp/vita/third_party/libftpvita/LICENSE`](recomp/vita/third_party/libftpvita/LICENSE).
The licence is also packaged as `app0:/licenses/libftpvita.txt`.

Rinnegatamante's GPL-3.0 `lpp-vita` was consulted only as user-interface prior
art for a user-controlled FTP toggle and endpoint display. No `lpp-vita` source
is included.

### musl libc 1.2.5 decimal floating-point formatter

The Vita CRT's bounded `%f`/`%g` conversion is a reduced, altered adaptation of
musl libc's allocation-free `fmt_fp`, pinned to release 1.2.5 commit
`0784374d561435f7c787a555aeab8ede699ed298`. Copyright © 2005-2020 Rich
Felker, et al.; upstream is MIT licensed. The adaptation is restricted to the
six formats admitted by the runtime, IEEE-754 binary64 and a bounded memory
sink. It does not include musl's stdio layer.

The exact upstream source blob/hash, alterations and retained MIT text are in
[`recomp/runtime/third_party/musl_fmt_fp/PROVENANCE.md`](recomp/runtime/third_party/musl_fmt_fp/PROVENANCE.md)
and
[`recomp/runtime/third_party/musl_fmt_fp/LICENSE`](recomp/runtime/third_party/musl_fmt_fp/LICENSE).

### stb_vorbis v1.04

[`recomp/runtime/third_party/stb_vorbis/stb_vorbis.c`](recomp/runtime/third_party/stb_vorbis/stb_vorbis.c)
is Sean Barrett's Ogg Vorbis decoder, upstream v1.04 (2014-08-28), placed in
the public domain by its author.  It is the exact revision statically linked
by the Repentance executable (as `KAGE\Source\Sound\Base\External\Ogg\Ogg.cpp`),
identified through its assertion expressions and line numbers, its 0x5f8-byte
decoder state and its compile-time options.  The local edits (an `assert.h`
include seam and redirected allocator calls) are documented in the accompanying
[README](recomp/runtime/third_party/stb_vorbis/README.md); the game's own
`FILE *` to KAGE-stream substitution is reproduced by preprocessor macros in
`recomp/runtime/host_vita_native_vorbis.c` without touching the vendored
text.  Used only when `ISAAC_VITA_NATIVE_VORBIS=ON`.

Frozen source:
<https://github.com/nothings/stb/blob/b8e0530fdfbe/stb_vorbis.c>

### miniz v1.15

[`recomp/runtime/host_vita_archive_miniz_native.c`](recomp/runtime/host_vita_archive_miniz_native.c)
contains a plainly marked fixed-width adaptation of Rich Geldreich's
`tinfl_decompress()` from miniz v1.15 (2013-10-13). The frozen source describes
itself as public-domain software and includes the Unlicense statement.

Frozen source:
<https://github.com/tessel/miniz/blob/dee3e1992f0abbe42c1871590f6f7246b517db46/miniz.c>

### libdeflate v1.26

[`recomp/runtime/vendor/libdeflate-strict/`](recomp/runtime/vendor/libdeflate-strict/)
holds eight decompressor files of Eric Biggers' libdeflate v1.26, pinned at
commit `92e6a0db9fa848d742f9eb286c92afc60f2c3dda`, altered to refuse every
DEFLATE construct outside a single complete final block (the alterations are
listed in that directory's `README.md`). Upstream is MIT licensed; Copyright
2016 Eric Biggers and Copyright 2024 Google LLC. The licence text is kept in
[`recomp/runtime/vendor/libdeflate-strict/COPYING`](recomp/runtime/vendor/libdeflate-strict/COPYING)
and in every imported source header. The subset is compiled into the release
eboot by `ISAAC_VITA_NATIVE_PNG_LIBDEFLATE_STRICT` as a bounded first-IDAT
probe of the native PNG path; the original inflater remains the fallback.

Frozen source:
<https://github.com/ebiggers/libdeflate/tree/92e6a0db9fa848d742f9eb286c92afc60f2c3dda>

### zlib 1.1.4

The guarded PNG fast paths implement altered fixed-width forms of frozen zlib
1.1.4 behavior, including CRC-32, Adler-32 and `inflate_flush` semantics:

- `recomp/runtime/host_vita_png_crc32_native.c`;
- `recomp/runtime/host_vita_zlib_inflate_flush_native.c`.

Original zlib 1.1.4 copyright: Copyright (C) 1995-2002 Jean-loup Gailly and
Mark Adler. The zlib licence permits use, modification and redistribution,
subject to these restrictions:

1. the origin must not be misrepresented;
2. altered versions must be plainly marked and not represented as original;
3. the notice must not be removed or altered from a source distribution.

The software is provided as-is, without express or implied warranty. These
project files are altered adaptations, not the original zlib sources.

The inflate side of zlib 1.1.4 is also vendored byte-identical under
`recomp/runtime/vendor/zlib114/` for the native inflate path; its
`PROVENANCE.md` pins the tarball and every file. The zlib licence applies.

Frozen licence/source:
<https://github.com/madler/zlib/blob/v1.1.4/zlib.h>

### zlib-ng 2.2.4 (prior art only)

The PNG-only NEON Adler-32 helper `recomp/runtime/host_vita_native_png_adler.c`
follows the decomposition into byte columns, prior-vector prefix sums and
deferred weighted multiplies of zlib-ng 2.2.4's ARM NEON Adler implementation
(credited there to Mark Adler and ARM Holdings; authors Adenilson Cavalcanti
and Adam Stylinski; zlib licence) as a new compact 16-byte/4096-byte
implementation, not a copy of its loop, prologue, wrappers or API. The NEON
match-copy helper of the native PNG inflater
(`recomp/runtime/host_vita_native_png_lz_neon.h`) shares zlib-ng's
distance-class idea from `chunkset_tpl.h` and `chunkset_neon.c` without
copying its overread schedule or lookup tables. Both are default OFF
(`ISAAC_VITA_NATIVE_PNG_ADLER_NEON`, `ISAAC_VITA_NATIVE_PNG_LZ_NEON`) and are
checked for exactness against the scalar code by
`recomp/test_vita_native_png_adler_neon.py` and
`recomp/test_vita_native_png_lz_neon.py`.

Sources:
<https://github.com/zlib-ng/zlib-ng/blob/2.2.4/arch/arm/adler32_neon.c>,
<https://github.com/zlib-ng/zlib-ng/blob/2.2.4/chunkset_tpl.h>,
<https://github.com/zlib-ng/zlib-ng/blob/2.2.4/arch/arm/chunkset_neon.c>.

### libpng 1.4.20 and the PNG filter specification

The native row-unfilter implementation in
`recomp/runtime/host_vita_png_unfilter_native.c` was written for this project
and validated against the PNG filter specification
(<https://www.w3.org/TR/png-3/#9Filters>) and libpng 1.4.20
`png_read_filter_row` (filters 0-4 only; any other filter type falls back to
the translated code so the original error path stays observable). It is
plainly different from libpng's later NEON kernels, which are not copied:
those consume whole vectors over libpng-owned padded rows, this helper uses
bounded chunks plus a scalar tail inside the exact guest row. Its exactness
against an independent expression of the PNG equations is proven by
`recomp/runtime/host_vita_png_unfilter_native_oracle.c` (433,613 cases, also
under AddressSanitizer and UndefinedBehaviorSanitizer). Default OFF
(`ISAAC_VITA_PNG_NATIVE_UNFILTER`).

libpng 1.4.20 is Copyright (c) 2000-2002, 2004, 2006-2016 Glenn
Randers-Pehrson and derives from earlier work by Glenn Randers-Pehrson,
Andreas Dilger, Guy Eric Schalnat, Group 42, Inc., and the listed Contributing
Authors. The libpng licence permits use, copy, modification and distribution
without fee provided that origin is not misrepresented, altered versions are
plainly marked, and the copyright notice is retained. The PNG Reference
Library is supplied as-is without warranty.

Frozen reference/licence:
<https://github.com/pnggroup/libpng/blob/v1.4.20/png.h>

The later `host_vita_native_png_rgba_neon.h` uses the PNG equations and the
lane-wise Paeth approach from the official libpng 1.6.37 ARM NEON source as
references. Its bounded byte-based loads/stores and scalar-tail integration
are project code, not libpng's padded-row kernels. Reference attribution:
Copyright (c) 2018 Cosmin Truta; Copyright (c) 2014, 2016 Glenn Randers-Pehrson;
written by James Yu (2013), based on Mans Rullgard's assembly (2011), under
the libpng licence. Scope: only Sub, Average and Paeth rows with four 8-bit
channels, in bounded four-pixel blocks (exactly sixteen source bytes per
block, no padding read, no alignment assumed) with the existing scalar
equations for the tail; Up is unchanged. Exactness against independent PNG
equations is checked by `recomp/test_vita_native_png_rgba_neon.py`, which
runs the ARM build of the decoder under emulation (1,752 cases per mode with
enforced memory bounds). Default OFF (`ISAAC_VITA_NATIVE_PNG_RGBA_NEON`).
Reference source:
<https://github.com/pnggroup/libpng/blob/v1.6.37/arm/filter_neon_intrinsics.c>.

### Specialist Dance loading animation

`recomp/runtime/kage_vita_loading_specialist.inc` holds 32 RGB565/RLE frames
converted by `tools/build_specialist_loader.py` from **Specialist Dance**,
Steam Workshop item 2575911103 (the file header records the input hashes):

- programming: Jiftoo;
- graphics and trailer: Devector;
- animation rip credited by the Workshop page: SetoKeino.

The Workshop description permits forks and edits. The animation is shown when
`ISAAC_VITA_LOADING_SPECIALIST=ON` (the default of `tools/build_vita.py`); a
procedural loading screen drawn by code is the fallback.

Source: <https://steamcommunity.com/sharedfiles/filedetails/?id=2575911103>

## Runtime and build dependencies

### Vita presentation artwork

The tracked `icon0.png`, `pic0.png`, `bg0.png`, `nicalis.png` and `startup.png`
are presentation assets from the Isaac/Nicalis Vita package lineage. They are
kept byte-pinned for the VPK build. Their names and hashes are not a
claim of ownership or a blanket redistribution licence.

### vitaGL and shader stack

[vitaGL](https://github.com/Rinnegatamante/vitaGL) supplies the OpenGL ES/GXM
backend. The reproducible recipe pins upstream commit
`73dd57a8857f89f2353881c6de5891959c5c1983` and applies tracked, plainly
separate project patches. Upstream reports LGPL-3.0 and GPL-3.0 licensed
material; the exact linked component set and corresponding source/relinking
obligations must accompany any binary distribution.

vitaGL's runtime compiler path uses vitaShaRK/SceShaccCg interfaces. Sony's
`libshacccg.suprx` is not included and is not redistributable here. Users must
provide their own compatible module as described in [INSTALL.md](INSTALL.md).

### OpenAL Soft

[OpenAL Soft](https://github.com/kcat/openal-soft) provides the native audio
backend. The tracked overlay targets upstream `openal-soft-1.19.1` and first
applies [isage](https://github.com/isage/openal-soft)'s Vita backend patch,
pinned by SHA-256 in `recomp/vita/openal-overlay/build.sh`. Upstream
offers that version under LGPL 2 or later. Because the Vita build links it
statically, a public binary must include the applicable notices and satisfy
source/relinking obligations.

### VitaSDK

[VitaSDK](https://vitasdk.org/) is the build toolchain. The
[vita-headers](https://github.com/vitasdk/vita-headers) repository is MIT
licensed. Toolchain files and system stubs are not copied into this repository;
a release receipt must identify the exact toolchain used.

### kubridge

[kubridge](https://github.com/TheOfficialFloW/kubridge) provides fixed virtual
address allocation required by the translated image. The project does not
redistribute the plugin; users install it separately.

### Khronos OpenGL Registry

The typed GL bridge is generated from the
[Khronos OpenGL Registry](https://github.com/KhronosGroup/OpenGL-Registry).
`recomp/gl_surface_manifest.json` records the pinned registry commit and
`gl.xml` hash. The applicable Khronos notice must accompany generated
interfaces in a public source release.

### Lua 5.3.3

Builds with `ISAAC_VITA_LUA=ON` compile the pristine Lua 5.3.3 source (MIT
licence, PUC-Rio) that the user extracts from
<https://www.lua.org/ftp/lua-5.3.3.tar.gz>; `recomp/vita/lua53_vanilla.cmake`
pins every file by SHA-256. The Lua source is not copied into this repository.

### capstone, pefile and Pillow

The PC tooling uses [capstone](https://www.capstone-engine.org/) 5.0.7
(BSD-3-Clause) for x86 disassembly and [pefile](https://github.com/erocarrera/pefile)
2023.2.7 (MIT) for PE parsing in the translator, and
[Pillow](https://python-pillow.github.io/) (MIT-CMU) to decode and scale
bounded Steam Workshop preview images in the Windows sync-manager UI. All
three are pinned in `requirements.txt` and none is shipped on the Vita.

### REPENTOGON and libzhl

[REPENTOGON](https://github.com/TeamREPENTOGON/REPENTOGON) is GPL-2.0; its
libzhl core is MIT. This project takes function names, signatures and layouts
from it as interface facts, plus three vanilla bug fixes (the Godhead partition
constant and two RenderBatch fixes) whose upstream commits are recorded in
`recomp/gen_all.py` next to the code. Those fixes are derived work, which is
one reason repentogxm is GPL-2.0-or-later: compatible with REPENTOGON and with
the GPL-3.0 parts of the Vita stack.

## Project-source licence

repentogxm's own code is licensed under the GNU General Public License,
version 2 or (at your option) any later version ([LICENSE](LICENSE)).
Third-party parts keep the licences listed above.
