#ifndef ISAAC_HOST_VITA_ARCHIVE_PREFETCH_H
#define ISAAC_HOST_VITA_ARCHIVE_PREFETCH_H

/* DESIGN STUB -- not compiled, not wired.  A trail-driven read-ahead for the
 * packed archives on the second CPU core.  Written 2026-09-10 against
 * the read-trail knob; measure with ISAAC_VITA_ARCHIVE_READ_TRAIL and a
 * ph120.rt log analyzer (not in this repository) before implementing.
 *
 * 1. WHAT EXISTS (and what does not)
 *
 * There is no data cache to "warm".  ISAAC_VITA_ARCHIVE_FILE_CACHE
 * (host_vita_archive_cache.c) keeps ONE idle newlib FILE per process so the
 * fopen/fclose ArchivedFile performs per entry reuses a descriptor; the raw
 * SceIo lane in host_vita_crt.c (vita_crt_raw_archive_*) is the ENOMEM
 * fallback and issues one sceIoPread per logical fread.  The newlib lane
 * refills a 16 KiB setvbuf buffer (ISAAC_VITA_CRT_FILE_READ_BUFFER_SIZE), so
 * a 400 KiB sheet is ~25 sceIoRead calls.  ISAAC_VITA_FIOS_CACHE (release ON)
 * is a kernel-side FIOS2 RAM cache of 8 x 64 KiB = 512 KiB over the data
 * root: too small for one room (0.8-3.5 MB of archive reads per transition
 * window on the device) and its replacement policy is not ours.  The
 * validation receipt (host_vita_archive_receipt.c) is boot-time proof that
 * the archives are the exact frozen files; it records no reads.
 *
 * Measured (bid perf:spike-r1, ph120.ioo/ioc/png, 20 transition windows):
 * archive-class native I/O 175-590 ms per window from 50-223 sceIoRead calls
 * of 16 KiB (0.8-3.5 MB, ~2.7 ms per call, ~6 MB/s), against 220-660 ms of
 * PNG decode in the same windows.  I/O is 40-47 % of a transition; decode is
 * the rest.  Read-ahead can hide the I/O, not the decode.
 *
 * All CRT FILE-token calls come from the single guest thread and run under
 * the CRT's FILE-registry spinlock (s_file_lock); the only cross-thread
 * reader today is the trail take (try-lock, never waits).
 *
 * 2. THE TRAIL (this round)
 *
 * One record per ArchivedFile run: (archive basename, table offset, bytes,
 * fread calls, process time, kind).  The analyzer groups them per "Room a.b"
 * marker and prints per transition: bytes, distinct entries, backward seeks,
 * how many entries were already read earlier in the launch (the warm-able
 * share) and how many read after the marker (the overlap-able share).  Those
 * two ratios decide whether a prefetch is worth its memory.
 *
 * 3. PROPOSED SHAPE: ISAAC_VITA_ARCHIVE_WINDOW_CACHE (future knob, default OFF)
 *
 * Storage: a dedicated USER_RW memblock, N windows of W bytes (start with
 * 6 x 512 KiB = 3 MiB; late gameplay has shown ~11.5 MiB USER free and the
 * guest heap is ~98 % full with EID, so never the guest heap).  Fail-open:
 * no memblock, no cache.
 *
 * Window table (under s_file_lock only; one lock, no nesting):
 *   struct window { uint8_t key; uint32_t offset;   -- W-aligned archive offset
 *                   uint32_t length; uint8_t state; -- FREE | FILLING | READY
 *                   uint32_t generation; uint32_t last_use_loop;
 *                   uint8_t pinned; }
 *
 * Game thread (vita_crt_fread on a keyed token, lock held):
 *   - hit:  a READY window covering [pos, pos + n) with pos + n <= archive
 *           size -> memcpy, advance the logical cursor (raw lane: position;
 *           newlib lane: the FILE's real cursor must be moved too, i.e. the
 *           hit path is only exact for the raw lane or for a token whose
 *           stream cursor is repositioned with one fseek before the next
 *           native read).  Serve only whole in-range requests; anything
 *           touching EOF, a short read or an error goes native so feof/
 *           ferror/errno/ftell stay byte-identical.
 *   - FILLING: never wait for the worker; go native.
 *   - miss: native as today.  Optional demand fill (single-thread, no
 *           worker): pread the whole W window into a FREE/evictable window
 *           and serve from it -- one 512 KiB pread instead of 32 x 16 KiB
 *           reads.  This is the smallest safe step and needs its own
 *           measurement: is the 2.7 ms per 16 KiB call per-call overhead
 *           (then W-sized reads win ~3x) or raw card bandwidth (then not)?
 *
 * Worker (core 2, priority below the game thread, above the logger;
 * SCE_KERNEL_CPU_MASK_USER_2 like the audio/vorbis/logger threads):
 *   - owns its own read-only SceIo descriptor per archive (never a newlib
 *     FILE, never the game's descriptor); pread is position-less.
 *   - takes s_file_lock only to allocate a FILLING window and to publish it
 *     READY (release barrier); never holds it across I/O.
 *   - at most one pread in flight, W <= 512 KiB, so it cannot queue the
 *     card behind the game thread's own read for long; skips a fill while
 *     the CRT flags a native archive read in progress (best effort).
 *   - ENODEV after suspend/resume (the DESCRIPTOR_RECOVER lesson): reopen
 *     its descriptors; READY windows are memory and stay valid.
 *
 * Prediction: the trail keyed by the room pair a.b (ISAAC_VITA_CRT_ROOM_LOG
 * seam, vita_crt_room_log_completed) -- on the "Room a.b(" line, queue the
 * (key, offset, length) list recorded for that pair (or the stage's union)
 * and let the worker fill while the game thread decodes the first sheets.
 * The next room is not known before its marker, so the gain is the overlap
 * of I/O with decode inside one transition (up to the I/O share, 40-47 %),
 * plus re-visits (the warm share).  Trail persistence across launches (a
 * small ux0: file like the receipt) is optional; first measure in-process.
 *
 * 4. WHAT MUST NEVER HAPPEN
 *
 *   a. Evicting a live window: a window is evictable only when FREE or
 *      READY, unpinned, and not covering the logical cursor of any live
 *      keyed token.  A window under memcpy is pinned for the copy.
 *   b. Racing the game thread: the worker writes only FILLING windows it
 *      allocated; the game thread reads only READY windows; both under the
 *      one existing lock for table changes; no second lock, no nesting.
 *   c. Changing an observable read: identical bytes (archives immutable for
 *      the process; the receipt proves identity), identical result counts,
 *      feof/ferror/errno/ftell as the native path; hits only for whole
 *      in-range requests.
 *   d. Blocking the game thread on the worker, or the worker on the game
 *      thread's spinlock for longer than a table update.
 *   e. Taking memory from the guest heap or from vitaGL's pool.
 *   f. Touching the game's own descriptors or FILEs from the worker.
 *   g. Repacking or re-laying-out the archives: the receipt pins the exact
 *      SHA-256 of afterbirth.a/animations.a and the user installs Steam's
 *      files; the layout question (the analyzer's back=, order files) is a
 *      measurement of how far from monotone the reads are, not a plan to
 *      rewrite archives.
 *
 * 5. VERDICT (2026-09-10)
 *
 * Not small: memblock + window table + worker + exact cursor mirroring in the
 * hit path + a room-pair trail store, and it needs the device numbers the
 * trail knob produces.  Delivered this round: the trail knob, the analyzer,
 * this design.  Smallest next step if the numbers justify it: the demand
 * fill (no worker) behind its own knob, measured on the device against the
 * ph120.ioo read us/call. */

#endif
