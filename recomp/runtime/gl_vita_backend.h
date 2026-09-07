#ifndef GL_VITA_BACKEND_H
#define GL_VITA_BACKEND_H

#include <stddef.h>
#include <stdint.h>

#if defined(ISAAC_VITA_TEXTURE_CHURN_PROFILE) && \
    !defined(ISAAC_VITA_PHASE_PROFILE)
#error "texture-churn profile requires the aggregate phase profile"
#endif

/* Driver-facing calls made by the translated game during one aggregate
 * profile window.  The counters live at the typed GL boundary, so internal
 * setup/diagnostic GL calls are not misattributed to game rendering.  The
 * suppressed counters are requests deliberately stopped at that boundary;
 * add them to the matching driver-facing count to recover guest requests.
 * With ATTRIB_ENABLE_COALESCE, toggle requests additionally include cancelled
 * requests and the change in pending count across the window (see below). */
typedef struct IsaacVitaGlPhaseProfileCounters {
    uint32_t draw_elements;
    uint32_t clear;
    uint32_t bind_texture;
    uint32_t use_program;
    uint32_t tex_image;
    uint32_t tex_sub_image;
    uint32_t uniform;
    uint32_t attrib_pointer;
    uint32_t attrib_toggle;
    uint32_t bind_framebuffer;
    uint32_t state;
    uint32_t use_program_suppressed;
    uint32_t uniform_suppressed;
    uint32_t typed_state_hit_active_texture;
    uint32_t typed_state_hit_bind_texture;
    uint32_t typed_state_hit_blend;
    uint32_t typed_state_hit_depth;
    uint32_t typed_state_hit_viewport;
    uint32_t typed_state_hit_attrib_toggle;
    uint32_t typed_state_hit_attrib_pointer;
    uint32_t typed_state_miss;
    uint32_t typed_state_reject_invalid;
    uint32_t typed_state_reject_unknown;
    uint32_t canonical_quad_hits;
    uint32_t canonical_quad_reject_callsite;
    uint32_t canonical_quad_reject_shape;
    uint32_t canonical_quad_reject_bounds;
    uint32_t canonical_quad_reject_pointer;
    uint32_t canonical_quad_driver_fallback;
    uint32_t canonical_quad_index_bytes_saved;
    uint32_t fusion_index_bytes;
    uint32_t fusion_blend_additive;
    uint32_t fusion_blend_alpha;
    uint32_t fusion_blend_other;
    uint32_t fusion_shader_same;
    uint32_t fusion_shader_change;
    uint32_t fusion_fbo_default;
    uint32_t fusion_fbo_offscreen;
    uint32_t fusion_atlas_same;
    uint32_t fusion_atlas_change;
    uint32_t fusion_additive_draws;
    uint32_t fusion_additive_runs;
    /* Offscreen render-target boundary (FBO_CLEAR_ELISION / FBO_RASTER_SCALE):
     * guest glClear requests absorbed at the call (colour part a no-op,
     * depth/stencil part owed), colour targets allocated at the reduced
     * raster, native viewports rewritten for a scaled draw target, the
     * permanent elision poison (guest enabled GL_SCISSOR_TEST), owed
     * depth/stencil clears issued as their own native glClear (before the
     * first draw or ahead of an unmodelled call; folds into a later native
     * clear are free and not counted), and scaled textures re-specified at
     * full size because the guest sub-imaged them. */
    uint32_t clear_suppressed;
    uint32_t fbo_raster_textures;
    uint32_t fbo_raster_viewports;
    uint32_t fbo_elision_poison;
    uint32_t clear_replayed;
    uint32_t fbo_raster_respecified;
    /* Location queries (ph120.a loc(a,u,h,m)): guest glGetAttribLocation and
     * glGetUniformLocation requests, and how many of them the shim-side
     * (program, generation, name) cache answered without entering vitaGL
     * (ISAAC_VITA_GL_LOCATION_CACHE; zero otherwise). */
    uint32_t attrib_location;
    uint32_t uniform_location;
    uint32_t location_cache_hit;
    /* ISAAC_VITA_GL_LOCATION_CACHE_VERIFY only: hits whose cached answer
     * differed from a shadow native query (must stay zero; zero otherwise).
     * ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO_VERIFY counts its replay-memo /
     * wrapper disagreements into the same field. */
    uint32_t location_cache_mismatch;
#if defined(ISAAC_VITA_FBO_CLEAR_ELISION_DEPTH_DROP)
    /* FBO_CLEAR_ELISION_DEPTH_DROP only (ph120.e clear a, d), appended so
     * every other option keeps its offsets: owed depth/stencil clears dropped
     * at a tracked colour attach of the owing framebuffer, and owed clears
     * dropped because stock vitaGL ends the owing scene (clear or draw on
     * another framebuffer, present, framebuffer deletion). */
    uint32_t clear_dropped_attach;
    uint32_t clear_dropped_scene;
#endif
#if defined(ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE) && \
    ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE
    /* Changed known-valid requests staged by the coalescer, including each
     * cancelling request. cancelled counts BOTH requests of a cancelled pair.
     * ph120.y coal(d,c,p0,p1) reports these deltas plus pending endpoint counts.
     * Toggle requests = attrib_toggle + hit_attrib_toggle + cancelled + p1-p0.
     * Deferred native emits = deferred-cancelled-(p1-p0). Do signed arithmetic;
     * a pending request may originate in the preceding profile window.
     * Actual emitted toggles alone increment attrib_toggle/typed_state_miss. */
    uint32_t attrib_deferred;
    uint32_t attrib_cancelled;
#endif
} IsaacVitaGlPhaseProfileCounters;

#if defined(ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE) && \
    ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE
/* Main/context-owner thread only. Synchronize before native consumers/queries,
 * swap, and any reset while the context remains live. No allocation/new timer.
 * External begin synchronizes THEN forgets attribute enable/pointer knowledge;
 * call before any untracked native attribute/VAO mutation, never afterwards. */
void gl_vita_backend_attrib_sync(void);
void gl_vita_backend_attrib_external_begin(void);
uint32_t gl_vita_backend_attrib_pending(void);
#else
#define gl_vita_backend_attrib_sync() ((void)0)
#define gl_vita_backend_attrib_external_begin() ((void)0)
#endif

#if defined(ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO)
/* ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO: the program-state generation the
 * Shader::EnableAttribs/DisableAttribs replay memo is keyed on
 * (host_vita_shader_attrib_fastpath.c).  Never zero, never rewound; bumped
 * by gl_vita_location_cache_invalidate/_reset, i.e. by exactly the events
 * that invalidate the shim location cache: glAttachShader, glCompileShader,
 * glCreateProgram, glDeleteProgram, glDeleteShader, glLinkProgram,
 * glShaderSource and every backend install/uninstall.  A memo entry filled
 * under generation G is valid while the word still reads G. */
