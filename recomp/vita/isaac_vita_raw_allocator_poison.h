#ifndef ISAAC_VITA_RAW_ALLOCATOR_POISON_H
#define ISAAC_VITA_RAW_ALLOCATOR_POISON_H

/*
 * This header is a production compiler boundary, not an allocator shim.
 * CMake force-includes it into every translated Vita target source.  Include
 * the owning libc declarations before poisoning the identifiers so later
 * ordinary includes are protected by their normal include guards.
 */
#if !defined(__vita__) || !defined(ISAAC_VITA_RAW_ALLOCATOR_GATE) || \
    ISAAC_VITA_RAW_ALLOCATOR_GATE != 1
#error "the raw allocator poison is only valid on the gated Vita target"
#endif

#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#if !defined(ISAAC_VITA_RAW_ALLOCATOR_EXEMPT)
/* Object-like poisoning rejects calls, address-taking and macro aliases. */
#pragma GCC poison malloc calloc realloc free memalign aligned_alloc strdup
#elif ISAAC_VITA_RAW_ALLOCATOR_EXEMPT != 1
#error "ISAAC_VITA_RAW_ALLOCATOR_EXEMPT must be exactly 1"
#endif

#endif
