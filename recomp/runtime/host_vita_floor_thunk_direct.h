#ifndef ISAAC_HOST_VITA_FLOOR_THUNK_DIRECT_H
#define ISAAC_HOST_VITA_FLOOR_THUNK_DIRECT_H

#include "guest.h"

/* HANDLED executes the already-registered UCRT floor import inline: the same
 * newlib floor(), the same x87 push and the same cdecl return-word pop as the
 * math family's vita_math_floor, with the receipts of the indexed direct
 * route (guest_import_call).  REJECTED (table not ready, slot word other than
 * the authenticated floor token, frame outside the bound guest stack) leaves
 * CPU, guest stack and every counter untouched so the generated thunk can
 * execute its unchanged GUEST_IMPORT_JMP spelling. */
int isaac_vita_floor_thunk_direct_try(
    CPU *__restrict c, uint32_t target);

#endif
