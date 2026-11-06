#ifndef ISAAC_KAGE_VITA_BACKDROP_WATERGFX_H
#define ISAAC_KAGE_VITA_BACKDROP_WATERGFX_H

#include <stdint.h>

/*
 * Log-only helpers for the two generated seams of
 * ISAAC_VITA_BACKDROP_WATERGFX_GUARD (gen_all.py, GitHub issue 5 items
 * 5.4/5.17).  Both take guest addresses (guest address == host address) and
 * neither writes guest memory or the CPU state; the generated owner performs
 * the skip itself.
 */

/* Backdrop::LoadGraphics (sub_00498850) skipped the wrap-mode call on a NULL
 * watergfx holder.  `backdrop` is the Backdrop object (ESI).  Logs once per
 * backdrop id. */
void isaac_vita_backdrop_watergfx_skipped(uint32_t backdrop);

/* The image loader (sub_0056c7e0) got NULL from its path resolver for the
 * NUL-terminated path at `path` (EDI) and returns an empty pair.  Logs once
 * per distinct path while the bounded table has room. */
void isaac_vita_backdrop_watergfx_resolve_failed(uint32_t path);

#endif