extern uint32_t g_isaac_vita_gl_location_generation;
# if defined(ISAAC_VITA_SHADER_ATTRIB_LOCATION_MEMO_VERIFY)
/* VERIFY only: a memo hit whose remembered location differed from the
 * wrapper lookup the VERIFY variant still performs; counted into ph120.a
 * loc(...,m) next to the shim cache's own VERIFY mismatches (must stay 0). */
void isaac_vita_gl_location_memo_note_mismatch(void);
# endif
#endif

#if defined(ISAAC_VITA_SHADER_ATTRIB_DIRECT_STATE)
/* ISAAC_VITA_SHADER_ATTRIB_DIRECT_STATE: the Shader::EnableAttribs /
 * DisableAttribs host replays (host_vita_shader_attrib_fastpath.c) hand the
 * whole attribute set to the typed backend in one native call instead of
 * calling the glGetAttribLocation / glEnableVertexAttribArray /
 * glVertexAttribPointer (glDisableVertexAttribArray) wrappers once per
 * attribute through the backend table.
 *
 * Contract.  For attribute i the batch performs, in this order, exactly the
 * statements of vita_glGetAttribLocation(program, names[i]) (only when
 * `names` is non-NULL; with NULL the caller supplies locations[i], as the
 * replay's location memo does), vita_glEnableVertexAttribArray(loc) and
 * vita_glVertexAttribPointer(loc, components[i], GL_FLOAT, 0, stride, base +
 * 4 * (components[0] + ... + components[i-1])) - respectively
 * vita_glDisableVertexAttribArray(loc) - so every vitaGL call, its
 * arguments and order, every typed-state / coalescer / fill-census /
 * location-cache write and every phase counter are those of the per-call
 * sequence.  What it does not pay: the table-indirect call, the
 * per-attribute sampler publish/restore pair is kept (tokens), and the
 * pointer wrapper's per-call argument validation is hoisted (the batch
 * checks components[i] in 1..4 once, GL_FLOAT is a constant of the frozen
 * bodies, only `stride >= 0` and `loc < 16` remain per call).  locations[]
 * receives every location the batch used (looked up or supplied), so the
 * caller can retire EAX/[ebp+8] and fill its memo exactly as before.
 * Returns 0 and changes nothing when the installed table's members are not
 * this backend's wrappers, count exceeds 16, a component is outside 1..4 or
 * neither names nor locations are given: the replay then takes its
 * per-call path.  tokens: the 0x7e registry tokens the replay would publish
 * to the guest sampler around each wrapper (get_location, toggle = enable
 * or disable, pointer) and the enclosing value it restores; ignored when the
 * backend is built without ISAAC_VITA_GUEST_SAMPLER. */
typedef struct IsaacVitaAttribReplayTokens {
    uint32_t get_location;
    uint32_t toggle;
    uint32_t pointer;
    uint32_t enclosing;
} IsaacVitaAttribReplayTokens;

/* Plain stdint spellings of guest_gl_uint / guest_gl_addr / guest_gl_int /
 * guest_gl_sizei (gl_bridge.h is included after this header). */
struct guest_gl_backend;
int gl_vita_backend_attribs_replay_enable(
    const struct guest_gl_backend *backend, uint32_t program,
    uint32_t count, const uint32_t *names, int32_t *locations,
    const uint8_t *components, int32_t stride, uint32_t base,
    const IsaacVitaAttribReplayTokens *tokens);
int gl_vita_backend_attribs_replay_disable(
    const struct guest_gl_backend *backend, uint32_t program,
    uint32_t count, const uint32_t *names, int32_t *locations,
    const IsaacVitaAttribReplayTokens *tokens);
# if defined(ISAAC_GL_VITA_BACKEND_ORACLE)
/* Oracle only: a byte copy of the typed state shadow (s_gl_typed_state) so
 * the per-call and the direct-state runs can be compared field for field.
 * Returns the number of bytes written (0 when `capacity` is too small or the
 * typed state cache is not compiled). */
size_t gl_vita_backend_oracle_typed_state(void *out, size_t capacity);
# endif
#endif

/* Diagnostic-only texture lifecycle and upload-path census.  Gen timing
 * brackets native glGenTextures after its safe pre-read and before lifecycle
 * classification.  Image timing brackets IO/FXRay/native dispatch but ends
 * before classification.  Delete-native timing brackets only native
 * glDeleteTextures; delete-post timing brackets only the profiler's lifecycle
 * bookkeeping after that call.  Every interval uses CPU process time, includes
 * two clock calls, and excludes asynchronous GPU completion; timings are
 * screening upper bounds, not CPU-cycle measurements.  First/redefine and byte
 * fields classify requests, not successful native allocations; logical-live
 * counts guest name lifetime, not deferred native-slot availability.  A
 * nonzero gen_ambiguous makes subsequent lifecycle, first/redefine and
 * recycled-name counts lower bounds: an unadopted successful same-ID recycle
 * remains unknown until the next full profiler reset.  Later windows retain a
 * nonzero bad marker even though gen_ambiguous itself belongs to the event
 * window.  The frozen guest GL dispatcher serializes wrapper
 * mutations; the phase-loop snapshot shares the existing main-thread
 * ownership assumption. */
typedef struct IsaacVitaTextureChurnProfile {
    uint32_t gen_calls;
    uint32_t gen_names;
    uint32_t gen_ambiguous;
    uint32_t gen_observed_us;
    uint32_t gen_scan_slots;
    uint32_t delete_calls;
    uint32_t delete_names;
    uint32_t delete_native_observed_us;
    uint32_t delete_native_max_observed_us;
    uint32_t delete_post_observed_us;
    uint32_t delete_post_max_observed_us;
    uint32_t image_first;
    uint32_t image_redefine;
    uint32_t image_unknown;
    uint32_t image_other_level;
    uint32_t path_linear;
    uint32_t path_converted;
    uint32_t path_other;
    uint32_t known_pixels;
    uint32_t known_alloc_bytes;
    uint32_t image_observed_us;
    uint32_t image_max_observed_us;
    uint32_t logical_live;
    uint32_t window_peak_live;
    uint32_t recycled_names;
    uint32_t bad;
    uint32_t clock_calls;
    uint32_t clock_pair_max_us;
} IsaacVitaTextureChurnProfile;

