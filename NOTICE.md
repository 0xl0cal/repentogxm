# Notices

This project is not affiliated with or endorsed by Edmund McMillen, Nicalis,
Valve, Sony, or the maintainers of the dependencies below. Their names,
copyrights, trademarks, licences and notices remain their own.

Tracked Vita presentation artwork consists of five byte-pinned Isaac/Nicalis
package images under `recomp/vita/assets/sce_sys/`. Playable game resources,
the PC executable, saves, keys, PlayStation firmware/modules and
`libshacccg.suprx` are not included.

repentogxm's own code is licensed under the GNU General Public License,
version 2 or (at your option) any later version; see [LICENSE](LICENSE).

The loading-screen animation frames in
`recomp/runtime/kage_vita_loading_specialist.inc` are converted by
`tools/build_specialist_loader.py` from the Specialist Dance Steam Workshop
mod (item 2575911103) by Jiftoo (programming), Devector (graphics) and
SetoKeino (animation rip); the Workshop page permits forks and edits.

Source adaptations include:

- the manager FTP server's reduced adaptation of xerpi/libftpvita commit
  `77a1d39c1d6f11a55fe65a6ed9fe707bff5ede4b`, Copyright (c) 2015-2016 and
  2019 Sergi Granell, under the MIT licence packaged in the VPK and retained in
  `recomp/vita/third_party/libftpvita/LICENSE`;
- the Vita CRT decimal formatter's reduced adaptation of musl libc 1.2.5
  `fmt_fp`, Copyright © 2005-2020 Rich Felker, et al., under the MIT licence
  retained in `recomp/runtime/third_party/musl_fmt_fp/LICENSE`;
- Sean Barrett's public-domain stb_vorbis v1.04 (`stb_vorbis.c`, commit
  `b8e0530fdfbe`, 2014-08-28) vendored with two documented local patches
  in `recomp/runtime/third_party/stb_vorbis/` for the optional native
  Vorbis decoder;
- Rich Geldreich's public-domain/Unlicense miniz v1.15
  `tinfl_decompress()` coroutine;
- altered fixed-width CRC-32, Adler-32 and `inflate_flush` behavior from zlib
  1.1.4, Copyright (C) 1995-2002 Jean-loup Gailly and Mark Adler;
- a native PNG row-unfilter implementation validated against libpng 1.4.20,
  whose copyright and contributing-author notice is retained in
  [THIRD_PARTY.md](THIRD_PARTY.md).

`lpp-vita` supplied GPL-3.0 workflow prior art only; its source is not included.

Build and runtime dependencies include VitaSDK/vita-headers, vitaGL and its
shader stack, OpenAL Soft 1.19.1, kubridge, Khronos OpenGL Registry data and
Pillow (MIT-CMU) for bounded PC-manager preview decoding.
REPENTOGON/libzhl-derived interface research is identified in source and
provenance notes. Exact revisions and distribution obligations are recorded in
[THIRD_PARTY.md](THIRD_PARTY.md); that file remains the detailed authority.
