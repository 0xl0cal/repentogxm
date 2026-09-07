#ifndef KAGE_VITA_ROOM_PROFILE_H
#define KAGE_VITA_ROOM_PROFILE_H

/* Included by the phase reporter after KVPP_PRINTF. Every line fits its
 * existing 512-byte allowance. All brackets are CPU elapsed time; native I/O
 * covers all calling threads. Neither nested costs nor thread totals add up
 * to main-thread frame time. IO u64 fields use hi:lo, exact unsigned halves,
 * to avoid sceClibPrintf's inconsistent long-long formatting on hardware.
 */
#if defined(ISAAC_VITA_IO_WINDOW_PROFILE)
# include "kage_vita_io_profile.h"
static uint32_t s_io_baseline_valid, s_io_previous_win;
#endif
#if defined(ISAAC_VITA_STATIC_SFX_PROFILE)
# include "host_vita_static_sfx_profile.h"
static uint32_t s_sfx_baseline_valid, s_sfx_previous_win;
#endif
#if defined(ISAAC_VITA_NATIVE_RESOURCE_PROFILE)
# include "../vita/vitagl-stock-reference/isaac_native_resource_profile.h"
# if defined(ISAAC_VITA_STOCK_FBO_RT_REUSE)
#  include "../vita/vitagl-stock-reference/isaac_fbo_rt_reuse.h"
# endif
#endif
#if defined(ISAAC_VITA_PNG_WINDOW_PROFILE)
# include "host_vita_png_window_profile.h"
# include "host_vita_png_outer_profile.h"
static IsaacVitaPngWindowSnapshot s_png_previous;
static IsaacVitaPngOuterSnapshot s_png_outer_previous;
#endif
#if defined(ISAAC_VITA_STOCK_FBO_RT_INTERLUDE_REUSE)
# include "../vita/vitagl-stock-reference/isaac_fbo_rt_reuse.h"
#endif
#if defined(ISAAC_VITA_ANM2_WINDOW_PROFILE)
# include "host_vita_anm2_outer_profile.h"
static IsaacVitaAnm2OuterSnapshot s_anm2_outer_previous;
#endif
#if defined(ISAAC_VITA_PNG_TEXEL_INIT_ELISION)
# include "host_vita_png_texel_init.h"
static IsaacVitaPngTexelInitSnapshot s_png_init_previous;
#endif
#if defined(ISAAC_VITA_CRT_FILE_LOOKUP_HINT)
# include "host_vita_crt.h"
static uint32_t s_file_hint_previous[2], s_file_hint_valid, s_file_hint_win;
#endif

static void kage_vita_room_profile_discard(void)
{
#if defined(ISAAC_VITA_STOCK_FBO_RT_INTERLUDE_REUSE)
    IsaacFboRtInterludeStats rti;
    vglIsaacFboRtInterludeProfileTake(&rti);
#endif
#if defined(ISAAC_VITA_CRT_FILE_LOOKUP_HINT)
    s_file_hint_valid = (uint32_t)isaac_vita_crt_file_lookup_hint_get(s_file_hint_previous);
    s_file_hint_win = 0u;
#endif
#if defined(ISAAC_VITA_PNG_WINDOW_PROFILE)
    isaac_vita_png_window_snapshot(&s_png_previous);
    isaac_vita_png_outer_snapshot(&s_png_outer_previous);
#endif
#if defined(ISAAC_VITA_ANM2_WINDOW_PROFILE)
    isaac_vita_anm2_outer_snapshot(&s_anm2_outer_previous);
#endif
#if defined(ISAAC_VITA_PNG_TEXEL_INIT_ELISION)
    isaac_vita_png_texel_init_snapshot(&s_png_init_previous);
#endif
#if defined(ISAAC_VITA_IO_WINDOW_PROFILE)
    kage_vita_io_window_snapshot io;
    kage_vita_io_window_take(&io);
    s_io_baseline_valid = io.snapshot_valid;
    s_io_previous_win = 0u;
#endif
#if defined(ISAAC_VITA_STATIC_SFX_PROFILE)
    isaac_vita_static_sfx_window sfx;
    s_sfx_baseline_valid = (uint32_t)isaac_vita_static_sfx_profile_take_window(&sfx);
    s_sfx_previous_win = 0u;
#endif
#if defined(ISAAC_VITA_NATIVE_RESOURCE_PROFILE)
    IsaacNativeResourceStats nr;
    vglIsaacNativeResourceProfileTake(&nr);
# if defined(ISAAC_VITA_STOCK_FBO_RT_REUSE)
    IsaacFboRtReuseStats rr;
    vglIsaacFboRtReuseProfileTake(&rr);
# endif
#endif
}

