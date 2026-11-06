/* GL recorder: the sceKernelFreeMemBlock wrap for links without the debug
 * overlay.  CMake links it only with ISAAC_VITA_GL_RECORD_ADMISSION_CACHE_STANDALONE=ON
 * and no overlay (ISAAC_GL_FREE_WRAP_BY_OVERLAY unset); when the overlay owns
 * the wrap, host_vita_debug_overlay.c's __wrap_sceKernelFreeMemBlock does the
 * same bump before the same real free.  Why the bump keeps the recorder's
 * admission cache exact: gl_record_backend.c, the memory admission cache
 * comment.
 *
 * Threads: any thread that frees a memblock through this link (--wrap
 * redirects every undefined reference in every input object, vitaGL and the
 * static libraries included).  No lock, allocation, stdio or logging here. */
#if !defined(ISAAC_GL_RECORD_FREE_WRAP_OWNER_RECORDER)
# error "gl_record_free_wrap.c is linked only as the recorder's free-wrap owner (CMake)"
#endif
#if defined(ISAAC_GL_RECORD_FREE_WRAP_ORACLE)
# include "gl_record_free_wrap_oracle.h"   /* SceUID + recording fakes */
#else
# if !defined(ISAAC_VITA_GL_THREAD_WORKER) || !ISAAC_VITA_GL_THREAD_WORKER
#  error "the recorder's free wrap needs the GL render thread (ISAAC_VITA_GL_THREAD_WORKER)"
# endif
# include <psp2/kernel/sysmem.h>
# include "gl_record_backend.h"            /* isaac_gl_record_memblock_freeing */
#endif

/* Prototypes as host_vita_debug_overlay.c declares them. */
int __real_sceKernelFreeMemBlock(SceUID uid);
int __wrap_sceKernelFreeMemBlock(SceUID uid);

int __wrap_sceKernelFreeMemBlock(SceUID uid)
{
#if defined(ISAAC_GL_RECORD_FREE_WRAP_ORACLE_MUTATE_LATE_BUMP)
    /* Oracle self-test only: the bump after the free must FAIL the oracle. */
    int result = __real_sceKernelFreeMemBlock(uid);
    isaac_gl_record_memblock_freeing();
    return result;
#else
    /* Before the free, so a producer that loads the generation after this
     * point re-queries.  One that loaded it before and copies after the
     * unmap is the check-then-copy window an uncached query has too; the
     * seqlock that closes it is a follow-up, shared
     * with the overlay's wrap. */
    isaac_gl_record_memblock_freeing();
    return __real_sceKernelFreeMemBlock(uid);
#endif
}
