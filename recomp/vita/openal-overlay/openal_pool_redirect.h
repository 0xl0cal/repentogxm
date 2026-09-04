#ifndef ISAAC_VITA_OPENAL_POOL_REDIRECT_H
#define ISAAC_VITA_OPENAL_POOL_REDIRECT_H

/* Include the libc declarations before replacing the six allocator tokens.
 * The pinned OpenAL 1.19.1 archive is C-only on Vita.  This forced include
 * makes every direct archive allocation resolve through one audited domain;
 * opaque allocations performed inside stdio/pthread remain owned by libc. */
#include <stdlib.h>
#include <string.h>

#include "host_vita_openal_pool.h"

#define malloc        isaac_vita_openal_pool_malloc
#define calloc        isaac_vita_openal_pool_calloc
#define realloc       isaac_vita_openal_pool_realloc
#define free          isaac_vita_openal_pool_free
#define aligned_alloc isaac_vita_openal_pool_aligned_alloc
#define strdup        isaac_vita_openal_pool_strdup

#endif