_Static_assert(sizeof(IsaacVitaTextureChurnProfile) == 28u * sizeof(uint32_t),
               "texture-churn profile ABI drifted");

#if defined(ISAAC_VITA_PHASE_PROFILE)
extern IsaacVitaGlPhaseProfileCounters
    g_isaac_vita_gl_phase_profile_counters;
#endif

#if defined(ISAAC_VITA_GL_TIME_PROFILE)
/* Wall time spent inside native vitaGL calls, split by what the call makes
 * GXM do.  Measured on the device: Game::Render is a 7 ms CPU floor that does
 * not move with display resolution, so the only way to attribute it is to
 * bracket every native boundary call with two process-clock reads (about
 * 0.3 us each on the 500 MHz A9; the bracket itself is charged to the bucket,
 * never to the guest).  rnd minus the sum of the six buckets is therefore the
 * translated guest code plus our own boundary bookkeeping.
 *   draw      glDrawElements / vglIsaacDrawCanonicalQuads
 *   clear     glClear (a full-target quad in vitaGL; may open a scene)
 *   bind_fb   glBindFramebuffer + glFramebufferTexture2D
 *   tex_upload glTexImage2D + glTexSubImage2D
 *   state     program/uniform/attrib/texture/blend/depth/viewport/enable
 *   present   vglSwapBuffers (also the ph120.t swp phase)
 * Totals are per 120-loop window and are zeroed by the phase profiler when
 * it reads them; a 2 s window cannot overflow 32 bits of microseconds. */
typedef struct IsaacVitaGlTimeBucket {
    uint32_t calls;
    uint32_t total_us;
    uint32_t max_us;
} IsaacVitaGlTimeBucket;

typedef struct IsaacVitaGlTimeProfile {
    IsaacVitaGlTimeBucket draw;
    IsaacVitaGlTimeBucket clear;
    IsaacVitaGlTimeBucket bind_fb;
    IsaacVitaGlTimeBucket tex_upload;
    IsaacVitaGlTimeBucket state;
    IsaacVitaGlTimeBucket present;
    uint32_t bad_clock;
} IsaacVitaGlTimeProfile;

extern IsaacVitaGlTimeProfile g_isaac_vita_gl_time_profile;

static inline void isaac_vita_gl_time_add(
    IsaacVitaGlTimeBucket *bucket, uint64_t started_at, uint64_t ended_at)
{
    uint64_t elapsed;

    if (ended_at < started_at) {
        ++g_isaac_vita_gl_time_profile.bad_clock;
        return;
    }
    elapsed = ended_at - started_at;
    if (elapsed > UINT32_MAX)
        elapsed = UINT32_MAX;
    ++bucket->calls;
    bucket->total_us += (uint32_t)elapsed;
    if ((uint32_t)elapsed > bucket->max_us)
        bucket->max_us = (uint32_t)elapsed;
}
#endif

#if defined(ISAAC_VITA_GL_TIME_SDK_SPARSE_ATTRIB)
/* ISAAC_VITA_GL_TIME_SDK_SPARSE_ATTRIB (wf/cpu-render-20260906): the sdk32
 * scheme (isaac_sdk_sparse_profile.h) applied to the two Shader attribute
 * replays of ISAAC_VITA_SHADER_ATTRIB_FASTPATH.  CMake hands the define to
 * host_vita_shader_attrib_fastpath.c and kage_vita_phase_profile.c only when
 * ISAAC_VITA_GL_TIME_SDK_SPARSE and the fast path are both ON.  Every 32nd
 * Enable replay and every 32nd Disable replay - ordinal counted per kind
 * over HANDLED replays, selected when (ordinal & 31) == residue, residue =
 * window & 31 like the native sdk32 sites - reads sceKernelGetProcessTimeWide
 * once at replay entry and once at HANDLED (the `ret 0xc` retirement, after
 * the last wrapper or batch call), and adds the difference to its kind's
 * bucket; the other 31 pay one load, one compare and one increment.  A
 * rejected replay is never counted or timed (the translated body runs).
 * enable_replays/disable_replays count every HANDLED replay of the window
 * (count-0 replays included), so bucket.calls * 32 ~ replays.  Taken and
 * zeroed by the phase profiler at the same loop head as the sdk32 rows
 * (residue re-derived from the next window), printed as one ph120.gt
 * mode=sdk32 scope=attrib row directly after scope=queue. */
typedef struct IsaacVitaAttribReplaySparseBucket {
    uint32_t calls;
    uint32_t us;
    uint32_t max_us;
} IsaacVitaAttribReplaySparseBucket;

typedef struct IsaacVitaAttribReplaySparse {
    uint32_t residue;
    uint32_t enable_replays;
    uint32_t disable_replays;
    uint32_t bad_clock;
    IsaacVitaAttribReplaySparseBucket enable;
    IsaacVitaAttribReplaySparseBucket disable;
} IsaacVitaAttribReplaySparse;

/* Take-and-zero (out == NULL discards); next_window selects the residue the
 * following window samples with.  Defined in host_vita_shader_attrib_fastpath.c. */
void isaac_vita_attrib_replay_sparse_take(
    IsaacVitaAttribReplaySparse *out, uint32_t next_window);
#endif

