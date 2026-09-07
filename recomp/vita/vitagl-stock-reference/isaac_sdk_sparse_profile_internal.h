#ifndef ISAAC_SDK_SPARSE_PROFILE_INTERNAL_H
#define ISAAC_SDK_SPARSE_PROFILE_INTERNAL_H

#include "isaac_sdk_sparse_profile.h"
#if !defined(HAVE_ISAAC_GL_TIME_SDK_SPARSE) || !HAVE_ISAAC_GL_TIME_SDK_SPARSE || \
    !defined(HAVE_ISAAC_GL_TIME_PROFILE) || \
    !defined(HAVE_ISAAC_CANONICAL_QUAD_ZERO_COPY)
#error "SDK-sparse mode requires the existing GL_TIME and canonical draw paths"
#endif

extern IsaacSdkSparseProfile isaac_sdk_sparse;
int isaacSdkSparseSelectCanonical(void);
uint64_t isaacSdkSparseClock(void);
void isaacSdkSparseAdd(unsigned bucket, uint64_t start, uint64_t end);

/* Included before the optional nearest-draw headers in draw.c. The adapter
 * reaches the real SDK call, not the helper that may return without drawing.
 * Its arguments are evaluated once; its original SDK return is preserved.
 * A stack-linked span keeps a nested canonical request from corrupting its
 * caller's sampling state; such a caller is not a one-draw paired sample. */
#ifdef ISAAC_SDK_SPARSE_DRAW_OWNER
typedef struct IsaacSdkSparseSpan {
    int selected;
    uint32_t issued, nested;
    uint64_t started, draw_started, draw_ended;
    struct IsaacSdkSparseSpan *previous;
} IsaacSdkSparseSpan;
static IsaacSdkSparseSpan *isaac_sdk_active_span;
static inline int isaacSdkSparseDraw(SceGxmContext *context,
        SceGxmPrimitiveType primitive, SceGxmIndexFormat format,
        const void *indices, unsigned int count)
{
    IsaacSdkSparseSpan *span = isaac_sdk_active_span;
    uint64_t start = 0;
    if (span) {
        ++isaac_sdk_sparse.canonical_issued;
        ++span->issued;
        if (span->selected) start = isaacSdkSparseClock();
    } else {
        ++isaac_sdk_sparse.other_draw_calls;
    }
    int result = sceGxmDraw(context, primitive, format, indices, count);
    if (span) {
        if (span->selected) {
            span->draw_ended = isaacSdkSparseClock();
            span->draw_started = start;
        }
        /* Same SDK success test as isaac_laser_light_nearest.h. This covers
         * all issued canonical calls, not just the selected timing subset. */
        if (result != 0) ++isaac_sdk_sparse.canonical_draw_errors;
    }
    return result;
}
#define sceGxmDraw isaacSdkSparseDraw
#define ISAAC_SDK_CANONICAL_BEGIN() \
    IsaacSdkSparseSpan isaac_sdk_span = { \
        isaacSdkSparseSelectCanonical(), 0, 0, 0, 0, 0, isaac_sdk_active_span }; \
    if (isaac_sdk_active_span) isaac_sdk_active_span->nested = 1; \
    isaac_sdk_active_span = &isaac_sdk_span; \
    if (isaac_sdk_span.selected) isaac_sdk_span.started = isaacSdkSparseClock()
#define ISAAC_SDK_CANONICAL_END() \
    do { if (isaac_sdk_span.selected) { \
        uint64_t isaac_sdk_end = isaacSdkSparseClock(); \
        if (isaac_sdk_span.issued == 1u && !isaac_sdk_span.nested) { \
            isaacSdkSparseAdd(ISAAC_SDK_DRAW, isaac_sdk_span.draw_started, isaac_sdk_span.draw_ended); \
            isaacSdkSparseAdd(ISAAC_SDK_CANONICAL, isaac_sdk_span.started, isaac_sdk_end); \
        } else ++isaac_sdk_sparse.canonical_unpaired; \
    } isaac_sdk_active_span = isaac_sdk_span.previous; } while (0)
#endif
#define ISAAC_SDK_BEGIN(name) uint64_t name = isaacSdkSparseClock()
#define ISAAC_SDK_END(kind, name) \
    isaacSdkSparseAdd((kind), (name), isaacSdkSparseClock())

#endif
