/* Guest CPU model for the recompiled Isaac.
 *
 * Design decisions, each one load-bearing. Two of them differ from the
 * original translation plan, and both differences make the job smaller.
 *
 * 1. GUEST DATA ADDRESS == HOST ADDRESS AT A CHOSEN BASE.
 *    The guest is x86-32 and every target we care about (MSVC x86 for the
 *    PC milestone, ARMv7 for the Vita) is 32-bit. The PE is rebased from its
 *    original 0x400000 ImageBase to a target-valid address and mapped there,
 *    so a relocated guest data pointer is directly dereferenceable and the
 *    guest heap can be the host heap.
 *
 *    This REPLACES the `IMG(addr)` macro used on ladder rungs 1 and 2. That
 *    macro only worked for displacements known at translation time; a pointer
 *    to a global that is loaded into a register and dereferenced later would
 *    have escaped it silently. Mapping the image at its chosen address removes
 *    the whole class of bug rather than handling one case of it.
 *
 * 2. INDIRECT DISPATCH KEEPS THE ORIGINAL ADDRESSES.
 *    The original plan was to rewrite address-takes into dense indices; it
 *    noted the real risk: one missed address-take puts a raw address in
 *    a register with nothing to map it to. Keeping the guest's own addresses
 *    as the values removes that risk entirely -- there is no rewriting pass
 *    to be incomplete, pointer comparisons and pointer arithmetic keep
 *    working, and vtables can be read by the guest as ordinary data.
 *    Relocation means those pointers are VAs, while the generated table stays
 *    target-independent in RVA space. The dispatch boundary normalises an
 *    in-image VA to its RVA before the lookup. The table holds ALL functions
 *    rather than the 8,451 whose address we could once prove was taken, which
 *    is 14k entries -- about 116 KB.
 *
 * 3. FULL CONTEXT, NOT LOCALS.
 *    Registers live in a context struct passed as `CPU *__restrict`. A
 *    recompiler cannot assume the guest honours any calling convention --
 *    MSVC emits custom ones and this binary is full of them -- so register
 *    state has to survive a call. `__restrict` is what lets the compiler keep
 *    hot fields in real registers despite the guest storing through pointers.
 *
 * 4. LAZY FLAGS.
 *    Only 8.12% of this binary's instructions read a flag, ZF is 66% of
 *    those, and OF alone is 195 instructions in 1.76M. So flags are never
 *    materialised eagerly: a producer records (op, a, b, result, width) and
 *    the consumer computes just the bit it needs. The translator additionally
 *    resolves producer/consumer pairs inside a basic block and emits a direct
 *    comparison, so the common `cmp`+`jcc` costs nothing at all.
 */
#ifndef ISAAC_GUEST_H
#define ISAAC_GUEST_H

#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#if defined(_MSC_VER)
#include <intrin.h>
#pragma intrinsic(_ReadWriteBarrier)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------ types */

/* GUEST_SSE_LOWER=1 (generated units only, GCC): the emitter's GUEST_XMM_*
 * tokens (recomp/sse_lower.py) become one NEON operation each and xmm_t gains
 * GCC vector views of the same 16 bytes.  With the default 0 the tokens
 * expand to exactly the lane loops and union copies generated code always
 * had, so the object code is unchanged; the union keeps its layout either way
 * (the vector members are declared aligned(4) so sizeof/alignof of xmm_t --
 * and of CPU -- do not move; the static assert below pins it).  Only bitwise,
 * integer, shuffle and int->float lane operations are ever lowered: NEON float
 * arithmetic flushes denormals and saturates float->int, so those stay
 * scalar C. */
#ifndef GUEST_SSE_LOWER
#define GUEST_SSE_LOWER 0
#endif
#if GUEST_SSE_LOWER != 0 && GUEST_SSE_LOWER != 1
#error GUEST_SSE_LOWER must be 0 or 1
#endif
#if GUEST_SSE_LOWER && !(defined(__GNUC__) || defined(__clang__))
#error GUEST_SSE_LOWER=1 requires the GCC vector extension
#endif
#if GUEST_SSE_LOWER
typedef uint8_t  guest_v_u8  __attribute__((vector_size(16), aligned(4)));
typedef uint16_t guest_v_u16 __attribute__((vector_size(16), aligned(4)));
typedef uint32_t guest_v_u32 __attribute__((vector_size(16), aligned(4)));
typedef uint64_t guest_v_u64 __attribute__((vector_size(16), aligned(4)));
typedef int16_t  guest_v_i16 __attribute__((vector_size(16), aligned(4)));
typedef int32_t  guest_v_i32 __attribute__((vector_size(16), aligned(4)));
typedef float    guest_v_f32 __attribute__((vector_size(16), aligned(4)));
#endif

typedef union {
    float    f[4];
    double   d[2];
    int8_t   i8[16];
    uint8_t  u8[16];
    int16_t  i16[8];
    uint16_t u16[8];
    int32_t  i32[4];
    uint32_t u32[4];
    int64_t  i64[2];
    uint64_t u64[2];
#if GUEST_SSE_LOWER
    guest_v_u8  v_u8;
    guest_v_u16 v_u16;
    guest_v_u32 v_u32;
    guest_v_u64 v_u64;
    guest_v_i16 v_i16;
    guest_v_i32 v_i32;
    guest_v_f32 v_f32;
#endif
} xmm_t;
#if GUEST_SSE_LOWER
_Static_assert(sizeof(xmm_t) == 16 && _Alignof(xmm_t) == 8 &&
               sizeof(guest_v_u32) == 16 && _Alignof(guest_v_u32) == 4,
               "GUEST_SSE_LOWER must not change the xmm_t layout");
#endif

/* A native setjmp has to live in the same generated C activation as the
 * guest `_setjmp3` call.  Keeping this record public lets the emitter declare
 * one automatic object at that exact call site; putting the native jmp_buf in
 * an ordinary import shim would leave a dead destination as soon as the shim
 * returned.  Runtime-owned links are per CPU and are valid only while their
 * generated owner remains active. */
typedef struct guest_jump_site {
    jmp_buf native_env;
    struct guest_jump_site *previous;
    uint32_t guest_env;
    uint32_t guest_esp;
    uint32_t guest_eip;
    uint32_t guest_registration;
} guest_jump_site;

/* Which operation last wrote the flags. The consumer needs this to know how
 * to derive CF and OF, which are the only two that are not a pure function
 * of the result. */
enum {
    FLAG_NONE = 0,
    FLAG_ADD, FLAG_SUB, FLAG_LOGIC, FLAG_INC, FLAG_DEC,
    FLAG_SHL, FLAG_SHR, FLAG_SAR, FLAG_NEG, FLAG_IMUL, FLAG_MUL,
    FLAG_BT,        /* CF only */
    FLAG_EXPLICIT,  /* the translator stored the bits directly */
    FLAG_PARTIAL    /* all five modelled flags are an explicit snapshot */
};

/* Production-generated code must never operate on an unowned synthetic
 * stack.  Focused host shims which deliberately place ESP in a native test
 * array may leave this at zero; the Vita target and the stack oracle define
 * it to one and therefore fail closed before the first access. */
#ifndef GUEST_STACK_REQUIRED
#define GUEST_STACK_REQUIRED 0
#endif

enum {
    GUEST_STACK_FAULT_NONE = 0,
    GUEST_STACK_FAULT_UNBOUND,
    GUEST_STACK_FAULT_OWNER,
    GUEST_STACK_FAULT_SET,
    GUEST_STACK_FAULT_ADJUST,
    GUEST_STACK_FAULT_ACCESS,
    GUEST_STACK_FAULT_PUSH,
    GUEST_STACK_FAULT_POP
};

/* The lazy flag state as its own object.  Generated code addresses it through
 * GUEST_FL: by default that is the CPU's own copy (`&c->fl`); a unit compiled
 * with GUEST_FLAGS_LOCAL=1 keeps a per-function automatic copy instead, so the
 * producer stores and the cc_/fl_ helper switches fold in the compiler's SSA and
 * nothing is written to memory.  Flags are dead across CALL/RET by design;
 * they cross a function boundary only through an unresolved indirect JMP,
 * where the emitter flushes them, and into a body whose entry block consumes
 * them, where it loads them. */
typedef struct guest_flags {
    uint32_t f_a, f_b, f_r;
    uint8_t  f_op;
    uint8_t  f_sz;          /* operand width in bytes: 1, 2 or 4 */
    uint8_t  f_cf;          /* used when f_op == FLAG_EXPLICIT */
    uint8_t  f_of;
    uint8_t  f_zf, f_sf, f_pf; /* preserved across RCL/RCR */
} guest_flags;

typedef struct CPU {
    /* GP registers, in Intel encoding order so a decoded register number
     * indexes this array directly. */
    union {
        uint32_t r[8];
        struct { uint32_t eax, ecx, edx, ebx, esp, ebp, esi, edi; };
    };

    xmm_t x[8];             /* xmm0..7 */

    /* x87: 3,831 instructions in the whole binary, so this is modelled for
     * correctness rather than speed. MSVC returns float and double in ST(0),
     * which is the only reason most of them exist. */
    double  st[8];
    int     st_top;

    /* x87 status word, only the condition codes. MSVC compares floats with
     * `fcom` / `fnstsw ax` / `test ah, 0x41` far more often than with the
     * `fcomi` form, so the C0/C2/C3 bits have to be real values somewhere.
     * Bit positions are the architectural ones: C0=8, C1=9, C2=10, C3=14,
     * which is what makes `fnstsw ax` a plain move. */
    uint16_t fsw;

    /* lazy flag state: `c->fl` for generated code, the flat names for every
     * existing runtime reader.  Both views are the same storage. */
    union {
        guest_flags fl;
        struct {
            uint32_t f_a, f_b, f_r;
            uint8_t  f_op;
            uint8_t  f_sz;
            uint8_t  f_cf;
            uint8_t  f_of;
            uint8_t  f_zf, f_sf, f_pf;
        };
    };
    uint8_t  df;            /* direction flag for string ops */

    /* Set by a translated function that reached something we cannot execute.
     * Never silently ignored: the guarded driver checks it. */
    const char *fault;
    uint32_t    fault_addr;

    /* A guest process exit is a normal noreturn control transfer, not a fault.
     * Keep it distinct so a failed assertion and `exit(1)` can never look like
     * the same bring-up result. `exit_api` identifies the exact CRT entry point
     * (`exit`, `_exit`, ...), while stop_kind is one of GUEST_RUN_* below. */
    const char *exit_api;
    int32_t     exit_code;
    int32_t     stop_kind;

    /* Host-private active stop boundary. Kept on the CPU rather than in a
     * process-global jmp_buf so one CPU can never longjmp into another CPU's
     * native stack. Other runtime globals are still single-threaded. */
    void       *run_scope;

    /* Active guest `_setjmp3` sites.  The records themselves belong to live
     * generated C frames; this per-CPU chain only supplies ownership and the
     * exact native destination for a later guest `longjmp`. */
    guest_jump_site *jump_sites;
    uint32_t         jump_value;

    /* Synthetic-stack ownership is deliberately per CPU.  stack_owner is a
     * host identity token, never a guest pointer: copying a CPU leaves the
     * token pointing at the original and makes the copy fail closed.  Bounds
     * are guest addresses, floor inclusive and ceiling the empty-stack ESP.
     * stack_low_water records the lowest checked pointer/access for a cheap
     * production high-water diagnostic. */
    struct CPU *stack_owner;
    uint32_t    stack_floor;
    uint32_t    stack_ceiling;
    uint32_t    stack_low_water;
    uint32_t    stack_fault_address;
    uint32_t    stack_fault_size;
    uint32_t    stack_fault_pc;
    uintptr_t   stack_fault_native_site;
    uint32_t    stack_fault_kind;

    /* Win32 GetLastError belongs to a guest thread, not to the native host
     * thread that happens to execute it.  CPU is that guest-thread context;
     * zero-initialization supplies the required initial ERROR_SUCCESS. */
    uint32_t         last_error;

    /* The production Vita runner executes this CPU synchronously on one
     * native thread.  Its sync edge binds that positive UID once; zero keeps
     * the exact per-operation identity lookup used by portable/test CPUs. */
    uint32_t         vita_sync_thread_id;
} CPU;

typedef void (*guest_fn)(CPU *__restrict);

/* Declared before the inline stack boundary because every rejected operation
 * must enter the normal guarded guest-fault path before touching memory. */
void guest_fault(CPU *__restrict c, uint32_t addr, const char *what);

/* A rejected stack operation is extraordinarily cold, but every generated
 * push/pop/access has to carry the check.  Keep only the owner/range compare
 * inline and share diagnostics/fault dispatch in one out-of-line block. */