#define KVPP_U64_ARGS(v) (uint32_t)((v) >> 32), (uint32_t)(v)
static void kage_vita_room_profile_report(const char *bid, uint32_t win,
                                          uint32_t loops)
{
#if defined(ISAAC_VITA_STOCK_FBO_RT_INTERLUDE_REUSE)
    {
        IsaacFboRtInterludeStats rti;
        vglIsaacFboRtInterludeProfileTake(&rti);
        KVPP_PRINTF("[kage-vita] ph120.rti bid=%.32s win=%u loops=%u "
            "hold=%u restore=%u sat=%u\n", bid, win, loops,
            rti.holds, rti.restores, rti.saturation_mask);
    }
#endif
#if defined(ISAAC_VITA_CRT_FILE_LOOKUP_HINT)
    {
        uint32_t values[2] = {0u, 0u};
        uint32_t valid = (uint32_t)isaac_vita_crt_file_lookup_hint_get(values);
        uint32_t hit = valid && s_file_hint_valid ? values[0] - s_file_hint_previous[0] : 0u;
        uint32_t miss = valid && s_file_hint_valid ? values[1] - s_file_hint_previous[1] : 0u;
        KVPP_PRINTF("[kage-vita] ph120.fh bid=%.32s win=%u loops=%u valid=%u base=%u from=%u "
            "hit=%u miss=%u total(h,m)=%u,%u sat=%u\n",
            bid, win, loops, valid, s_file_hint_valid, s_file_hint_win, hit, miss,
            values[0], values[1], values[0] == UINT32_MAX || values[1] == UINT32_MAX);
        if (valid) {
            s_file_hint_previous[0] = values[0];
            s_file_hint_previous[1] = values[1];
            s_file_hint_valid = 1u;
            s_file_hint_win = win;
        }
    }
#endif
#if defined(ISAAC_VITA_PNG_WINDOW_PROFILE)
    {
        IsaacVitaPngWindowSnapshot s;
        isaac_vita_png_window_snapshot(&s);
#define NP_DELTA(f) (s.f - s_png_previous.f)
        KVPP_PRINTF("[kage-vita] ph120.png bid=%.32s win=%u loops=%u "
            "images(start,native,fb)=%u,%u,%u rows=%u kib=%u "
            "decode=%u:%u io=%u:%u serve=%u:%u translated=%u:%u "
            "maxever(dec,trans)=%u,%u active=%u mode=%u\n",
            bid, win, loops, NP_DELTA(images), NP_DELTA(native), NP_DELTA(fallbacks),
            NP_DELTA(rows), NP_DELTA(kib), KVPP_U64_ARGS(NP_DELTA(decode_us)),
            KVPP_U64_ARGS(NP_DELTA(io_us)), KVPP_U64_ARGS(NP_DELTA(serve_us)),
            KVPP_U64_ARGS(NP_DELTA(translated_us)), s.decode_max_ever,
            s.translated_max_ever, s.active, s.mode);
        KVPP_PRINTF("[kage-vita] ph120.pngf bid=%.32s win=%u loops=%u "
            "fb(shape,state,stream,memory,decode)=%u,%u,%u,%u,%u "
            "allocrefused=%u reserved=%u oversize=%u rewindunsafe=%u "
            "pass=%u busy=%u abandoned=%u"
#if defined(ISAAC_VITA_NATIVE_PNG_LIBDEFLATE_STRICT) && ISAAC_VITA_NATIVE_PNG_LIBDEFLATE_STRICT
            " strict(attempt,success,refusal)=%u,%u,%u"
#endif
#if defined(ISAAC_VITA_NATIVE_PNG_REUSE) && ISAAC_VITA_NATIVE_PNG_REUSE
            " reuse(lookup,hit,store,evict,skip,kib)=%u,%u,%u,%u,%u,%u"
#endif
            "\n",
            bid, win, loops, NP_DELTA(fb_shape), NP_DELTA(fb_state), NP_DELTA(fb_stream),
            NP_DELTA(fb_memory), NP_DELTA(fb_decode), NP_DELTA(alloc_refused),
            NP_DELTA(reserved), NP_DELTA(oversize), NP_DELTA(rewind_unsafe),
            NP_DELTA(passthrough), NP_DELTA(busy), NP_DELTA(abandoned)
#if defined(ISAAC_VITA_NATIVE_PNG_LIBDEFLATE_STRICT) && ISAAC_VITA_NATIVE_PNG_LIBDEFLATE_STRICT
            , NP_DELTA(strict_attempts), NP_DELTA(strict_successes), NP_DELTA(strict_refusals)
#endif
#if defined(ISAAC_VITA_NATIVE_PNG_REUSE) && ISAAC_VITA_NATIVE_PNG_REUSE
            , NP_DELTA(reuse_lookups), NP_DELTA(reuse_hits), NP_DELTA(reuse_stores)
            , NP_DELTA(reuse_evictions), NP_DELTA(reuse_skipped), NP_DELTA(reuse_hit_kib)
#endif
            );
#undef NP_DELTA
        s_png_previous = s;
    }
    {
        IsaacVitaPngOuterSnapshot s;
        isaac_vita_png_outer_snapshot(&s);
#define NPO_DELTA(f) (s.f - s_png_outer_previous.f)
        KVPP_PRINTF("[kage-vita] ph120.pngo bid=%.32s win=%u loops=%u abi=%u "
            "images(start,done,timed)=%u,%u,%u us=%u:%u maxever=%u:%u "
            "depth=%u nested=%u bad(clock,sequence)=%u,%u sat=%u\n",
            bid, win, loops, ISAAC_VITA_PNG_OUTER_PROFILE_ABI,
            NPO_DELTA(started), NPO_DELTA(completed), NPO_DELTA(timed),
            KVPP_U64_ARGS(NPO_DELTA(total_us)), KVPP_U64_ARGS(s.max_us),
            s.depth, NPO_DELTA(nested), NPO_DELTA(bad_clock),
            NPO_DELTA(bad_sequence), s.saturated);
#undef NPO_DELTA
        s_png_outer_previous = s;
    }
#endif
#if defined(ISAAC_VITA_ANM2_WINDOW_PROFILE)
    {
        IsaacVitaAnm2OuterSnapshot s;
        isaac_vita_anm2_outer_snapshot(&s);
#define ANM2O_DELTA(f) (s.f - s_anm2_outer_previous.f)
        /* Inclusive loader duration; nested PNG/XML costs are not additive.
         * A completed C scope does not certify that loading succeeded.
         * Every ANM2 bad_sequence permanently poisons pairing. Preserve its
         * lifetime value across window deltas/discard: later zero deltas
         * must not masquerade as a healthy loader with no calls. */
        KVPP_PRINTF("[kage-vita] ph120.anm2o bid=%.32s win=%u loops=%u abi=%u "
            "calls(start,done,timed)=%u,%u,%u us=%u:%u maxever=%u:%u "
            "depth=%u nested=%u bad(clock,sequence)=%u,%u sat=%u badseqever=%u\n",
            bid, win, loops, ISAAC_VITA_ANM2_OUTER_PROFILE_ABI,
            ANM2O_DELTA(started), ANM2O_DELTA(completed), ANM2O_DELTA(timed),
            KVPP_U64_ARGS(ANM2O_DELTA(total_us)), KVPP_U64_ARGS(s.max_us),
            s.depth, ANM2O_DELTA(nested), ANM2O_DELTA(bad_clock),
            ANM2O_DELTA(bad_sequence), s.saturated, s.bad_sequence);
#undef ANM2O_DELTA
        s_anm2_outer_previous = s;
    }
#endif
#if defined(ISAAC_VITA_PNG_TEXEL_INIT_ELISION)
    {
        IsaacVitaPngTexelInitSnapshot s;
        isaac_vita_png_texel_init_snapshot(&s);
#define PNGI_DELTA(f) (s.f - s_png_init_previous.f)
        KVPP_PRINTF("[kage-vita] ph120.pngi bid=%.32s win=%u loops=%u abi=%u "
            "attempt=%u elided=%u bytes=%u:%u fault=%u sat=%u\n",
            bid, win, loops, ISAAC_VITA_PNG_TEXEL_INIT_ABI,
            PNGI_DELTA(attempts), PNGI_DELTA(elided),
            KVPP_U64_ARGS(PNGI_DELTA(bytes)), PNGI_DELTA(faults), s.saturated);
#undef PNGI_DELTA
        s_png_init_previous = s;
    }
#endif
#if defined(ISAAC_VITA_IO_WINDOW_PROFILE)
    {
        kage_vita_io_window_snapshot io;
        static const char *const op[] = {"open", "close", "read", "pread", "seek32", "seek64", "write", "pwrite", "sync", "syncfd"};
        uint32_t i;
        kage_vita_io_window_take(&io);
        KVPP_PRINTF("[kage-vita] ph120.io bid=%.32s win=%u loops=%u abi=%u u64=hi:lo "
            "max(us,op,cls,req,err)=%u:%u,%u,%u,%u,%d clock=%u collision=%u unknown=%u "
            "valid=%u base=%u from=%u miss=%u drop(rec,map)=%u,%u\n",
            bid, win, loops, io.abi_version, KVPP_U64_ARGS(io.max_us),
            io.max_op, io.max_class, io.max_requested_bytes, io.max_error,
            io.clock_reversals, io.map_collisions, io.unknown_calls,
            io.snapshot_valid, s_io_baseline_valid, s_io_previous_win,
            io.take_misses, io.record_drops, io.map_drops);
        for (i = 0u; i < KAGE_IO_OP_COUNT; ++i) {
            const kage_vita_io_window_operation *p = &io.op[i];
            KVPP_PRINTF("[kage-vita] ph120.ioo bid=%.32s win=%u loops=%u op=%.6s "
                "valid=%u base=%u calls=%u err=%u req=%u:%u ret=%u:%u us=%u:%u\n",
                bid, win, loops, op[i], io.snapshot_valid, s_io_baseline_valid, p->calls, p->errors,
                KVPP_U64_ARGS(p->requested_bytes), KVPP_U64_ARGS(p->returned_bytes),
                KVPP_U64_ARGS(p->time_us));
        }
        KVPP_PRINTF("[kage-vita] ph120.ioc bid=%.32s win=%u loops=%u valid=%u base=%u "
            "us(unknown,archive,resource,save,config,shader,log,other)="
            "%u:%u,%u:%u,%u:%u,%u:%u,%u:%u,%u:%u,%u:%u,%u:%u\n",
            bid, win, loops, io.snapshot_valid, s_io_baseline_valid, KVPP_U64_ARGS(io.class_us[0]),
            KVPP_U64_ARGS(io.class_us[1]), KVPP_U64_ARGS(io.class_us[2]),
            KVPP_U64_ARGS(io.class_us[3]), KVPP_U64_ARGS(io.class_us[4]),
            KVPP_U64_ARGS(io.class_us[5]), KVPP_U64_ARGS(io.class_us[6]),
            KVPP_U64_ARGS(io.class_us[7]));
        if (io.snapshot_valid) {
            s_io_baseline_valid = 1u;
            s_io_previous_win = win;
        }
    }
#endif
#if defined(ISAAC_VITA_STATIC_SFX_PROFILE)
    {
        isaac_vita_static_sfx_window s;
        int taken;
        memset(&s, 0, sizeof s);
        taken = isaac_vita_static_sfx_profile_take_window(&s);
        KVPP_PRINTF("[kage-vita] ph120.sfx bid=%.32s win=%u loops=%u take=%d base=%u from=%u "
            "wav(c,us,max)=%u,%u,%u ogg=%u,%u,%u riff=%u,%u,%u play=%u,%u,%u "
            "fail(w,o,fault)=%u,%u,%u skip=%u clock=%u overflow=%u\n",
            bid, win, loops, taken, s_sfx_baseline_valid, s_sfx_previous_win,
            s.wav.count, s.wav.sum_us, s.wav.max_us,
            s.ogg.count, s.ogg.sum_us, s.ogg.max_us,
            s.riff.count, s.riff.sum_us, s.riff.max_us,
            s.play.count, s.play.sum_us, s.play.max_us,
            s.wav_failed, s.ogg_failed, s.faulted, s.skipped, s.bad_clock, s.overflow);
        KVPP_PRINTF("[kage-vita] ph120.sfxn bid=%.32s win=%u loops=%u take=%d base=%u "
            "upload(c,us,max,req)=%u,%u,%u,%u create(c,us,max)=%u,%u,%u "
            "al(query,error,last)=%u,%u,%u\n",
            bid, win, loops, taken, s_sfx_baseline_valid, s.upload.count, s.upload.sum_us,
            s.upload.max_us, s.upload_bytes, s.create.count, s.create.sum_us,
            s.create.max_us, s.error_queries, s.al_errors, s.last_al_error);
        if (taken) {
            s_sfx_baseline_valid = 1u;
            s_sfx_previous_win = win;
        }
    }
#endif
#if defined(ISAAC_VITA_NATIVE_RESOURCE_PROFILE)
    {
        IsaacNativeResourceStats s;
        static const char *const names[] = {"gc", "finish", "rtcreate", "rtdestroy", "depth", "reccpu", "recgpu"};
        uint32_t i;
        vglIsaacNativeResourceProfileTake(&s);
        KVPP_PRINTF("[kage-vita] ph120.nr bid=%.32s win=%u loops=%u abi=%u "
            "sizes=%u overflow(size,reg)=%u,%u unknown(destroy,retire,scene)=%u,%u,%u "
            "clock=%u sat=%u gcobj=%u retire(rt,z)=%u,%u recoverbytes(cpu,gpu)=%u,%u\n",
            bid, win, loops, s.abi, s.sizes_used, s.size_overflow, s.registry_overflow,
            s.unknown_destroy, s.unknown_retire, s.scene_unknown, s.clock_clamp,
            s.saturation, s.gc_objects, s.rt_retired, s.depth_retired,
            s.recovery_cpu_bytes, s.recovery_gpu_bytes);
        KVPP_PRINTF("[kage-vita] ph120.free bid=%.32s win=%u loops=%u "
            "scope=vgl-pool-endpoint ram(f,t)=%u,%u vram(f,t)=%u,%u\n",
            bid, win, loops, s.ram_free, s.ram_total, s.vram_free, s.vram_total);
        for (i = 0u; i < ISAAC_NR_METRIC_COUNT; ++i) {
            const IsaacNativeResourceMetric *p = &s.metric[i];
            KVPP_PRINTF("[kage-vita] ph120.nrm bid=%.32s win=%u loops=%u op=%.9s "
                "c=%u us=%u max=%u failed=%u\n",
                bid, win, loops, names[i], p->count, p->us, p->max_us, p->failed);
        }
        for (i = 0u; i < s.sizes_used && i < ISAAC_NATIVE_RESOURCE_SIZE_SLOTS; ++i) {
            const IsaacNativeResourceSize *p = &s.size[i];
            KVPP_PRINTF("[kage-vita] ph120.nrs bid=%.32s win=%u loops=%u "
                "size=%ux%u end(c,us,max,fail)=%u,%u,%u,%u "
                "create=%u,%u,%u,%u destroy=%u,%u,%u,%u depth=%u,%u,%u,%u retire(rt,z)=%u,%u\n",
                bid, win, loops, p->width, p->height,
                p->end_scene.count, p->end_scene.us, p->end_scene.max_us, p->end_scene.failed,
                p->rt_create.count, p->rt_create.us, p->rt_create.max_us, p->rt_create.failed,
                p->rt_destroy.count, p->rt_destroy.us, p->rt_destroy.max_us, p->rt_destroy.failed,
                p->depth_create.count, p->depth_create.us, p->depth_create.max_us, p->depth_create.failed,
                p->rt_retired, p->depth_retired);
        }
# if defined(ISAAC_VITA_STOCK_FBO_RT_REUSE)
        {
            IsaacFboRtReuseStats rr;
            vglIsaacFboRtReuseProfileTake(&rr);
            /* Each switch has one outcome. Expiry and effective external
             * invalidation are separate events, not additional misses.
             * No clocks are read by this producer or the helper. */
            KVPP_PRINTF("[kage-vita] ph120.rr bid=%.32s win=%u loops=%u abi=%u "
                "switch=%u hit=%u first=%u expire(reuse,revoked)=%u,%u invalidate=%u "
                "reject(owner,size,policy,current,args,spare)=%u,%u,%u,%u,%u,%u sat=%u "
                "lease(grants,hits,drains)=%u,%u,%u\n",
                bid, win, loops, ISAAC_FBO_RT_REUSE_PROFILE_ABI,
                rr.switch_calls, rr.hits, rr.first_misses, rr.expired_reusable,
                rr.expired_revoked, rr.explicit_invalidations, rr.reject_owner,
                rr.reject_size, rr.reject_policy, rr.reject_current,
                rr.reject_args, rr.reject_spare, rr.saturation_mask,
                rr.lease_grants, rr.lease_hits, rr.lease_drains);
        }
# endif
    }
#endif
    (void)bid; (void)win; (void)loops;
}
#undef KVPP_U64_ARGS
#endif
