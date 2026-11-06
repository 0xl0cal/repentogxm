#ifndef GUEST_ENTRY_CENSUS_H
#define GUEST_ENTRY_CENSUS_H

/* Body-entry hooks of the profiling censuses (guest.h includes this only when
 * one of them is ON; every other build keeps guest.h's no-op hook).
 *
 * ISAAC_VITA_PROFILE_FUNCTION_ENTRIES: one corpus-wide counter (ph120.ik ent=).
 *
 * ISAAC_VITA_PROFILE_FUNCTION_ENTRY_CENSUS: entries of the render watch list
 * (kage_vita_render_watch.h) per sampler phase, printed by the guest sampler
 * as ph120.fe.  guest_entry_census_ids.h is generated at configure time by
 * recomp/vita/vita_render_watch.py from the compiled corpus's
 * guest_coverage.json; every generated body passes a constant coverage id
 * (guest_coverage_function(170U) at sub_00004b60's entry), so a watched entry
 * costs one phase load and one increment and an unwatched one compiles to
 * nothing.  An entry is a translated body entry: a direct call, a tail jump,
 * an indirect dispatch, a cross-root label hook or a runtime seam that replays
 * the hook; a native seam that replaces a body without replaying it is not
 * counted.  The phase is the main thread's (g_kage_vita_guest_sampler_phase,
 * written only by the phase hooks).  The Vita runtime has exactly one guest
 * CPU, bound to the main thread (gl_bridge.c's FASTDISPATCH note), so every
 * translated body runs there: the plain increment races with nothing, and
 * the ph120.fe reporter reads and zeroes the rows on that same thread at the
 * window boundary. */
#include <stdint.h>

#if defined(ISAAC_VITA_PROFILE_FUNCTION_ENTRIES)
extern uint32_t g_guest_function_entries;
#endif

#if defined(ISAAC_VITA_PROFILE_FUNCTION_ENTRY_CENSUS)
/* The phase word and the reporter live in kage_vita_guest_sampler.c, which
 * CMake builds only with ISAAC_VITA_GUEST_SAMPLER (a FATAL_ERROR refuses the
 * census without it).  The sampler macro itself is source-scoped and never
 * reaches the generated units that include this header, so it is not
 * checked here. */
# include "guest_entry_census_ids.h"
# define GUEST_ENTRY_CENSUS_PHASES 8U
extern volatile uint32_t g_kage_vita_guest_sampler_phase;
extern uint32_t g_guest_entry_census[GUEST_ENTRY_CENSUS_PHASES]
                                    [GUEST_ENTRY_CENSUS_COUNT];
#endif

static inline void guest_profile_note_function_entry(uint32_t id)
{
#if defined(ISAAC_VITA_PROFILE_FUNCTION_ENTRIES)
    ++g_guest_function_entries;
#endif
#if defined(ISAAC_VITA_PROFILE_FUNCTION_ENTRY_CENSUS)
    {
        uint32_t slot = guest_entry_census_slot(id);
        if (slot != UINT32_MAX)
            ++g_guest_entry_census[g_kage_vita_guest_sampler_phase &
                                   (GUEST_ENTRY_CENSUS_PHASES - 1U)][slot];
    }
#else
    (void)id;
#endif
}

/* Three runtime seams (host_vita_anm2_pool_init.c, host_vita_archive_xor.c,
 * host_vita_shader_sampler_fastpath.c) replay only the corpus-wide counter and
 * call the hook with an empty argument: `+ 0U` turns that into id 0
 * (sub_00001000), which vita_render_watch.py refuses to watch, so they add no
 * census entry. */
#define GUEST_PROFILE_NOTE_FUNCTION_ENTRY(id) \
    guest_profile_note_function_entry((id + 0U))

#endif