#if defined(__GNUC__) || defined(__clang__)
#define GUEST_STACK_UNLIKELY(value) __builtin_expect(!!(value), 0)
#define GUEST_STACK_COLD_NOINLINE __attribute__((cold, noinline))
#define GUEST_STACK_HOT_NOINLINE __attribute__((hot, noinline))
#elif defined(_MSC_VER)
#define GUEST_STACK_UNLIKELY(value) (!!(value))
#define GUEST_STACK_COLD_NOINLINE __declspec(noinline)
#define GUEST_STACK_HOT_NOINLINE __declspec(noinline)
#else
#define GUEST_STACK_UNLIKELY(value) (!!(value))
#define GUEST_STACK_COLD_NOINLINE
#define GUEST_STACK_HOT_NOINLINE
#endif

GUEST_STACK_COLD_NOINLINE int guest_stack_violation(
    CPU *__restrict c, uint32_t pc, uint32_t kind, uint32_t address,
    uint32_t size);
GUEST_STACK_COLD_NOINLINE int guest_stack_owner_violation(
    CPU *__restrict c, uint32_t pc);

/* One exact import-directory entry.  The generated table is sorted by the
 * canonical RVA of the IAT slot, not by whatever pointer value a platform
 * loader would normally put there. */
typedef struct guest_import {
    uint32_t    slot_rva;
    const char *name;              /* exact "DLL!symbol", or "DLL!#ordinal" */
} guest_import;

/* ------------------------------------------------------------- memory ---- */
/* Guest address == host address, so these are plain loads and stores. They
 * exist as functions so that a debug build can range-check every access
 * without the translator emitting anything different. */

#ifdef GUEST_CHECKED_MEMORY
void guest_check(uint32_t addr, uint32_t size, int write);
#define G_CHK(a, n, w) guest_check((a), (n), (w))
#else
#define G_CHK(a, n, w) ((void)0)
#endif

/* Atomic 64-bit compare/exchange used by LOCK CMPXCHG8B.  Returns the value
 * observed at `addr`; equality with `expected` therefore reports success.
 * The measured mimalloc operands are all naturally eight-byte aligned. */
uint64_t guest_atomic_cmpxchg64(uint32_t addr, uint64_t expected,
                                uint64_t desired);

static inline uint8_t  ld8 (uint32_t a) { G_CHK(a,1,0); return *(uint8_t  *)(uintptr_t)a; }
static inline uint16_t ld16(uint32_t a) { G_CHK(a,2,0); return *(uint16_t *)(uintptr_t)a; }
static inline uint32_t ld32(uint32_t a) { G_CHK(a,4,0); return *(uint32_t *)(uintptr_t)a; }
static inline uint64_t ld64(uint32_t a) { G_CHK(a,8,0); uint64_t v; memcpy(&v,(void*)(uintptr_t)a,8); return v; }
static inline xmm_t    ldx (uint32_t a) { G_CHK(a,16,0); xmm_t v; memcpy(&v,(void*)(uintptr_t)a,16); return v; }

static inline void st8 (uint32_t a, uint8_t  v) { G_CHK(a,1,1); *(uint8_t  *)(uintptr_t)a = v; }
static inline void st16(uint32_t a, uint16_t v) { G_CHK(a,2,1); *(uint16_t *)(uintptr_t)a = v; }
static inline void st32(uint32_t a, uint32_t v) { G_CHK(a,4,1); *(uint32_t *)(uintptr_t)a = v; }
static inline void st64(uint32_t a, uint64_t v) { G_CHK(a,8,1); memcpy((void*)(uintptr_t)a,&v,8); }
static inline void stx (uint32_t a, xmm_t    v) { G_CHK(a,16,1); memcpy((void*)(uintptr_t)a,&v,16); }

/* Scalar double guest accesses (movsd/addsd/cvtsd2ss memory operands, fld/fstp
 * qword): the memcpy pun.  Measured on the box (arm-vita-eabi-gcc 10.3,
 * cortex-a9 softfp): GCC only spells a double load as vldr.64 when the
 * pointee is 8-aligned; a 4-aligned may_alias double becomes ldr+ldr+vmov
 * d,r,r -- the same code the pun produces -- so a VFP spelling behind a
 * word-alignment test would add the test and gain nothing, and claiming
 * 8-alignment would lie to the compiler about the x86 stack (doubles there
 * are 4-aligned).  Doubles are also rare (3.9k ldd / 1.1k std_ sites vs
 * 115k ldf / 54k stf), so they stay as they are under both knob values. */
static inline double   ldd (uint32_t a) { G_CHK(a,8,0); double v; memcpy(&v,(void*)(uintptr_t)a,8); return v; }
static inline void std_(uint32_t a, double   v) { G_CHK(a,8,1); memcpy((void*)(uintptr_t)a,&v,8); }

/* Scalar float guest accesses (movss, the memory operand of addss/mulss/
 * subss/divss/sqrtss/comiss/cvtss2sd/cvttss2si, fld/fstp dword).
 *
 * GUEST_F32_VFP_ACCESS=0 (default): the memcpy pun generated code always had.
 * GCC lowers the 4-byte memcpy to an integer ldr/str, so every float that
 * continues in VFP arithmetic costs a core->VFP transfer (vmov s, r) and
 * every scalar store a VFP->core transfer (vmov r, s) -- the transfer stall
 * the GUEST_XMM_LAUNDER comment describes; sub_0055f990 alone carried 49 + 27
 * of them.
 *
 * GUEST_F32_VFP_ACCESS=1 (generated units only, GCC/ARM; CMake
 * ISAAC_VITA_TRANSLATED_CPU_F32_VFP): a word-aligned address is dereferenced
 * as a 4-aligned may_alias float, which GCC spells as one vldr/vstr straight
 * from/to the VFP register file (hot path per access: lsls, bne .cold,
 * vldr).  VLDR/VSTR fault on a non-word-aligned address regardless of
 * SCTLR.A, so the alignment test is mandatory; the misaligned remainder keeps
 * the memcpy pun in an out-of-line cold function (GCC gives it the local VFP
 * calling convention, so the caller's cold block is a bl plus a branch back).
 * Spelling the fallback inline instead is worse (GCC predicates both paths:
 * 18 instructions per load), hence the separate noinline callee.  The value
 * semantics are those of memcpy in both modes (raw bits move, no conversion,
 * NaN payloads intact); test_sse_lower_semantics.py runs every alignment
 * 0..15 through both knob values against native x86. */
#ifndef GUEST_F32_VFP_ACCESS
#define GUEST_F32_VFP_ACCESS 0
#endif
#if GUEST_F32_VFP_ACCESS != 0 && GUEST_F32_VFP_ACCESS != 1
#error GUEST_F32_VFP_ACCESS must be 0 or 1
#endif
#if GUEST_F32_VFP_ACCESS && !(defined(__GNUC__) || defined(__clang__))
#error GUEST_F32_VFP_ACCESS=1 requires GCC attributes (may_alias, cold)
#endif
#if GUEST_F32_VFP_ACCESS
typedef float guest_f32_word __attribute__((may_alias, aligned(4)));
#define GUEST_F32_MISALIGNED(a) __builtin_expect(((a) & 3U) != 0U, 0)
/* The helpers are cold (size-optimised, their calls predicted not taken) but
 * carry an explicit .text.<name> section.  Left to GCC they would go to
 * .text.unlikely.<name>, which the ISAAC_VITA_LAYOUT_HUB linker script claims
 * for the hub preamble ahead of the cold-half-B unit patterns; the post-link
 * contract (vita_text_layout.py check) then fails closed with "<unit>
 * .text.unlikely.guest_ldf_misaligned is outside cold half B" for every unit
 * of half B.  A .text.guest_* section stays with its unit under both half
 * patterns (.text.[!s]* and .text.*); the only caller is the unit itself, so
 * the bl is always in reach.  Section names aside, the object code is the
 * same as with the implicit cold section (cmp-verified on guest_0000/0166). */
static __attribute__((noinline, cold, unused, section(".text.guest_ldf_misaligned")))
float guest_ldf_misaligned(uint32_t a)
{ float v; memcpy(&v, (void *)(uintptr_t)a, 4); return v; }
static __attribute__((noinline, cold, unused, section(".text.guest_stf_misaligned")))
void guest_stf_misaligned(uint32_t a, float v)
{ memcpy((void *)(uintptr_t)a, &v, 4); }
static inline float ldf(uint32_t a)
{
    G_CHK(a,4,0);
    if (GUEST_F32_MISALIGNED(a)) return guest_ldf_misaligned(a);
    return *(const guest_f32_word *)(uintptr_t)a;
}
static inline void stf(uint32_t a, float v)
{
    G_CHK(a,4,1);
    if (GUEST_F32_MISALIGNED(a)) { guest_stf_misaligned(a, v); return; }
    *(guest_f32_word *)(uintptr_t)a = v;
}
#else
static inline float    ldf (uint32_t a) { G_CHK(a,4,0); float  v; memcpy(&v,(void*)(uintptr_t)a,4); return v; }
static inline void stf (uint32_t a, float    v) { G_CHK(a,4,1); memcpy((void*)(uintptr_t)a,&v,4); }
#endif

/* ------------------------------------------------ SSE lowering tokens ---- */
/* Emitted only by a generation with the GUEST_SSE_LOWER switch on
 * (recomp/sse_lower.py holds the spelling and the exact legacy text each token
 * stands for).  Statement tokens are one statement `TOKEN(...);` each;
 * GUEST_XMM_FCC is the condition of a paired comis consumer.
 *
 * Header knob GUEST_SSE_LOWER=0: the legacy lane loop / union copy / lazy
 * flag producer, byte for byte the statements the emitter produced before the
 * tokens.  GUEST_SSE_LOWER=1: the same lanes through the vector views.  Guest
 * memory is reached through a 1-byte-aligned may_alias vector type (movups has
 * no alignment guarantee; GCC emits vld1/vst1 without an alignment qualifier,
 * which the SCTLR.A=0 configuration the existing unaligned ld32/st32 already
 * rely on permits at any byte address).  Accepted deviation from the
 * research gate "no :64/:128 qualifiers": where GCC proves the address 8- or
 * 16-aligned from its value bits (a constant guest address, an `and esp,-8`
 * or `and esp,-16` mask) it emits vld1.64 {..}[:64|:128] or VLDR for the
 * same access; that inference is exact in uint32 arithmetic, the unknown-
 * alignment access stays an unqualified vld1.32 (2-aligned constants too).
 * Lane arithmetic is on the unsigned
 * views (wrap-around is defined); cvtdq2ps is __builtin_convertvector, whose
 * int->float round-to-nearest-even equals the scalar (float) cast lane for
 * lane (NEON vcvt.f32.s32 rounds to nearest; int->float can produce neither a
 * denormal nor an overflow).
 *
 * Every vector write is followed by GUEST_XMM_LAUNDER(d), an empty asm with
 * the destination as an in/out memory operand.  Without it GCC forwards lane 0
 * of the q register into the next scalar consumer through a core register
 * (vmov.32 r, d[0]; vmov s, r), a NEON->ARM transfer that stalls the
 * Cortex-A9; with it the register file stays in c->x[] and the scalar
 * consumer reloads with one vldr, exactly like the code around it.
 *
 * Lane-0 forms (*0 tokens) are used where the emitter's upper-lane taint
 * analysis proved lanes 1..3 of the result unobserved: knob 1 writes lane 0
 * only (no NEON at all); knob 0 the full legacy statement.
 *
 * GUEST_XMM_COMIS_PAIRED / GUEST_XMM_FCC: knob 0 keeps the FLAG_PARTIAL
 * producer and the cc_* consumer; knob 1 drops the producer and evaluates one
 * C99 relation with x86 unordered semantics (comis: unordered => ZF=PF=CF=1):
 *   a  isgreater  ae isgreaterequal  b !isgreaterequal  be !isgreater
 *   e  !islessgreater  ne islessgreater  p isunordered  np !isunordered
 * The GCC builtins evaluate each operand exactly once (a memory operand is
 * loaded once, as in the lazy producer) and never raise on quiet NaNs. */
#if GUEST_SSE_LOWER
typedef guest_v_u32 guest_v_u32_unaligned __attribute__((aligned(1), may_alias));
static inline __attribute__((always_inline)) guest_v_u32 guest_ldxv(uint32_t a)
{
    G_CHK(a, 16, 0);
    return *(const guest_v_u32_unaligned *)(uintptr_t)a;
}
static inline __attribute__((always_inline)) void guest_stxv(uint32_t a, guest_v_u32 v)
{
    G_CHK(a, 16, 1);
    *(guest_v_u32_unaligned *)(uintptr_t)a = v;
}
#define GUEST_XMM_LAUNDER(d) __asm__("" : "+m"(c->x[d].v_u32))
#define GUEST_XMM_MOV(d, s) \
    do { c->x[d].v_u32 = c->x[s].v_u32; GUEST_XMM_LAUNDER(d); } while (0)
#define GUEST_XMM_MOV0(d, s) \
    do { c->x[d].u32[0] = c->x[s].u32[0]; } while (0)
#define GUEST_XMM_LDX(d, a) \
    do { c->x[d].v_u32 = guest_ldxv(a); GUEST_XMM_LAUNDER(d); } while (0)
