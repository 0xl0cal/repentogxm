#ifndef ISAAC_HOST_VITA_RTTI_H
#define ISAAC_HOST_VITA_RTTI_H

#include <stdint.h>

#include "guest.h"

/* Exact VCRUNTIME RTTI import in the frozen Repentance PE.  The physical
 * census scans every E8 in .text, including sites outside an emitted CFG;
 * it is therefore a PE denominator, not a dynamic-call count. */
#define ISAAC_VITA_RTTI_IMPORT_NAME \
    "VCRUNTIME140.dll!__RTDynamicCast"
#define ISAAC_VITA_RTTI_IAT_RVA                  0x00606464U
#define ISAAC_VITA_RTTI_THUNK_RVA                0x005ec34cU
#define ISAAC_VITA_RTTI_IMPORT_COUNT                      1U
#define ISAAC_VITA_RTTI_CDECL_ARGUMENT_COUNT              5U
#define ISAAC_VITA_RTTI_PHYSICAL_CALL_COUNT              662U
#define ISAAC_VITA_RTTI_PHYSICAL_CALL_FNV64 \
    UINT64_C(0x6acddf0a2f8c8863)

/* Dense import-bit ID under the unchanged 413-slot PE import mapping.  The
 * 2026-08-21 normal-PC cumulative map marked this bit; coverage proves reach,
 * while the static census above supplies the complete callsite denominator. */
#define ISAAC_VITA_RTTI_PC_COVERAGE_IMPORT_ID            272U
#define ISAAC_VITA_RTTI_PC_COVERAGE_HIT_COUNT              1U

int isaac_vita_rtti_import(CPU *__restrict c, const char *name);
int isaac_vita_rtti_import_counted(CPU *__restrict c, const char *name,
                                   unsigned *call_count);

#endif