#if defined(ISAAC_VITA_GL_WRAPPER_TIME)
# include <string.h>
# if !defined(ISAAC_VITA_GL_TIME_PROFILE)
#  error "ISAAC_VITA_GL_WRAPPER_TIME requires ISAAC_VITA_GL_TIME_PROFILE"
# endif
# if defined(ISAAC_VITA_GL_TIME_SDK_SPARSE) && ISAAC_VITA_GL_TIME_SDK_SPARSE
#  error "ISAAC_VITA_GL_WRAPPER_TIME needs the broad ph120.gt buckets, not GL_TIME_SDK_SPARSE"
# endif
/* ISAAC_VITA_GL_WRAPPER_TIME (wf/cpu-render-20260906): wall time of every
 * GL shim wrapper the guest reaches, from the moment gl_bridge.c hands the
 * CPU to the generated adapter (guest_gl_run_owned) until the adapter
 * returns, on the same sceKernelGetProcessTimeWide clock as ph120.gt and
 * with the same bucket assignment, so wrapper minus gt body is the shim's
 * own cost (argument decode, attribute coalescing, location cache, fill
 * census, coloroffset staging, typed-state shadows).  Buckets, in record
 * order w(d,c,b,t,s,p,a,u,o):
 *   d glDrawElements; c glClear; b glBindFramebuffer + glFramebufferTexture2D;
 *   t glTexImage2D + glTexSubImage2D; s the remaining gt `state` owners
 *   (glUseProgram, glActiveTexture, glBindTexture, glBlendFuncSeparate,
 *   glClearColor, glClearDepth, glCullFace, glDepthFunc, glEnable,
 *   glGetAttribLocation, glGetUniformLocation, glTexParameteri, glViewport);
 *   p the KAGE Present method (kage_vita_backend_present entry to return,
 *   whose gt body is the vglSwapBuffers bracket); a glEnableVertexAttribArray
 *   + glDisableVertexAttribArray + glVertexAttribPointer; u glUniform*;
 *   o every other registry entry (shader/texture/framebuffer object
 *   management, queries, glReadPixels) - none of these has a gt bracket
 *   except through `state`, so a and u overhead is read against gt `s`
 *   together with the s bucket.
 * Slots k(en,di): the two host-native callees sub_0056d500 reaches through
 * its vtable slots (the flush reaches eight indirect targets; only these two
 * are replaced natively, see ISAAC_VITA_SHADER_ATTRIB_FASTPATH in
 * vita/README.md): the ISAAC_VITA_SHADER_ATTRIB_FASTPATH replays of
 * Shader::EnableAttribs / DisableAttribs, entry to `ret 0xc`, handled
 * replays only (a rejected replay costs a few loads and the translated body
 * then pays the wrapper buckets).  Their wrapper bodies bypass the adapters,
 * so they appear in k, not in a; the gt `state` bucket still holds their
 * vitaGL bodies.  Zero without the fast path.  Two 64-bit clock reads per
 * wrapper when ON, nothing when OFF; take-and-zero per phase window. */
enum {
    ISAAC_VITA_GL_WRAPPER_DRAW = 0,
    ISAAC_VITA_GL_WRAPPER_CLEAR = 1,
    ISAAC_VITA_GL_WRAPPER_BIND_FB = 2,
    ISAAC_VITA_GL_WRAPPER_TEX = 3,
    ISAAC_VITA_GL_WRAPPER_STATE = 4,
    ISAAC_VITA_GL_WRAPPER_PRESENT = 5,
    ISAAC_VITA_GL_WRAPPER_ATTRIB = 6,
    ISAAC_VITA_GL_WRAPPER_UNIFORM = 7,
    ISAAC_VITA_GL_WRAPPER_OTHER = 8,
    ISAAC_VITA_GL_WRAPPER_BUCKETS = 9
};
enum {
    ISAAC_VITA_GL_WRAPPER_K_ATTRIB_ENABLE = 0,
    ISAAC_VITA_GL_WRAPPER_K_ATTRIB_DISABLE = 1,
    ISAAC_VITA_GL_WRAPPER_K_SLOTS = 2
};

/* ph120.gd: the glDrawElements wrapper's entry-to-return time split along
 * vita_glDrawElements' own statement order (gl_vita_backend.c), record order
 * d(disp,sync,cls,cen,fbo,gl,tail), one clock read per boundary:
 *   disp gl_bridge entry read -> wrapper body start (adapter argument decode,
 *        KAGE_VITA_DEEP_SCOPE enter);
 *   sync gl_vita_backend_attrib_sync: the deferred Enable/DisableVertexAttrib-
 *        Array toggles (ISAAC_VITA_GL_ATTRIB_ENABLE_COALESCE) issued natively
 *        before the draw, their gt `state` bodies included;
 *   cls  kage_vita_world_seam_diag_note_draw + kage_vita_canonical_quad_
 *        classify (ISAAC_VITA_CANONICAL_QUAD_ZERO_COPY: return-RVA and
 *        index-array shape check) + the first-frame probe counters;
 *   cen  GL_VITA_PHASE_COUNT(draw_elements) + gl_vita_fusion_profile_draw +
 *        gl_vita_fill_census_draw (ISAAC_VITA_GL_FILL_CENSUS projection);
 *   fbo  gl_vita_fbo_note_draw (ISAAC_VITA_FBO_CLEAR_ELISION: owed clear
 *        materialize incl. its native glClear, colour-note forget) up to the
 *        gt draw bracket's BEGIN read; a canonical driver fallback re-opens
 *        the body and its gap lands here too;
 *   gl   the gt draw bracket itself (vglIsaacDrawCanonicalQuads or
 *        glDrawElements): identical reads, so gd.gl == gt.d per window;
 *   tail gt END read -> gl_bridge exit read (deep-scope leave, adapter
 *        return, stdcall retirement).
 * n(dr,cq,ea,da): draws (disp calls), canonical zero-copy quad hits, attribs
 * replayed by the handled EnableAttribs / DisableAttribs replays.
 * en(own,loc,gl) / di(own,loc,gl): the k(en,di) replay time split
 * (host_vita_shader_attrib_fastpath.c): own = frame/object/token checks
 * before the loop + register retirement after it; loc = per-attrib
 * backend->glGetAttribLocation (the shim location memo); gl = per-attrib
 * backend->glEnableVertexAttribArray + glVertexAttribPointer (enable) or
 * glDisableVertexAttribArray (disable), typed wrappers and gt bodies
 * included.  own + loc + gl == k us.
 * ph120.gu u(m4,4f,2f,1i,o)=us,n/...: the u wrapper bucket by frozen entry
 * name: glUniformMatrix4fv, glUniform4fv, glUniform2fv, glUniform1i, every
 * other glUniform*; same two reads as w, no extra clock. */
enum {
    ISAAC_VITA_GL_DRAW_SPLIT_DISPATCH = 0,
    ISAAC_VITA_GL_DRAW_SPLIT_SYNC = 1,
    ISAAC_VITA_GL_DRAW_SPLIT_CLASSIFY = 2,
    ISAAC_VITA_GL_DRAW_SPLIT_CENSUS = 3,
    ISAAC_VITA_GL_DRAW_SPLIT_FBO = 4,
    ISAAC_VITA_GL_DRAW_SPLIT_BODY = 5,
    ISAAC_VITA_GL_DRAW_SPLIT_TAIL = 6,
    ISAAC_VITA_GL_DRAW_SPLIT_BUCKETS = 7
};
enum {
    ISAAC_VITA_GL_REPLAY_OWN = 0,
    ISAAC_VITA_GL_REPLAY_LOC = 1,
    ISAAC_VITA_GL_REPLAY_GL = 2,
    ISAAC_VITA_GL_REPLAY_PARTS = 3
};
enum {
    ISAAC_VITA_GL_UNIFORM_M4 = 0,
    ISAAC_VITA_GL_UNIFORM_4F = 1,
    ISAAC_VITA_GL_UNIFORM_2F = 2,
    ISAAC_VITA_GL_UNIFORM_1I = 3,
    ISAAC_VITA_GL_UNIFORM_OTHER = 4,
    ISAAC_VITA_GL_UNIFORM_KINDS = 5,
    ISAAC_VITA_GL_UNIFORM_NONE = 0xff
};

