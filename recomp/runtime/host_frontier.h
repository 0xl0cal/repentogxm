#ifndef ISAAC_HOST_FRONTIER_H
#define ISAAC_HOST_FRONTIER_H

/* Diagnostics owned by the host/platform boundary.  Keep them out of
 * guest.h: changing an inventory counter must not invalidate all generated
 * guest objects and trigger a multi-minute rebuild. */
#define HOST_PROC_LOG_MAX 128U

extern unsigned g_guest_getproc_calls;
extern unsigned g_guest_getproc_logged;
const char *guest_host_getproc_name(unsigned index);

#endif
