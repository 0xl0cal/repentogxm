/* Frozen constants of the native zlib 1.1.4 inflate_codes seam: PE addresses,
 * corpus census identities and the ILP32 zlib 1.1.4 field offsets the seam
 * overlays on guest memory.  No guest.h dependency so the vendored-code
 * wrapper (host_vita_native_inflate_zlib114_codes.c) can pin the offsets
 * against the real zlib structs with _Static_assert under the eboot toolchain.
 * See host_vita_native_inflate.h for the seam itself. */
#ifndef ISAAC_HOST_VITA_NATIVE_INFLATE_LAYOUT_H
#define ISAAC_HOST_VITA_NATIVE_INFLATE_LAYOUT_H

/* Frozen PE addresses (RVAs; VA = GUEST_IMAGE_BASE + RVA). */
#define ISAAC_NI_INFLATE_RVA            0x005c38a0U
#define ISAAC_NI_INFLATE_BLOCKS_RVA     0x005ce7d0U
#define ISAAC_NI_INFLATE_CODES_RVA      0x005d6d80U
#define ISAAC_NI_INFLATE_FAST_RVA       0x005d7610U
#define ISAAC_NI_INFLATE_FLUSH_RVA      0x005d6720U
#define ISAAC_NI_ADLER32_RVA            0x005cf3d0U
/* return word of the only call site: `call 0x5d6d80` at 0x5cf07b. */
#define ISAAC_NI_CODES_RETURN_RVA       0x005cf080U
/* .rdata messages inflate_codes / inflate_fast can store into z->msg. */
#define ISAAC_NI_MSG_BAD_LITLEN_RVA     0x0076a170U
#define ISAAC_NI_MSG_BAD_DIST_RVA       0x0076a158U
/* inffixed.h tables the PE's inflate_blocks hands out (.data). */
#define ISAAC_NI_FIXED_TD_RVA           0x007e8570U   /*  32 hufts */
#define ISAAC_NI_FIXED_TL_RVA           0x007e8670U   /* 512 hufts */
#define ISAAC_NI_FIXED_TD_HUFTS         32U
#define ISAAC_NI_FIXED_TL_HUFTS         512U

/* Generated-corpus census identities (guest_coverage / guest_table). */
#define ISAAC_NI_INFLATE_CODES_COVERAGE 12193U
#define ISAAC_NI_INFLATE_FAST_COVERAGE  12195U
#define ISAAC_NI_INFLATE_FLUSH_COVERAGE 12191U
#define ISAAC_NI_ADLER32_COVERAGE       12124U
#define ISAAC_NI_MEMCPY_IAT_RVA         0x00606488U   /* VCRUNTIME140!memcpy */
#define ISAAC_NI_MEMCPY_IMPORT_ID       281U
#define ISAAC_NI_MEMCPY_THUNK_COVERAGE  12570U        /* sub_005ec14c */

/* zlib 1.1.4 constants the guards depend on. */
#define ISAAC_NI_MANY                   1440U         /* hufts per block state */
#define ISAAC_NI_HUFT_BYTES             8U
#define ISAAC_NI_WINDOW_MIN             0x100U        /* 1 << 8 */
#define ISAAC_NI_WINDOW_MAX             0x8000U       /* 1 << 15 */
#define ISAAC_NI_MODE_BADCODE           9U            /* last inflate_codes mode */
#define ISAAC_NI_MODE_LEN               1U
#define ISAAC_NI_MODE_LENEXT            2U
#define ISAAC_NI_MODE_DIST              3U
#define ISAAC_NI_MODE_DISTEXT           4U

/* ILP32 zlib 1.1.4 field offsets (the corpus uses them verbatim, e.g.
 * `mov ecx, [ebx + 4]` = s->sub.decode.codes at 0x5d6d90). */
enum {
    ISAAC_NI_Z_NEXT_IN    = 0x00,
    ISAAC_NI_Z_AVAIL_IN   = 0x04,
    ISAAC_NI_Z_TOTAL_IN   = 0x08,
    ISAAC_NI_Z_NEXT_OUT   = 0x0c,
    ISAAC_NI_Z_AVAIL_OUT  = 0x10,
    ISAAC_NI_Z_TOTAL_OUT  = 0x14,
    ISAAC_NI_Z_MSG        = 0x18,
    ISAAC_NI_Z_STATE      = 0x1c,
    ISAAC_NI_Z_ADLER      = 0x30,
    ISAAC_NI_Z_BYTES      = 0x38,

    ISAAC_NI_BLK_MODE     = 0x00,
    ISAAC_NI_BLK_CODES    = 0x04,   /* sub.decode.codes */
    ISAAC_NI_BLK_BITK     = 0x1c,
    ISAAC_NI_BLK_BITB     = 0x20,
    ISAAC_NI_BLK_HUFTS    = 0x24,
    ISAAC_NI_BLK_WINDOW   = 0x28,
    ISAAC_NI_BLK_END      = 0x2c,
    ISAAC_NI_BLK_READ     = 0x30,
    ISAAC_NI_BLK_WRITE    = 0x34,
    ISAAC_NI_BLK_CHECKFN  = 0x38,
    ISAAC_NI_BLK_CHECK    = 0x3c,
    ISAAC_NI_BLK_BYTES    = 0x40,

    ISAAC_NI_CS_MODE      = 0x00,
    ISAAC_NI_CS_LEN       = 0x04,
    ISAAC_NI_CS_SUB0      = 0x08,   /* code.tree | lit | copy.get */
    ISAAC_NI_CS_SUB1      = 0x0c,   /* code.need | copy.dist */
    ISAAC_NI_CS_LBITS     = 0x10,
    ISAAC_NI_CS_DBITS     = 0x11,
    ISAAC_NI_CS_LTREE     = 0x14,
    ISAAC_NI_CS_DTREE     = 0x18,
    ISAAC_NI_CS_BYTES     = 0x1c,

    /* inflate()'s internal_state: blocks pointer (the flush seam pins it too). */
    ISAAC_NI_IS_BLOCKS    = 0x14
};

#endif /* ISAAC_HOST_VITA_NATIVE_INFLATE_LAYOUT_H */
