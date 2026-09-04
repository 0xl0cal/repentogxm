/* Typed guest-OpenGL boundary generated from the measured game surface.
 *
 * This header intentionally does not include a native OpenGL header.  The
 * guest is a 32-bit Windows/APIENTRY program; a PC GL header, vitaGL's header,
 * and the guest ABI are three different interfaces.  Mixing any two in one
 * declaration is exactly the kind of silent signedness/calling-convention bug
 * this boundary exists to prevent.
 *
 * Pointer-shaped GL parameters remain guest_gl_addr values.  A concrete PC or
 * Vita backend may translate them only at its own edge.  In particular,
 * glShaderSource receives the address of an array of 32-bit guest addresses,
 * never a native FARPROC-compatible char ** by accident.
 */
#ifndef REPENTOGXM_GL_BRIDGE_H
#define REPENTOGXM_GL_BRIDGE_H

#include <stddef.h>
#include <stdint.h>

#include "guest.h"

typedef uint32_t guest_gl_addr;
typedef uint32_t guest_gl_enum;
typedef uint32_t guest_gl_bitfield;
typedef uint32_t guest_gl_uint;
typedef int32_t  guest_gl_int;
typedef int32_t  guest_gl_sizei;
typedef uint8_t  guest_gl_boolean;
typedef float    guest_gl_float;
typedef double   guest_gl_double;

#define REPENTOGXM_GL_BRIDGE_TYPES_READY 1
#include "gl_surface_generated.h"
#undef REPENTOGXM_GL_BRIDGE_TYPES_READY

typedef enum guest_gl_return_kind {
    GUEST_GL_RETURN_VOID = 0,
    GUEST_GL_RETURN_U32 = 1,
    GUEST_GL_RETURN_I32 = 2,
    GUEST_GL_RETURN_GUEST_ADDR = 3
} guest_gl_return_kind;

typedef struct guest_gl_symbol {
    uint32_t             token;
    const char          *name;
    uint16_t             x86_stack_bytes;
    uint8_t              argument_count;
    guest_gl_return_kind return_kind;
} guest_gl_symbol;

/* Longest guest spelling accepted at the provider seam.  The frozen typed
 * surface is much shorter; this bound exists so an attributed unknown name
 * can be logged without ever handing a guest pointer to native stdio. */
#define GUEST_GL_PROCEDURE_NAME_MAX 63U

/* Copy a backend table.  NULL resets every function to loud-unsupported.  A
 * partial table is valid during bring-up: only a call through an absent member
 * faults, and the fault names that exact GL symbol. */
void guest_gl_install_backend(const guest_gl_backend *backend);

/* Resolve only names in the frozen typed registry: 51 original measured names
 * plus 22 supplemental direct-generated edges.  Unknown names return 0, never
 * a native function pointer. */
uint32_t guest_gl_resolve(const char *name);

/* Copy one bounded ASCII name out of identity-mapped guest memory and resolve
 * it through the same exact typed registry.  `copied_name` is always
 * terminated; a malformed pointer/name leaves it empty.  Unknown but valid
 * ASCII remains in the buffer and returns zero so the platform boundary can
 * produce an attributed loud fault. */
uint32_t guest_gl_resolve_guest(
    uint32_t guest_name,
    char copied_name[GUEST_GL_PROCEDURE_NAME_MAX + 1U]);

/* Exact fastcall success ABI of the frozen libepoxy provider resolver at
 * RVA 0x005700e0: ECX is the canonical guest name, EAX receives a typed token,
 * and plain RET consumes only the guest return word.  The caller-owned
 * entrypoint-offset argument at the new ESP is deliberately preserved.
 * Unknown/malformed names return zero without changing CPU state. */
int guest_gl_resolve_provider(
    CPU *__restrict c,
    char copied_name[GUEST_GL_PROCEDURE_NAME_MAX + 1U]);

/* Dispatch one exact synthetic token.  Returns zero only when the token does
 * not belong to this registry.  A known-but-unimplemented symbol returns one
 * after recording a loud guest fault (or exits through an active run boundary). */
int guest_gl_dispatch(CPU *__restrict c, uint32_t token);

/* Exact registry membership of one token: the same verdict as scanning
 * guest_gl_symbols() for an equal token, with no CPU or backend side effect.
 * Under ISAAC_VITA_GL_SHIM_FASTDISPATCH this is the O(1) direct-mapped probe
 * that guest_gl_dispatch itself uses; the linear-scan spelling is compiled
 * only for the differential oracle (ISAAC_GL_SHIM_FASTDISPATCH_ORACLE), so a
 * production build without the option is instruction-identical to before. */
