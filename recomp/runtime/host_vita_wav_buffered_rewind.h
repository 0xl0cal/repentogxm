#ifndef HOST_VITA_WAV_BUFFERED_REWIND_H
#define HOST_VITA_WAV_BUFFERED_REWIND_H

#include "guest.h"

/* Entry-only try for frozen ArchivedFile::Seek 0059c690. Success consumes
 * its ret 8. Every refusal leaves CPU/guest bytes/errno untouched. */
int isaac_vita_wav_buffered_rewind_try(CPU *__restrict c);

#endif
