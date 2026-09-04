/* Asynchronous whole-file writer for the guest's binary save files.
 *
 * Measured rationale (Bundle35, bid=perf:bundle35-raster720-v1): every
 * Game::Update window without texture uploads that still stalled 0.5-0.9 s,
 * and the 12-33 ms in-room spikes, are the only remaining synchronous host
 * I/O on the frame thread: the guest rewrites gamestateN.dat (68,741 bytes
 * measured) on every room change and persistentgamedataN.dat (5,996 bytes)
 * on stat changes.  host_vita_crt.c enlarges the FILE buffer only for read
 * modes, so newlib's 1 KiB default buffer turns one 68 KiB fwrite into ~68
 * sceIoWrite syscalls plus open/truncate/close, all on the main thread.
 *
 * With ISAAC_VITA_ASYNC_SAVE_WRITE the CRT keeps a "wb" *.dat stream below
 * the Documents save root as a host memory image.  fclose hands the complete
 * image to one SceKernel worker thread which performs a single
 * open/write/close.  Only host memory crosses threads: the worker never reads
 * guest memory or guest state, and the translated code observes the same
 * fwrite/fseek/ftell/fclose results it would from newlib.
 *
 * Read-your-writes: every path-based import (fopen, remove, _access,
 * GetFileAttributesA, FindFirstFileW) first waits for pending jobs on that
 * path (or all jobs for directory scans), so a later read always observes
 * the completed file.  Process exit drains the queue before teardown. */
#ifndef ISAAC_HOST_VITA_ASYNC_WRITE_H
#define ISAAC_HOST_VITA_ASYNC_WRITE_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The startup path mapper bounds native paths at 1024 bytes. */
#define ISAAC_VITA_ASYNC_WRITE_PATH_MAX          1024U
/* The raw allocator gate poisons malloc in every runtime source, so images
 * live in fixed lanes of one lazily reserved USER_RW memblock.  gamestate1.dat
 * measured 68,741 bytes and persistentgamedata1.dat 5,996 bytes; a 256 KiB
 * lane leaves 3.7x headroom, four lanes cover the observed burst of one
 * gamestate plus one persistent rewrite per room with two spare.  A fifth
 * concurrent image falls back to the native FILE, never to a fault. */
#define ISAAC_VITA_ASYNC_WRITE_LANE_BYTES        (256U * 1024U)
#define ISAAC_VITA_ASYNC_WRITE_LANE_COUNT        4U
#define ISAAC_VITA_ASYNC_WRITE_QUEUE_CAPACITY    ISAAC_VITA_ASYNC_WRITE_LANE_COUNT
#define ISAAC_VITA_ASYNC_WRITE_ARENA_BYTES \
    (ISAAC_VITA_ASYNC_WRITE_LANE_BYTES * ISAAC_VITA_ASYNC_WRITE_LANE_COUNT)
#define ISAAC_VITA_ASYNC_WRITE_SNAPSHOT_ABI      1U

typedef struct isaac_vita_async_write_image {
    uint8_t *data;
    uint32_t size;
    uint32_t capacity;
    uint32_t position;
    uint8_t live;
    uint8_t error;
    uint8_t lane;
    uint8_t reserved;
    char path[ISAAC_VITA_ASYNC_WRITE_PATH_MAX + 1U];
} isaac_vita_async_write_image;

typedef struct isaac_vita_async_write_snapshot {
    uint32_t abi;
    uint32_t images_opened;
    uint32_t images_lane_exhausted;
    uint32_t images_arena_failed;
    uint32_t images_closed_with_error;
    uint32_t jobs_queued;
    uint32_t jobs_synchronous;
    uint32_t jobs_completed;
    uint32_t jobs_failed;
    uint32_t bytes_written;
    uint32_t sync_waits;
    uint32_t sync_wait_us;
    uint32_t sync_wait_us_max;
    uint32_t io_us_total;
    uint32_t io_us_max;
    uint32_t peak_queue;
    uint32_t worker_state;
} isaac_vita_async_write_snapshot;

enum {
    ISAAC_VITA_ASYNC_WRITE_WORKER_COLD = 0,
    ISAAC_VITA_ASYNC_WRITE_WORKER_READY = 1,
    ISAAC_VITA_ASYNC_WRITE_WORKER_FAILED = 2,
    ISAAC_VITA_ASYNC_WRITE_WORKER_STOPPED = 3
};

/* 1 when the native path is a "*.dat" below the Documents save root opened
 * in exact mode "wb" or "w".  Archives, log.txt and options.ini stay native. */
int isaac_vita_async_write_eligible(const char *native_path,
                                    const char *mode);

/* Start an empty image for the path.  Returns 0 (image not live) when no
 * lane is free or the arena could not be reserved, so the caller can fall
 * back to the native FILE. */
int isaac_vita_async_write_image_open(isaac_vita_async_write_image *image,
                                      const char *native_path);

/* newlib-compatible fwrite/fseek/ftell semantics on the memory image. */
size_t isaac_vita_async_write_image_write(isaac_vita_async_write_image *image,
                                          const void *buffer, size_t size,
                                          size_t count);
int isaac_vita_async_write_image_seek(isaac_vita_async_write_image *image,
                                      long offset, int origin);
long isaac_vita_async_write_image_tell(
    const isaac_vita_async_write_image *image);

/* Hand the image to the worker (or write it synchronously when no worker is
 * available).  Returns 0, or EOF when the image carries an error flag; the
 * image is never live afterwards. */
int isaac_vita_async_write_image_close(isaac_vita_async_write_image *image);

/* Wait until no queued or running job targets this exact native path. */
void isaac_vita_async_write_sync_path(const char *native_path);

/* Wait until the queue is empty. */
void isaac_vita_async_write_drain(void);

/* Drain, publish one summary receipt and stop accepting new work. */
void isaac_vita_async_write_shutdown(void);

void isaac_vita_async_write_snapshot_read(
    isaac_vita_async_write_snapshot *snapshot);

#if defined(ISAAC_VITA_ASYNC_WRITE_ORACLE)
/* The host oracle drives the worker by hand: perform one queued job. */
int isaac_vita_async_write_oracle_run_one(void);
/* Number of jobs currently queued or running. */
uint32_t isaac_vita_async_write_oracle_pending(void);
/* Strand the oldest PENDING job in RUNNING without completing it, to exercise
 * the bounded read-your-writes wait against a wedged worker. */
int isaac_vita_async_write_oracle_strand_one(void);
/* Return the module to a pristine, non-shutdown state for an independent case. */
void isaac_vita_async_write_oracle_reset(void);
#endif

#ifdef __cplusplus
}
#endif

#endif
