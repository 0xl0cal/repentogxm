#ifndef HOST_VITA_KAGE_BIND_PACKET_H
#define HOST_VITA_KAGE_BIND_PACKET_H

/* ISAAC_VITA_KAGE_BIND_PACKET (RENDER60 W-pkt, bind packet stage 1): the
 * --wrap seam over frozen KAGE Shader::Bind (sub_005671d0), see
 * host_vita_kage_bind_packet.c.  The wrapper runs the native replay when
 * every entry check passes and the translated body otherwise. */

#include <stdint.h>

#include "guest.h"

void __wrap_sub_005671d0(CPU *__restrict c);

/* Cumulative verdicts since boot: {handled, declined, cold lookups taken
 * synchronously inside a handled bind}.  Game thread only. */
void isaac_vita_kage_bind_packet_counts(uint32_t out[3]);

/* Cumulative declines by the first check that failed: {frame, object,
 * records, image, target, tokens, recorder, owner} (the order of the log
 * line).  Game thread only. */
void isaac_vita_kage_bind_packet_declines(uint32_t out[8]);

#endif
