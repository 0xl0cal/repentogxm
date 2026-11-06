#ifndef ISAAC_HOST_VITA_OGG_READAHEAD_H
#define ISAAC_HOST_VITA_OGG_READAHEAD_H

/* ISAAC_VITA_OGG_READAHEAD: music stream read-ahead below newlib.
 *
 * The native Vorbis mirrors (host_vita_native_vorbis.c) already decode on
 * CPU 2, but their OGG bytes still come from the game thread: nv_topup calls
 * the KAGE stream through nv_guest_vcall, the translated ArchivedFile reads
 * its blocks through vita_crt_fread, and newlib refills its 16 KiB FILE
 * buffer with one blocking sceIoRead (fread -> __srefill_r -> __sread ->
 * _read_r).  This module answers those _read_r calls from chunks a CPU-2
 * worker has already read with sceIoPread on its own descriptor.
 *
 * The cut is below newlib on purpose: newlib's FILE state is a function of
 * the values _read_r/_lseek_r/_close_r return, so a wrapper that returns the
 * same values (same bytes, same counts, same errno) keeps every FILE field,
 * every cursor and every guest byte identical without mirroring any of them.
 * See host_vita_ogg_readahead.c for the exact contract and its one
 * documented failure-path difference.
 *
 * Game thread only, except where noted. */

/* Set to 1 by nv_guest_vcall around its guest_call and back to 0 after it:
 * the CRT registers a descriptor only for reads issued inside that window,
 * i.e. by the native Vorbis stream top-ups and seeks. */
extern int isaac_vita_ogg_ra_context;

/* vita_crt_fread, under the CRT FILE lock, before the native read: start
 * read-ahead for the newlib descriptor `fd` of a read-only token whose
 * resolved native path is `path`.  Idempotent; never changes errno. */
void isaac_vita_ogg_ra_note(int fd, const char *path);

/* vita_crt_fclose, before the archive cache idles or closes the FILE: give
 * the kernel position back to newlib's view and leave the read-ahead. */
void isaac_vita_ogg_ra_forget(int fd);

/* nv_guest_vcall, after the guest call (outside the CRT FILE lock): print
 * the bounded receipt lines when due.  Preserves errno. */
void isaac_vita_ogg_ra_report_if_due(void);

typedef struct isaac_vita_ogg_ra_stats {
    /* game thread */
    unsigned reads;         /* _read_r on a registered descriptor */
    unsigned hits;          /* ... answered from worker chunks */
    unsigned hit_eof;       /* ... answered 0 at/after the end */
    unsigned misses;        /* ... handed to newlib's own _read_r */
    unsigned hit_kb;
    unsigned miss_kb;
    unsigned miss_us;       /* time inside the real _read_r on misses */
    unsigned waits;         /* reads that waited for a FILLING chunk (then
                               hit, or timed out into a miss) */
    unsigned wait_us;
    unsigned wait_timeouts;
    unsigned lock_busy;     /* coverage check gave up on the entry lock */
    unsigned vseeks;        /* _lseek_r answered without a syscall */
    unsigned fseeks;        /* _lseek_r forwarded (SEEK_END, out of range) */
    unsigned syncs;         /* real SEEK_SET issued before a forwarded call */
    unsigned sync_fail;
    unsigned pass_reads;    /* reads forwarded because of passthrough */
    unsigned foreign;       /* registered descriptor used by another thread */
    unsigned registered;
    unsigned reg_full;
    unsigned reg_fail;
    unsigned retired;
    unsigned forget_sync;
    unsigned real_fail;     /* a forwarded real read failed */
    /* worker */
    unsigned fills;
    unsigned fill_kb;
    unsigned fill_us;
    unsigned fill_err;
    unsigned fill_short;
    unsigned open_fail;
    unsigned fill_dropped;  /* chunk finished after its entry retired */
    /* start */
    int worker_rc;
    unsigned worker_started;
} isaac_vita_ogg_ra_stats;

void isaac_vita_ogg_ra_snapshot(isaac_vita_ogg_ra_stats *out);

#endif