typedef struct IsaacVitaGlWrapperTimeProfile {
    IsaacVitaGlTimeBucket wrapper[ISAAC_VITA_GL_WRAPPER_BUCKETS];
    IsaacVitaGlTimeBucket slot[ISAAC_VITA_GL_WRAPPER_K_SLOTS];
    IsaacVitaGlTimeBucket draw[ISAAC_VITA_GL_DRAW_SPLIT_BUCKETS];
    uint32_t draw_canonical;
    IsaacVitaGlTimeBucket replay[ISAAC_VITA_GL_WRAPPER_K_SLOTS]
                                [ISAAC_VITA_GL_REPLAY_PARTS];
    IsaacVitaGlTimeBucket uniform[ISAAC_VITA_GL_UNIFORM_KINDS];
    uint32_t bad_clock;
} IsaacVitaGlWrapperTimeProfile;

/* gl_bridge.c owns the storage in production (the phase-profile oracle
 * defines its own). */
extern IsaacVitaGlWrapperTimeProfile g_isaac_vita_gl_wrapper_time;
/* gl_bridge.c: the entry read of the adapter run in flight (the draw
 * wrapper charges its `disp` from it) and the draw wrapper's last gt END
 * read (gl_bridge charges `tail` up to its exit read).  Not part of the
 * take-and-zero: cursors, not accumulators. */
extern uint64_t g_isaac_vita_gl_wrapper_entry_at;
extern uint64_t g_isaac_vita_gl_draw_body_ended_at;

/* Same arithmetic as isaac_vita_gl_time_add; a reversed clock taints this
 * record's own bad= instead of ph120.gt's. */
static inline void isaac_vita_gl_wrapper_time_add(
    IsaacVitaGlTimeBucket *bucket, uint64_t started_at, uint64_t ended_at)
{
    uint64_t elapsed;

    if (ended_at < started_at) {
        ++g_isaac_vita_gl_wrapper_time.bad_clock;
        return;
    }
    elapsed = ended_at - started_at;
    if (elapsed > UINT32_MAX)
        elapsed = UINT32_MAX;
    ++bucket->calls;
    bucket->total_us += (uint32_t)elapsed;
    if ((uint32_t)elapsed > bucket->max_us)
        bucket->max_us = (uint32_t)elapsed;
}

/* Bucket of one frozen-registry entry by its exact name (the table above);
 * unknown names are `o`.  Evaluated once per entry at backend install. */
static inline uint8_t isaac_vita_gl_wrapper_bucket_for_name(const char *name)
{
    static const char *const k_state[] = {
        "glUseProgram", "glActiveTexture", "glBindTexture",
        "glBlendFuncSeparate", "glClearColor", "glClearDepth", "glCullFace",
        "glDepthFunc", "glEnable", "glGetAttribLocation",
        "glGetUniformLocation", "glTexParameteri", "glViewport",
    };
    size_t i;

    if (!name)
        return ISAAC_VITA_GL_WRAPPER_OTHER;
    if (!strcmp(name, "glDrawElements"))
        return ISAAC_VITA_GL_WRAPPER_DRAW;
    if (!strcmp(name, "glClear"))
        return ISAAC_VITA_GL_WRAPPER_CLEAR;
    if (!strcmp(name, "glBindFramebuffer") ||
            !strcmp(name, "glFramebufferTexture2D"))
        return ISAAC_VITA_GL_WRAPPER_BIND_FB;
    if (!strcmp(name, "glTexImage2D") || !strcmp(name, "glTexSubImage2D"))
        return ISAAC_VITA_GL_WRAPPER_TEX;
    if (!strcmp(name, "glEnableVertexAttribArray") ||
            !strcmp(name, "glDisableVertexAttribArray") ||
            !strcmp(name, "glVertexAttribPointer"))
        return ISAAC_VITA_GL_WRAPPER_ATTRIB;
    if (!strncmp(name, "glUniform", 9u))
        return ISAAC_VITA_GL_WRAPPER_UNIFORM;
    for (i = 0u; i < sizeof k_state / sizeof k_state[0]; ++i)
        if (!strcmp(name, k_state[i]))
            return ISAAC_VITA_GL_WRAPPER_STATE;
    return ISAAC_VITA_GL_WRAPPER_OTHER;
}

/* ph120.gu kind of one registry entry (NONE for non-uniform entries). */
static inline uint8_t isaac_vita_gl_uniform_kind_for_name(const char *name)
{
    if (!name || strncmp(name, "glUniform", 9u))
        return ISAAC_VITA_GL_UNIFORM_NONE;
    if (!strcmp(name, "glUniformMatrix4fv"))
        return ISAAC_VITA_GL_UNIFORM_M4;
    if (!strcmp(name, "glUniform4fv"))
        return ISAAC_VITA_GL_UNIFORM_4F;
    if (!strcmp(name, "glUniform2fv"))
        return ISAAC_VITA_GL_UNIFORM_2F;
    if (!strcmp(name, "glUniform1i"))
        return ISAAC_VITA_GL_UNIFORM_1I;
    return ISAAC_VITA_GL_UNIFORM_OTHER;
}
#endif

/* Preserve current requested GL state but start a fresh adjacency/run census. */
void gl_vita_backend_phase_profile_window_boundary(void);

#if defined(ISAAC_VITA_FBO_CLEAR_ELISION)
/* Call immediately before vglSwapBuffers: the present ends the GXM scene, so
 * a depth/stencil clear still owed to an offscreen target is dropped exactly
 * as stock vitaGL would have lost it. */
void gl_vita_backend_fbo_present(void);
#endif

#if defined(ISAAC_VITA_TEXTURE_CHURN_PROFILE) || \
    defined(ISAAC_KAGE_VITA_PHASE_PROFILE_ORACLE)
/* Copies and clears the completed-window counters while preserving the
 * logical texture-name lifecycle needed to classify the next window. */
void gl_vita_backend_texture_churn_profile_take_window(
    IsaacVitaTextureChurnProfile *profile);
#endif

#if defined(ISAAC_VITA_GL_FILL_CENSUS) || \
    defined(ISAAC_KAGE_VITA_PHASE_PROFILE_ORACLE)
