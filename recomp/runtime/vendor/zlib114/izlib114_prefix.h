/* Namespacing prefix for the vendored zlib 1.1.4 inflate subset.
 *
 * The PS Vita port already links vitaGL's system zlib for other consumers.
 * The native inflate seam must run the *exact same* algorithm as the game's
 * statically linked zlib 1.1.4 (the one libpng calls) so its output, return
 * codes and z_stream field evolution are identical by construction.  To carry
 * that source into the ELF without clashing with the system zlib symbols,
 * every zlib 1.1.4 public and cross-TU symbol is renamed with the "iz_"
 * prefix.  The .c files are compiled unmodified with -include of this header
 * (see recomp/vita/CMakeLists.txt, ISAAC_VITA_NATIVE_INFLATE).
 *
 * Vendored source: zlib 1.1.4 (public inflate path only), zlib licence.
 */
#ifndef IZLIB114_PREFIX_H
#define IZLIB114_PREFIX_H

#define adler32                    iz_adler32
#define huft_build                 iz_huft_build
#define inflate                    iz_inflate
#define inflate_blocks             iz_inflate_blocks
#define inflate_blocks_free        iz_inflate_blocks_free
#define inflate_blocks_new         iz_inflate_blocks_new
#define inflate_blocks_reset       iz_inflate_blocks_reset
#define inflate_blocks_sync_point  iz_inflate_blocks_sync_point
#define inflate_codes              iz_inflate_codes
#define inflate_codes_free         iz_inflate_codes_free
#define inflate_codes_new          iz_inflate_codes_new
#define inflate_copyright          iz_inflate_copyright
#define inflateEnd                 iz_inflateEnd
#define inflate_fast               iz_inflate_fast
#define inflate_flush              iz_inflate_flush
#define inflateInit_               iz_inflateInit_
#define inflateInit2_              iz_inflateInit2_
#define inflate_mask               iz_inflate_mask
#define inflateReset               iz_inflateReset
#define inflate_set_dictionary     iz_inflate_set_dictionary
#define inflateSetDictionary       iz_inflateSetDictionary
#define inflateSync                iz_inflateSync
#define inflateSyncPoint           iz_inflateSyncPoint
#define inflate_trees_bits         iz_inflate_trees_bits
#define inflate_trees_dynamic      iz_inflate_trees_dynamic
#define inflate_trees_fixed        iz_inflate_trees_fixed
#define zcalloc                    iz_zcalloc
#define zcfree                     iz_zcfree
#define z_errmsg                   iz_z_errmsg
#define zError                     iz_zError
#define zlibVersion                iz_zlibVersion
#define z_stream_s                 iz_z_stream_s

#endif /* IZLIB114_PREFIX_H */
