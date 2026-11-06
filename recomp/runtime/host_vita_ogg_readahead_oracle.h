#ifndef ISAAC_HOST_VITA_OGG_READAHEAD_ORACLE_H
#define ISAAC_HOST_VITA_OGG_READAHEAD_ORACLE_H

/* Host-only seam for host_vita_ogg_readahead.c (ISAAC_VITA_OGG_READAHEAD_ORACLE).
 * The oracle supplies newlib's reentrant syscalls (the __real_ side of the
 * link-time wraps), the worker's descriptor calls and the clock, so a test
 * can run the module against POSIX files and inject delays and failures. */

#include <stddef.h>
#include <stdint.h>

/* Vita newlib: struct _reent starts with _errno; _off_t is a 32-bit long on
 * arm-vita-eabi and _ssize_t an int.  The oracle keeps those widths so the
 * module's arithmetic is exercised at the target's sizes. */
struct _reent {
    int _errno;
};
typedef int32_t isaac_ogg_ra_off_t;
typedef int32_t isaac_ogg_ra_ssize_t;

isaac_ogg_ra_ssize_t __real__read_r(struct _reent *r, int fd, void *buf,
                                    size_t n);
isaac_ogg_ra_off_t __real__lseek_r(struct _reent *r, int fd,
                                   isaac_ogg_ra_off_t off, int dir);
int __real__close_r(struct _reent *r, int fd);

isaac_ogg_ra_ssize_t __wrap__read_r(struct _reent *r, int fd, void *buf,
                                    size_t n);
isaac_ogg_ra_off_t __wrap__lseek_r(struct _reent *r, int fd,
                                   isaac_ogg_ra_off_t off, int dir);
int __wrap__close_r(struct _reent *r, int fd);

/* The reent newlib would pass for the calling thread (_REENT). */
struct _reent *isaac_ogg_ra_oracle_reent(void);
/* fstat(fd).st_size of the game descriptor; 0 on success. */
int isaac_ogg_ra_oracle_fstat_size(int fd, uint32_t *size);
/* Worker descriptor: open read-only, pread, close (negative = failure). */
int isaac_ogg_ra_oracle_open(const char *path);
int32_t isaac_ogg_ra_oracle_pread(int wfd, void *buf, uint32_t n,
                                  uint32_t offset);
int isaac_ogg_ra_oracle_close(int wfd);
uint32_t isaac_ogg_ra_oracle_thread_id(void);
void isaac_vita_log(const char *format, ...);

/* Module-side hooks the oracle calls between scenarios. */
void isaac_vita_ogg_ra_oracle_shutdown(void);
/* Number of entries not FREE (0 once the worker retired everything). */
unsigned isaac_vita_ogg_ra_oracle_busy_entries(void);
/* Hold (1) or release (0) the entry lock of registered `fd` as the worker
 * would; 0 when `fd` is not registered. */
int isaac_vita_ogg_ra_oracle_hold_lock(int fd, int hold);

#endif