#define GUEST_XMM_STX(a, s) \
    do { guest_stxv((a), c->x[s].v_u32); } while (0)
#define GUEST_XMM_LANEOP_R(fld, n, op, d, s) \
    do { c->x[d].v_##fld op##= c->x[s].v_##fld; GUEST_XMM_LAUNDER(d); } while (0)
#define GUEST_XMM_LANEOP_M(fld, n, op, d, a) \
    do { c->x[d].v_##fld op##= (guest_v_##fld)guest_ldxv(a); \
         GUEST_XMM_LAUNDER(d); } while (0)
#define GUEST_XMM_LANEOP0_R(fld, n, op, d, s) \
    do { c->x[d].fld[0] op##= c->x[s].fld[0]; } while (0)
#define GUEST_XMM_LANEOP0_M(fld, n, op, d, a) \
    do { c->x[d].fld[0] op##= ld32(a); } while (0)
#define GUEST_XMM_PANDN_R(d, s) \
    do { c->x[d].v_u32 = ~c->x[d].v_u32 & c->x[s].v_u32; GUEST_XMM_LAUNDER(d); } while (0)
#define GUEST_XMM_PANDN_M(d, a) \
    do { c->x[d].v_u32 = ~c->x[d].v_u32 & guest_ldxv(a); GUEST_XMM_LAUNDER(d); } while (0)
#define GUEST_XMM_CVTDQ2PS_R(d, s) \
    do { c->x[d].v_f32 = __builtin_convertvector(c->x[s].v_i32, guest_v_f32); \
         GUEST_XMM_LAUNDER(d); } while (0)
#define GUEST_XMM_CVTDQ2PS_M(d, a) \
    do { c->x[d].v_f32 = __builtin_convertvector((guest_v_i32)guest_ldxv(a), \
                                                 guest_v_f32); \
         GUEST_XMM_LAUNDER(d); } while (0)
#define GUEST_XMM_CVTDQ2PS0_R(d, s) \
    do { c->x[d].f[0] = (float)c->x[s].i32[0]; } while (0)
#define GUEST_XMM_CVTDQ2PS0_M(d, a) \
    do { c->x[d].f[0] = (float)(int32_t)ld32(a); } while (0)
/* Byte shift right by 1..15: bytes n..15 of the register followed by zeros
 * (a two-source shuffle with a zero vector; GCC emits one vext.8). */
#define GUEST_XMM_PSRLDQ(d, n) \
    do { c->x[d].v_u8 = __builtin_shuffle(c->x[d].v_u8, (guest_v_u8){0}, \
             (guest_v_u8){ (n) + 0, (n) + 1, (n) + 2, (n) + 3, (n) + 4, (n) + 5, \
                           (n) + 6, (n) + 7, (n) + 8, (n) + 9, (n) + 10, (n) + 11, \
                           (n) + 12, (n) + 13, (n) + 14, (n) + 15 }); \
         GUEST_XMM_LAUNDER(d); } while (0)
/* Immediate lane shifts with 1 <= n < bits (the emitter keeps every other
 * count on the legacy path). */
#define GUEST_XMM_PSRA_IMM(bits, d, n) \
    do { c->x[d].v_i##bits >>= (n); GUEST_XMM_LAUNDER(d); } while (0)
#define GUEST_XMM_PSHL_IMM(bits, op, d, n) \
    do { c->x[d].v_u##bits op##= (n); GUEST_XMM_LAUNDER(d); } while (0)
/* Register counts: the low qword of the source is the count and a count >=
 * the lane width saturates (sign fill for the arithmetic shift, zero for the
 * logical ones); the clamp keeps the vector shift defined.  GCC broadcasts
 * the scalar count (vdup + vshl/vneg). */
#define GUEST_XMM_PSRA_REG(bits, d, s) \
    do { uint64_t _n = c->x[s].u64[0]; \
         if (_n > (bits) - 1) _n = (bits) - 1; \
         c->x[d].v_i##bits >>= (int)_n; GUEST_XMM_LAUNDER(d); } while (0)
#define GUEST_XMM_PSHL_REG(bits, op, d, s) \
    do { uint64_t _n = c->x[s].u64[0]; \
         if (_n >= (bits)) c->x[d].v_u##bits = (guest_v_u##bits){0}; \
         else c->x[d].v_u##bits op##= (int)_n; \
         GUEST_XMM_LAUNDER(d); } while (0)
/* Constant-immediate shuffles: the lane selectors are compile-time constants,
 * so GCC picks vrev/vext/vzip/vdup or a two-instruction vtbl. */
#define GUEST_XMM_PSHUFD_MASK(imm) \
    (guest_v_u32){ (imm) & 3, ((imm) >> 2) & 3, ((imm) >> 4) & 3, ((imm) >> 6) & 3 }
#define GUEST_XMM_PSHUFD_R(d, s, imm) \
    do { c->x[d].v_u32 = __builtin_shuffle(c->x[s].v_u32, GUEST_XMM_PSHUFD_MASK(imm)); \
         GUEST_XMM_LAUNDER(d); } while (0)
#define GUEST_XMM_PSHUFD_M(d, a, imm) \
    do { c->x[d].v_u32 = __builtin_shuffle(guest_ldxv(a), GUEST_XMM_PSHUFD_MASK(imm)); \
         GUEST_XMM_LAUNDER(d); } while (0)
#define GUEST_XMM_SHUFPS_MASK(imm) \
    (guest_v_u32){ (imm) & 3, ((imm) >> 2) & 3, \
                   4 + (((imm) >> 4) & 3), 4 + (((imm) >> 6) & 3) }
#define GUEST_XMM_SHUFPS_R(d, s, imm) \
    do { c->x[d].v_u32 = __builtin_shuffle(c->x[d].v_u32, c->x[s].v_u32, \
                                           GUEST_XMM_SHUFPS_MASK(imm)); \
         GUEST_XMM_LAUNDER(d); } while (0)
#define GUEST_XMM_SHUFPS_M(d, a, imm) \
    do { c->x[d].v_u32 = __builtin_shuffle(c->x[d].v_u32, guest_ldxv(a), \
                                           GUEST_XMM_SHUFPS_MASK(imm)); \
         GUEST_XMM_LAUNDER(d); } while (0)
/* unpck{l,h}{ps,pd} / punpck{l,h}{bw,wd,dq}: interleave the low (hi=0) or
 * high (hi=1) half of both operands at `unit` bytes per lane. */
#define GUEST_XMM_UNPCK_4_0(D, S) \
    __builtin_shuffle((D), (S), (guest_v_u32){ 0, 4, 1, 5 })
#define GUEST_XMM_UNPCK_4_1(D, S) \
    __builtin_shuffle((D), (S), (guest_v_u32){ 2, 6, 3, 7 })
#define GUEST_XMM_UNPCK_8_0(D, S) \
    (guest_v_u32)__builtin_shuffle((guest_v_u64)(D), (guest_v_u64)(S), \
                                   (guest_v_u64){ 0, 2 })
#define GUEST_XMM_UNPCK_8_1(D, S) \
    (guest_v_u32)__builtin_shuffle((guest_v_u64)(D), (guest_v_u64)(S), \
                                   (guest_v_u64){ 1, 3 })
#define GUEST_XMM_UNPCK_2_0(D, S) \
    (guest_v_u32)__builtin_shuffle((guest_v_u16)(D), (guest_v_u16)(S), \
                                   (guest_v_u16){ 0, 8, 1, 9, 2, 10, 3, 11 })
#define GUEST_XMM_UNPCK_2_1(D, S) \
    (guest_v_u32)__builtin_shuffle((guest_v_u16)(D), (guest_v_u16)(S), \
                                   (guest_v_u16){ 4, 12, 5, 13, 6, 14, 7, 15 })
#define GUEST_XMM_UNPCK_1_0(D, S) \
    (guest_v_u32)__builtin_shuffle((guest_v_u8)(D), (guest_v_u8)(S), \
                                   (guest_v_u8){ 0, 16, 1, 17, 2, 18, 3, 19, \
                                                 4, 20, 5, 21, 6, 22, 7, 23 })
#define GUEST_XMM_UNPCK_1_1(D, S) \
    (guest_v_u32)__builtin_shuffle((guest_v_u8)(D), (guest_v_u8)(S), \
                                   (guest_v_u8){ 8, 24, 9, 25, 10, 26, 11, 27, \
                                                 12, 28, 13, 29, 14, 30, 15, 31 })
#define GUEST_XMM_UNPCK_R(unit, hi, d, s) \
    do { c->x[d].v_u32 = GUEST_XMM_UNPCK_##unit##_##hi(c->x[d].v_u32, c->x[s].v_u32); \
         GUEST_XMM_LAUNDER(d); } while (0)
#define GUEST_XMM_UNPCK_M(unit, hi, d, a) \
    do { c->x[d].v_u32 = GUEST_XMM_UNPCK_##unit##_##hi(c->x[d].v_u32, guest_ldxv(a)); \
         GUEST_XMM_LAUNDER(d); } while (0)
/* pshuflw (hi=0) / pshufhw (hi=1): one 64-bit half permuted by the immediate,
 * the other half copied from the source. */
#define GUEST_XMM_PSHUFW_0(S, imm) \
    (guest_v_u32)__builtin_shuffle((guest_v_u16)(S), \
        (guest_v_u16){ (imm) & 3, ((imm) >> 2) & 3, ((imm) >> 4) & 3, \
                       ((imm) >> 6) & 3, 4, 5, 6, 7 })
#define GUEST_XMM_PSHUFW_1(S, imm) \
    (guest_v_u32)__builtin_shuffle((guest_v_u16)(S), \
        (guest_v_u16){ 0, 1, 2, 3, 4 + ((imm) & 3), 4 + (((imm) >> 2) & 3), \
                       4 + (((imm) >> 4) & 3), 4 + (((imm) >> 6) & 3) })
#define GUEST_XMM_PSHUFW_R(hi, d, s, imm) \
    do { c->x[d].v_u32 = GUEST_XMM_PSHUFW_##hi(c->x[s].v_u32, imm); \
         GUEST_XMM_LAUNDER(d); } while (0)
#define GUEST_XMM_PSHUFW_M(hi, d, a, imm) \
    do { c->x[d].v_u32 = GUEST_XMM_PSHUFW_##hi(guest_ldxv(a), imm); \
         GUEST_XMM_LAUNDER(d); } while (0)
#define GUEST_XMM_PUNPCKLDQ_R(d, s) GUEST_XMM_UNPCK_R(4, 0, d, s)
#define GUEST_XMM_PUNPCKLDQ_M(d, a) GUEST_XMM_UNPCK_M(4, 0, d, a)
#define GUEST_XMM_COMIS_PAIRED(fld, d, rhs) do { } while (0)
#define GUEST_XMM_FCC_a(x, y)  __builtin_isgreater((x), (y))
#define GUEST_XMM_FCC_ae(x, y) __builtin_isgreaterequal((x), (y))
#define GUEST_XMM_FCC_b(x, y)  (!__builtin_isgreaterequal((x), (y)))
#define GUEST_XMM_FCC_be(x, y) (!__builtin_isgreater((x), (y)))
#define GUEST_XMM_FCC_e(x, y)  (!__builtin_islessgreater((x), (y)))
#define GUEST_XMM_FCC_ne(x, y) __builtin_islessgreater((x), (y))
#define GUEST_XMM_FCC_p(x, y)  __builtin_isunordered((x), (y))
#define GUEST_XMM_FCC_np(x, y) (!__builtin_isunordered((x), (y)))
#define GUEST_XMM_FCC(cc, x, y) GUEST_XMM_FCC_##cc((x), (y))
#else
#define GUEST_XMM_MOV(d, s) \
    do { c->x[d] = c->x[s]; } while (0)
#define GUEST_XMM_MOV0(d, s) \
    do { c->x[d] = c->x[s]; } while (0)
#define GUEST_XMM_LDX(d, a) \
    do { c->x[d] = ldx(a); } while (0)
#define GUEST_XMM_STX(a, s) \
    do { stx((a), c->x[s]); } while (0)
#define GUEST_XMM_LANEOP_R(fld, n, op, d, s) \
    do { xmm_t _s = c->x[s]; int _i; \
         for (_i = 0; _i < (n); _i++) c->x[d].fld[_i] op##= _s.fld[_i]; } while (0)
#define GUEST_XMM_LANEOP_M(fld, n, op, d, a) \
    do { xmm_t _s = ldx(a); int _i; \
         for (_i = 0; _i < (n); _i++) c->x[d].fld[_i] op##= _s.fld[_i]; } while (0)
#define GUEST_XMM_LANEOP0_R(fld, n, op, d, s) GUEST_XMM_LANEOP_R(fld, n, op, d, s)
#define GUEST_XMM_LANEOP0_M(fld, n, op, d, a) GUEST_XMM_LANEOP_M(fld, n, op, d, a)
#define GUEST_XMM_PANDN_R(d, s) \
    do { xmm_t _d = c->x[d], _s = c->x[s]; int _i; \
         for (_i = 0; _i < 4; _i++) c->x[d].u32[_i] = (~_d.u32[_i]) & _s.u32[_i]; } while (0)
#define GUEST_XMM_PANDN_M(d, a) \
    do { xmm_t _d = c->x[d], _s = ldx(a); int _i; \
         for (_i = 0; _i < 4; _i++) c->x[d].u32[_i] = (~_d.u32[_i]) & _s.u32[_i]; } while (0)
#define GUEST_XMM_CVTDQ2PS_R(d, s) \
    do { xmm_t _s = c->x[s]; int _i; \
         for (_i = 0; _i < 4; _i++) c->x[d].f[_i] = (float)_s.i32[_i]; } while (0)
#define GUEST_XMM_CVTDQ2PS_M(d, a) \
    do { xmm_t _s = ldx(a); int _i; \
         for (_i = 0; _i < 4; _i++) c->x[d].f[_i] = (float)_s.i32[_i]; } while (0)
#define GUEST_XMM_CVTDQ2PS0_R(d, s) GUEST_XMM_CVTDQ2PS_R(d, s)
#define GUEST_XMM_CVTDQ2PS0_M(d, a) GUEST_XMM_CVTDQ2PS_M(d, a)
#define GUEST_XMM_PSRLDQ(d, n) \
    do { xmm_t _r = (xmm_t){0}; unsigned _n = (n), _i; \
         for (_i = 0; _i + _n < 16; _i++) _r.u8[_i] = c->x[d].u8[_i + _n]; \
         c->x[d] = _r; } while (0)
#define GUEST_XMM_PSRA_IMM(bits, d, n) \
    do { unsigned _n = (n); int _i; \
         if (_n > (bits)) _n = (bits); \
         for (_i = 0; _i < 128 / (bits); _i++) c->x[d].i##bits[_i] >>= _n; } while (0)
#define GUEST_XMM_PSHL_IMM(bits, op, d, n) \
    do { unsigned _n = (n); int _i; \
         if (_n >= (bits)) { c->x[d] = (xmm_t){0}; } else \
         for (_i = 0; _i < 128 / (bits); _i++) c->x[d].u##bits[_i] op##= _n; } while (0)
/* Register counts: low qword, saturating (the switch-off corpus reads the low
 * dword and clamps the arithmetic count to the lane width). */
#define GUEST_XMM_PSRA_REG(bits, d, s) \
    do { uint64_t _n = c->x[s].u64[0]; int _i; \
         if (_n > (bits) - 1) _n = (bits) - 1; \
         for (_i = 0; _i < 128 / (bits); _i++) c->x[d].i##bits[_i] >>= (unsigned)_n; } while (0)
#define GUEST_XMM_PSHL_REG(bits, op, d, s) \
    do { uint64_t _n = c->x[s].u64[0]; int _i; \
         if (_n >= (bits)) { c->x[d] = (xmm_t){0}; } else \
         for (_i = 0; _i < 128 / (bits); _i++) c->x[d].u##bits[_i] op##= (unsigned)_n; } while (0)
#define GUEST_XMM_PSHUFD_X(d, src, imm) \
    do { xmm_t _s = src, _r; unsigned _c = (imm); int _i; \
         for (_i = 0; _i < 4; _i++) _r.u32[_i] = _s.u32[(_c >> (_i*2)) & 3]; \
         c->x[d] = _r; } while (0)
#define GUEST_XMM_PSHUFD_R(d, s, imm) GUEST_XMM_PSHUFD_X(d, c->x[s], imm)
#define GUEST_XMM_PSHUFD_M(d, a, imm) GUEST_XMM_PSHUFD_X(d, ldx(a), imm)
#define GUEST_XMM_SHUFPS_X(d, src, imm) \
    do { xmm_t _d = c->x[d], _s = src, _r; unsigned _c = (imm); \
         _r.u32[0] = _d.u32[_c & 3]; _r.u32[1] = _d.u32[(_c>>2)&3]; \
         _r.u32[2] = _s.u32[(_c>>4)&3]; _r.u32[3] = _s.u32[(_c>>6)&3]; \
         c->x[d] = _r; } while (0)
#define GUEST_XMM_SHUFPS_R(d, s, imm) GUEST_XMM_SHUFPS_X(d, c->x[s], imm)
#define GUEST_XMM_SHUFPS_M(d, a, imm) GUEST_XMM_SHUFPS_X(d, ldx(a), imm)
#define GUEST_XMM_UNPCK_LANE_1(_r, _d, _s, _i, _k) \
    _r.u8[_i*2] = _d.u8[_k]; _r.u8[_i*2+1] = _s.u8[_k]
#define GUEST_XMM_UNPCK_LANE_2(_r, _d, _s, _i, _k) \
    _r.u16[_i*2] = _d.u16[_k]; _r.u16[_i*2+1] = _s.u16[_k]
#define GUEST_XMM_UNPCK_LANE_4(_r, _d, _s, _i, _k) \
    _r.u32[_i*2] = _d.u32[_k]; _r.u32[_i*2+1] = _s.u32[_k]
#define GUEST_XMM_UNPCK_LANE_8(_r, _d, _s, _i, _k) \
    _r.u64[_i*2] = _d.u64[_k]; _r.u64[_i*2+1] = _s.u64[_k]
#define GUEST_XMM_UNPCK_X(unit, hi, d, src) \
    do { xmm_t _d = c->x[d], _s = src, _r; int _i; \
         for (_i = 0; _i < (16 / (unit)) / 2; _i++) { \
             int _k = _i + ((hi) ? (16 / (unit)) / 2 : 0); \
             GUEST_XMM_UNPCK_LANE_##unit(_r, _d, _s, _i, _k); \
         } c->x[d] = _r; } while (0)
#define GUEST_XMM_UNPCK_R(unit, hi, d, s) GUEST_XMM_UNPCK_X(unit, hi, d, c->x[s])
#define GUEST_XMM_UNPCK_M(unit, hi, d, a) GUEST_XMM_UNPCK_X(unit, hi, d, ldx(a))
#define GUEST_XMM_PSHUFW_X(hi, d, src, imm) \
    do { xmm_t _s = src, _r = _s; unsigned _c = (imm); int _i; \
         for (_i = 0; _i < 4; _i++) \
             _r.u16[((hi) ? 4 : 0) + _i] = _s.u16[((hi) ? 4 : 0) + ((_c >> (_i * 2)) & 3U)]; \
         c->x[d] = _r; } while (0)
#define GUEST_XMM_PSHUFW_R(hi, d, s, imm) GUEST_XMM_PSHUFW_X(hi, d, c->x[s], imm)
#define GUEST_XMM_PSHUFW_M(hi, d, a, imm) GUEST_XMM_PSHUFW_X(hi, d, ldx(a), imm)
#define GUEST_XMM_PUNPCKLDQ_X(d, src) \
    do { xmm_t _d = c->x[d], _s = src, _r; \
         _r.u32[0] = _d.u32[0]; _r.u32[1] = _s.u32[0]; \
         _r.u32[2] = _d.u32[1]; _r.u32[3] = _s.u32[1]; \
         c->x[d] = _r; } while (0)
#define GUEST_XMM_PUNPCKLDQ_R(d, s) GUEST_XMM_PUNPCKLDQ_X(d, c->x[s])
#define GUEST_XMM_PUNPCKLDQ_M(d, a) GUEST_XMM_PUNPCKLDQ_X(d, ldx(a))
#define GUEST_XMM_COMIS_PAIRED(fld, d, rhs) \
    do { double _x = (double)c->x[d].fld[0], _y = (double)(rhs); \
         GUEST_FL->f_op = FLAG_PARTIAL; GUEST_FL->f_sz = 4; \
         GUEST_FL->f_sf = 0; GUEST_FL->f_of = 0; \
         if (_x != _x || _y != _y) { \
             GUEST_FL->f_cf = 1; GUEST_FL->f_zf = 1; GUEST_FL->f_pf = 1; \
         } else { \
             GUEST_FL->f_cf = (uint8_t)(_x < _y); \
             GUEST_FL->f_zf = (uint8_t)(_x == _y); GUEST_FL->f_pf = 0; \
         } } while (0)
#define GUEST_XMM_FCC(cc, x, y) cc_##cc(GUEST_FL)
#endif

/* Store the current model's binary64 value as the x87 80-bit memory format.
 * MSVC maps `long double` to binary64, so a cast/memcpy through long double
 * would write eight bytes, not the explicit-integer-bit 10-byte x87 value.
 * Encode it ourselves; this also keeps PC MSVC and little-endian ARM outputs
 * byte-identical.  The model cannot manufacture precision beyond binary64,
 * but every represented finite value, zero, infinity and NaN payload is
 * widened exactly. */
static inline void st80d(uint32_t a, double v)
{
    uint64_t raw, fraction, significand;
    uint32_t exponent;
    uint16_t sign_exponent;
    uint8_t out[10];
    unsigned i;

    memcpy(&raw, &v, sizeof raw);
    fraction = raw & UINT64_C(0x000fffffffffffff);
    exponent = (uint32_t)((raw >> 52) & 0x7ffU);
    sign_exponent = (uint16_t)((raw >> 48) & 0x8000U);

    if (exponent == 0U) {
        if (fraction == 0U) {
            significand = 0U;                    /* signed zero */
        } else {
            int unbiased = -1022;
            while ((fraction & UINT64_C(0x0010000000000000)) == 0U) {
                fraction <<= 1;
                --unbiased;
            }
            significand = fraction << 11;
            sign_exponent |= (uint16_t)(unbiased + 16383);
        }
    } else if (exponent == 0x7ffU) {
        /* Infinity has only the explicit integer bit; NaNs retain the
         * binary64 quiet/signalling bit and payload in the high 52 bits. */
        significand = UINT64_C(0x8000000000000000) | (fraction << 11);
        sign_exponent |= 0x7fffU;
    } else {
        significand = (UINT64_C(0x0010000000000000) | fraction) << 11;
        sign_exponent |= (uint16_t)((int)exponent - 1023 + 16383);
    }

    for (i = 0; i < 8; ++i)
        out[i] = (uint8_t)(significand >> (i * 8));
    out[8] = (uint8_t)sign_exponent;
    out[9] = (uint8_t)(sign_exponent >> 8);
    G_CHK(a, 10, 1);
    memcpy((void *)(uintptr_t)a, out, sizeof out);
}

/* -------------------------------------------------------------- stack ---- */

static inline int guest_stack_is_bound(const CPU *c)
{
    return c && c->stack_owner == c &&
           c->stack_floor < c->stack_ceiling &&
           c->stack_low_water >= c->stack_floor &&
           c->stack_low_water <= c->stack_ceiling;
}

static inline int guest_stack_contains(const CPU *c, uint32_t address,
                                       uint32_t size)
{
    uint32_t capacity;
    if (!guest_stack_is_bound(c) || size == 0U)
        return 0;
    capacity = c->stack_ceiling - c->stack_floor;
    return size <= capacity && address >= c->stack_floor &&
           address <= c->stack_ceiling - size;
}

static inline uint32_t guest_stack_capacity(const CPU *c)
{
    return guest_stack_is_bound(c)
         ? c->stack_ceiling - c->stack_floor : 0U;
}

static inline uint32_t guest_stack_high_water(const CPU *c)
{
    return guest_stack_is_bound(c)
         ? c->stack_ceiling - c->stack_low_water : 0U;
}

/* The production fast path deliberately validates only immutable ownership
 * and the non-wrapping floor/ceiling pair here.  stack_low_water starts at the
 * ceiling and is only moved to a range-checked address, so rechecking that
 * diagnostic field at half a million call sites would buy no memory safety. */
static inline int guest_stack_fast_bound(const CPU *c)
{
    return c && c->stack_owner == c &&
           c->stack_floor < c->stack_ceiling;
}

static inline void guest_stack_note_low(CPU *__restrict c, uint32_t address)
{
    if (GUEST_STACK_UNLIKELY(address < c->stack_low_water))
        c->stack_low_water = address;
}

#if !GUEST_STACK_REQUIRED
/* Return guarded=0 only for legacy focused host tests.  A partially-bound or
 * copied context is never treated as legacy: that is an ownership failure. */
static inline int guest_stack_legacy_guard(CPU *__restrict c, uint32_t pc,
                                           int *guarded)
{
    if (!c || !guarded)
        return 0;
    if (!c->stack_owner) {
        if (c->stack_floor || c->stack_ceiling || c->stack_low_water)
            return guest_stack_violation(
                c, pc, GUEST_STACK_FAULT_OWNER, c->esp, 0U);
        *guarded = 0;
        return 1;
    }
    if (!guest_stack_fast_bound(c))
        return guest_stack_owner_violation(c, pc);
    *guarded = 1;
    return 1;
}
#endif

#if GUEST_STACK_REQUIRED
/* The Vita corpus has roughly 645k physical stack operations.  Keeping a
 * complete owner/range/diagnostic branch at every site exceeded the retained
 * image budget, so production calls five shared hot validators.  Fault work
 * remains a separate cold block; no operation is left unchecked. */
GUEST_STACK_HOT_NOINLINE int guest_stack_set(
    CPU *__restrict c, uint32_t value, uint32_t pc);
GUEST_STACK_HOT_NOINLINE int guest_stack_adjust(
    CPU *__restrict c, uint32_t amount, uint32_t pc);
GUEST_STACK_HOT_NOINLINE uint32_t guest_stack_address(
    CPU *__restrict c, uint32_t address, uint32_t size, uint32_t pc);
GUEST_STACK_HOT_NOINLINE void gpush_at(
    CPU *__restrict c, uint32_t value, uint32_t pc);
GUEST_STACK_HOT_NOINLINE uint32_t gpop_at(
    CPU *__restrict c, uint32_t pc);

/* Generated instructions use the same checks without materialising a guest
 * PC literal at roughly 645k sites.  On rejection the noinline callee records
 * its native continuation; the linked map identifies its compiled owner, not
 * necessarily a unique guest instruction after optimizer tail sharing. */
GUEST_STACK_HOT_NOINLINE int guest_stack_set_generated(
    CPU *__restrict c, uint32_t value);
GUEST_STACK_HOT_NOINLINE int guest_stack_adjust_generated(
    CPU *__restrict c, uint32_t amount);
GUEST_STACK_HOT_NOINLINE uint32_t guest_stack_address_generated(
    CPU *__restrict c, uint32_t address, uint32_t size);
GUEST_STACK_HOT_NOINLINE void gpush_generated(
    CPU *__restrict c, uint32_t value);
GUEST_STACK_HOT_NOINLINE uint32_t gpop_generated(CPU *__restrict c);

/* A void push followed immediately by a generated return must still be a BL,
 * not a sibling-call B: LR is the production attribution key on failure. */
#if defined(__GNUC__) || defined(__clang__)
#define GUEST_STACK_CALLSITE_BARRIER() \
    __asm__ __volatile__("" ::: "memory")
#elif defined(_MSC_VER)
#define GUEST_STACK_CALLSITE_BARRIER() _ReadWriteBarrier()
#else
#define GUEST_STACK_CALLSITE_BARRIER() ((void)0)
#endif

static inline void gpush(CPU *__restrict c, uint32_t value)
{
    gpush_at(c, value, 0U);
}

static inline uint32_t gpop(CPU *__restrict c)
{
    return gpop_at(c, 0U);
}
#else
static inline int guest_stack_set(CPU *__restrict c, uint32_t value,
                                  uint32_t pc)
{
    uint32_t capacity;
#if GUEST_STACK_REQUIRED
    if (GUEST_STACK_UNLIKELY(!guest_stack_fast_bound(c)))
        return guest_stack_owner_violation(c, pc);
#else
    int guarded;
    if (!guest_stack_legacy_guard(c, pc, &guarded))
        return 0;
    if (!guarded) {
        c->esp = value;
        return 1;
    }
#endif
    capacity = c->stack_ceiling - c->stack_floor;
    if (GUEST_STACK_UNLIKELY(value - c->stack_floor > capacity))
        return guest_stack_violation(
            c, pc, GUEST_STACK_FAULT_SET, value, 0U);
    c->esp = value;
    guest_stack_note_low(c, value);
    return 1;
}

/* Positive x86 caller/callee cleanup.  Offset arithmetic checks the current
 * ESP before subtracting from capacity, so neither operand can wrap into an
 * apparently valid result. */
static inline int guest_stack_adjust(CPU *__restrict c, uint32_t amount,
                                     uint32_t pc)
{
    uint32_t capacity, offset;
#if GUEST_STACK_REQUIRED
    if (GUEST_STACK_UNLIKELY(!guest_stack_fast_bound(c)))
        return guest_stack_owner_violation(c, pc);
#else
    int guarded;
    if (!guest_stack_legacy_guard(c, pc, &guarded))
        return 0;
    if (!guarded) {
        c->esp += amount;
        return 1;
    }
#endif
    capacity = c->stack_ceiling - c->stack_floor;
    offset = c->esp - c->stack_floor;
    if (GUEST_STACK_UNLIKELY(
            offset > capacity || amount > capacity - offset))
        return guest_stack_violation(
            c, pc, GUEST_STACK_FAULT_ADJUST, c->esp, amount);
    c->esp += amount;
    return 1;
}

/* Use only for an address derived syntactically from guest ESP.  Generic
 * guest loads/stores intentionally remain unchanged: image/heap addresses and
 * host-native oracle pointers do not belong to this stack contract. */
static inline uint32_t guest_stack_address(CPU *__restrict c,
                                           uint32_t address, uint32_t size,
                                           uint32_t pc)
{
    uint32_t capacity, offset;
#if GUEST_STACK_REQUIRED
    if (GUEST_STACK_UNLIKELY(!guest_stack_fast_bound(c))) {
        (void)guest_stack_owner_violation(c, pc);
        return 0U;
    }
#else
    int guarded;
    if (!guest_stack_legacy_guard(c, pc, &guarded))
        return 0U;
    if (!guarded)
        return address;
#endif
    capacity = c->stack_ceiling - c->stack_floor;
    offset = address - c->stack_floor;
    if (GUEST_STACK_UNLIKELY(
            size == 0U || size > capacity || offset > capacity - size)) {
        (void)guest_stack_violation(
            c, pc, GUEST_STACK_FAULT_ACCESS, address, size);
        return 0U;
    }
    guest_stack_note_low(c, address);
    return address;
}

static inline void gpush_at(CPU *__restrict c, uint32_t value, uint32_t pc)
{
    uint32_t capacity, offset, next;
#if GUEST_STACK_REQUIRED
    if (GUEST_STACK_UNLIKELY(!guest_stack_fast_bound(c))) {
        (void)guest_stack_owner_violation(c, pc);
        return;
    }
#else
    int guarded;
    if (!guest_stack_legacy_guard(c, pc, &guarded))
        return;
    if (!guarded) {
        c->esp -= 4U;
        st32(c->esp, value);
        return;
    }
#endif
    capacity = c->stack_ceiling - c->stack_floor;
    offset = c->esp - c->stack_floor;
    if (GUEST_STACK_UNLIKELY(offset < 4U || offset > capacity)) {
        (void)guest_stack_violation(
            c, pc, GUEST_STACK_FAULT_PUSH, c->esp - 4U, 4U);
        return;
    }
    next = c->esp - 4U;
    c->esp = next;
    guest_stack_note_low(c, next);
    st32(next, value);
}

static inline uint32_t gpop_at(CPU *__restrict c, uint32_t pc)
{
    uint32_t capacity, offset, value;
#if GUEST_STACK_REQUIRED
    if (GUEST_STACK_UNLIKELY(!guest_stack_fast_bound(c))) {
        (void)guest_stack_owner_violation(c, pc);
        return 0U;
    }
#else
    int guarded;
    if (!guest_stack_legacy_guard(c, pc, &guarded))
        return 0U;
    if (!guarded) {
        value = ld32(c->esp);
        c->esp += 4U;
        return value;
    }
#endif
    capacity = c->stack_ceiling - c->stack_floor;
    offset = c->esp - c->stack_floor;
    if (GUEST_STACK_UNLIKELY(capacity < 4U || offset > capacity - 4U)) {
        (void)guest_stack_violation(
            c, pc, GUEST_STACK_FAULT_POP, c->esp, 4U);
        return 0U;
    }
    value = ld32(c->esp);
    c->esp += 4U; /* the checked four-byte range proves this cannot wrap */
    return value;
}

/* Runtime shims predating instruction attribution still get the same checked
 * contract, but pc=0 records no guest PC or native site.  Explicit `_at`
 * callers can supply a guest PC; required generated code uses out-of-line
 * `_generated` forms and records the compiled native continuation above. */
static inline void gpush(CPU *__restrict c, uint32_t value)
{
    gpush_at(c, value, 0U);
}

static inline uint32_t gpop(CPU *__restrict c)
{
    return gpop_at(c, 0U);
}

static inline int guest_stack_set_generated(CPU *__restrict c,
                                            uint32_t value)
{
    return guest_stack_set(c, value, 0U);
}

static inline int guest_stack_adjust_generated(CPU *__restrict c,
                                               uint32_t amount)
{
    return guest_stack_adjust(c, amount, 0U);
}

static inline uint32_t guest_stack_address_generated(
    CPU *__restrict c, uint32_t address, uint32_t size)
{
    return guest_stack_address(c, address, size, 0U);
}

static inline void gpush_generated(CPU *__restrict c, uint32_t value)
{
    gpush_at(c, value, 0U);
}

static inline uint32_t gpop_generated(CPU *__restrict c)
{
    return gpop_at(c, 0U);
}

#define GUEST_STACK_CALLSITE_BARRIER() ((void)0)
#endif

/* ------------------------------------------------------- flag derivation - */
/* Each of these is a pure function of the recorded producer state. The
 * translator calls them only where it could not pair the producer with its
 * consumer inside one basic block. */

static inline uint32_t f_mask(uint8_t sz)
{
    return sz == 1 ? 0xFFu : sz == 2 ? 0xFFFFu : 0xFFFFFFFFu;
}
static inline uint32_t f_sign(uint8_t sz)
{
    return sz == 1 ? 0x80u : sz == 2 ? 0x8000u : 0x80000000u;
}

static inline int fl_zf(const guest_flags *c) {
    return c->f_op == FLAG_PARTIAL ? c->f_zf != 0
                                  : (c->f_r & f_mask(c->f_sz)) == 0;
}
static inline int fl_sf(const guest_flags *c) {
    return c->f_op == FLAG_PARTIAL ? c->f_sf != 0
                                  : (c->f_r & f_sign(c->f_sz)) != 0;
}

static inline int fl_pf(const guest_flags *c)
{
    uint8_t v = (uint8_t)c->f_r;
    if (c->f_op == FLAG_PARTIAL) return c->f_pf != 0;
    v ^= (uint8_t)(v >> 4); v ^= (uint8_t)(v >> 2); v ^= (uint8_t)(v >> 1);
    return (v & 1) == 0;
}

static inline int fl_cf(const guest_flags *c)
{
    uint32_t m = f_mask(c->f_sz);
    switch (c->f_op) {
    case FLAG_SUB:  return (c->f_a & m) < (c->f_b & m);
    case FLAG_ADD:  return (c->f_r & m) < (c->f_a & m);
    case FLAG_NEG:  return (c->f_a & m) != 0;
    case FLAG_SHL: {
        uint32_t n = c->f_b & 31U, bits = (uint32_t)c->f_sz * 8U;
        return n && n <= bits ? ((c->f_a & m) >> (bits - n)) & 1U : 0;
    }
    case FLAG_SHR:
    case FLAG_SAR: {
        uint32_t n = c->f_b & 31U, bits = (uint32_t)c->f_sz * 8U;
        return n && n <= bits ? ((c->f_a & m) >> (n - 1U)) & 1U : 0;
    }
    case FLAG_LOGIC: return 0;
    case FLAG_INC:
    case FLAG_DEC:  return c->f_cf != 0;   /* preserved by the emitter */
    default:        return c->f_cf != 0;
    }
}

static inline int fl_of(const guest_flags *c)
{
    uint32_t s = f_sign(c->f_sz);
    switch (c->f_op) {
    case FLAG_SUB:  return (((c->f_a ^ c->f_b) & (c->f_a ^ c->f_r)) & s) != 0;
    case FLAG_ADD:  return (((c->f_a ^ c->f_r) & (c->f_b ^ c->f_r)) & s) != 0;
    case FLAG_INC:  return (c->f_r & f_mask(c->f_sz)) == s;
    case FLAG_DEC:  return (c->f_a & f_mask(c->f_sz)) == s;
    case FLAG_LOGIC: return 0;
    case FLAG_SHL:   return c->f_b == 1U &&
                            (((c->f_r & s) != 0) != (fl_cf(c) != 0));
    case FLAG_SHR:   return c->f_b == 1U && (c->f_a & s) != 0;
    case FLAG_SAR:   return 0;
    default:        return c->f_of != 0;
    }
}

/* The jcc conditions, named as the mnemonics that use them. */
static inline int cc_b (const guest_flags *c) { return  fl_cf(c); }
static inline int cc_ae(const guest_flags *c) { return !fl_cf(c); }
static inline int cc_e (const guest_flags *c) { return  fl_zf(c); }
static inline int cc_ne(const guest_flags *c) { return !fl_zf(c); }
static inline int cc_be(const guest_flags *c) { return  fl_cf(c) ||  fl_zf(c); }
static inline int cc_a (const guest_flags *c) { return !fl_cf(c) && !fl_zf(c); }
static inline int cc_s (const guest_flags *c) { return  fl_sf(c); }
static inline int cc_ns(const guest_flags *c) { return !fl_sf(c); }
static inline int cc_p (const guest_flags *c) { return  fl_pf(c); }
static inline int cc_np(const guest_flags *c) { return !fl_pf(c); }
static inline int cc_l (const guest_flags *c) { return  fl_sf(c) != fl_of(c); }
static inline int cc_ge(const guest_flags *c) { return  fl_sf(c) == fl_of(c); }
static inline int cc_le(const guest_flags *c) { return  fl_zf(c) || fl_sf(c) != fl_of(c); }
static inline int cc_g (const guest_flags *c) { return !fl_zf(c) && fl_sf(c) == fl_of(c); }
static inline int cc_o (const guest_flags *c) { return  fl_of(c); }
static inline int cc_no(const guest_flags *c) { return !fl_of(c); }

/* Record a producer. Kept as a macro so the translator can emit exactly the
 * fields a given operation defines and the optimiser can drop dead stores. */
#define SET_FLAGS(c, op, a, b, r, sz) \
    do { (c)->f_op = (uint8_t)(op); (c)->f_a = (uint32_t)(a); \
         (c)->f_b = (uint32_t)(b); (c)->f_r = (uint32_t)(r); \
         (c)->f_sz = (uint8_t)(sz); } while (0)

/* Where generated code keeps the flag state.  GUEST_FLAGS_DECL opens every
 * generated body; LOAD/FLUSH are emitted only at the two boundary kinds where
 * flags are architecturally live (see guest_flags). */
#if defined(GUEST_FLAGS_LOCAL) && GUEST_FLAGS_LOCAL
#if defined(__GNUC__) || defined(__clang__)
#define GUEST_FLAGS_DECL guest_flags _fl __attribute__((unused)) = {0}
#else
#define GUEST_FLAGS_DECL guest_flags _fl = {0}
#endif
#define GUEST_FL (&_fl)
#define GUEST_FLAGS_LOAD(c) ((void)((_fl) = (c)->fl))
#define GUEST_FLAGS_FLUSH(c) ((void)((c)->fl = (_fl)))
#else
#define GUEST_FLAGS_DECL ((void)0)
#define GUEST_FL (&c->fl)
#define GUEST_FLAGS_LOAD(c) ((void)0)
#define GUEST_FLAGS_FLUSH(c) ((void)0)
#endif

/* ---------------------------------------------------------------- x87 ---- */
/* A plain 8-entry ring with a top index, which is what the hardware is. */

static inline void fpush(CPU *__restrict c, double v)
{
    c->st_top = (c->st_top - 1) & 7;
    c->st[c->st_top] = v;
}
static inline double fpop(CPU *__restrict c)
{
    double v = c->st[c->st_top];
    c->st_top = (c->st_top + 1) & 7;
    return v;
}
/* x87 compare: set C3 C2 C0 exactly as the architecture defines them.
 * greater = 000, less = 001, equal = 100, unordered = 111. The unordered case
 * is not decoration -- NaN reaches here through ordinary game maths, and
 * collapsing it onto "less" is how a comparison silently inverts. */
static inline void fcmp_set(CPU *__restrict c, double a, double b)
{
    c->fsw &= (uint16_t)~((1u << 8) | (1u << 10) | (1u << 14));
    if (a != a || b != b)
        c->fsw |= (uint16_t)((1u << 8) | (1u << 10) | (1u << 14));
    else if (a > b)
        ;                                       /* C3=C2=C0=0 */
    else if (a < b)
        c->fsw |= (uint16_t)(1u << 8);          /* C0 */
    else
        c->fsw |= (uint16_t)(1u << 14);         /* C3 */
}

static inline double *fst(CPU *__restrict c, int i)
{
    return &c->st[(c->st_top + i) & 7];
}

/* ----------------------------------------------------------- dispatch ---- */

/* Registered once at startup from the generated table. */
#ifdef GUEST_GENERATION_SYMBOL
extern const char GUEST_GENERATION_SYMBOL[];
#endif
void      guest_register(const uint32_t *addrs, const guest_fn *fns, uint32_t n);
/* Accepts either a canonical function RVA or a relocated in-image VA. */
guest_fn  guest_lookup(uint32_t addr);          /* NULL when unknown */
/* Read-only view of the registered ascending function-start RVA table, for
 * diagnostics that map an arbitrary code RVA to its owning function. */
const uint32_t *guest_registered_addresses(uint32_t *count);

/* Native replacements for a frozen translated leaf may bypass nested guest
 * dispatch without changing its observable coverage/import/phase census.
 * Both helpers authenticate the registered target before recording the call;
 * zero means the generated/runtime contract has drifted. */
int guest_note_authenticated_translated_call(
    uint32_t addr, uint32_t coverage_function_id);
int guest_note_authenticated_import_call(
    uint32_t slot_addr, uint32_t import_id,
    uint32_t thunk_coverage_function_id);

/* A generator-authenticated direct IAT site may enter the already-validated
 * sync family without repeating guest_call's import classification.  One is
 * returned after either the exact endpoint or its attributed fault owns the
 * call.  Zero is a side-effect-free rejection; the caller must execute its
 * original guest_call fallback. */
int guest_try_direct_sync_import_call(
    CPU *__restrict c, uint32_t target, uint32_t exact_slot_rva);

/* ---------------------------------------- direct IAT import dispatch ---- */
/* The emitter knows, at every `call/jmp dword ptr [IAT slot]` site and at
 * every `call reg` whose register was loaded from an IAT slot earlier in the
 * same basic block, which import that slot names: the PE import directory
 * gives DLL!symbol per slot and the dense import ID is the index of the
 * slot-RVA-sorted table (guest_table.c == host_vita_import_id_map.inc).
 * GUEST_IMPORT_CALL/GUEST_IMPORT_JMP carry that knowledge as constants.
 *
 * GUEST_IMPORT_DIRECT=0 (default: the PC build and every unit compiled
 * without the knob) spells exactly the guest_call the corpus always had.
 * GUEST_IMPORT_DIRECT=1 enters guest_import_call, which compares the value
 * the site read from the slot with the validated slot token of that ID and,
 * only on an exact match, runs the same family endpoint guest_call's import
 * branch would reach (one call census event, import coverage, the same
 * g_host_import_calls owner, the same unresolved fault text).  Any other
 * value -- untagged slot, re-registered table, a guest store into the IAT,
 * a family compiled out of this build -- takes the unchanged guest_call.
 * The slot RVA argument is the corpus contract's textual witness; the
 * token compare already proves the ID/slot pair at run time.
 *
 * Flags.  Neither token publishes the x86 flag state.  `call [IAT]` sites
 * never did; for `jmp [IAT]` the emitter now treats the edge as flags-dead
 * (trans.flag_liveness) because no host import shim reads c->fl (the only
 * c->fl access in runtime/host*.c is the png_crc32 guest seam, a writer).
 * This holds on the fallback branch of GUEST_IMPORT_JMP as well: were a
 * slot word ever a translated address -- only a guest store into the IAT
 * could make it one; Isaac never writes its IAT and
 * test_iat_direct_corpus_contract.py pins zero constant-address stores into
 * the IAT range -- that destination would observe c->fl as of the previous
 * publish, whereas the pre-token thunk republished an identity copy.  The
 * same emitter change is why a GUEST_IMPORT_DIRECT=0 corpus is not
 * text-identical to the pre-token one: the 1237 `jmp [IAT]` sites (1237
 * functions in 12 units) lost GUEST_FLAGS_FLUSH and 71 bodies (67
 * one-instruction thunks) lost GUEST_FLAGS_LOAD; guest_call is still reached
 * at exactly the same sites with the same target word.
 *
 * Divergences from guest_call's import branch, all on unreachable or
 * diagnostic paths: (1) for the two SYNC IDs guest_call's
 * ISAAC_VITA_SYNC_IMPORT_FASTPATH branch faults with "validated Vita sync
 * import rejected its local ID" where guest_import_call faults with the
 * import name -- either text needs isaac_vita_sync_import_indexed to reject
 * a registration-validated index, which the 413-row registration excludes
 * (no *_import_indexed family returns a negative value, so both routes test
 * success identically); (2) the KAGE_VITA_STALL_NOTE_DISPATCH breadcrumb
 * (ISAAC_VITA_STALL_PROBE, diagnostic only) is not recorded on the direct
 * route.
 *
 * Cost (arm-vita-eabi-gcc 10.3, -O2 Thumb-2, perf configuration):
 * guest_import_call is 59 instructions / 0xa4 B (51 / 0x90 B before the
 * per-import census note GUEST_PHASE_PROFILE_NOTE_IMPORT joined the fast
 * path), ~37 of them on the fast path to the family `blx` without the note;
 * the table is 6144 B of bss; ELF-wide .text
 * grows by ~7.7 KB with the option ON (one `mov r2, #id` per site minus the
 * thunk and flush savings).  test_import_id_dispatch.sh pins the shape. */
typedef int (*guest_import_family_fn)(CPU *__restrict, uint32_t, unsigned *);
typedef struct guest_import_direct {
    uint32_t               slot_va;      /* image base + slot RVA; 0 = fallback */
    guest_import_family_fn fn;
    uint32_t               local_index;
} guest_import_direct;
#define GUEST_IMPORT_DIRECT_CAPACITY 512U
void guest_import_call(CPU *__restrict c, uint32_t target, uint32_t import_id);
#ifndef GUEST_IMPORT_DIRECT
#define GUEST_IMPORT_DIRECT 0
#endif
#if GUEST_IMPORT_DIRECT != 0 && GUEST_IMPORT_DIRECT != 1
#error GUEST_IMPORT_DIRECT must be 0 or 1
#endif
#if GUEST_IMPORT_DIRECT
#define GUEST_IMPORT_CALL(target, slot_rva, import_id) \
    guest_import_call(c, (target), (import_id))
#define GUEST_IMPORT_JMP(target, slot_rva, import_id) \
    guest_import_call(c, (target), (import_id))
#else
#define GUEST_IMPORT_CALL(target, slot_rva, import_id) guest_call(c, (target))
#define GUEST_IMPORT_JMP(target, slot_rva, import_id) guest_call(c, (target))
#endif

/* Optional aggregate cost census for the indirect-dispatch binary search.
 * The translated runtime has one CPU writer, so profiler builds use ordinary
 * 32-bit increments.  There is no clock, log, atomic, or production-OFF work
 * on this path.  The loop profiler snapshots unsigned deltas every 120 loops. */
typedef struct GuestPhaseProfileCounters {
    uint32_t guest_calls;
    uint32_t guest_lookups;
    uint32_t lookup_iterations;
#if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE)
    /* ISAAC_VITA_GUEST_DISPATCH_TABLE: guest_call entries (its only hot-path
     * increment) and the entries that left the inline probe for the complete
     * classification.  The profiler prints g(c) = guest_calls + dispatch_calls
     * and g(l) = guest_lookups + (dispatch_calls - dispatch_slow), so the
     * established record keeps its meaning while the fast path pays one
     * increment.  Only the owners of the option (guest.c and
     * kage_vita_phase_profile.c, both compiled with the define) name these
     * fields; every other unit sees the historical 12-byte prefix, whose
     * offsets do not move, and the option-OFF objects stay byte-identical to
     * the base (no field, no counter, no census array). */
    uint32_t dispatch_calls;
    uint32_t dispatch_slow;
    /* Validated Enter/LeaveCriticalSection calls that took a sync fast path
     * (guest_try_direct_sync_import_call or guest_call's validated branch). */
    uint32_t sync_fastpath;
# if defined(ISAAC_VITA_GL_SHIM_TABLE_TOKENS)
    /* ISAAC_VITA_GL_SHIM_TABLE_TOKENS (wf/opt-gltok): typed-GL tokens that
     * hit the inline probe directly; their gl_bridge.c trampoline counts them
     * here where guest_call_slow used to count them in dispatch_slow.
     * Appended after the dispatch-table fields so every owner without this
     * option keeps its offsets.  The profiler derives g(l) as
     * calls - slow - gl, so ph120.c and ph120.ik are unchanged and ph120.d
     * reports the raw count as the fourth field of d(c,s,sf,g). */
    uint32_t dispatch_gl;
# endif
#endif
} GuestPhaseProfileCounters;

/* Per-import call census, indexed by the dense frozen import ID (413 rows;
 * guest.c checks the bound against host_vita_import_id.h).  One indexed
 * increment per host import call, recorded where the ID is already known.
 * One array serves both readers: the dispatch table's ph120.i record (top
 * IDs and binding kinds) and the import-kinds census ph120.ik/ph120.ih
 * (per-kind totals, fourteen named hot imports, the g(c) identity gate).
 * It exists only when one of those options is compiled into the owner, so
 * the option-OFF import paths keep the base instruction stream. */
#define GUEST_PHASE_PROFILE_IMPORT_ID_SLOTS 413U

#if defined(ISAAC_VITA_PHASE_PROFILE)
extern GuestPhaseProfileCounters g_guest_phase_profile_counters;
# define GUEST_PHASE_PROFILE_NOTE_CALL() \
    (++g_guest_phase_profile_counters.guest_calls)
# define GUEST_PHASE_PROFILE_NOTE_LOOKUP() \
    (++g_guest_phase_profile_counters.guest_lookups)
# define GUEST_PHASE_PROFILE_ADD_LOOKUP_ITERATIONS(count) \
    (g_guest_phase_profile_counters.lookup_iterations += (uint32_t)(count))
#else
# define GUEST_PHASE_PROFILE_NOTE_CALL() ((void)0)
# define GUEST_PHASE_PROFILE_NOTE_LOOKUP() ((void)0)
# define GUEST_PHASE_PROFILE_ADD_LOOKUP_ITERATIONS(count) ((void)0)
#endif

#if defined(ISAAC_VITA_PHASE_PROFILE) && \
    defined(ISAAC_VITA_GUEST_DISPATCH_TABLE)
# define GUEST_PHASE_PROFILE_NOTE_DISPATCH_CALL() \
    (++g_guest_phase_profile_counters.dispatch_calls)
# define GUEST_PHASE_PROFILE_NOTE_DISPATCH_SLOW() \
    (++g_guest_phase_profile_counters.dispatch_slow)
# define GUEST_PHASE_PROFILE_NOTE_SYNC_FASTPATH() \
    (++g_guest_phase_profile_counters.sync_fastpath)
#else
# define GUEST_PHASE_PROFILE_NOTE_DISPATCH_CALL() ((void)0)
# define GUEST_PHASE_PROFILE_NOTE_DISPATCH_SLOW() ((void)0)
# define GUEST_PHASE_PROFILE_NOTE_SYNC_FASTPATH() ((void)0)
#endif
/* ISAAC_VITA_GL_SHIM_TABLE_TOKENS: the trampoline in gl_bridge.c (compiled
 * with the table and profile defines of its owner) notes a typed-GL token
 * that hit the inline probe. */
#if defined(ISAAC_VITA_PHASE_PROFILE) && \
    defined(ISAAC_VITA_GUEST_DISPATCH_TABLE) && \
    defined(ISAAC_VITA_GL_SHIM_TABLE_TOKENS)
# define GUEST_PHASE_PROFILE_NOTE_DISPATCH_GL() \
    (++g_guest_phase_profile_counters.dispatch_gl)
#else
# define GUEST_PHASE_PROFILE_NOTE_DISPATCH_GL() ((void)0)
#endif

/* The census array is owned by guest.c and read by kage_vita_phase_profile.c;
 * both are compiled with the option that needs it (ISAAC_VITA_DISPATCH_TABLE
 * or ISAAC_VITA_PROFILE_IMPORT_KINDS).  Generated units and every other
 * owner see the no-op. */
#if defined(ISAAC_VITA_PHASE_PROFILE) && \
    (defined(ISAAC_VITA_GUEST_DISPATCH_TABLE) || \
     defined(ISAAC_VITA_PROFILE_IMPORT_KINDS))
extern uint32_t
    g_guest_phase_profile_import_calls[GUEST_PHASE_PROFILE_IMPORT_ID_SLOTS];
# define GUEST_PHASE_PROFILE_NOTE_IMPORT(import_id) \
    ((void)((import_id) < GUEST_PHASE_PROFILE_IMPORT_ID_SLOTS && \
            ++g_guest_phase_profile_import_calls[(import_id)]))
#else
# define GUEST_PHASE_PROFILE_NOTE_IMPORT(import_id) ((void)(import_id))
#endif

/* A proven direct replacement for a cached indirect edge can retain the
 * profiler's logical cache-hit census without exposing dispatch internals. */
#if defined(ISAAC_VITA_PHASE_PROFILE)
void guest_phase_profile_note_lookup_cache_hit(void);
#endif

/* Optional semantic coverage.  Generated code writes stable dense IDs into
 * three parent-owned byte arrays.  The disabled path is one nullable load and
 * branch; enabled stores are volatile so a translated entry immediately
 * before a fault remains observable.  IDs are generator-proved against the
 * registered schema, so the hot path deliberately has no redundant bound
 * check. */
extern unsigned char *g_guest_coverage_functions;
extern unsigned char *g_guest_coverage_imports;
extern unsigned char *g_guest_coverage_cases;

/* GUEST_COVERAGE_HOOKS=0 compiles the three hooks out of a translation unit.
 * The Vita runtime keeps all three pointers NULL for its whole lifetime
 * (guest.c: the __vita__ coverage stubs), so on that target the enabled form
 * is a dead literal load plus a taken branch at every one of the ~15k
 * translated function entries and ~6.8k switch cases.  Only generated units
 * are compiled with the hooks off; guest.c keeps them for the PC transport. */
#ifndef GUEST_COVERAGE_HOOKS
#define GUEST_COVERAGE_HOOKS 1
#endif
#if GUEST_COVERAGE_HOOKS != 0 && GUEST_COVERAGE_HOOKS != 1
#error GUEST_COVERAGE_HOOKS must be 0 or 1
#endif

/* Opt-in census of translated body entries (ISAAC_VITA_PROFILE_FUNCTION_ENTRIES,
 * profile builds only): every generated body calls guest_coverage_function
 * exactly once on entry, so this counts direct calls, tail jumps and indirect
 * dispatches alike; direct + tail = ent - g(l) in ph120.ik.  One increment per
 * entry; production and every default build compile it to nothing. */
#if defined(ISAAC_VITA_PROFILE_FUNCTION_ENTRIES)
extern uint32_t g_guest_function_entries;
# define GUEST_PROFILE_NOTE_FUNCTION_ENTRY() (++g_guest_function_entries)
#else
# define GUEST_PROFILE_NOTE_FUNCTION_ENTRY() ((void)0)
#endif

static inline void guest_coverage_function(uint32_t id)
{
    GUEST_PROFILE_NOTE_FUNCTION_ENTRY();
#if GUEST_COVERAGE_HOOKS
    unsigned char *bytes = g_guest_coverage_functions;
    if (bytes) ((volatile unsigned char *)bytes)[id] = 1u;
#else
    (void)id;
#endif
}

static inline void guest_coverage_import(uint32_t id)
{
#if GUEST_COVERAGE_HOOKS
    unsigned char *bytes = g_guest_coverage_imports;
    if (bytes) ((volatile unsigned char *)bytes)[id] = 1u;
#else
    (void)id;
#endif
}

static inline void guest_coverage_case(uint32_t id)
{
#if GUEST_COVERAGE_HOOKS
    unsigned char *bytes = g_guest_coverage_cases;
    if (bytes) ((volatile unsigned char *)bytes)[id] = 1u;
#else
    (void)id;
#endif
}

/* The generated registration table supplies the exact schema/build identity
 * before registering any translated function.  entry_test maps the parent-
 * created file only after guest_register_all() has completed and before the
 * first guest call.  With coverage disabled this API is a no-op. */
void guest_register_coverage_contract(const char *schema_id,
                                      const char *build_id,
                                      uint32_t function_count,
                                      uint32_t import_count,
                                      uint32_t case_count);
int  guest_coverage_init_from_env(void);
void guest_coverage_shutdown(void);
const char *guest_coverage_error(void);

/* Called by the generated registration table.  Besides printing attribution,
 * the generated symbol makes a current guest.c fail to link against stale
 * guest_table.obj even when filesystem timestamps happen to look plausible. */
void        guest_note_generation(const char *generation_id);
const char *guest_generation_id(void);

/* Register generated PE import metadata before guest_image_load().  The
 * loader tags each IAT slot with its own relocated address; this preserves
 * identity even when guest code loads an import into a register before
 * calling it.  Lookup accepts either the slot RVA or that relocated VA. */
void        guest_register_imports(const guest_import *imports, uint32_t n);
const char *guest_import_name(uint32_t slot_addr); /* NULL when not an IAT slot */

/* Target backend hook.  Return non-zero only after fully emulating the named
 * import, including its x86 stack cleanup. */
int guest_host_import(CPU *__restrict c, const char *name);
extern unsigned g_host_import_calls;

/* Dynamically resolved platform functions (GetProcAddress/wglGetProcAddress)
 * use stable synthetic guest addresses. Lookup is exact: an arbitrary value
 * in the token range is not callable, and each handler owns its x86 ABI. */
int guest_host_dynamic(CPU *__restrict c, uint32_t token);
extern unsigned g_host_dynamic_calls;

#if defined(ISAAC_VITA_SHADER_ATTRIB_FASTPATH)
/* ---- ISAAC_VITA_SHADER_ATTRIB_FASTPATH (wf/opt-attrib) begin ----------
 * Read-only view of the 0x7e-first startup fact of the GL shim fast
 * dispatch: while it holds, guest_call hands every registered typed-GL token
 * straight to the GL dynamic entry, so a host replay of the same tokens
 * (host_vita_shader_attrib_fastpath.c) is that dispatch minus the
 * classification; when it does not hold the replay declines. */
int guest_gl_dynamic_first_holds(void);
/* ---- ISAAC_VITA_SHADER_ATTRIB_FASTPATH end ---------------------------- */
#endif
extern int g_guest_gl_inventory_mode;

/* Exact caller-side KAGE handoff.  False means execute the original guest
 * bytes; true requires an explicitly enabled, already-ready PC backend. */
int guest_kage_pc_handoff_ready(void);

/* Platform capability helpers used by translated CPUID/XGETBV. */
void guest_cpuid(CPU *__restrict c);
void guest_xgetbv(CPU *__restrict c);

/* Base of the synthetic single-thread x86 TEB/TIB used by FS overrides. */
uint32_t guest_fs_base(CPU *__restrict c);
/* Value of the last completed guest_fs_base() while every TIB field it
 * refreshes is unchanged; zero whenever the stack binding or the static TLS
 * block moved, so the next access re-runs the full refresh.  Owned by guest.c. */
extern uint32_t g_guest_fs_base_cached;

/* An indirect call or jump. Faults, loudly, on an address we have no
 * translation for. A fault exits through the active top-level boundary; a
 * call made without one is a programming error and terminates the process. */
void guest_call(CPU *__restrict c, uint32_t addr);

#if defined(ISAAC_VITA_GUEST_DISPATCH_TABLE)
/* Boot-built O(1) dispatch table keyed by the exact relocated VA of every
 * registered translated function (see guest.c).  The status is diagnostic:
 * a table that is not ready is empty, and every guest_call then takes the
 * complete classification path unchanged. */
typedef struct guest_dispatch_table_status {
    uint32_t    ready;              /* 1: built and exhaustively verified */
    uint32_t    keys;               /* functions reachable through the table */
    uint32_t    skipped;            /* registered functions left to the slow path */
    uint32_t    max_bucket;         /* largest first-level bucket */
    uint32_t    bucket_tries;       /* multiplier candidates tried in the build */
    const char *reason;             /* NULL when ready */
#if defined(ISAAC_VITA_GL_SHIM_TABLE_TOKENS)
    /* ISAAC_VITA_GL_SHIM_TABLE_TOKENS: typed-GL tokens placed in the table
     * (gl_bridge's whole registry while no IAT slot key and no image window
     * can alias the 0x7e family, else 0: every token keeps the slow path). */
    uint32_t    gl_tokens;
#endif
} guest_dispatch_table_status;
void guest_dispatch_table_get_status(guest_dispatch_table_status *out);
#endif

/* MSVC x86 `_setjmp3` / `longjmp` bridge.  The emitter must call prepare,
 * execute native setjmp(site.native_env) as a controlling expression in the
 * generated owner, and call finish from both branches.  No ordinary host
 * `_setjmp3` import handler can satisfy that lifetime contract. */
void guest_setjmp_prepare(CPU *__restrict c, guest_jump_site *site);
void guest_setjmp_finish(CPU *__restrict c, guest_jump_site *site,
                         int resumed);
void guest_setjmp_leave(CPU *__restrict c, guest_jump_site *site);

#if defined(_MSC_VER)
# define GUEST_NORETURN __declspec(noreturn)
#elif defined(__GNUC__)
# define GUEST_NORETURN __attribute__((noreturn))
#else
# define GUEST_NORETURN
#endif
GUEST_NORETURN void guest_longjmp(CPU *__restrict c, uint32_t guest_env,
                                  int32_t value);

enum {
    GUEST_RUN_INVALID  = -1,
    GUEST_RUN_RETURNED = 0,
    GUEST_RUN_FAULT    = 1,
    GUEST_RUN_EXIT     = 2
};

/* Execute one translated top-level call and stop immediately at the first
 * guest_fault or guest_exit, even when it happened many generated C calls
 * below. This is the bring-up boundary: without it a loud unresolved import
 * would otherwise return to its generated caller with an unbalanced guest
 * stack, while a noreturn CRT exit would run into its following INT3.
 *
 * The CPU after a trapped stop is a diagnostic snapshot, not resumable guest
 * state: native frames have been unwound while guest registers/memory retain
 * partial changes. Returns one of GUEST_RUN_RETURNED, GUEST_RUN_FAULT, or
 * GUEST_RUN_EXIT; GUEST_RUN_INVALID means bad input or a nested boundary on
 * the same CPU. */
int guest_run_until_stop(CPU *__restrict c, guest_fn entry);

/* Model a noreturn guest CRT/process termination without terminating the host
 * harness. This leaves fault==NULL and records the exact API and exit code.
 * Calling it outside a guarded boundary is a runtime contract breach. */
void guest_exit(CPU *__restrict c, int32_t code, const char *api);

/* Reached when a translated function hits something we chose not to
 * implement (an unimplemented instruction, `throw` in the first build). */
void guest_fault(CPU *__restrict c, uint32_t addr, const char *what);
void guest_int3(CPU *__restrict c, uint32_t addr);
extern uint32_t g_guest_checkpoint_rva;
int  guest_checkpoint(CPU *__restrict c, uint32_t rva);

/* ------------------------------------------------------------- image ----- */

/* PC milestone base. A target may override this at compile time, but the
 * translator must rebase to the same value before generated C is built. */
#ifndef GUEST_IMAGE_BASE
#define GUEST_IMAGE_BASE 0x30000000u
#endif

/* A generator-proved indirect edge may call its exact translated owner
 * directly, while preserving guest_call for every other value.  Vtables in
 * the mapped PE hold relocated VAs; focused callers may still use an RVA. */
static inline int guest_direct_translated_target(
    uint32_t target, uint32_t exact_rva)
{
    return target == exact_rva ||
           target == (uint32_t)(GUEST_IMAGE_BASE + exact_rva);
}

/* Map the original image at GUEST_IMAGE_BASE and apply its PE relocations.
 * Returns 0 on success. */
int  guest_image_load(const char *path);
void guest_image_free(void);

/* Return non-zero only when the complete byte range belongs to the currently
 * mapped PE image.  Import shims use this before following compiler-owned
 * metadata pointers; ordinary heap/stack guest pointers are intentionally
 * outside this predicate. */
int guest_image_contains(uint32_t address, uint32_t size);

/* Bind the PE static-TLS template into the fake TIB and invoke every
 * IMAGE_TLS_DIRECTORY callback with DLL_PROCESS_ATTACH. Call under a
 * guest_run_until_stop boundary after dispatch registration. Returns zero
 * on success/idempotent re-entry; configuration failures become loud faults. */
int guest_tls_process_attach(CPU *__restrict c);

/* Initialise all architectural and host-private CPU state to zero. */
void guest_cpu_init(CPU *c);

/* The guest stack. The biggest function in the binary asks __chkstk for a
 * 20,560-byte frame in its prologue, so this is not a place to be frugal --
 * and the Vita gives a created thread 64 KB by default. */
#ifndef GUEST_STACK_SIZE
#define GUEST_STACK_SIZE (4u * 1024u * 1024u)
#endif
int  guest_stack_init(CPU *c);
void guest_stack_free(CPU *c);
int  guest_stack_bind(CPU *c, uint32_t floor, uint32_t ceiling);

/* Keep the runtime-owned stack contract separate from the generated hot path.
 * This opt-out lives after all existing declarations and inline definitions
 * deliberately: when it is absent/default-ON, every pre-existing source
 * location and active token above stays unchanged.  That matters to GCC 10's
 * inliner and maybe-uninitialized diagnostics in legacy host oracles.
 *
 * Compile only generated translation units with this set to zero.  The public
 * names are then redirected to the exact raw operations used before generated
 * stack diagnostics, while guest.c and every generic runtime helper keep the
 * checked GUEST_STACK_REQUIRED contract and ownership metadata. */
#ifndef GUEST_GENERATED_STACK_GUARD
#define GUEST_GENERATED_STACK_GUARD 1
#endif
#if GUEST_GENERATED_STACK_GUARD != 0 && GUEST_GENERATED_STACK_GUARD != 1
#error GUEST_GENERATED_STACK_GUARD must be 0 or 1
#endif
#if !GUEST_GENERATED_STACK_GUARD
static inline int guest_stack_set_generated_raw(CPU *__restrict c,
                                                uint32_t value)
{
    c->esp = value;
    return 1;
}

static inline int guest_stack_adjust_generated_raw(CPU *__restrict c,
                                                   uint32_t amount)
{
    c->esp += amount;
    return 1;
}

static inline uint32_t guest_stack_address_generated_raw(
    CPU *__restrict c, uint32_t address, uint32_t size)
{
    (void)c;
    (void)size;
    return address;
}

static inline void gpush_generated_raw(CPU *__restrict c, uint32_t value)
{
    c->esp -= 4U;
    st32(c->esp, value);
}

static inline uint32_t gpop_generated_raw(CPU *__restrict c)
{
    uint32_t value = ld32(c->esp);
    c->esp += 4U;
    return value;
}

#define guest_stack_set_generated guest_stack_set_generated_raw
#define guest_stack_adjust_generated guest_stack_adjust_generated_raw
#define guest_stack_address_generated guest_stack_address_generated_raw
#define gpush_generated gpush_generated_raw
#define gpop_generated gpop_generated_raw
#undef GUEST_STACK_CALLSITE_BARRIER
#define GUEST_STACK_CALLSITE_BARRIER() ((void)0)
#endif

/* GUEST_FS_BASE_INLINE=1 (generated units only) answers fs-segment accesses
 * from the cached TIB base.  The corpus has 5,668 static fs:[..] sites, most
 * of them the MSVC SEH prologue/epilogue pair of every C++ frame; the exported
 * guest_fs_base() is an out-of-line call that bumps a diagnostic counter and
 * rewrites three TIB words per access.  guest.c zeroes the cache whenever one
 * of those words can change, so the observed TIB contents are identical. */
#ifndef GUEST_FS_BASE_INLINE
#define GUEST_FS_BASE_INLINE 0
#endif
#if GUEST_FS_BASE_INLINE != 0 && GUEST_FS_BASE_INLINE != 1
#error GUEST_FS_BASE_INLINE must be 0 or 1
#endif
#if GUEST_FS_BASE_INLINE
static inline uint32_t guest_fs_base_generated(CPU *__restrict c)
{
    uint32_t base = g_guest_fs_base_cached;
    return base ? base : guest_fs_base(c);
}
#define guest_fs_base guest_fs_base_generated
#endif

/* ------------------------------------------ general registers as locals --- */
/* Generated code names the eight general registers through GR(reg) and the
 * five synthetic-stack operations through GPUSH/GPOP/GESP_SET/GESP_ADJ/
 * GSTACK_ADDR.  By default (GUEST_GPR_LOCAL=0) they spell exactly the CPU
 * field and the exact `_generated` helper call that generated code always
 * used, so the object code is unchanged.
 *
 * GUEST_GPR_LOCAL=1 (generated units only) keeps eax..edi and esp in eight
 * automatic variables for the whole body.  The Cortex-A9 disassembly showed
 * why: a guest store `*(uint32_t *)addr = v` may alias the CPU struct as far
 * as GCC can prove, so every `c->reg` was reloaded after every guest memory
 * access and stored back before every call.  Automatic variables whose
 * address is never taken cannot alias guest memory.
 *
 * The contract in locals mode: the body opens with GUEST_GPR_DECL (loads all
 * eight); every place where code outside this body may read or write the
 * registers -- a translated callee, guest_call, guest_fault/guest_int3,
 * guest_cpuid/guest_xgetbv, a host seam, a `_setjmp3` site, `return` -- is
 * preceded by GUEST_GPR_FLUSH(c) and, if control comes back, followed by
 * GUEST_GPR_RELOAD(c).  Between those points `c->eax..edi` are stale and
 * must not be read; gen_all's seam pass brackets every host seam with the
 * same pair, and test_flags_local_corpus_contract.py re-derives the rule
 * from the emitted text. */
#ifndef GUEST_GPR_LOCAL
#define GUEST_GPR_LOCAL 0
#endif
#if GUEST_GPR_LOCAL != 0 && GUEST_GPR_LOCAL != 1
#error GUEST_GPR_LOCAL must be 0 or 1
#endif

/* A corpus generated with GUEST_LEAF_INLINE=1 (recomp/leaf_inline.py) spells
 * every direct call to a proven tiny leaf twice: the callee's statements
 * copied in place under `#if GUEST_LEAF_INLINE` and the ordinary
 * `GUEST_GPR_FLUSH(c); sub_X(c); GUEST_GPR_RELOAD(c);` under `#else`.  The
 * return word is pushed and popped either way, so the guest stack is
 * identical; the knob only selects the C call or the copy and lets a device
 * A/B bisect by relink.  Corpora without inlined sites ignore it. */
#ifndef GUEST_LEAF_INLINE
#define GUEST_LEAF_INLINE 1
#endif
#if GUEST_LEAF_INLINE != 0 && GUEST_LEAF_INLINE != 1
#error GUEST_LEAF_INLINE must be 0 or 1
#endif
#if GUEST_GPR_LOCAL
#if defined(__GNUC__) || defined(__clang__)
#define GUEST_GPR_UNUSED __attribute__((unused))
#else
#define GUEST_GPR_UNUSED
#endif
#define GR(reg) _##reg
#define GUEST_GPR_FLUSH(c) \
    ((void)((c)->eax = _eax, (c)->ecx = _ecx, (c)->edx = _edx, \
            (c)->ebx = _ebx, (c)->esp = _esp, (c)->ebp = _ebp, \
            (c)->esi = _esi, (c)->edi = _edi))
#define GUEST_GPR_RELOAD(c) \
    ((void)(_eax = (c)->eax, _ecx = (c)->ecx, _edx = (c)->edx, \
            _ebx = (c)->ebx, _esp = (c)->esp, _ebp = (c)->ebp, \
            _esi = (c)->esi, _edi = (c)->edi))
#if GUEST_GENERATED_STACK_GUARD
/* Checked stack: the out-of-line validators read and write c->esp, so the
 * local is published before and reloaded after each one.  A rejected
 * operation has already recorded its fault; GESP_SET/GESP_ADJ then publish
 * every register so the caller's `return` leaves a complete snapshot. */
#define GUEST_GPR_DECL \
    uint32_t GUEST_GPR_UNUSED _eax = c->eax, _ecx = c->ecx, _edx = c->edx, \
             _ebx = c->ebx, _esp = c->esp, _ebp = c->ebp, _esi = c->esi, \
             _edi = c->edi, _gsp_value GUEST_GPR_UNUSED = 0U; \
    int GUEST_GPR_UNUSED _gsp_ok = 0
#define GPUSH(value) \
    do { (c)->esp = _esp; gpush_generated((c), (value)); \
         _esp = (c)->esp; } while (0)
#define GPOP() \
    ((c)->esp = _esp, _gsp_value = gpop_generated(c), _esp = (c)->esp, \
     _gsp_value)
#define GESP_SET(value) \
    ((c)->esp = _esp, _gsp_ok = guest_stack_set_generated((c), (value)), \
     _esp = (c)->esp, _gsp_ok ? 1 : (GUEST_GPR_FLUSH(c), 0))
#define GESP_ADJ(amount) \
    ((c)->esp = _esp, _gsp_ok = guest_stack_adjust_generated((c), (amount)), \
     _esp = (c)->esp, _gsp_ok ? 1 : (GUEST_GPR_FLUSH(c), 0))
#define GSTACK_ADDR(address, size) \
    ((c)->esp = _esp, guest_stack_address_generated((c), (address), (size)))
#else
/* Raw stack (the production corpus): plain arithmetic on the local.  The
 * pushed value is evaluated before the decrement, exactly as the call
 * argument of gpush_generated_raw was (`push esp` stores the old ESP). */
#define GUEST_GPR_DECL \
    uint32_t GUEST_GPR_UNUSED _eax = c->eax, _ecx = c->ecx, _edx = c->edx, \
             _ebx = c->ebx, _esp = c->esp, _ebp = c->ebp, _esi = c->esi, \
             _edi = c->edi
#define GPUSH(value) \
    do { uint32_t _gsp_value = (uint32_t)(value); _esp -= 4U; \
         st32(_esp, _gsp_value); } while (0)
#define GPOP() (_esp += 4U, ld32(_esp - 4U))
#define GESP_SET(value) (_esp = (uint32_t)(value), 1)
#define GESP_ADJ(amount) (_esp += (uint32_t)(amount), 1)
#define GSTACK_ADDR(address, size) ((uint32_t)(address))
#endif
#else
#define GUEST_GPR_DECL ((void)0)
#define GR(reg) c->reg
#define GUEST_GPR_FLUSH(c) ((void)0)
#define GUEST_GPR_RELOAD(c) ((void)0)
#define GPUSH(value) gpush_generated(c, value)
#define GPOP() gpop_generated(c)
#define GESP_SET(value) guest_stack_set_generated(c, value)
#define GESP_ADJ(amount) guest_stack_adjust_generated(c, amount)
#define GSTACK_ADDR(address, size) guest_stack_address_generated(c, address, size)
#endif

#ifdef __cplusplus
}
#endif
#endif /* ISAAC_GUEST_H */
