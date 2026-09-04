#ifndef KAGE_VITA_IO_PROFILE_H
#define KAGE_VITA_IO_PROFILE_H

#include <stdint.h>

/* Optional startup-only native I/O profiler.  It is linked with
 * --wrap=sceIoRead/--wrap=sceIoLseek32/--wrap=sceIoLseek only in a diagnostic
 * build, so release builds pay no per-I/O clock or atomic cost. */
void kage_vita_io_profile_begin(void);
void kage_vita_io_profile_report(const char *reason);
/* Called only at bounded completed-fread checkpoints.  It leaves the startup
 * epoch active and emits at most one compact progress pair per checkpoint. */
void kage_vita_io_profile_progress(uint32_t completed_calls);

/* Texture uploads are rare enough to time individually, unlike the two
 * million logical fread calls.  KIND is zero for allocation/upload and one
 * for sub-image replacement. */
void kage_vita_io_profile_texture_begin(
    uint32_t kind, int32_t width, int32_t height);
void kage_vita_io_profile_texture_end(void);

#endif
