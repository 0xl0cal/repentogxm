#ifndef MANUAL_PORTABLE_H
#define MANUAL_PORTABLE_H

#include "guest.h"

/* Portable replacements for game-owned utility boundaries whose temporary
 * MSVC iostream objects never escape.  Keeping the boundary at game semantics
 * avoids reproducing implementation-private stream vtables in guest memory. */
#define GUEST_MANUAL_PORTABLE_FUNCTIONS(X) \
    X(00259650, 0x00259650u, SplitAppend) \
    X(002597b0, 0x002597b0u, SplitConstruct) \
    X(002599c0, 0x002599c0u, PathFromBase) \
    X(002c8700, 0x002c8700u, PerformanceCounters)

#define GUEST_DECLARE_PORTABLE(symbol, rva, label) \
    void sub_##symbol(CPU *__restrict c); \
    void guest_original_##symbol(CPU *__restrict c);
GUEST_MANUAL_PORTABLE_FUNCTIONS(GUEST_DECLARE_PORTABLE)
#undef GUEST_DECLARE_PORTABLE

#endif
