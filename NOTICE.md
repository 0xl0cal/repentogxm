# Notices

This project is not affiliated with or endorsed by Edmund McMillen, Nicalis,
Valve, Sony, or the maintainers of the dependencies below. Their names,
copyrights, trademarks, licences and notices remain their own.

Tracked Vita presentation artwork consists of nine byte-pinned package images
under `recomp/vita/assets/sce_sys/`, all showing Isaac/Nicalis game artwork:
`bg0.png` and `nicalis.png` from the Isaac/Nicalis Vita package lineage, and
`icon0.png`, `pic0.png`, `startup.png` and the four LiveArea title strips
`1.png`-`4.png` from the community LiveArea contributed by GitHub user
Lordchickenblaster in GitHub issue #9 (laid out by
`livearea/contents/template.xml`). The contributor credit and the pinned
hashes are not a claim of ownership or a blanket redistribution licence for
the game artwork. Playable game resources, the PC executable, saves, keys,
PlayStation firmware/modules and `libshacccg.suprx` are not included.

The Manager UI2 font atlases `recomp/vita/assets/manager/font-*.imf` are
rasterised from DejaVu Sans and DejaVu Sans Bold by
`tools/manager_font_bake.py`. DejaVu fonts are derived from Bitstream Vera:
Copyright (c) 2003 by Bitstream, Inc. (Bitstream Vera is a trademark of
Bitstream, Inc.); glyphs imported from Arev fonts are Copyright (c) 2006 by
Tavmjong Bah; DejaVu changes are in the public domain. The licence is retained
in `recomp/vita/third_party/dejavu/LICENSE` and packaged in the VPK as
`licenses/dejavu.txt` whenever the atlases are (`ISAAC_VITA_MANAGER_UI2=ON`).

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
shader stack, OpenAL Soft 1.19.1, kubridge, Khronos OpenGL Registry data,
Pillow (MIT-CMU) for bounded PC-manager preview decoding and, for the PC-only
font bake, Pillow with FreeType and fontTools (MIT).
REPENTOGON/libzhl-derived interface research is identified in source and
provenance notes. Exact revisions and distribution obligations are recorded in
[THIRD_PARTY.md](THIRD_PARTY.md); that file remains the detailed authority.
