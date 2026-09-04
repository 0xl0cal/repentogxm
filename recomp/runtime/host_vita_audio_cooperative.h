#ifndef ISAAC_HOST_VITA_AUDIO_COOPERATIVE_H
#define ISAAC_HOST_VITA_AUDIO_COOPERATIVE_H

#include "guest.h"

/* Run one throttled Sound::Manager::Update on the translated CPU owner.
 * The audio-off build provides the same symbol as a strict no-op. */
void isaac_vita_audio_cooperative_poll(CPU *__restrict c);

/* Save parsing may call its safe point thousands of times.  Keep the stream
 * alive at the save-specific cadence without making every 5 ms of parser work
 * pay for another synchronous Sound::Manager::Update. */
void isaac_vita_audio_cooperative_save_poll(CPU *__restrict c);

#endif
