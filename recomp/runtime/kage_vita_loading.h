#ifndef KAGE_VITA_LOADING_H
#define KAGE_VITA_LOADING_H

#include <stdint.h>

/* CPU-rendered startup overlay owned by the already initialized vitaGL
 * display queue.  No second GXM context and no guest GL state are involved.
 * Stages and elapsed wall time are truthful activity signals; archive read
 * counts are deliberately never converted into a percentage.  The public
 * start/finish pair is a serial reusable lifecycle: start is idempotent while
 * active, and finish removes the callback then drains the queue.  A later UI
 * phase may reuse it only after kage_vita_loading_active() becomes false. */
enum kage_vita_loading_stage {
    KAGE_VITA_LOADING_INACTIVE = 0,
    KAGE_VITA_LOADING_BOOT = 1,
    KAGE_VITA_LOADING_VERIFY = 2,
    KAGE_VITA_LOADING_ARCHIVES = 3,
    KAGE_VITA_LOADING_SHADERS = 4,
    KAGE_VITA_LOADING_GAME = 5
};

void     kage_vita_loading_start(void);
void     kage_vita_loading_note_verify(void);
void     kage_vita_loading_note_fread(uint32_t completed_calls);
void     kage_vita_loading_note_shader(void);
void     kage_vita_loading_finish(void);
int      kage_vita_loading_active(void);
unsigned kage_vita_loading_stage(void);
unsigned kage_vita_loading_elapsed_seconds(void);
unsigned kage_vita_loading_swap_count(void);

#endif
