# musl decimal floating-point formatter provenance

`musl_fmt_fp.h` is a reduced and altered adaptation of musl libc's
allocation-free `fmt_fp` implementation.  It retains only lower-case decimal
`%f` and `%g`, targets IEEE-754 binary64 `double`, writes to a bounded memory
sink, and preserves the caller's `errno`.  Width, flags, locale-specific radix
characters, hexadecimal formatting and `long double` are deliberately absent.

- upstream: <https://git.musl-libc.org/cgit/musl>
- mirror used for the pin: <https://github.com/bminor/musl>
- release/tag: `v1.2.5`
- pinned commit: `0784374d561435f7c787a555aeab8ede699ed298`
- upstream commit date: 2024-02-29 21:07:33 -0500
- adapted source: `src/stdio/vfprintf.c`, `fmt_fp` and `fmt_u`
- pinned source Git blob: `497c5e19372dcb6d7f742325a36979dd2231eb75`
- pinned 16,763-byte LF source file SHA-256:
  `5ae7748a197b3c334f2b6791f8613940f27d3bac6f27bcfa73706222fff4ffa8`
- pinned upstream `COPYRIGHT` Git blob:
  `c1628e9ac84f927fe791124ec172fb61c760bb9b`
- pinned upstream LF `COPYRIGHT` SHA-256:
  `f9bc4423732350eb0b3f7ed7e91d530298476f8fec0c6c427a1c04ade22655af`
- licence: MIT; the standard MIT grant from upstream `COPYRIGHT` is retained
  verbatim as `LICENSE` (the unrelated contributor list and notices for other
  musl components are not copied)

The adaptation replaces musl's `FILE`/padding interface with a truncating
memory sink, specializes the compile-time bounds from `long double` to
`double`, prefixes all internal names, validates the six Isaac runtime formats,
and restores `errno` on every return.  The base-1e9 expansion and musl's
rounding decision are otherwise kept structurally intact.

Alternatives reviewed but not copied were official Ryu (exact and
allocation-free, but substantially larger for fixed precision plus `%g`) and
nanoprintf (small, but its own documentation does not promise correctly rounded
conversion across the full binary64 range).
