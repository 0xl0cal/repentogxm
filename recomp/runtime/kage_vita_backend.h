#ifndef KAGE_VITA_BACKEND_H
#define KAGE_VITA_BACKEND_H

#include <stdint.h>

/* Process-wide vitaGL owner used by the high-level KAGE boundary.  The
 * physical display is 960x544; KAGE keeps its measured 960x540 logical size. */
int         kage_vita_backend_initialize(uint32_t width, uint32_t height);
void        kage_vita_backend_deactivate(void);
int         kage_vita_backend_present(void);
/* Records a request but always applies GL_FALSE: guest software pacing owns
 * the Vita session's actual 60 Hz policy. */
int         kage_vita_backend_set_vsync(int enabled);
int         kage_vita_backend_time_seconds(double *seconds);
int         kage_vita_backend_ready(void);
uint32_t    kage_vita_backend_width(void);
uint32_t    kage_vita_backend_height(void);
unsigned    kage_vita_backend_present_count(void);
unsigned    kage_vita_backend_vsync_change_count(void);
int         kage_vita_backend_vsync_enabled(void);
const char *kage_vita_backend_gl_version(void);
const char *kage_vita_backend_last_error(void);

#define KAGE_VITA_MEMORY_SNAPSHOT_MAX_BODY 366u

#if defined(ISAAC_VITA_IO_PROFILE)
/* Called by the project-side vitaGL render-target event endpoint at the first
 * 1024x1024 FBO acquisition.  The implementation is diagnostic-only: query
 * failures are recorded and never change rendering or startup. */
void kage_vita_backend_memory_snapshot_at_first_fbo(
    int32_t event_result, const void *target);
#endif

#endif
