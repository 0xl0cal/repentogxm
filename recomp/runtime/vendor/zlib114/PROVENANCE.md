# Vendored zlib 1.1.4 (inflate side only)

Source: `zlib-1.1.4.tar.gz` (released 2002-03-11, zlib licence)
- md5    `abc405d0bdd3ee22782d7aa20e440f08`
- sha256 `9e3e973174f9910fd51539ef9ce94c86a3943d4f897fab8e9adf4b19e6a8291e`

Every `.c`/`.h` here is byte-identical to the tarball file of the same name
(`recomp/test_vita_native_inflate.py` pins their sha256).  Nothing is
modified; namespacing and hook routing happen only through the `-include`
prefix headers (`izlib114_prefix*.h`), which are not part of zlib.

Why 1.1.4 and not the vitasdk zlib: the frozen PE statically links zlib 1.1.4
(inflate_blocks / inflate_codes / inflate_flush architecture, `inflate_mask`
table at RVA 0x73df70, fixed Huffman tables at RVA 0x7e8570/0x7e8670, the
1.1.4 "wrap the copy source into the window" loops in inflate_codes COPY and
inflate_fast).  The native seam (`host_vita_native_inflate.c`) runs the
identical algorithm on the guest's own zlib state in place, so per-call
behaviour equals the translated body by construction.

Files linked into the eboot when `ISAAC_VITA_NATIVE_INFLATE=ON`:
`infcodes.c` (via `host_vita_native_inflate_zlib114_codes.c`), `inffast.c`,
`infutil.c`, `adler32.c`.  The remaining files (`inflate.c`, `infblock.c`,
`inftrees.c`, `zutil.c`, `inffixed.h`) are only used by the host oracles as
the pristine reference decoder.
