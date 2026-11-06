/*
 * The runtime's tinfl (recomp/runtime/host_vita_archive_miniz_native.c, the
 * fixed-width miniz v1.15 tinfl_decompress() the eboot's archive reader
 * uses) compiled a second time for the manager under its own symbol, the
 * same pattern as host_vita_native_png_miniz.c.  The eboot's object of that
 * source is a different target and is not affected.
 *
 * One hook differs from the eboot's archive entry.  tinfl v1.15 maps the
 * reserved length symbols 286/287 to length 0 (length_base[29..30] == 0)
 * and the reserved distance symbols 30/31 to distance 0 (dist_base[30..31]
 * == 0).  A length-0 match passes the output-boundary test
 * (`output_end - max(output_cur, source) < counter` is never true for 0)
 * and falls into the three-bytes-per-step copy loop, which stores three
 * bytes before testing the count: at the end of the output it writes past
 * output_end, and the fast literal loop that follows then keeps writing,
 * because its room test is unsigned.  The game's own files never contain
 * these symbols (tools/isaac_vita_repack.py ModelInflater rejects them as
 * "reserved symbols tinfl mis-handles"), but the manager decodes whatever
 * is on the memory card and must fail rather than crash.
 *
 * ISAAC_MINIZ_MATCH_COPY (host_vita_archive_miniz_native.c, the
 * `#ifdef ISAAC_MINIZ_MATCH_COPY` block after the boundary branch) is
 * consulted for every match that did not take the byte-wise boundary loop,
 * but only with the non-wrapping flag; every manager caller passes it.  The
 * hook claims (skips) both reserved forms before any byte is stored, raises
 * *reserved_symbol, and lets tinfl's own copy handle every normal match:
 * a length-0 match then advances by 0 bytes, a distance-0 match (only
 * reachable with length >= 3, i.e. inside the checked room) leaves bytes
 * that were never written - the caller discards the whole result anyway.
 */
#include "manager_tinfl.h"

#include <stdint.h>

static int manager_tinfl_refuse(uint32_t *reserved_symbol)
{
    *reserved_symbol = 1U;
    return 1;
}

#define ISAAC_MINIZ_NATIVE_ENTRY manager_tinfl_decompress
#define ISAAC_MINIZ_HISTORY_ARGUMENT , uint32_t *reserved_symbol
#define ISAAC_MINIZ_MATCH_COPY(out, src, n, d, history)                        \
    (((n) < 3U || (d) == 0U) ? manager_tinfl_refuse(reserved_symbol) : 0)
#include "host_vita_archive_miniz_native.c"
#undef ISAAC_MINIZ_MATCH_COPY
#undef ISAAC_MINIZ_HISTORY_ARGUMENT
