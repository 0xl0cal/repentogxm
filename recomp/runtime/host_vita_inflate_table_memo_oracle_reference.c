/* Host oracle only: the untouched archive tinfl decoder under a second name.
 * Compiled WITHOUT ISAAC_VITA_INFLATE_TABLE_MEMO so the oracle can run the
 * memoised decoder (host_vita_archive_miniz_native.c built with the knob) and
 * this reference on the same pieces and compare output bytes and the full
 * coroutine state after every call.  Never part of the eboot. */
#if defined(ISAAC_VITA_INFLATE_TABLE_MEMO)
#error The reference wrapper must be compiled without the memo knob
#endif
#define ISAAC_MINIZ_NATIVE_ENTRY isaac_vita_inflate_memo_oracle_reference
#include "host_vita_archive_miniz_native.c"