/* GL fill census (ISAAC_VITA_GL_FILL_CENSUS): per phase window, the
 * projected screen area every guest glDrawElements would rasterise, from
 * the Position attribute + Transform matrix + viewport shadows of the typed
 * shim.  Pure CPU bookkeeping, no GL query; kilo-pixels (1024 px) rounded
 * per draw (a draw under 512 px counts 0, each draw carries at most 0.5 kpx
 * of rounding; the class sums prog e+o == blend a+d+o == sum(pass kpx) ==
 * kpx_clipped hold exactly).  Reported as ph120.fa (totals/classes) and
 * ph120.fp (per pass ordinal, attachment, tiles).  Legend:
 *   kpx_unclipped / kpx_clipped: triangle area before / after clipping its
 *     bounding box to the viewport (area scaled by the box overlap ratio);
 *   kpx_clear: area of every glClear vitaGL received: the level-0 size of
 *     the colour texture attached to the target framebuffer, or the native
 *     960x544 display surface for framebuffer 0 (not the viewport); owed
 *     depth/stencil clears the FBO clear elision issues natively count on
 *     the framebuffer that owed them;
 *   clear_unknown: native clears whose attachment size the census did not
 *     know (renderbuffer, texture defined before the census saw its level-0
 *     glTexImage2D, or a name past GL_VITA_TEXTURE_CAPACITY): counted 0 kpx
 *     in kpx_clear, so kpx_clear is a lower bound when this is non-zero;
 *   max_draw_kpx: largest clipped single draw;
 *   big: triangles whose bbox exceeds twice the viewport in either axis;
 *   bad: triangles with a non-finite or |coordinate| > 65536 pixel vertex,
 *     or a near-zero clip w;
 *   synthesized: canonical zero-copy draws whose indices were synthesized
 *     (base+{0,2,1,1,2,3} per quad) instead of read;
 *   projective: draws whose Transform was not affine (m3/m7/m11 != 0 or
 *     m15 != 1), measured through the full divide;
 *   tiles: 32x32 pixel tiles covered by clipped triangle boxes (summed per
 *     triangle, so overlapping triangles count twice);
 *   viewport_changes: glViewport calls that changed the rectangle (not the
 *     number of distinct viewports);
 *   miss: draws counted in draws/triangles but not measured (no viewport,
 *     unknown program, Position/Transform not resolved, attribute disabled,
 *     non-float or size 1/4 attribute, misaligned pointer/stride, non
 *     triangle-list mode, invalid index array);
 *   prog_kpx: e = program whose attribute queries included
 *     PixelationAmount (the coloroffset layout), o = every other program;
 *   blend_kpx: a = blend enabled with dst ONE (additive, as ph120.f),
 *     d = dst ONE_MINUS_SRC_ALPHA, o = other factors or blend disabled;
 *   pass_*: [0..3] = offscreen pass ordinal since present (3 = 3+), [4] =
 *     display (framebuffer 0), one pass = a run of draws/clears on the
 *     same framebuffer, split by attach-to-bound, glReadPixels on the pass
 *     framebuffer and delete (the vitaGL scene_reset predicate);
 *   attachment_*: level-0 glTexImage2D size of the colour texture attached
 *     to the last offscreen pass of the window. */
typedef struct IsaacVitaGlFillCensus {
    uint32_t draws;
    uint32_t triangles;
    uint32_t kpx_unclipped;
    uint32_t kpx_clipped;
    uint32_t kpx_clear;
    uint32_t max_draw_kpx;
    uint32_t big;
    uint32_t bad;
    uint32_t synthesized;
    uint32_t projective;
    uint32_t tiles;
    uint32_t viewport_width;
    uint32_t viewport_height;
    uint32_t viewport_changes;
    uint32_t miss;
    uint32_t prog_kpx[2];
    uint32_t blend_kpx[3];
    uint32_t pass_draws[5];
    uint32_t pass_kpx[5];
    uint32_t pass_clears[5];
    uint32_t attachment_width;
    uint32_t attachment_height;
    uint32_t clear_unknown;
} IsaacVitaGlFillCensus;

/* Copies and zeroes the completed-window census (take-and-zero; the
 * viewport size is the current shadow, not a counter).  `window` and the
 * window's render p50 arm the optional one-frame draw dump
 * (ISAAC_VITA_GL_FILL_CENSUS_DUMP), whose lines are emitted from here with
 * isaac_vita_log, never from a wrapper. */
void gl_vita_backend_fill_census_take_window(
    IsaacVitaGlFillCensus *census, uint32_t window,
    uint32_t render_p50_us);
/* Call at present, before the swap: resets the pass ordinal and moves the
 * draw dump between armed -> capturing -> complete. */
void gl_vita_backend_fill_census_present(void);
/* Boot receipt for the kage_vita_backend.c banner: returns 1 and the dump
 * thresholds (render p50 arm in microseconds, first eligible window, forced
 * window, frames per launch) when ISAAC_VITA_GL_FILL_CENSUS_DUMP is compiled
 * in, else 0 and zeros. */
uint32_t gl_vita_backend_fill_census_dump_config(
    uint32_t *render_p50_us, uint32_t *min_window, uint32_t *window,
    uint32_t *frames);
#endif

#ifndef ISAAC_VITAGL_DISPLAY_SURFACE_STATUS_DEFINED
#define ISAAC_VITAGL_DISPLAY_SURFACE_STATUS_DEFINED 1
#define ISAAC_VITAGL_DISPLAY_SURFACE_COUNT 3u
#define ISAAC_VITAGL_DISPLAY_SURFACE_STAGE_NONE        0u
#define ISAAC_VITAGL_DISPLAY_SURFACE_STAGE_ALLOC       1u
#define ISAAC_VITAGL_DISPLAY_SURFACE_STAGE_GET_BASE    2u
#define ISAAC_VITAGL_DISPLAY_SURFACE_STAGE_MAP         3u
#define ISAAC_VITAGL_DISPLAY_SURFACE_STAGE_COLOR_INIT  4u
#define ISAAC_VITAGL_DISPLAY_SURFACE_STAGE_SYNC_CREATE 5u
typedef struct IsaacVitaGlDisplaySurfaceStatus {
    uint32_t size;
    uint32_t display_size;
    uint32_t buffer_count;
    uint32_t dedicated_count;
    uint32_t system_app_mode;
    uint32_t failure_stage;
    uint32_t failure_index;
    int32_t alloc_result[ISAAC_VITAGL_DISPLAY_SURFACE_COUNT];
    int32_t get_base_result[ISAAC_VITAGL_DISPLAY_SURFACE_COUNT];
    int32_t map_result[ISAAC_VITAGL_DISPLAY_SURFACE_COUNT];
    uint32_t dedicated[ISAAC_VITAGL_DISPLAY_SURFACE_COUNT];
    int32_t color_init_result[ISAAC_VITAGL_DISPLAY_SURFACE_COUNT];
    int32_t sync_create_result[ISAAC_VITAGL_DISPLAY_SURFACE_COUNT];
} IsaacVitaGlDisplaySurfaceStatus;
_Static_assert(sizeof(IsaacVitaGlDisplaySurfaceStatus) == 100u,
               "vitaGL display-surface status ABI drifted");
