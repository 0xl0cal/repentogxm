/* The vendored zlib 1.1.4 inflate_codes, compiled unmodified, plus the
 * compile-time pins of the ILP32 struct layout the seam overlays on guest
 * memory (host_vita_native_inflate_layout.h).  Compiled with
 * -include izlib114_prefix_codes.h (iz_ namespace + hook routing).
 *
 * struct inflate_codes_state is private to infcodes.c, which is why the pins
 * live in the TU that compiles it.  Any drift fails the eboot build, the ARM
 * cross-compile in test_vita_native_inflate.py and the ILP32 host oracle. */
#include "infcodes.c"

#include <stddef.h>

#include "host_vita_native_inflate_layout.h"

#if defined(__SIZEOF_POINTER__) && __SIZEOF_POINTER__ == 4

_Static_assert(sizeof(uInt) == 4 && sizeof(uLong) == 4 && sizeof(Bytef *) == 4,
               "zlib 1.1.4 ILP32 scalar widths");

/* z_stream public fields (guest ABI). */
_Static_assert(offsetof(z_stream, next_in)   == ISAAC_NI_Z_NEXT_IN,   "z.next_in");
_Static_assert(offsetof(z_stream, avail_in)  == ISAAC_NI_Z_AVAIL_IN,  "z.avail_in");
_Static_assert(offsetof(z_stream, total_in)  == ISAAC_NI_Z_TOTAL_IN,  "z.total_in");
_Static_assert(offsetof(z_stream, next_out)  == ISAAC_NI_Z_NEXT_OUT,  "z.next_out");
_Static_assert(offsetof(z_stream, avail_out) == ISAAC_NI_Z_AVAIL_OUT, "z.avail_out");
_Static_assert(offsetof(z_stream, total_out) == ISAAC_NI_Z_TOTAL_OUT, "z.total_out");
_Static_assert(offsetof(z_stream, msg)       == ISAAC_NI_Z_MSG,       "z.msg");
_Static_assert(offsetof(z_stream, state)     == ISAAC_NI_Z_STATE,     "z.state");
_Static_assert(offsetof(z_stream, adler)     == ISAAC_NI_Z_ADLER,     "z.adler");
_Static_assert(sizeof(z_stream)              == ISAAC_NI_Z_BYTES,     "z size");

/* inflate_blocks_state (infutil.h). */
_Static_assert(offsetof(struct inflate_blocks_state, mode)    == ISAAC_NI_BLK_MODE,    "blk.mode");
_Static_assert(offsetof(struct inflate_blocks_state, sub)     == ISAAC_NI_BLK_CODES,   "blk.sub.decode.codes");
_Static_assert(offsetof(struct inflate_blocks_state, bitk)    == ISAAC_NI_BLK_BITK,    "blk.bitk");
_Static_assert(offsetof(struct inflate_blocks_state, bitb)    == ISAAC_NI_BLK_BITB,    "blk.bitb");
_Static_assert(offsetof(struct inflate_blocks_state, hufts)   == ISAAC_NI_BLK_HUFTS,   "blk.hufts");
_Static_assert(offsetof(struct inflate_blocks_state, window)  == ISAAC_NI_BLK_WINDOW,  "blk.window");
_Static_assert(offsetof(struct inflate_blocks_state, end)     == ISAAC_NI_BLK_END,     "blk.end");
_Static_assert(offsetof(struct inflate_blocks_state, read)    == ISAAC_NI_BLK_READ,    "blk.read");
_Static_assert(offsetof(struct inflate_blocks_state, write)   == ISAAC_NI_BLK_WRITE,   "blk.write");
_Static_assert(offsetof(struct inflate_blocks_state, checkfn) == ISAAC_NI_BLK_CHECKFN, "blk.checkfn");
_Static_assert(offsetof(struct inflate_blocks_state, check)   == ISAAC_NI_BLK_CHECK,   "blk.check");
_Static_assert(sizeof(struct inflate_blocks_state)            == ISAAC_NI_BLK_BYTES,   "blk size");

/* inflate_codes_state (infcodes.c). */
_Static_assert(offsetof(struct inflate_codes_state, mode)          == ISAAC_NI_CS_MODE,  "cs.mode");
_Static_assert(offsetof(struct inflate_codes_state, len)           == ISAAC_NI_CS_LEN,   "cs.len");
_Static_assert(offsetof(struct inflate_codes_state, sub.code.tree) == ISAAC_NI_CS_SUB0,  "cs.sub.code.tree");
_Static_assert(offsetof(struct inflate_codes_state, sub.code.need) == ISAAC_NI_CS_SUB1,  "cs.sub.code.need");
_Static_assert(offsetof(struct inflate_codes_state, sub.lit)       == ISAAC_NI_CS_SUB0,  "cs.sub.lit");
_Static_assert(offsetof(struct inflate_codes_state, sub.copy.get)  == ISAAC_NI_CS_SUB0,  "cs.sub.copy.get");
_Static_assert(offsetof(struct inflate_codes_state, sub.copy.dist) == ISAAC_NI_CS_SUB1,  "cs.sub.copy.dist");
_Static_assert(offsetof(struct inflate_codes_state, lbits)         == ISAAC_NI_CS_LBITS, "cs.lbits");
_Static_assert(offsetof(struct inflate_codes_state, dbits)         == ISAAC_NI_CS_DBITS, "cs.dbits");
_Static_assert(offsetof(struct inflate_codes_state, ltree)         == ISAAC_NI_CS_LTREE, "cs.ltree");
_Static_assert(offsetof(struct inflate_codes_state, dtree)         == ISAAC_NI_CS_DTREE, "cs.dtree");
_Static_assert(sizeof(struct inflate_codes_state)                  == ISAAC_NI_CS_BYTES, "cs size");

/* Huffman table entry: {Exop, Bits, pad, pad, base}; sub-table links are
 * index offsets (t + t->base), so the tables hold no pointers. */
_Static_assert(sizeof(inflate_huft) == ISAAC_NI_HUFT_BYTES, "huft size");
_Static_assert(offsetof(inflate_huft, base) == 4, "huft.base");
_Static_assert(offsetof(inflate_huft, word.what.Exop) == 0, "huft.exop");
_Static_assert(offsetof(inflate_huft, word.what.Bits) == 1, "huft.bits");
_Static_assert(MANY == ISAAC_NI_MANY, "MANY");
_Static_assert(START == 0 && LEN == ISAAC_NI_MODE_LEN && LENEXT == ISAAC_NI_MODE_LENEXT &&
               DIST == ISAAC_NI_MODE_DIST && DISTEXT == ISAAC_NI_MODE_DISTEXT &&
               BADCODE == ISAAC_NI_MODE_BADCODE, "inflate_codes modes");

/* The mode enums are 4 bytes under the PE's MSVC (and int-enum host ABIs)
 * and 1 byte under arm-vita-eabi's default short enums.  Both are exact for
 * the in-place run: the translated side stores the modes as dwords 0..9 and
 * the guard verifies the dword, so the upper three bytes are zero and a byte
 * read or write observes/leaves the same value; the fields after them keep
 * their 4-byte alignment either way (pinned above). */
_Static_assert(sizeof(inflate_codes_mode) == 1 || sizeof(inflate_codes_mode) == 4,
               "inflate_codes mode width");
_Static_assert(sizeof(inflate_block_mode) == 1 || sizeof(inflate_block_mode) == 4,
               "inflate_block mode width");

#endif /* ILP32 */