int guest_gl_token_is_registered(uint32_t token);

/* Production Vita entry for one 0x7e token: registry membership, the dynamic
 * call census and the ownership-guarded adapter in one pass.  Returns zero
 * with no CPU, backend or counter side effect when the token is not in the
 * frozen registry; otherwise increments *call_count (when non-NULL) before
 * the adapter runs - a missing Vita callback faults inside the adapter and
 * production guest_fault does not return - and returns one.  Defined under
 * ISAAC_VITA_GL_SHIM_FASTDISPATCH (one direct-mapped lookup for both the
 * membership verdict and the adapter) and, as the historical host_vita_gl.c
 * sequence (linear scan, count, guest_gl_dispatch), for the differential
 * oracle only (ISAAC_GL_SHIM_FASTDISPATCH_ORACLE). */
int guest_gl_dispatch_counted(CPU *__restrict c, uint32_t token,
                              unsigned *call_count);

#if defined(ISAAC_VITA_SHADER_ATTRIB_FASTPATH)
/* ---- ISAAC_VITA_SHADER_ATTRIB_FASTPATH (wf/opt-attrib) begin ----------
 * Host replay of an authenticated translated GL call sequence
 * (host_vita_shader_attrib_fastpath.c) into the same typed wrappers the
 * generated adapters call.  The table is the installed backend (never NULL;
 * an absent member is NULL and the replay declines).  enter/leave are the
 * ownership state machine of guest_gl_run_owned around one adapter, split so
 * one replay can wrap several wrapper calls: enter returns zero and changes
 * nothing unless the word is free (IDLE, or a returning oracle's FAULTED
 * sentinel) and `c` is or becomes the pinned CPU; leave returns the word to
 * IDLE from `c`/FAULTED and otherwise raises run_owned's "ownership changed"
 * fault attributed to `token`.  Requires ISAAC_VITA_GL_SHIM_FASTDISPATCH. */
const guest_gl_backend *guest_gl_installed_backend(void);
int guest_gl_owned_enter(CPU *__restrict c);
void guest_gl_owned_leave(CPU *__restrict c, uint32_t token);
/* ---- ISAAC_VITA_SHADER_ATTRIB_FASTPATH end ---------------------------- */
#endif

#if defined(ISAAC_VITA_GL_SHIM_TABLE_TOKENS)
/* ISAAC_VITA_GL_SHIM_TABLE_TOKENS (wf/opt-gltok): the frozen registry as
 * dispatch-table keys.  *tokens receives the exact 0x7e tokens and
 * *functions the matching guest_fn-shaped trampolines (generated, one per
 * entry); the count is GUEST_GL_SURFACE_COUNT.  A trampoline performs what
 * guest_call_slow's 0x7e block reaches for a registered token -- the
 * dynamic-call census increment and the ownership-guarded adapter -- minus
 * the classification the exact-key probe already made.  guest.c inserts the
 * pairs only while no IAT slot key and no image window can alias the family
 * (the same g_gl_dynamic_first fact the slow block tests), so a table hit
 * for a token is the slow path's outcome with fewer instructions.  Requires
 * ISAAC_VITA_GL_SHIM_FASTDISPATCH. */
uint32_t guest_gl_table_tokens(const uint32_t **tokens,
                               const guest_fn **functions);
#endif

/* Return the exact RVA which the guest APIENTRY call will resume at.  Generated
 * CALLs stack that canonical RVA directly; a relocated in-image VA is also
 * normalized for native/test callers.  Zero is returned outside a synchronous
 * backend callback or when the stacked word is in neither exact image domain.
 * This is a provenance seam, not a general stack-inspection API. */
uint32_t guest_gl_backend_return_rva(void);

/* A typed backend callback has no CPU parameter of its own.  During dispatch
 * this seam owns the active CPU long enough for a backend invariant failure
 * to become the same controlled guest fault as a missing callback.  It must
 * only be called synchronously from a callback entered by guest_gl_dispatch. */
GUEST_NORETURN void guest_gl_backend_fault(
    uint32_t address, const char *message);

/* Read-only metadata, sorted by name. */
const guest_gl_symbol *guest_gl_symbols(size_t *count);

#endif