#endif

/* vitaGL calls the strong status endpoint synchronously during initialization.
 * The getter lets the KAGE owner fail before loading if the A/B did not reach
 * three dedicated, mapped, initialized scanout buffers. */
void isaac_vita_vitagl_display_surface_status(
    const IsaacVitaGlDisplaySurfaceStatus *status);
void isaac_vita_vitagl_display_surface_lifecycle_failure(
    const char *site, int32_t result);
int gl_vita_backend_get_display_surface_status(
    IsaacVitaGlDisplaySurfaceStatus *status);

#ifndef ISAAC_VITAGL_RT_TELEMETRY_DEFINED
#define ISAAC_VITAGL_RT_TELEMETRY_DEFINED 1
typedef struct IsaacVitaGlRenderTargetTelemetry {
    const char *site;
    int32_t first_result;
    int32_t retry_result;
    int32_t finish_result;
    uint32_t finish_called;
    int32_t destroy_result;
    const void *first_target;
    const void *retry_target;
    uint32_t retry_attempted;
    uint32_t pool_full;
    uint32_t pending_duplicate;
    uint32_t invariant_failure;
    uint32_t pending[4];
    uint32_t drained_targets;
    uint32_t live_targets;
    uint32_t reference_sum;
    uint32_t width;
    uint32_t height;
    uint32_t frame;
    uint32_t recovered;
} IsaacVitaGlRenderTargetTelemetry;
#endif

/* Exact 32-bit worst case for the bounded durable record emitted by the
 * project-side endpoint (42-byte site plus UINT32_MAX in every field). */
#define ISAAC_VITAGL_RT_EVENT_MAX_BODY 381u

void isaac_vita_vitagl_render_target_event(
    const IsaacVitaGlRenderTargetTelemetry *event);

/* Install every symbol from the frozen typed surface that the Vita backend
 * can implement.  The 11 remaining desktop-only symbols stay NULL and
 * therefore fault by exact name in gl_bridge.c. */
int         gl_vita_backend_install(void);
void        gl_vita_backend_uninstall(void);
int         gl_vita_backend_installed(void);
size_t      gl_vita_backend_resolved_count(void);
size_t      gl_vita_backend_missing_count(void);
const char *gl_vita_backend_missing_symbol(size_t index);
const char *gl_vita_backend_last_error(void);

#if defined(ISAAC_VITA_VITAGL_SHADER_CACHE)
/* Emit one aggregate snapshot at the next game-present boundary, never from
 * a shader compile or display callback. */
void gl_vita_backend_shader_cache_report(void);
#endif

/* High-level KAGE creates its initial depth renderbuffer through raw vitaGL
 * calls before guest GL dispatch starts.  Register that exact native object
 * here so the typed glGetRenderbufferParameteriv emulation observes the same
 * lifecycle as objects created through glGenRenderbuffers. */
int gl_vita_backend_register_renderbuffer(
    uint32_t renderbuffer, uint32_t internal_format,
    int32_t width, int32_t height);
void gl_vita_backend_unregister_renderbuffer(uint32_t renderbuffer);

#if defined(ISAAC_VITA_FIRST_FRAME_PROBE)
/* The legacy bounded presentation experiment mutates three finite windows.
 * ISAAC_VITA_DIRECT_DEFAULT instead changes the frozen Render CFG itself;
 * with that mode selected this owner retains only passive readback, queue,
 * and GL-call observations. */
#if !defined(ISAAC_VITA_DIRECT_DEFAULT)
#define ISAAC_VITA_FIRST_FRAME_CLEAR_END_PRESENT 120u
#define ISAAC_VITA_FIRST_FRAME_BLIT_END_PRESENT  240u
#define ISAAC_VITA_FIRST_FRAME_BYPASS_END_PRESENT 360u

static inline int isaac_vita_first_frame_postprocess_bypass_active(
    uint32_t present_index)
{
    return present_index >= ISAAC_VITA_FIRST_FRAME_BLIT_END_PRESENT &&
        present_index < ISAAC_VITA_FIRST_FRAME_BYPASS_END_PRESENT;
}

static inline int isaac_vita_first_frame_postprocess_bypass_complete(
    uint32_t successful_present_count)
{
    return successful_present_count ==
        ISAAC_VITA_FIRST_FRAME_BYPASS_END_PRESENT;
}

static inline int isaac_vita_first_frame_postprocess_restored(
    uint32_t successful_present_count)
{
    return successful_present_count ==
        ISAAC_VITA_FIRST_FRAME_BYPASS_END_PRESENT + 1u;
}
#endif

/* Kept byte-for-byte in sync with the optional vitaGL queue hook.  The
 * runtime header appears before vitaGL.h, so the shared guard prevents a
 * second typedef regardless of include order. */
#ifndef ISAAC_VITAGL_DISPLAY_QUEUE_PROBE_DEFINED
#define ISAAC_VITAGL_DISPLAY_QUEUE_PROBE_DEFINED 1
#define ISAAC_VITAGL_DISPLAY_QUEUE_SAMPLE_COUNT 8u
#define ISAAC_VITAGL_DISPLAY_QUEUE_STAGE_ADD_RESULT 1u
#define ISAAC_VITAGL_DISPLAY_QUEUE_STAGE_DISPLAY_CALLBACK 2u
#define ISAAC_VITAGL_DISPLAY_LINEAGE_BEGIN_MISMATCH 0x00000001u
#define ISAAC_VITAGL_DISPLAY_LINEAGE_END_MISMATCH   0x00000002u
typedef struct IsaacVitaGlDisplayQueueProbe {
    uint32_t size;
    uint32_t stage;
    uint32_t sequence;
    uint32_t front_index;
    uint32_t back_index;
    uint32_t address;
    union {
        struct {
            int32_t result;
            uint32_t old_sync;
            uint32_t new_sync;
            uint32_t begin_context;
            uint32_t begin_render_target;
            uint32_t begin_fragment_sync;
            uint32_t begin_color_surface;
            uint32_t begin_color_data;
            int32_t begin_result;
            uint32_t begin_count;
            uint32_t end_context;
            int32_t end_result;
            uint32_t end_count;
            uint32_t mismatch_mask;
            int32_t back_memblock_uid;
            int32_t back_get_base_result;
            int32_t back_map_result;
            uint32_t back_dedicated;
        } add;
        struct {
            int32_t result;
            uint32_t size;
            uint32_t base;
            uint32_t pitch;
            uint32_t pixel_format;
            uint32_t width;
            uint32_t height;
            uint32_t sync;
            uint32_t sample_count;
            uint32_t sparse_hash;
            uint32_t rgba[ISAAC_VITAGL_DISPLAY_QUEUE_SAMPLE_COUNT];
        } display;
    } detail;
} IsaacVitaGlDisplayQueueProbe;
#if defined(__cplusplus)
static_assert(sizeof(IsaacVitaGlDisplayQueueProbe) == 96u,
              "vitaGL display-lineage ABI drifted");
#else
_Static_assert(sizeof(IsaacVitaGlDisplayQueueProbe) == 96u,
               "vitaGL display-lineage ABI drifted");
#endif
#endif

void isaac_vitagl_display_queue_probe(
    const IsaacVitaGlDisplayQueueProbe *event);

#if defined(ISAAC_VITA_KNOWN_COLOR_PROBE)
/* Kept byte-for-byte in sync with the optional vitaGL p2 transfer hook. */
#ifndef ISAAC_VITAGL_KNOWN_COLOR_PROBE_DEFINED
#define ISAAC_VITAGL_KNOWN_COLOR_PROBE_DEFINED 1
#define ISAAC_VITAGL_KNOWN_COLOR_PRESENT_INDEX 2u
#define ISAAC_VITAGL_KNOWN_COLOR_FILL          0xffff00ffu
#define ISAAC_VITAGL_KNOWN_COLOR_FORMAT        0x00060000u
#define ISAAC_VITAGL_KNOWN_COLOR_X             64u
#define ISAAC_VITAGL_KNOWN_COLOR_Y             64u
#define ISAAC_VITAGL_KNOWN_COLOR_WIDTH         512u
#define ISAAC_VITAGL_KNOWN_COLOR_HEIGHT        256u
#define ISAAC_VITAGL_KNOWN_COLOR_NOT_CALLED    ((int32_t)0x80000000u)
#define ISAAC_VITAGL_KNOWN_COLOR_GATE_TAG        0x00000001u
#define ISAAC_VITAGL_KNOWN_COLOR_GATE_NON_SYSTEM 0x00000002u
#define ISAAC_VITAGL_KNOWN_COLOR_GATE_COUNT      0x00000004u
#define ISAAC_VITAGL_KNOWN_COLOR_GATE_BACK       0x00000008u
#define ISAAC_VITAGL_KNOWN_COLOR_GATE_ADDRESS    0x00000010u
#define ISAAC_VITAGL_KNOWN_COLOR_GATE_DEDICATED  0x00000020u
#define ISAAC_VITAGL_KNOWN_COLOR_GATE_BEGIN_ONE  0x00000040u
#define ISAAC_VITAGL_KNOWN_COLOR_GATE_END_ONE    0x00000080u
#define ISAAC_VITAGL_KNOWN_COLOR_GATE_BEGIN_OK   0x00000100u
#define ISAAC_VITAGL_KNOWN_COLOR_GATE_END_OK     0x00000200u
#define ISAAC_VITAGL_KNOWN_COLOR_GATE_CONTEXT    0x00000400u
#define ISAAC_VITAGL_KNOWN_COLOR_GATE_DATA       0x00000800u
#define ISAAC_VITAGL_KNOWN_COLOR_GATE_SYNC       0x00001000u
#define ISAAC_VITAGL_KNOWN_COLOR_GATE_MISMATCH   0x00002000u
#define ISAAC_VITAGL_KNOWN_COLOR_GATE_ALL        0x00003fffu
typedef struct IsaacVitaGlKnownColorProbe {
    uint32_t size;
    uint32_t present_index;
    uint32_t sequence;
    uint32_t front_index;
    uint32_t back_index;
    uint32_t address;
    uint32_t dedicated;
    uint32_t color;
    uint32_t format;
    uint32_t x;
    uint32_t y;
    uint32_t width;
    uint32_t height;
    uint32_t stride_bytes;
    uint32_t sync_object;
    uint32_t sync_flags;
    uint32_t gate_mask;
    uint32_t context_finish_called;
    int32_t fill_result;
    uint32_t transfer_finish_called;
    int32_t transfer_finish_result;
    int32_t queue_result;
} IsaacVitaGlKnownColorProbe;
#if defined(__cplusplus)
static_assert(sizeof(IsaacVitaGlKnownColorProbe) == 88u,
              "vitaGL known-color ABI drifted");
#else
_Static_assert(sizeof(IsaacVitaGlKnownColorProbe) == 88u,
               "vitaGL known-color ABI drifted");
#endif
#endif

/* The query is valid only while the game owner brackets vglSwapBuffers.
 * The event endpoint is a fixed bounded copy: no logging, allocation, GL,
 * or waiting is legal inside the vitaGL hook. */
uint32_t isaac_vita_vitagl_known_color_present_index(void);
void isaac_vita_vitagl_known_color_probe(
    const IsaacVitaGlKnownColorProbe *event);
#endif

typedef struct IsaacVitaFirstFrameSnapshot {
    uint32_t current_guest_fbo;
    uint32_t manager_fbo;
    /* Last texture argument observed at the manager FBO's COLOR_ATTACHMENT0
     * boundary.  vitaGL's void call cannot prove the resulting attachment. */
    uint32_t manager_color_attach_arg;
    /* Total matching calls, including the first attachment. */
    uint32_t manager_color_attach_calls;
    uint32_t bind_zero;
    uint32_t bind_nonzero;
    uint32_t clear_default;
    uint32_t clear_offscreen;
    uint32_t draw_default;
    uint32_t draw_offscreen;
    uint32_t last_draw_fbo;
    uint32_t control_clears;
    uint32_t blit_attempts;
    uint32_t blit_successes;
    uint32_t blit_no_manager;
    uint32_t blit_incomplete;
} IsaacVitaFirstFrameSnapshot;

void gl_vita_backend_first_frame_set_manager_framebuffer(
    uint32_t framebuffer);
void gl_vita_backend_first_frame_before_present(uint32_t present_index);
void gl_vita_backend_first_frame_queue_begin(uint32_t present_index);
void gl_vita_backend_first_frame_queue_end(void);
void gl_vita_backend_first_frame_snapshot(
    IsaacVitaFirstFrameSnapshot *snapshot);
#endif

#endif
