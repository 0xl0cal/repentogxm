/* Differential host oracle for ISAAC_VITA_KAGE_QUAD_FASTPATH
 * (wf/cpu-render; driven by recomp/test_vita_kage_quad_fastpath.py).
 *
 * The production dispatcher (guest.c, included) is built with the quad
 * option, the floor thunk option it depends on and the per-import census
 * array.  Ten translated bodies of the frozen corpus (sub_0055f990
 * Image::PushQuad with its host seam, sub_0055ee80 Ring::Alloc, sub_00561e20
 * attribute size table, sub_0055e590 snapped-quad rebuild, sub_005ec3b2 the
 * floor IAT thunk, and the tail callees the replay enters by direct call:
 * sub_005607a0 blend/batch select, sub_00561ff0 blend reset with its
 * sub_0055ed30 descriptor builder and sub_00562030 SetBlend, sub_00026fb0
 * dirty-vector growth; extracted verbatim by the test) are linked four
 * times: with the seam compiled in (production shape) and without it
 * (reference: the exact translated body), in both GPR spellings
 * (GUEST_GPR_LOCAL=1 production, 0 memory mode).  The callees outside that
 * set (the batch lookup sub_005603f0 with its eleven callees, the pair-vector
 * growth sub_0008d900, the std::vector allocator/memmove/swap helpers of the
 * dirty growth, the memcpy thunk, the unknown-format log) are stateful
 * stand-ins that trace their arguments and the register file at entry.
 *
 * Every case runs the reference and the seamed body on byte-identical
 * synthetic state (Image object, attribute formats, batch object with its
 * vertex/index rings and buffers, a second batch with empty ring records the
 * select may return, quad/uv/colour inputs, dirty vector, render target with
 * a vtable of registered getters, the Renderer object at 0x7c7a30 with its
 * getters and SetBlend, the current blend descriptor, batch-group pointer,
 * (batch, image) pair vector and rebind words, the globals and .rdata
 * constants at their real image addresses, the relocated 561e58 jump table,
 * the floor IAT slot, a bound guest stack) and requires the recorded
 * call/import trace (getter entries with the full register file, floor
 * imports with argument and address, every stand-in callee), the CPU after
 * the return (eight registers, stop kind, fault, x87 top, low-water mark),
 * the caller's frame words, every byte of the synthetic arena, the grown
 * storage and the global words to be identical, and every census
 * (guest_calls, host import calls, per-import census slot 313, import
 * coverage byte 313, getters, allocator calls, stand-ins) to advance by the
 * same amount.  Hostile inputs (count above the bound, unknown format, a
 * stride the formats do not sum to, unauthenticated floor slot, import IDs
 * not ready, null vtable or getter slot, a stack too shallow for the
 * translated frame, ESP below the floor, a frame straddling the ceiling)
 * must decline before the first side effect; inputs the translated body
 * cannot execute at all (null this/quad/formats/batch/records/buffers) are
 * probed against the fast path alone and must leave the CPU and memory
 * untouched; states only the batch select can produce (a null or hollow
 * fresh batch) are probed for the replay's fault after the select.
 *
 * Executed on a 32-bit identity-mapped host (MSVC x86, /LARGEADDRESSAWARE so
 * the 0x98xxxxxx image words can be committed at their real addresses). */
#include "guest.c"

#include <math.h>
#include <stdarg.h>

#include "host_vita_import_id.h"

/* Floor receipts of the replay: the hook records the argument the way the
 * translated route's import handler below does. */
static void oracle_floor_note(CPU *__restrict c, double value, uint32_t at);
#define ISAAC_KAGE_QUAD_ORACLE_FLOOR_NOTE(c, value, arg) \
    oracle_floor_note((c), (value), (arg))
/* The fast path itself, with its entry point renamed so the oracle can wrap
 * it and count verdicts; the seamed body calls the public name. */
#define isaac_vita_kage_quad_try oracle_kage_quad_impl
#include "host_vita_kage_quad_fastpath.c"
#undef isaac_vita_kage_quad_try

#include <windows.h>

#if UINTPTR_MAX != UINT32_MAX
# error host_vita_kage_quad_fastpath_oracle requires a 32-bit host
#endif
#if !defined(ISAAC_VITA_IMPORT_ID_DISPATCH) || \
    !defined(ISAAC_VITA_GUEST_LOOKUP_CACHE) || \
    !defined(ISAAC_VITA_PHASE_PROFILE) || GUEST_STACK_REQUIRED != 1 || \
    !defined(ISAAC_VITA_PROFILE_IMPORT_KINDS) || \
    !defined(ISAAC_VITA_KAGE_QUAD_FASTPATH) || \
    !defined(ISAAC_VITA_FLOOR_THUNK_FASTPATH)
# error build this oracle with the production guest.c configuration
#endif
#if GUEST_IMAGE_BASE != 0x98000000u
# error the frozen corpus text is rebased to 0x98000000
#endif

#define VA(rva) ((uint32_t)(GUEST_IMAGE_BASE + (rva)))

/* ---- the translated bodies (four link-time variants) ------------------ */

void sub_0055f990(CPU *__restrict c);          /* seam, GPR locals */
void ref_sub_0055f990(CPU *__restrict c);      /* reference, GPR locals */
void mem_sub_0055f990(CPU *__restrict c);      /* seam, memory mode */
void mem_ref_sub_0055f990(CPU *__restrict c);  /* reference, memory mode */
/* The translated SetBlend (GPR-local build) the Renderer vtable reaches. */
void sub_00562030(CPU *__restrict c);

/* ---- trace ------------------------------------------------------------ */

static char s_trace[1u << 16];
static size_t s_trace_len;
static int s_trace_overflow;

static void trace(const char *format, ...)
{
    va_list ap;
    int n;

    if (s_trace_len >= sizeof s_trace - 1u) {
        s_trace_overflow = 1;
        return;
    }
    va_start(ap, format);
    n = vsnprintf(s_trace + s_trace_len, sizeof s_trace - s_trace_len,
                  format, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= sizeof s_trace - s_trace_len) {
        s_trace_overflow = 1;
        s_trace_len = sizeof s_trace - 1u;
        return;
    }
    s_trace_len += (size_t)n;
}

static void trace_reset(void)
{
    s_trace_len = 0u;
    s_trace[0] = '\0';
    s_trace_overflow = 0;
}

static uint64_t double_bits(double v)
{
    uint64_t bits;
    memcpy(&bits, &v, sizeof bits);
    return bits;
}

/* ---- stubs owned by other Vita runtime units ------------------------ */

unsigned g_host_import_calls;
unsigned g_host_dynamic_calls;
/* host_vita_import_id.c owns this in production. */
int g_isaac_vita_import_ids_ready = 1;
static unsigned s_generic_import_calls;
static unsigned s_floor_imports;
static unsigned s_floor_notes;

int guest_host_import_ids_register(const guest_import *imports, uint32_t count)
{
    (void)imports;
    (void)count;
    return 1;
}

/* Row 313: vita_math_floor (host_vita_math.c) verbatim; every other row is
 * the generic stand-in of the attrib oracle. */
int guest_host_import_id(CPU *__restrict c, uint32_t import_id)
{
    ++g_host_import_calls;
    if (import_id == ISAAC_VITA_KAGE_QUAD_FLOOR_IMPORT_ID) {
        uint32_t at = guest_stack_address(c, c->esp + 4U, 8U, 0U);
        double value = ldd(at);
        ++s_floor_imports;
        trace("floor arg=%016llx at=%08x\n",
              (unsigned long long)double_bits(value), at);
        fpush(c, floor(value));
        (void)gpop(c);
        return 1;
    }
    ++s_generic_import_calls;
    c->eax = UINT32_C(0x60000000) | import_id;
    (void)gpop(c);
    return 1;
}

static void oracle_floor_note(CPU *__restrict c, double value, uint32_t at)
{
    (void)c;
    ++s_floor_notes;
    trace("floor arg=%016llx at=%08x\n",
          (unsigned long long)double_bits(value), at);
}

const char *guest_host_import_id_error(void)
{
    return "oracle generic import-ID error";
}

int guest_host_dynamic(CPU *__restrict c, uint32_t token)
{
    (void)c;
    (void)token;
    return 0;
}

/* ---- verdict-counting wrapper around the fast path -------------------- */

static unsigned s_handled;
static unsigned s_rejected;

int isaac_vita_kage_quad_try(CPU *__restrict c)
{
    int handled = oracle_kage_quad_impl(c);
    if (handled)
        ++s_handled;
    else
        ++s_rejected;
    trace("fastpath %s\n", handled ? "HANDLED" : "REJECTED");
    return handled;
}

/* ---- synthetic guest state --------------------------------------------- */

#define ORACLE_STACK_WORDS 512u
#define ORACLE_STACK_GUARD_WORDS 256u
#define ORACLE_STACK_ABOVE_WORDS 16u
#define ORACLE_RETURN_RVA UINT32_C(0x0055f42a)
#define ORACLE_MAX_FORMATS 17u
#define ORACLE_VERTEX_BYTES 8192u
#define ORACLE_INDEX_WORDS 64u
#define ORACLE_DIRTY_SLOTS 8u
#define ORACLE_PAIR_SLOTS 8u

/* Guard words below the floor absorb the raw pushes of the translated body
 * on a stack too shallow for its frame; words above the ceiling absorb the
 * argument reads of a frame that straddles it. */
static uint32_t s_stack_storage[ORACLE_STACK_GUARD_WORDS + ORACLE_STACK_WORDS +
                                ORACLE_STACK_ABOVE_WORDS];

typedef struct ring_record {
    uint32_t ptr, count, capacity, base;
} ring_record;

typedef struct arena {
    uint8_t image[0x80];
    uint8_t formats[32];
    uint8_t batch[0x40];
    ring_record vrecs[2];
    ring_record irecs[2];
    uint8_t batch2[0x40];             /* the batch the select may return */
    ring_record vrecs2[2];
    ring_record irecs2[2];
    uint8_t vertices[ORACLE_VERTEX_BYTES];
    uint16_t indices[ORACLE_INDEX_WORDS];
    float quad[8];
    float uv[9];
    float colours[4][4];
    uint32_t dirty[ORACLE_DIRTY_SLOTS];
    uint32_t pairs[2u * ORACLE_PAIR_SLOTS];  /* (batch, image) pair vector */
    uint8_t prev_image[0x80];         /* the rebind clears its [+0x5c] */
    uint32_t target[0x108 / 4];       /* vtable at 0, w at 0x100, h at 0x104 */
    uint32_t target_vtable[16];
    uint32_t allocator_vtable[16];
    uint32_t renderer_vtable[16];     /* +0x30 GetWidth, +0x34 GetHeight, +0x38 SetBlend */
    uint32_t renderer_size[2];
} arena;

static arena s_arena;
static CPU s_cpu;
static unsigned char s_coverage_imports[GUEST_PHASE_PROFILE_IMPORT_ID_SLOTS];

#define A(field) ((uint32_t)(uintptr_t)&s_arena.field)

/* ---- direct callees the bodies reach outside the replayed set --------- */

static unsigned s_stub_calls;
static unsigned s_batch_selects;
static unsigned s_vector_allocs;
static unsigned s_set_blends;
static unsigned s_getter_calls;
static unsigned s_alloc_calls;
static unsigned char s_grow_arena[65536];
static uint32_t s_grow_used;
/* The batch the select returns for the running case: 0 the image's batch,
 * 1 the fresh batch2, 2 null. */
static int s_batch_select;

static uint32_t grow_take(CPU *__restrict c, uint32_t at_rva, uint32_t bytes)
{
    uint32_t at;
    s_grow_used = (s_grow_used + 15u) & ~15u;
    if (bytes > sizeof s_grow_arena - s_grow_used) {
        guest_fault(c, VA(at_rva), "oracle grow arena exhausted");
        return 0u;
    }
    at = (uint32_t)(uintptr_t)&s_grow_arena[s_grow_used];
    s_grow_used += bytes;
    return at;
}

/* sub_005603f0: the batch lookup/creation of the blend select (thiscall,
 * one bool argument; ret 4; 736 translated lines over hash containers and
 * allocators).  Stand-in: returns the batch the case prescribes and traces
 * the full register file at entry, which the replay's call state
 * determines through the select body. */
void sub_005603f0(CPU *__restrict c)
{
    ++s_stub_calls;
    ++s_batch_selects;
    trace("batch-select this=%08x arg=%u ret=%08x eax=%08x edx=%08x "
          "ebx=%08x esp=%08x ebp=%08x esi=%08x edi=%08x\n", c->ecx,
          ld8(c->esp + 4u), ld32(c->esp), c->eax, c->edx, c->ebx, c->esp,
          c->ebp, c->esi, c->edi);
    c->eax = s_batch_select == 0 ? A(batch[0]) :
             s_batch_select == 1 ? A(batch2[0]) : 0u;
    c->ecx = UINT32_C(0x22222222);
    c->edx = UINT32_C(0x33333333);
    (void)gpop(c);
    c->esp += 4u;
}

/* sub_0008d900: growth of the (batch, image) pair vector at 0x7e7d28
 * (thiscall, insert position, &pair; ret 8).  Stand-in: storage for twice
 * the pairs (at least one) from the grow arena, the pairs copied, the new
 * pair appended, begin/end/cap rewritten. */
void sub_0008d900(CPU *__restrict c)
{
    uint32_t vec = c->ecx, pos = ld32(c->esp + 4u), value = ld32(c->esp + 8u);
    uint32_t begin = ld32(vec), end = ld32(vec + 4u), cap = ld32(vec + 8u);
    uint32_t count = (end - begin) / 8u;
    uint32_t newcap = cap == begin ? 1u : (cap - begin) / 8u * 2u;
    uint32_t at;

    ++s_stub_calls;
    trace("pairs-grow vec=%08x pos=%08x pair=%08x,%08x begin=%08x end=%08x "
          "cap=%08x\n", vec, pos, ld32(value), ld32(value + 4u), begin, end,
          cap);
    if (pos != end)
        guest_fault(c, VA(0x0008d900u), "oracle pair growth not at the end");
    at = grow_take(c, 0x0008d900u, newcap * 8u);
    memcpy((void *)(uintptr_t)at, (const void *)(uintptr_t)begin, count * 8u);
    st32(at + count * 8u, ld32(value));
    st32(at + count * 8u + 4u, ld32(value + 4u));
    st32(vec, at);
    st32(vec + 4u, at + (count + 1u) * 8u);
    st32(vec + 8u, at + newcap * 8u);
    c->eax = at + count * 8u;
    c->ecx = UINT32_C(0x44444444);
    c->edx = UINT32_C(0x55555555);
    (void)gpop(c);
    c->esp += 8u;
}

/* sub_00018920: std::allocator<T*>::allocate(n) (ret 4) of the dirty-vector
 * growth: n words from the grow arena. */
void sub_00018920(CPU *__restrict c)
{
    uint32_t n = ld32(c->esp + 4u);
    ++s_stub_calls;
    ++s_vector_allocs;
    trace("vector-alloc n=%u ret=%08x\n", n, ld32(c->esp));
    c->eax = grow_take(c, 0x00018920u, n * 4u);
    c->ecx = UINT32_C(0x66666666);
    c->edx = UINT32_C(0x77777777);
    (void)gpop(c);
    c->esp += 4u;
}

/* sub_005ec358: memmove IAT thunk (cdecl dst, src, n). */
void sub_005ec358(CPU *__restrict c)
{
    uint32_t dst = ld32(c->esp + 4u), src = ld32(c->esp + 8u);
    uint32_t n = ld32(c->esp + 12u);
    ++s_stub_calls;
    trace("memmove dst=%08x src=%08x n=%u\n", dst, src, n);
    memmove((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, n);
    c->eax = dst;
    c->ecx = UINT32_C(0x88888888);
    c->edx = UINT32_C(0x99999999);
    (void)gpop(c);
}

/* sub_000188c0: the vector's storage swap (thiscall new buffer, size,
 * capacity; ret 0xc): begin/end/cap rewritten, the old storage released. */
void sub_000188c0(CPU *__restrict c)
{
    uint32_t vec = c->ecx, buf = ld32(c->esp + 4u), size = ld32(c->esp + 8u);
    uint32_t cap = ld32(c->esp + 12u);
    ++s_stub_calls;
    trace("vector-swap vec=%08x old=%08x buf=%08x size=%u cap=%u\n", vec,
          ld32(vec), buf, size, cap);
    st32(vec, buf);
    st32(vec + 4u, buf + 4u * size);
    st32(vec + 8u, buf + 4u * cap);
    c->eax = UINT32_C(0xaaaaaaaa);
    c->ecx = UINT32_C(0xbbbbbbbb);
    c->edx = UINT32_C(0xcccccccc);
    (void)gpop(c);
    c->esp += 12u;
}

/* sub_00010b40: std::_Xlength_error (never returns). */
void sub_00010b40(CPU *__restrict c)
{
    ++s_stub_calls;
    guest_fault(c, VA(0x00010b40u), "oracle vector length error");
}

/* sub_005ec14c: memcpy IAT thunk (cdecl dst, src, n). */
void sub_005ec14c(CPU *__restrict c)
{
    uint32_t dst = ld32(c->esp + 4u), src = ld32(c->esp + 8u);
    uint32_t n = ld32(c->esp + 12u);
    ++s_stub_calls;
    trace("memcpy dst=%08x src=%08x n=%u\n", dst, src, n);
    memmove((void *)(uintptr_t)dst, (const void *)(uintptr_t)src, n);
    c->eax = dst;
    c->ecx = UINT32_C(0xaaaaaaaa);
    c->edx = UINT32_C(0xbbbbbbbb);
    (void)gpop(c);
}

/* sub_0055e330: the unknown-format log (cdecl level, text). */
void sub_0055e330(CPU *__restrict c)
{
    ++s_stub_calls;
    trace("log level=%u text=%08x\n", ld32(c->esp + 4u), ld32(c->esp + 8u));
    c->eax = UINT32_C(0xcccccccc);
    c->ecx = UINT32_C(0xdddddddd);
    c->edx = UINT32_C(0xeeeeeeee);
    (void)gpop(c);
}

/* ---- registered guest functions (reached through guest_call) --------- */

#define ORACLE_GETTER_W_VA      UINT32_C(0x985a0b90)
#define ORACLE_GETTER_H_VA      UINT32_C(0x985a0ba0)
#define ORACLE_GETTER_WC_VA     UINT32_C(0x985a0bb0)   /* clobbering */
#define ORACLE_GETTER_HC_VA     UINT32_C(0x985a0bc0)
#define ORACLE_ALLOC_VA         UINT32_C(0x98500000)
#define ORACLE_FREE_VA          UINT32_C(0x98500010)
#define ORACLE_FLOOR_FOREIGN_VA UINT32_C(0x985a0bd0)   /* rebound IAT slot */
#define ORACLE_RENDERER_W_VA    UINT32_C(0x985a0be0)
#define ORACLE_RENDERER_H_VA    UINT32_C(0x985a0bf0)
#define ORACLE_SET_BLEND_VA     UINT32_C(0x985a0c00)

static void getter_trace(const char *which, const CPU *c)
{
    ++s_getter_calls;
    trace("getter %s this=%08x ret=%08x eax=%08x edx=%08x ebx=%08x esp=%08x "
          "ebp=%08x esi=%08x edi=%08x\n", which, c->ecx, ld32(c->esp),
          c->eax, c->edx, c->ebx, c->esp, c->ebp, c->esi, c->edi);
}

/* mov eax, [ecx+0x100]; ret  (the frozen GetWidth shape). */
static void oracle_get_width(CPU *__restrict c)
{
    getter_trace("w", c);
    c->eax = ld32(c->ecx + 0x100u);
    (void)gpop(c);
}

static void oracle_get_height(CPU *__restrict c)
{
    getter_trace("h", c);
    c->eax = ld32(c->ecx + 0x104u);
    (void)gpop(c);
}

/* A getter that also leaves EBX and EDX behind (EBX is never saved by the
 * body, EDX is caller-saved): the retirement must mirror both. */
static void oracle_get_width_clobber(CPU *__restrict c)
{
    getter_trace("wc", c);
    c->eax = ld32(c->ecx + 0x100u);
    c->ebx = UINT32_C(0xb1b1b1b1);
    c->edx = UINT32_C(0xd2d2d2d2);
    (void)gpop(c);
}

static void oracle_get_height_clobber(CPU *__restrict c)
{
    getter_trace("hc", c);
    c->eax = ld32(c->ecx + 0x104u);
    c->ebx = UINT32_C(0xb3b3b3b3);
    c->edx = UINT32_C(0xd4d4d4d4);
    (void)gpop(c);
}

/* The Renderer's own GetWidth/GetHeight (vtable +0x30/+0x34), which the
 * blend select calls when no render target is bound. */
static void oracle_renderer_get_width(CPU *__restrict c)
{
    getter_trace("rw", c);
    c->eax = s_arena.renderer_size[0];
    (void)gpop(c);
}

static void oracle_renderer_get_height(CPU *__restrict c)
{
    getter_trace("rh", c);
    c->eax = s_arena.renderer_size[1];
    (void)gpop(c);
}

/* Renderer vtable +0x38: the translated SetBlend (sub_00562030 stores the
 * four descriptor words at this+4..+0x10; ret 0x10), traced. */
static void oracle_set_blend(CPU *__restrict c)
{
    ++s_set_blends;
    trace("setblend this=%08x d=%08x,%08x,%08x,%08x ret=%08x\n", c->ecx,
          ld32(c->esp + 4u), ld32(c->esp + 8u), ld32(c->esp + 12u),
          ld32(c->esp + 16u), ld32(c->esp));
    sub_00562030(c);
}

/* Allocator methods the ring growth path calls: [vtable+0x20](size, 0, 1)
 * -> bump pointer, [vtable+0x24](ptr); both stdcall/thiscall. */
static void oracle_alloc(CPU *__restrict c)
{
    uint32_t size = ld32(c->esp + 4u);
    uint32_t at;
    ++s_alloc_calls;
    at = grow_take(c, 0x007e8170u, size);
    trace("alloc size=%u a=%u b=%u -> %08x\n", size, ld32(c->esp + 8u),
          ld32(c->esp + 12u), at);
    c->eax = at;
    c->ecx = UINT32_C(0x12121212);
    c->edx = UINT32_C(0x34343434);
    (void)gpop(c);
    c->esp += 12u;
}

static void oracle_free(CPU *__restrict c)
{
    ++s_alloc_calls;
    trace("free ptr=%08x\n", ld32(c->esp + 4u));
    c->eax = UINT32_C(0x56565656);
    c->ecx = UINT32_C(0x78787878);
    c->edx = UINT32_C(0x9a9a9a9a);
    (void)gpop(c);
    c->esp += 4u;
}

/* A translated function the floor IAT slot has been rebound to (a hooked
 * import): cdecl floor(double) through the x87 stack.  The fast path must
 * decline (FLOOR) and let the translated body reach it. */
static void oracle_floor_foreign(CPU *__restrict c)
{
    double v = ldd(c->esp + 4u);
    ++s_getter_calls;
    trace("stub floor-foreign arg=%016llx\n",
          (unsigned long long)ld64(c->esp + 4u));
    fpush(c, floor(v));
    (void)gpop(c);
}

static const uint32_t s_registered_addrs[10] = {
    ORACLE_ALLOC_VA, ORACLE_FREE_VA, ORACLE_GETTER_W_VA, ORACLE_GETTER_H_VA,
    ORACLE_GETTER_WC_VA, ORACLE_GETTER_HC_VA, ORACLE_FLOOR_FOREIGN_VA,
    ORACLE_RENDERER_W_VA, ORACLE_RENDERER_H_VA, ORACLE_SET_BLEND_VA,
};
static const guest_fn s_registered_fns[10] = {
    oracle_alloc, oracle_free, oracle_get_width, oracle_get_height,
    oracle_get_width_clobber, oracle_get_height_clobber, oracle_floor_foreign,
    oracle_renderer_get_width, oracle_renderer_get_height, oracle_set_blend,
};

/* ---- image pages ------------------------------------------------------ */

/* VirtualAlloc reserves in 64 KiB granules: one commit per granule. */
#define ORACLE_PAGE 0x10000u
static const uint32_t s_image_pages[] = {
    0x00560000u,   /* 561e58 jump table */
    0x00600000u,   /* floor IAT slot 606524 */
    0x00760000u,   /* 76a18c/76a214/76a480/76a8cc constants, 76b060/76b3b0/
                      76b3c0 blend descriptors, 76c940 table */
    0x007a0000u,   /* 7aa92c/7aa930 fallback size */
    0x007b0000u,   /* 7bf894 depth cursor */
    0x007c0000u,   /* 7c7a30 Renderer, 7c7a34.. blend, 7c7a44 group,
                      7c7a48.. cull/snap/premultiply globals */
    0x007e0000u,   /* 7e7d10.. dirty vector, 7e7d28.. pair vector and rebind
                      words, 7e8170 allocator pointer */
    0x00800000u,   /* 802fec/802ff0 unbatched reset floats */
};

static int commit_image_page(uint32_t base, uint32_t bytes)
{
    void *result = VirtualAlloc((LPVOID)(uintptr_t)base, bytes,
                                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!result || (uintptr_t)result > base)
        return 0;
    st32(base, 0x11223344u);
    st32(base + bytes - 4u, 0x55667788u);
    return ld32(base) == 0x11223344u && ld32(base + bytes - 4u) == 0x55667788u;
}

static int hex_nibble(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}

/* argv[1]: 256 hex digits = 128 frozen PE bytes in this order: the eight
 * .rdata words 0.0f (76a18c), 0.5f (76a480), 0.01f (76a214), 4.0f (76a8cc),
 * 480.0f (7aa92c), 270.0f (7aa930), 802fec, 802ff0; the 16-byte
 * u32->f64 table (76c940); the 32-byte 561e58 jump table exactly as the PE
 * stores it (image base 0x400000), relocated to GUEST_IMAGE_BASE the way the
 * loader applies the HIGHLOW fixups; the three 16-byte blend descriptors
 * the select compares and installs (76b060 default, 76b3b0 premultiplied
 * translucent, 76b3c0 translucent). */
#define ORACLE_PE_BASE UINT32_C(0x00400000)
#define ORACLE_BLOB_BYTES 128u
#define ORACLE_BLEND_DEFAULT_RVA ISAAC_VITA_KAGE_QUAD_C_BLEND_DEFAULT
#define ORACLE_BLEND_PREMULTIPLIED_RVA ISAAC_VITA_KAGE_QUAD_C_BLEND_PREMULTIPLIED
#define ORACLE_BLEND_TRANSLUCENT_RVA ISAAC_VITA_KAGE_QUAD_C_BLEND_TRANSLUCENT

static int install_constants(const char *hex)
{
    static const uint32_t word_rvas[8] = {
        ISAAC_VITA_KAGE_QUAD_C_ZERO, ISAAC_VITA_KAGE_QUAD_C_HALF,
        ISAAC_VITA_KAGE_QUAD_C_DEPTH_STEP, ISAAC_VITA_KAGE_QUAD_C_ALPHA_OPAQUE,
        ISAAC_VITA_KAGE_QUAD_C_FALLBACK_WIDTH,
        ISAAC_VITA_KAGE_QUAD_C_FALLBACK_HEIGHT,
        ISAAC_VITA_KAGE_QUAD_G_RESET_OFFSET_X,
        ISAAC_VITA_KAGE_QUAD_G_RESET_OFFSET_Y,
    };
    static const uint32_t blend_rvas[3] = {
        ORACLE_BLEND_DEFAULT_RVA, ORACLE_BLEND_PREMULTIPLIED_RVA,
        ORACLE_BLEND_TRANSLUCENT_RVA,
    };
    unsigned char blob[ORACLE_BLOB_BYTES];
    uint32_t i;

    if (strlen(hex) != ORACLE_BLOB_BYTES * 2u)
        return 0;
    for (i = 0u; i < ORACLE_BLOB_BYTES; ++i) {
        int hi = hex_nibble(hex[i * 2u]), lo = hex_nibble(hex[i * 2u + 1u]);
        if (hi < 0 || lo < 0)
            return 0;
        blob[i] = (unsigned char)((hi << 4) | lo);
    }
    for (i = 0u; i < 8u; ++i) {
        uint32_t word;
        memcpy(&word, blob + i * 4u, 4u);
        st32(VA(word_rvas[i]), word);
    }
    for (i = 0u; i < 16u; ++i)
        st8(VA(ISAAC_VITA_KAGE_QUAD_C_U32_TO_F64) + i, blob[32u + i]);
    for (i = 0u; i < 8u; ++i) {
        uint32_t word;
        memcpy(&word, blob + 48u + i * 4u, 4u);
        if (word - ORACLE_PE_BASE > UINT32_C(0x0085f000))
            return 0;
        st32(VA(0x00561e58u) + i * 4u, word - ORACLE_PE_BASE + GUEST_IMAGE_BASE);
    }
    for (i = 0u; i < 48u; ++i)
        st8(VA(blend_rvas[i / 16u]) + i % 16u, blob[80u + i]);
    /* Sanity: the pinned values. */
    return ldf(VA(ISAAC_VITA_KAGE_QUAD_C_ZERO)) == 0.0f &&
           ldf(VA(ISAAC_VITA_KAGE_QUAD_C_HALF)) == 0.5f &&
           ldf(VA(ISAAC_VITA_KAGE_QUAD_C_ALPHA_OPAQUE)) == 4.0f &&
           ldf(VA(ISAAC_VITA_KAGE_QUAD_C_FALLBACK_WIDTH)) == 480.0f &&
           ldf(VA(ISAAC_VITA_KAGE_QUAD_C_FALLBACK_HEIGHT)) == 270.0f &&
           ldd(VA(ISAAC_VITA_KAGE_QUAD_C_U32_TO_F64)) == 0.0 &&
           ldd(VA(ISAAC_VITA_KAGE_QUAD_C_U32_TO_F64) + 8u) == 4294967296.0 &&
           ld32(VA(0x00561e58u)) == VA(0x00561e2du) &&
           ld32(VA(ORACLE_BLEND_DEFAULT_RVA)) == 1u &&
           ld32(VA(ORACLE_BLEND_DEFAULT_RVA) + 4u) == 0u &&
           ld32(VA(ORACLE_BLEND_PREMULTIPLIED_RVA) + 4u) == 7u &&
           ld32(VA(ORACLE_BLEND_TRANSLUCENT_RVA)) == 6u;
}

typedef struct kq_case {
    const char *name;
    float quad[8];
    float uv[8];
    float colours[4][4];
    uint32_t count;
    uint8_t formats[ORACLE_MAX_FORMATS];
    uint32_t stride;            /* Image u16 stride */
    uint32_t velem, ielem;      /* ring element sizes */
    uint32_t vidx, iidx;        /* ring record index */
    uint32_t vcount, vcap, vbase, icount, icap, ibase;
    int fresh_v, fresh_i;       /* the image's batch records {0,0,0,0} */
    uint32_t flags;             /* [this+0x10] */
    uint32_t flags_shadow;      /* [this+0x14] */
    uint32_t group;             /* [this+0x20] batch group 0..2, 3 none */
    uint32_t snap_a, snap_b;    /* [this+0x24], [this+0x28] */
    float snap_scale, offx, offy, su, sv;
    uint8_t dirty_byte;         /* [this+0x71] */
    uint8_t unbatched_byte;     /* [this+0x70] */
    uint32_t dirty_used, dirty_cap;
    uint8_t cull_enable, size_override, snap_enable, premultiply;
    int target_mode;            /* 0 none, 1 getters, 2 clobbering getters,
                                   3 null vtable, 4 null width slot,
                                   5 null height slot */
    int32_t target_w, target_h;
    float depth;
    /* The blend select's world (unbatched images). */
    int batch_select;           /* 0 the image's batch, 1 batch2, 2 null */
    int group_ptr;              /* 0x7c7a44: 0/1/2 the three groups, 3 foreign */
    int blend_custom;           /* 0x7c7a34: 0 the default descriptor, 1 blend[] */
    uint32_t blend[4];
    int prev_batch;             /* 0x7e7d34: 0 null, 1 batch, 2 batch2, 3 foreign */
    int prev_image;             /* 0x7e7d38: 0 null, 1 the image, 2 prev_image */
    uint32_t pairs_used, pairs_cap;
    int32_t renderer_w, renderer_h;
    uint32_t b2_velem, b2_ielem;
    ring_record b2_vrec, b2_irec;
    int b2_vrecords_null, b2_irecords_null;
    int quad_null, uv_null, colour_null, formats_null, batch_null;
    int vrecords_null, irecords_null, vptr_null, iptr_null;
    int import_not_ready, slot_bad;
    uint32_t esp_offset;        /* from the floor; 0 = ceiling - 28 */
    int esp_below_floor, esp_straddle;
    int expect_handled;         /* 1 or 0 */
    int expect_reason;          /* decline reason when expect_handled == 0 */
} kq_case;

static uint32_t s_lcg;

static uint32_t lcg(void)
{
    s_lcg = s_lcg * 1664525u + 1013904223u;
    return s_lcg;
}

static float lcg_float(float lo, float hi)
{
    return lo + (hi - lo) * ((float)(lcg() >> 8) / 16777216.0f);
}

static float from_bits(uint32_t bits)
{
    float v;
    memcpy(&v, &bits, 4u);
    return v;
}

static uint32_t to_bits(float v)
{
    uint32_t bits;
    memcpy(&bits, &v, 4u);
    return bits;
}

static const uint32_t k_format_sizes[8] = { 4u, 8u, 12u, 16u, 12u, 16u, 8u, 4u };

static uint32_t layout_stride(const kq_case *k)
{
    uint32_t i, sum = 0u;
    for (i = 0u; i < k->count && i < ORACLE_MAX_FORMATS; ++i)
        if (k->formats[i] >= 1u && k->formats[i] <= 8u)
            sum += k_format_sizes[k->formats[i] - 1u];
    return sum;
}

/* The game shape: position, uv, colour (stride 36). */
static void case_init(kq_case *k, const char *name)
{
    uint32_t i;
    memset(k, 0, sizeof *k);
    k->name = name;
    k->quad[0] = 10.0f; k->quad[1] = 20.0f;
    k->quad[2] = 42.0f; k->quad[3] = 20.0f;
    k->quad[4] = 10.0f; k->quad[5] = 52.0f;
    k->quad[6] = 42.0f; k->quad[7] = 52.0f;
    for (i = 0u; i < 8u; ++i)
        k->uv[i] = 0.125f * (float)(i + 1u);
    for (i = 0u; i < 4u; ++i) {
        k->colours[i][0] = 0.25f * (float)(i + 1u);
        k->colours[i][1] = 0.5f;
        k->colours[i][2] = 0.75f;
        k->colours[i][3] = 1.0f - 0.125f * (float)i;
    }
    k->count = 3u;
    k->formats[0] = 5u; k->formats[1] = 7u; k->formats[2] = 6u;
    k->stride = 36u;
    k->velem = 36u;
    k->ielem = 2u;
    k->vcount = 8u; k->vcap = 64u; k->vbase = 4u;
    k->icount = 12u; k->icap = 48u; k->ibase = 0u;
    k->flags = 0x20u | 0x101u;
    k->flags_shadow = 0x5a5a5a5au;
    k->group = 0u;
    k->snap_a = 0u; k->snap_b = 0u;
    k->snap_scale = 2.0f;
    k->offx = 3.0f; k->offy = -7.5f;
    k->su = 0.5f; k->sv = 0.25f;
    k->dirty_byte = 0u;
    k->dirty_used = 2u; k->dirty_cap = ORACLE_DIRTY_SLOTS;
    k->cull_enable = 1u; k->snap_enable = 1u;
    k->target_mode = 1;
    k->target_w = 480; k->target_h = 270;
    k->depth = -0.5f;
    k->batch_select = 0;
    k->group_ptr = 1;                     /* not the image's group: rewritten */
    k->blend_custom = 0;                  /* the default descriptor: SetBlend */
    k->prev_batch = 3;                    /* foreign: the rebind runs */
    k->prev_image = 2;
    k->pairs_used = 2u; k->pairs_cap = ORACLE_PAIR_SLOTS;
    k->renderer_w = 640; k->renderer_h = 360;
    k->b2_velem = 36u; k->b2_ielem = 2u;  /* fresh records: {0,0,0,0} */
    k->expect_handled = 1;
    k->expect_reason = -1;
}

static void decline(kq_case *k, int reason)
{
    k->expect_handled = 0;
    k->expect_reason = reason;
}

/* The unbatched shape: flag 0x20 clear, the select runs. */
static void unbatch(kq_case *k)
{
    k->flags &= ~0x20u;
}

static void opaque(kq_case *k)
{
    uint32_t i;
    for (i = 0u; i < 4u; ++i)
        k->colours[i][3] = 1.0f;           /* alpha sum 4.0: bool 0 */
}

static void prepare(const kq_case *k, CPU *c, uint32_t *entry_esp_out)
{
    uint32_t floor = (uint32_t)(uintptr_t)&s_stack_storage[ORACLE_STACK_GUARD_WORDS];
    uint32_t ceiling = (uint32_t)(uintptr_t)
        &s_stack_storage[ORACLE_STACK_GUARD_WORDS + ORACLE_STACK_WORDS];
    uint32_t esp = k->esp_offset ? floor + k->esp_offset : ceiling - 28u;
    uint32_t i, word;
    uint32_t image = A(image[0]);
    uint32_t batch = A(batch[0]);
    uint32_t batch2 = A(batch2[0]);

    if (k->esp_below_floor)
        esp = floor - 4u;
    if (k->esp_straddle)
        esp = ceiling - 16u;
    memset(s_stack_storage, 0xcd, sizeof s_stack_storage);
    memset(&s_arena, 0, sizeof s_arena);
    memset(s_arena.vertices, 0xa5, sizeof s_arena.vertices);
    memset(s_arena.indices, 0x5a, sizeof s_arena.indices);
    for (i = 0u; i < sizeof s_arena.image; ++i)
        s_arena.image[i] = (uint8_t)(0x80u + i);
    for (i = 0u; i < sizeof s_arena.batch; ++i)
        s_arena.batch[i] = (uint8_t)(0x40u + i);
    for (i = 0u; i < sizeof s_arena.batch2; ++i)
        s_arena.batch2[i] = (uint8_t)(0xc0u + i);
    for (i = 0u; i < sizeof s_arena.prev_image; ++i)
        s_arena.prev_image[i] = (uint8_t)(0x20u + i);

    /* Image object. */
    stf(image + ISAAC_VITA_KAGE_QUAD_OFF_UV_SCALE_U, k->su);
    stf(image + ISAAC_VITA_KAGE_QUAD_OFF_UV_SCALE_V, k->sv);
    st32(image + ISAAC_VITA_KAGE_QUAD_OFF_FLAGS, k->flags);
    st32(image + ISAAC_VITA_KAGE_QUAD_OFF_FLAGS_SHADOW, k->flags_shadow);
    st32(image + 0x20u, k->group);
    st32(image + ISAAC_VITA_KAGE_QUAD_OFF_SNAP_WORD_A, k->snap_a);
    st32(image + ISAAC_VITA_KAGE_QUAD_OFF_SNAP_WORD_B, k->snap_b);
    st32(image + ISAAC_VITA_KAGE_QUAD_OFF_FORMATS,
         k->formats_null ? 0u : A(formats[0]));
    st8(image + ISAAC_VITA_KAGE_QUAD_OFF_FORMAT_COUNT, (uint8_t)k->count);
    st8(image + ISAAC_VITA_KAGE_QUAD_OFF_FORMAT_COUNT + 1u, 0x99u);
    st16(image + ISAAC_VITA_KAGE_QUAD_OFF_STRIDE, (uint16_t)k->stride);
    st32(image + ISAAC_VITA_KAGE_QUAD_OFF_BATCH, k->batch_null ? 0u : batch);
    stf(image + ISAAC_VITA_KAGE_QUAD_OFF_OFFSET_X, k->offx);
    stf(image + ISAAC_VITA_KAGE_QUAD_OFF_OFFSET_Y, k->offy);
    stf(image + ISAAC_VITA_KAGE_QUAD_OFF_SNAP_SCALE, k->snap_scale);
    st8(image + ISAAC_VITA_KAGE_QUAD_OFF_BLEND_RESET, k->unbatched_byte);
    st8(image + ISAAC_VITA_KAGE_QUAD_OFF_DIRTY, k->dirty_byte);
    for (i = 0u; i < ORACLE_MAX_FORMATS; ++i)
        s_arena.formats[i] = k->formats[i];

    /* Batch: vertex ring at +0x20, index ring at +0x2c. */
    st32(batch + 0x20u, k->vrecords_null ? 0u : A(vrecs[0]));
    st32(batch + 0x24u, k->vidx);
    st32(batch + 0x28u, k->velem);
    st32(batch + 0x2cu, k->irecords_null ? 0u : A(irecs[0]));
    st32(batch + 0x30u, k->iidx);
    st32(batch + 0x34u, k->ielem);
    for (i = 0u; i < 2u; ++i) {
        s_arena.vrecs[i].ptr = 0xdead0000u + i;
        s_arena.vrecs[i].count = 0x7e00u + i;
        s_arena.vrecs[i].capacity = 0x7e00u + i;
        s_arena.vrecs[i].base = 0x7e00u + i;
        s_arena.irecs[i] = s_arena.vrecs[i];
        s_arena.vrecs2[i] = s_arena.vrecs[i];
        s_arena.irecs2[i] = s_arena.vrecs[i];
    }
    /* The ring writes land at ptr + count * element.  A growth copies the
       count * element bytes below them, so the buffer starts at the arena
       when they fit; the huge counts (u16 base wrap cases, which never grow)
       back the pointer up so the writes still land inside the arena. */
    s_arena.vrecs[k->vidx & 1u].ptr = k->vptr_null ? 0u :
        (k->vcount + 4u) * k->velem <= ORACLE_VERTEX_BYTES ? A(vertices[0]) :
        A(vertices[0]) - k->vcount * k->velem;
    s_arena.vrecs[k->vidx & 1u].count = k->vcount;
    s_arena.vrecs[k->vidx & 1u].capacity = k->vcap;
    s_arena.vrecs[k->vidx & 1u].base = k->vbase;
    s_arena.irecs[k->iidx & 1u].ptr = k->iptr_null ? 0u :
        (k->icount + 6u) * k->ielem <= sizeof s_arena.indices ? A(indices[0]) :
        A(indices[0]) - k->icount * k->ielem;
    s_arena.irecs[k->iidx & 1u].count = k->icount;
    s_arena.irecs[k->iidx & 1u].capacity = k->icap;
    s_arena.irecs[k->iidx & 1u].base = k->ibase;
    if (k->fresh_v)
        memset(&s_arena.vrecs[k->vidx & 1u], 0, sizeof(ring_record));
    if (k->fresh_i)
        memset(&s_arena.irecs[k->iidx & 1u], 0, sizeof(ring_record));

    /* The batch the select may return: record index 0 of each ring. */
    st32(batch2 + 0x20u, k->b2_vrecords_null ? 0u : A(vrecs2[0]));
    st32(batch2 + 0x24u, 0u);
    st32(batch2 + 0x28u, k->b2_velem);
    st32(batch2 + 0x2cu, k->b2_irecords_null ? 0u : A(irecs2[0]));
    st32(batch2 + 0x30u, 0u);
    st32(batch2 + 0x34u, k->b2_ielem);
    s_arena.vrecs2[0] = k->b2_vrec;
    s_arena.irecs2[0] = k->b2_irec;
    s_batch_select = k->batch_select;

    memcpy(s_arena.quad, k->quad, sizeof k->quad);
    memcpy(s_arena.uv, k->uv, sizeof k->uv);
    s_arena.uv[8] = 1.0f;
    memcpy(s_arena.colours, k->colours, sizeof k->colours);
    for (i = 0u; i < ORACLE_DIRTY_SLOTS; ++i)
        s_arena.dirty[i] = 0xd1000000u + i;
    for (i = 0u; i < 2u * ORACLE_PAIR_SLOTS; ++i)
        s_arena.pairs[i] = 0xa1000000u + i;

    /* Render target and its vtable. */
    s_arena.target[0] = k->target_mode == 3 ? 0u : A(target_vtable[0]);
    for (i = 1u; i < 0x100u / 4u; ++i)
        s_arena.target[i] = 0x7a000000u + i;
    s_arena.target[0x100u / 4u] = (uint32_t)k->target_w;
    s_arena.target[0x104u / 4u] = (uint32_t)k->target_h;
    for (i = 0u; i < 16u; ++i)
        s_arena.target_vtable[i] = 0x7b000000u + i;
    s_arena.target_vtable[0x28u / 4u] =
        k->target_mode == 2 ? ORACLE_GETTER_WC_VA :
        k->target_mode == 4 ? 0u : ORACLE_GETTER_W_VA;
    s_arena.target_vtable[0x2cu / 4u] =
        k->target_mode == 2 ? ORACLE_GETTER_HC_VA :
        k->target_mode == 5 ? 0u : ORACLE_GETTER_H_VA;

    /* Allocator (ring growth) and its vtable. */
    for (i = 0u; i < 16u; ++i)
        s_arena.allocator_vtable[i] = 0x7c000000u + i;
    s_arena.allocator_vtable[0x20u / 4u] = ORACLE_ALLOC_VA;
    s_arena.allocator_vtable[0x24u / 4u] = ORACLE_FREE_VA;
    s_grow_used = 0u;
    memset(s_grow_arena, 0x3c, sizeof s_grow_arena);

    /* The Renderer at 0x7c7a30: vtable, then the current blend descriptor
       (0x7c7a34..0x7c7a43), then the batch-group pointer (0x7c7a44). */
    for (i = 0u; i < 16u; ++i)
        s_arena.renderer_vtable[i] = 0x7d000000u + i;
    s_arena.renderer_vtable[0x30u / 4u] = ORACLE_RENDERER_W_VA;
    s_arena.renderer_vtable[0x34u / 4u] = ORACLE_RENDERER_H_VA;
    s_arena.renderer_vtable[0x38u / 4u] = ORACLE_SET_BLEND_VA;
    s_arena.renderer_size[0] = (uint32_t)k->renderer_w;
    s_arena.renderer_size[1] = (uint32_t)k->renderer_h;
    st32(VA(ISAAC_VITA_KAGE_QUAD_G_RENDERER), A(renderer_vtable[0]));
    for (i = 0u; i < 4u; ++i)
        st32(VA(ISAAC_VITA_KAGE_QUAD_G_RENDERER) + 4u + 4u * i,
             k->blend_custom ? k->blend[i] :
             ld32(VA(ORACLE_BLEND_DEFAULT_RVA) + 4u * i));
    st32(VA(0x007c7a44u),
         k->group_ptr == 0 ? VA(0x007c7a70u) :
         k->group_ptr == 1 ? VA(0x007c7a9cu) :
         k->group_ptr == 2 ? VA(0x007c7ac8u) : VA(0x007c7b00u));
    /* The (batch, image) pair vector and the rebind words. */
    st32(VA(0x007e7d28u), A(pairs[0]));
    st32(VA(0x007e7d2cu), A(pairs[0]) + 8u * k->pairs_used);
    st32(VA(0x007e7d30u), A(pairs[0]) + 8u * k->pairs_cap);
    st32(VA(0x007e7d34u),
         k->prev_batch == 0 ? 0u : k->prev_batch == 1 ? batch :
         k->prev_batch == 2 ? batch2 : 0x5a5a5a5au);
    st32(VA(0x007e7d38u),
         k->prev_image == 0 ? 0u : k->prev_image == 1 ? image :
         A(prev_image[0]));

    /* Globals. */
    st32(VA(ISAAC_VITA_KAGE_QUAD_G_CULL_TARGET),
         k->target_mode ? A(target[0]) : 0u);
    st8(VA(ISAAC_VITA_KAGE_QUAD_G_SIZE_OVERRIDE), k->size_override);
    st8(VA(ISAAC_VITA_KAGE_QUAD_G_CULL_ENABLE), k->cull_enable);
    st8(VA(ISAAC_VITA_KAGE_QUAD_G_SNAP_ENABLE), k->snap_enable);
    st8(VA(ISAAC_VITA_KAGE_QUAD_G_PREMULTIPLY), k->premultiply);
    stf(VA(ISAAC_VITA_KAGE_QUAD_G_DEPTH), k->depth);
    st32(VA(ISAAC_VITA_KAGE_QUAD_G_DIRTY_VECTOR), A(dirty[0]));
    st32(VA(ISAAC_VITA_KAGE_QUAD_G_DIRTY_END), A(dirty[0]) + 4u * k->dirty_used);
    st32(VA(ISAAC_VITA_KAGE_QUAD_G_DIRTY_CAP), A(dirty[0]) + 4u * k->dirty_cap);
    /* The allocator object lives at 7e8170 itself: its first word is the
       vtable the ring growth calls through ([vtable+0x20] alloc,
       [vtable+0x24] free) with ecx = 7e8170. */
    st32(VA(0x007e8170u), A(allocator_vtable[0]));
    st32(VA(ISAAC_VITA_KAGE_QUAD_FLOOR_IAT_RVA),
         k->slot_bad ? ORACLE_FLOOR_FOREIGN_VA :
         VA(ISAAC_VITA_KAGE_QUAD_FLOOR_IAT_RVA));
    g_isaac_vita_import_ids_ready = !k->import_not_ready;

    /* CPU: ECX = this, the six stdcall arguments above the return word. */
    c->eax = 0x11110000u;
    c->ecx = image;
    c->edx = 0x33330000u;
    c->ebx = 0x44440000u;
    c->esp = esp;
    c->ebp = 0x55550000u;
    c->esi = 0x66660000u;
    c->edi = 0x77770000u;
    c->st_top = 0;
    c->fsw = 0;
    memset(c->st, 0, sizeof c->st);
    memset(c->x, 0, sizeof c->x);
    c->fault = NULL;
    c->fault_addr = 0u;
    c->exit_api = NULL;
    c->exit_code = 0;
    c->stop_kind = 0;
    c->stack_low_water = ceiling;
    for (word = 0u; word < 7u; ++word) {
        uint32_t value =
            word == 0u ? ORACLE_RETURN_RVA :
            word == 1u ? (k->uv_null ? 0u : A(uv[0])) :
            word == 2u ? (k->quad_null ? 0u : A(quad[0])) :
            (k->colour_null ? 0u : A(colours[word - 3u][0]));
        st32(esp + word * 4u, value);
    }
    *entry_esp_out = esp;
}

typedef struct census {
    unsigned calls, imports, floor_slot, floor_imports, floor_notes;
    unsigned coverage, handled, rejected, stubs, getters, allocs;
    unsigned selects, vector_allocs, set_blends;
} census;

static void census_take(census *out)
{
    out->calls = g_guest_phase_profile_counters.guest_calls;
    out->imports = g_host_import_calls;
    out->floor_slot = g_guest_phase_profile_import_calls[
        ISAAC_VITA_KAGE_QUAD_FLOOR_IMPORT_ID];
    out->floor_imports = s_floor_imports;
    out->floor_notes = s_floor_notes;
    out->coverage = s_coverage_imports[ISAAC_VITA_KAGE_QUAD_FLOOR_IMPORT_ID];
    out->handled = s_handled;
    out->rejected = s_rejected;
    out->stubs = s_stub_calls;
    out->getters = s_getter_calls;
    out->allocs = s_alloc_calls;
    out->selects = s_batch_selects;
    out->vector_allocs = s_vector_allocs;
    out->set_blends = s_set_blends;
}

static void census_delta(const census *before, census *after)
{
    after->calls -= before->calls;
    after->imports -= before->imports;
    after->floor_slot -= before->floor_slot;
    after->floor_imports -= before->floor_imports;
    after->floor_notes -= before->floor_notes;
    after->coverage -= before->coverage;
    after->handled -= before->handled;
    after->rejected -= before->rejected;
    after->stubs -= before->stubs;
    after->getters -= before->getters;
    after->allocs -= before->allocs;
    after->selects -= before->selects;
    after->vector_allocs -= before->vector_allocs;
    after->set_blends -= before->set_blends;
}

static void describe(char *out, size_t cap, const CPU *c, uint32_t entry_esp,
                     int stop)
{
    size_t n = 0u;
    const unsigned char *bytes;
    size_t i;

    n += (size_t)snprintf(out + n, cap - n,
        "eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%+d ebp=%08x esi=%08x "
        "edi=%08x stop=%d st_top=%d fsw=%04x low=%u fault=%s addr=%08x ",
        c->eax, c->ecx, c->edx, c->ebx, (int)(c->esp - entry_esp), c->ebp,
        c->esi, c->edi, stop, (int)c->st_top, (unsigned)c->fsw,
        (unsigned)(c->stack_ceiling - c->stack_low_water),
        c->fault ? c->fault : "-", c->fault_addr);
    n += (size_t)snprintf(out + n, cap - n, "frame=");
    for (i = 0u; i < 7u; ++i)
        n += (size_t)snprintf(out + n, cap - n, "%08x,",
                              ld32(entry_esp + (uint32_t)i * 4u));
    n += (size_t)snprintf(out + n, cap - n,
        " depth=%08x dirty(v,e,c)=%08x,%08x,%08x globals=%08x,%08x "
        "slot=%08x alloc=%08x grow=%u ready=%d renderer=%08x "
        "blend=%08x,%08x,%08x,%08x group=%08x pairs(b,e,c)=%08x,%08x,%08x "
        "prev(b,i)=%08x,%08x arena=",
        ld32(VA(ISAAC_VITA_KAGE_QUAD_G_DEPTH)),
        ld32(VA(ISAAC_VITA_KAGE_QUAD_G_DIRTY_VECTOR)),
        ld32(VA(ISAAC_VITA_KAGE_QUAD_G_DIRTY_END)),
        ld32(VA(ISAAC_VITA_KAGE_QUAD_G_DIRTY_CAP)),
        ld32(VA(ISAAC_VITA_KAGE_QUAD_G_CULL_TARGET)),
        ld32(VA(ISAAC_VITA_KAGE_QUAD_G_SIZE_OVERRIDE)),
        ld32(VA(ISAAC_VITA_KAGE_QUAD_FLOOR_IAT_RVA)),
        ld32(VA(0x007e8170u)), s_grow_used, g_isaac_vita_import_ids_ready,
        ld32(VA(ISAAC_VITA_KAGE_QUAD_G_RENDERER)),
        ld32(VA(ISAAC_VITA_KAGE_QUAD_G_RENDERER) + 4u),
        ld32(VA(ISAAC_VITA_KAGE_QUAD_G_RENDERER) + 8u),
        ld32(VA(ISAAC_VITA_KAGE_QUAD_G_RENDERER) + 12u),
        ld32(VA(ISAAC_VITA_KAGE_QUAD_G_RENDERER) + 16u),
        ld32(VA(0x007c7a44u)), ld32(VA(0x007e7d28u)), ld32(VA(0x007e7d2cu)),
        ld32(VA(0x007e7d30u)), ld32(VA(0x007e7d34u)), ld32(VA(0x007e7d38u)));
    bytes = (const unsigned char *)&s_arena;
    for (i = 0u; i < sizeof s_arena && n + 3u < cap; ++i)
        n += (size_t)snprintf(out + n, cap - n, "%02x", bytes[i]);
    n += (size_t)snprintf(out + n, cap - n, " grown=");
    bytes = s_grow_arena;
    for (i = 0u; i < s_grow_used && n + 3u < cap; ++i)
        n += (size_t)snprintf(out + n, cap - n, "%02x", bytes[i]);
}

static guest_fn s_body;

static void oracle_entry(CPU *__restrict c)
{
    s_body(c);
}

static int run_variant(const kq_case *k, guest_fn body, char *trace_out,
                       size_t trace_cap, char *state_out, size_t state_cap,
                       census *delta)
{
    uint32_t entry_esp;
    census before;
    int stop;

    prepare(k, &s_cpu, &entry_esp);
    s_coverage_imports[ISAAC_VITA_KAGE_QUAD_FLOOR_IMPORT_ID] = 0u;
    trace_reset();
    census_take(&before);
    s_body = body;
    if (getenv("KQ_ORACLE_DEBUG")) {
        fprintf(stderr, "run %s body=%p\n", k->name, (void *)body);
        fflush(stderr);
    }
    stop = guest_run_until_stop(&s_cpu, oracle_entry);
    census_take(delta);
    census_delta(&before, delta);
    describe(state_out, state_cap, &s_cpu, entry_esp, stop);
    if (getenv("KQ_ORACLE_DUMP") && strcmp(getenv("KQ_ORACLE_DUMP"), k->name) == 0) {
        fprintf(stderr, "--- %s state\n%s--- trace\n%s", k->name, state_out,
                s_trace);
        fflush(stderr);
    }
    s_cpu.fault = NULL;
    s_cpu.fault_addr = 0u;
    if (s_trace_overflow || s_trace_len + 1u > trace_cap)
        return 0;
    memcpy(trace_out, s_trace, s_trace_len + 1u);
    return 1;
}

static char s_ref_trace[sizeof s_trace];
static char s_seam_trace[sizeof s_trace];
static char s_ref_state[1u << 17];
static char s_seam_state[1u << 17];

static unsigned s_cases;
static unsigned s_failures;
static unsigned s_total_handled;
static unsigned s_total_declined;
static unsigned s_total_culled;
static unsigned s_total_snapped;
static unsigned s_total_getters;
static unsigned s_total_floors;
static unsigned s_total_unbatched;
static unsigned s_total_ring_growths;
static unsigned s_total_dirty_growths;
static unsigned s_probes;
static unsigned s_faults;

static void fail(const char *name, const char *tag, const char *why)
{
    ++s_failures;
    printf("FAIL %s [%s]: %s\n", name, tag, why);
}

static void strip_lines(char *buffer, const char *prefix)
{
    size_t plen = strlen(prefix);
    char *read = buffer, *write = buffer;

    while (*read) {
        char *eol = strchr(read, '\n');
        size_t len = eol ? (size_t)(eol - read) + 1u : strlen(read);
        if (strncmp(read, prefix, plen) != 0) {
            memmove(write, read, len);
            write += len;
        }
        read += len;
    }
    *write = '\0';
}

/* Whether the run that just finished reserved (was not culled): the index
 * record of the batch the quad ends in advanced by six. */
static int reserved(const kq_case *k)
{
    int unbatched_case = (k->flags & 0x20u) == 0u;
    const ring_record *rec;
    uint32_t before;

    if (unbatched_case && k->batch_select == 1) {
        rec = &s_arena.irecs2[0];
        before = k->b2_irec.count;
    } else {
        rec = &s_arena.irecs[k->iidx & 1u];
        before = k->fresh_i ? 0u : k->icount;
    }
    return rec->count != before;
}

static void compare(const kq_case *k, const char *tag, guest_fn ref,
                    guest_fn seam)
{
    census ref_census, seam_census;
    IsaacVitaKageQuadCounters counters;
    unsigned reasons = 0u, i, select_ran, body_getters;
    int unbatched_case = (k->flags & 0x20u) == 0u;
    int ref_reserved;

    isaac_vita_kage_quad_fastpath_take(NULL);
    if (!run_variant(k, ref, s_ref_trace, sizeof s_ref_trace, s_ref_state,
                     sizeof s_ref_state, &ref_census)) {
        fail(k->name, tag, "reference trace overflow");
        return;
    }
    ref_reserved = reserved(k);
    isaac_vita_kage_quad_fastpath_take(&counters);
    if (ref_census.handled || ref_census.rejected || counters.calls) {
        fail(k->name, tag, "reference body reached the fast path");
        return;
    }
    if (!run_variant(k, seam, s_seam_trace, sizeof s_seam_trace, s_seam_state,
                     sizeof s_seam_state, &seam_census)) {
        fail(k->name, tag, "seamed trace overflow");
        return;
    }
    isaac_vita_kage_quad_fastpath_take(&counters);
    ++s_cases;
    s_total_handled += counters.handled;
    s_total_declined += counters.calls - counters.handled;
    s_total_culled += counters.culled;
    s_total_snapped += counters.snapped;
    s_total_getters += counters.getters;
    s_total_floors += ref_census.floor_imports;
    s_total_unbatched += counters.unbatched;
    s_total_ring_growths += counters.ring_growths;
    s_total_dirty_growths += counters.dirty_growths;
    printf("case %-32s %-3s verdict=%u/%u calls=%u,%u imp=%u,%u fl=%u,%u "
           "get=%u,%u stubs=%u,%u cull=%u snap=%u unb=%u,%u grow=%u,%u,%u\n",
           k->name, tag, seam_census.handled, seam_census.rejected,
           ref_census.calls, seam_census.calls, ref_census.imports,
           seam_census.imports, ref_census.floor_imports,
           seam_census.floor_notes, ref_census.getters, seam_census.getters,
           ref_census.stubs, seam_census.stubs, counters.culled,
           counters.snapped, counters.unbatched, counters.translucent,
           counters.fresh_records, counters.ring_growths,
           counters.dirty_growths);
    strip_lines(s_seam_trace, "fastpath ");
    if (strcmp(s_ref_trace, s_seam_trace) != 0) {
        fail(k->name, tag, "call/import trace differs");
        printf("--- reference\n%s--- seamed\n%s", s_ref_trace, s_seam_trace);
    }
    if (strcmp(s_ref_state, s_seam_state) != 0) {
        fail(k->name, tag, "CPU/frame/memory state differs");
        printf("--- reference\n%.900s\n--- seamed\n%.900s\n", s_ref_state,
               s_seam_state);
    }
#if defined(ISAAC_VITA_KAGE_QUAD_FASTPATH_OBSERVE)
    /* OBSERVE: the checks run and count, the verdict is always REJECTED and
     * the translated body runs (trace/state identical by construction). */
    if (seam_census.handled != 0u || seam_census.rejected != 1u ||
        counters.calls != 1u || counters.handled != (unsigned)k->expect_handled)
        fail(k->name, tag, "OBSERVE verdict differs from the expectation");
    if (counters.mode != 2u)
        fail(k->name, tag, "counters report a mode other than OBSERVE");
#else
    if (seam_census.handled != (unsigned)k->expect_handled ||
        seam_census.rejected != (unsigned)!k->expect_handled ||
        counters.calls != 1u || counters.handled != (unsigned)k->expect_handled)
        fail(k->name, tag, "fast-path verdict differs from the expectation");
    if (counters.mode != 1u)
        fail(k->name, tag, "counters report a mode other than ON");
#endif
    for (i = 0u; i < ISAAC_VITA_KAGE_QUAD_DECLINE_REASONS; ++i)
        reasons += counters.declined[i];
    if (k->expect_handled ? reasons != 0u :
        (reasons != 1u || counters.declined[k->expect_reason] != 1u))
        fail(k->name, tag, "decline reason differs from the expectation");
    /* Receipts: every census advances identically.  The reference floor
     * route is one guest_call + one host import + one census slot + the
     * coverage byte per floor; the replay writes exactly those.  Getters,
     * the allocator methods and SetBlend go through guest_call on both
     * sides; the stand-ins are direct callees on both sides. */
    if (seam_census.calls != ref_census.calls ||
        seam_census.imports != ref_census.imports ||
        seam_census.floor_slot != ref_census.floor_slot ||
        seam_census.coverage != ref_census.coverage ||
        seam_census.getters != ref_census.getters ||
        seam_census.stubs != ref_census.stubs ||
        seam_census.allocs != ref_census.allocs ||
        seam_census.selects != ref_census.selects ||
        seam_census.vector_allocs != ref_census.vector_allocs ||
        seam_census.set_blends != ref_census.set_blends)
        fail(k->name, tag, "censuses differ between reference and replay");
    /* The select ran (on both sides) iff the stand-in it always reaches
     * traced; its two getters are not the body's. */
    select_ran = ref_census.selects != 0u;
    body_getters = ref_census.getters - (select_ran ? 2u : 0u);
    if (k->expect_handled) {
        if (ref_census.floor_slot != ref_census.floor_imports ||
            ref_census.imports != ref_census.floor_imports ||
            ref_census.calls != ref_census.floor_imports + ref_census.getters +
                                ref_census.allocs + ref_census.set_blends ||
            ref_census.coverage != (ref_census.floor_imports ? 1u : 0u) ||
            ref_census.selects > 1u || ref_census.vector_allocs > 1u ||
            ref_census.set_blends > 2u ||
            (select_ran && !unbatched_case) ||
            (body_getters != 0u && body_getters != 2u) ||
            (ref_census.floor_imports != 0u && ref_census.floor_imports != 4u))
            fail(k->name, tag, "reference receipts differ from the design");
        if (counters.translucent > counters.unbatched ||
            counters.fresh_records > 2u || counters.ring_growths > 2u ||
            counters.dirty_growths > 1u ||
            (counters.getters != 0u) != (body_getters == 2u) ||
            (counters.constant_cull != 0u) !=
            (k->cull_enable && body_getters == 0u))
            fail(k->name, tag, "path counters differ from the design");
#if defined(ISAAC_VITA_KAGE_QUAD_FASTPATH_OBSERVE)
        /* snap/get/b/t count the candidates before the cull; f/r are the
         * entry snapshot (batched images), d the prediction of the select's
         * [this+0x71] write: exact whenever the reference reserved (was not
         * culled). */
        if (seam_census.floor_notes != 0u ||
            seam_census.floor_imports != ref_census.floor_imports ||
            counters.culled != 0u ||
            ref_census.floor_imports > 4u * counters.snapped ||
            counters.unbatched != (unsigned)unbatched_case ||
            (ref_reserved &&
             (counters.dirty_growths != 0u) != (ref_census.vector_allocs != 0u)) ||
            (ref_reserved && !unbatched_case &&
             (counters.ring_growths != 0u) != (ref_census.allocs != 0u)) ||
            (!ref_reserved && (ref_census.allocs != 0u ||
                               ref_census.vector_allocs != 0u)))
            fail(k->name, tag, "OBSERVE receipts differ from the design");
#else
        if (seam_census.floor_imports != 0u ||
            seam_census.floor_notes != ref_census.floor_imports ||
            ref_census.floor_imports != 4u * counters.snapped ||
            counters.unbatched != select_ran ||
            (counters.culled != 0u) == ref_reserved ||
            (counters.ring_growths != 0u) != (ref_census.allocs != 0u) ||
            (counters.dirty_growths != 0u) != (ref_census.vector_allocs != 0u) ||
            /* The cull exit retires eax = 0 (a vertex pointer of 0 - element
               size 0 - retires it too, so only this direction holds). */
            (counters.culled != 0u &&
             strncmp(s_seam_state, "eax=00000000 ", 13u) != 0) ||
            (counters.culled != 0u && (counters.snapped != 0u ||
                                       counters.unbatched != 0u)))
            fail(k->name, tag, "handled replay receipts differ from the design");
#endif
    } else if (seam_census.floor_notes != 0u ||
               seam_census.floor_imports != ref_census.floor_imports ||
               counters.culled || counters.snapped || counters.getters ||
               counters.constant_cull || counters.unbatched ||
               counters.translucent || counters.fresh_records ||
               counters.ring_growths || counters.dirty_growths) {
        fail(k->name, tag, "declined replay left a receipt behind");
    }
}

static void compare_both(const kq_case *k)
{
    compare(k, "gpr", ref_sub_0055f990, sub_0055f990);
    compare(k, "mem", mem_ref_sub_0055f990, mem_sub_0055f990);
}

/* Inputs the translated body cannot execute (it would dereference a null
 * pointer): the fast path alone must decline without touching anything. */
static void probe_rejects(const kq_case *k)
{
    uint32_t entry_esp;
    IsaacVitaKageQuadCounters counters;
    census before, after;
    int verdict;

    prepare(k, &s_cpu, &entry_esp);
    s_coverage_imports[ISAAC_VITA_KAGE_QUAD_FLOOR_IMPORT_ID] = 0u;
    describe(s_ref_state, sizeof s_ref_state, &s_cpu, entry_esp, 0);
    isaac_vita_kage_quad_fastpath_take(NULL);
    census_take(&before);
    trace_reset();
    verdict = oracle_kage_quad_impl(&s_cpu);
    census_take(&after);
    census_delta(&before, &after);
    isaac_vita_kage_quad_fastpath_take(&counters);
    describe(s_seam_state, sizeof s_seam_state, &s_cpu, entry_esp, 0);
    ++s_probes;
    printf("probe %-32s verdict=%d reason=%d\n", k->name, verdict,
           k->expect_reason);
    if (verdict != 0 || counters.calls != 1u || counters.handled != 0u ||
        counters.declined[k->expect_reason] != 1u)
        fail(k->name, "probe", "fast path did not decline as expected");
    if (strcmp(s_ref_state, s_seam_state) != 0 || s_trace_len != 0u ||
        after.calls || after.imports || after.floor_notes || after.coverage ||
        after.stubs)
        fail(k->name, "probe", "declined probe changed state");
}

/* States only the batch select produces (a null or hollow batch): the
 * translated body would dereference a null pointer after the select, the
 * replay must fault with its message instead (after the select's own
 * effects, which the x86 also performs).  OBSERVE never enters the replay:
 * its checks pass and nothing changes. */
static int s_probe_verdict;

static void oracle_impl_entry(CPU *__restrict c)
{
    s_probe_verdict = oracle_kage_quad_impl(c);
}

static void probe_fault(const kq_case *k, const char *expected)
{
    uint32_t entry_esp;
    IsaacVitaKageQuadCounters counters;
    census before, after;
    int stop;

    prepare(k, &s_cpu, &entry_esp);
    s_coverage_imports[ISAAC_VITA_KAGE_QUAD_FLOOR_IMPORT_ID] = 0u;
    describe(s_ref_state, sizeof s_ref_state, &s_cpu, entry_esp, 0);
    isaac_vita_kage_quad_fastpath_take(NULL);
    census_take(&before);
    trace_reset();
    s_probe_verdict = -1;
    stop = guest_run_until_stop(&s_cpu, oracle_impl_entry);
    census_take(&after);
    census_delta(&before, &after);
    isaac_vita_kage_quad_fastpath_take(&counters);
    ++s_faults;
    printf("fault %-32s stop=%d verdict=%d fault=%s\n", k->name, stop,
           s_probe_verdict, s_cpu.fault ? s_cpu.fault : "-");
    if (counters.calls != 1u || counters.handled != 1u)
        fail(k->name, "fault", "fault probe did not pass the checks");
#if defined(ISAAC_VITA_KAGE_QUAD_FASTPATH_OBSERVE)
    describe(s_seam_state, sizeof s_seam_state, &s_cpu, entry_esp, 0);
    if (s_probe_verdict != 0 || s_cpu.fault != NULL || s_trace_len != 0u ||
        after.calls || after.imports || after.stubs ||
        strcmp(s_ref_state, s_seam_state) != 0)
        fail(k->name, "fault", "OBSERVE probe changed state");
#else
    if (s_probe_verdict != -1 || s_cpu.fault == NULL ||
        strstr(s_cpu.fault, expected) == NULL ||
        s_cpu.fault_addr != VA(ISAAC_VITA_KAGE_QUAD_ROOT_RVA) ||
        after.selects != 1u)
        fail(k->name, "fault", "replay did not fault as expected");
#endif
    (void)expected;
    s_cpu.fault = NULL;
    s_cpu.fault_addr = 0u;
}

/* ---- case generation ----------------------------------------------------- */

static const uint8_t k_layouts[8][17] = {
    { 5, 7, 6 },                    /* game shape, stride 36 */
    { 5 },                          /* position only */
    { 7 },                          /* uv only */
    { 6 },                          /* colour only */
    { 5, 6, 7, 1, 2, 3, 4, 8 },     /* every format once */
    { 6, 6, 5, 7, 7, 8 },
    { 1, 5, 2, 7, 3, 6, 4, 8, 5, 7, 6, 1, 2, 3, 4, 8 },  /* sixteen */
    { 8, 8, 8, 8, 5, 8, 8, 8, 7, 8, 8, 8, 6, 8, 8 },
};
static const uint32_t k_layout_counts[8] = { 3u, 1u, 1u, 1u, 8u, 6u, 16u, 15u };

static void random_case(kq_case *k, const char *name, uint32_t seed)
{
    uint32_t i, layout, roll;

    case_init(k, name);
    s_lcg = seed * 2654435761u + 0x9e3779b9u;
    for (i = 0u; i < 8u; ++i)
        k->quad[i] = lcg_float(-120.0f, 620.0f);
    roll = lcg() % 4u;
    if (roll == 0u)
        k->quad[4] = k->quad[0];             /* snap candidate: x0 == x2 */
    if (roll == 1u) {
        /* A rectangle like the game emits: snaps, and most of it visible. */
        float x = lcg_float(-20.0f, 460.0f), y = lcg_float(-20.0f, 260.0f);
        float w = lcg_float(1.0f, 80.0f), h = lcg_float(1.0f, 80.0f);
        k->quad[0] = x; k->quad[1] = y; k->quad[2] = x + w; k->quad[3] = y;
        k->quad[4] = x; k->quad[5] = y + h; k->quad[6] = x + w; k->quad[7] = y + h;
    }
    roll = lcg() % 16u;
    if (roll == 0u)
        k->quad[lcg() % 8u] = from_bits(0x7fc00000u);            /* NaN */
    else if (roll == 1u)
        k->quad[lcg() % 8u] = from_bits(0x7f800000u);            /* +inf */
    else if (roll == 2u)
        k->quad[lcg() % 8u] = from_bits(0xff800000u);            /* -inf */
    else if (roll == 3u)
        k->quad[lcg() % 8u] = from_bits(0x80000000u);            /* -0 */
    else if (roll == 4u)
        k->quad[lcg() % 8u] = lcg_float(-3.0e9f, 3.0e9f);       /* huge */
    else if (roll == 5u)
        k->quad[lcg() % 8u] = from_bits(lcg());                   /* any bits */
    for (i = 0u; i < 8u; ++i)
        k->uv[i] = lcg_float(-2.0f, 3.0f);
    for (i = 0u; i < 4u; ++i) {
        uint32_t j;
        for (j = 0u; j < 4u; ++j)
            k->colours[i][j] = lcg_float(-1.0f, 2.0f);
    }
    if ((lcg() & 7u) == 0u)
        k->colours[lcg() & 3u][lcg() & 3u] = from_bits(lcg());
    layout = lcg() % 8u;
    k->count = k_layout_counts[layout];
    memcpy(k->formats, k_layouts[layout], sizeof k->formats);
    if ((lcg() & 15u) == 0u) {
        k->count = 0u;
        memset(k->formats, 0, sizeof k->formats);
    }
    k->stride = layout_stride(k);
    k->velem = k->stride;
    k->vidx = lcg() & 1u;
    k->iidx = lcg() & 1u;
    k->vcount = lcg() % 12u;
    k->vcap = k->vcount + 4u + lcg() % 8u;
    k->vbase = lcg() % 5u;
    if ((lcg() & 7u) == 0u) {
        k->vcount = 65534u + lcg() % 3u;      /* (u16)base wraps */
        k->vcap = k->vcount + 4u;
        k->vbase = lcg() % 3u;
    }
    k->icount = lcg() % 20u;
    k->icap = k->icount + 6u + lcg() % 12u;
    k->ibase = lcg() % 7u;
    k->flags = 0x20u | (lcg() & ~0x24u);
    if ((lcg() & 3u) == 0u)
        k->flags |= 0x4u;                         /* already dirty */
    k->flags_shadow = lcg();
    k->snap_a = lcg() & 1u ? lcg() : 0u;
    k->snap_b = lcg() & 1u ? lcg() : 0u;
    roll = lcg() % 5u;
    k->snap_scale = roll == 0u ? 1.0f : roll == 1u ? 2.0f : roll == 2u ? 0.5f :
                    roll == 3u ? 3.7f : lcg_float(0.01f, 8.0f);
    k->offx = lcg_float(-400.0f, 400.0f);
    k->offy = lcg_float(-400.0f, 400.0f);
    k->su = lcg_float(0.0f, 2.0f);
    k->sv = lcg_float(0.0f, 2.0f);
    k->dirty_byte = (lcg() & 3u) == 0u;
    k->dirty_used = lcg() % ORACLE_DIRTY_SLOTS;
    k->dirty_cap = ORACLE_DIRTY_SLOTS;
    k->cull_enable = (lcg() & 7u) != 0u;
    k->size_override = (lcg() & 3u) == 0u;
    k->snap_enable = (lcg() & 3u) != 0u;
    k->premultiply = lcg() & 1u;
    roll = lcg() % 8u;
    k->target_mode = roll < 4u ? 1 : roll < 6u ? 0 : 2;
    k->target_w = (int32_t)(lcg() % 2000u) - 100;
    k->target_h = (int32_t)(lcg() % 1200u) - 100;
    if ((lcg() & 15u) == 0u)
        k->target_w = (int32_t)lcg();             /* negative as int32 */
    if ((lcg() & 15u) == 0u)
        k->target_h = (int32_t)0x80000000u;
    roll = lcg() % 6u;
    k->depth = roll == 0u ? -1.0f : roll == 1u ? 0.0f : roll == 2u ? 0.005f :
               roll == 3u ? from_bits(0xff7fffffu) : roll == 4u ?
               from_bits(0x7fc00000u) : lcg_float(-100.0f, 100.0f);
    /* Step 2: the unbatched path, growth and the select's world (drawn
     * after the original fields so the numeric shapes above are unchanged). */
    if (lcg() % 3u == 0u) {
        unbatch(k);
        k->batch_select = (lcg() & 3u) == 0u ? 1 : 0;
    }
    if ((lcg() & 3u) == 0u)
        opaque(k);
    k->unbatched_byte = lcg() & 1u;
    k->group = lcg() % 5u;                        /* 3 = none, 4 = foreign */
    k->group_ptr = (int)(lcg() % 4u);
    k->blend_custom = (int)(lcg() & 1u);
    for (i = 0u; i < 4u; ++i)
        k->blend[i] = lcg() % 9u;
    k->prev_batch = (int)(lcg() % 4u);
    k->prev_image = (int)(lcg() % 3u);
    k->pairs_used = lcg() % (ORACLE_PAIR_SLOTS + 1u);   /* == cap: grows */
    if ((lcg() & 7u) == 0u) {
        k->pairs_used = 0u;
        k->pairs_cap = 0u;
    }
    k->renderer_w = (int32_t)(lcg() % 2000u) - 100;
    k->renderer_h = (int32_t)(lcg() % 1200u) - 100;
    if (k->vcount < 60000u && (lcg() & 7u) == 0u)
        k->vcap = k->vcount + lcg() % 4u;         /* the vertex reserve grows */
    if ((lcg() & 7u) == 0u)
        k->icap = k->icount + lcg() % 6u;         /* the index reserve grows */
    if ((lcg() & 15u) == 0u)
        k->fresh_v = 1;
    if ((lcg() & 15u) == 0u)
        k->fresh_i = 1;
    k->dirty_used = lcg() % (ORACLE_DIRTY_SLOTS + 1u);  /* == cap: grows */
    if ((lcg() & 15u) == 0u) {
        k->dirty_used = 0u;
        k->dirty_cap = 0u;
    }
    k->b2_velem = k->stride;
}

int main(int argc, char **argv)
{
    static guest_import imports[ISAAC_VITA_KAGE_QUAD_FLOOR_IMPORT_ID + 1u];
    kq_case k;
    uint32_t i, seed;
    uint32_t stack_floor, stack_ceiling;
    static char names[512][40];
    unsigned name_index = 0u;

    if (argc != 2) {
        fprintf(stderr, "usage: oracle <256 hex digits of the frozen bytes>\n");
        return 2;
    }
    for (i = 0u; i < sizeof s_image_pages / sizeof s_image_pages[0]; ++i) {
        if (!commit_image_page(VA(s_image_pages[i]), ORACLE_PAGE)) {
            fprintf(stderr, "could not commit image page %08x (build with "
                            "/LARGEADDRESSAWARE)\n", VA(s_image_pages[i]));
            return 2;
        }
    }
    if (!install_constants(argv[1])) {
        fprintf(stderr, "bad frozen-bytes argument\n");
        return 2;
    }

    guest_cpu_init(&s_cpu);
    stack_floor = (uint32_t)(uintptr_t)&s_stack_storage[ORACLE_STACK_GUARD_WORDS];
    stack_ceiling = (uint32_t)(uintptr_t)
        &s_stack_storage[ORACLE_STACK_GUARD_WORDS + ORACLE_STACK_WORDS];
    if (guest_stack_bind(&s_cpu, stack_floor, stack_ceiling)) {
        fprintf(stderr, "stack bind failed\n");
        return 2;
    }
    g_guest_coverage_imports = s_coverage_imports;
    guest_register(s_registered_addrs, s_registered_fns,
                   sizeof s_registered_addrs / sizeof s_registered_addrs[0]);
    /* 314 rows whose slot keys are the slot VAs (the oracle has no loaded
     * image, so image_rva() is the identity); row 313 is the floor slot,
     * the key the frozen dense-ID contract gives it. */
    for (i = 0u; i <= ISAAC_VITA_KAGE_QUAD_FLOOR_IMPORT_ID; ++i) {
        imports[i].slot_rva = VA(ISAAC_VITA_KAGE_QUAD_FLOOR_IAT_RVA) -
            4u * (ISAAC_VITA_KAGE_QUAD_FLOOR_IMPORT_ID - i);
        imports[i].name = i == ISAAC_VITA_KAGE_QUAD_FLOOR_IMPORT_ID ?
            "api-ms-win-crt-math-l1-1-0.dll!floor" : "oracle!generic";
    }
    guest_register_imports(imports, ISAAC_VITA_KAGE_QUAD_FLOOR_IMPORT_ID + 1u);

#define NAME(...) \
    (snprintf(names[name_index], sizeof names[0], __VA_ARGS__), \
     names[name_index++])

    printf("=== hand-written handled cases ===\n");
    case_init(&k, "game-shape");
    compare_both(&k);
    case_init(&k, "game-shape-premultiplied");
    k.premultiply = 1u;
    compare_both(&k);
    case_init(&k, "fallback-size");
    k.target_mode = 0;
    compare_both(&k);
    case_init(&k, "size-override");
    k.size_override = 1u;
    compare_both(&k);
    case_init(&k, "cull-disabled");
    k.cull_enable = 0u;
    compare_both(&k);
    case_init(&k, "clobbering-getters");
    k.target_mode = 2;
    compare_both(&k);
    case_init(&k, "snap-disabled-global");
    k.snap_enable = 0u;
    compare_both(&k);
    case_init(&k, "snap-off-words");
    k.snap_a = 1u; k.snap_b = 1u;
    compare_both(&k);
    case_init(&k, "snap-word-b-zero");
    k.snap_a = 1u; k.snap_b = 0u;
    compare_both(&k);
    case_init(&k, "no-snap-x0-ne-x2");
    k.quad[4] = 10.5f;
    compare_both(&k);
    case_init(&k, "no-snap-nan");
    k.quad[0] = from_bits(0x7fc00000u); k.quad[4] = from_bits(0x7fc00000u);
    k.cull_enable = 0u;
    compare_both(&k);
    case_init(&k, "count-0");
    k.count = 0u; k.stride = 0u; k.velem = 0x40u;
    compare_both(&k);
    case_init(&k, "count-16");
    k.count = 16u;
    memcpy(k.formats, k_layouts[6], sizeof k.formats);
    k.stride = layout_stride(&k); k.velem = k.stride;
    compare_both(&k);
    for (i = 1u; i <= 8u; ++i) {
        case_init(&k, NAME("format-%u-alone", i));
        k.count = 1u;
        memset(k.formats, 0, sizeof k.formats);
        k.formats[0] = (uint8_t)i;
        k.stride = k_format_sizes[i - 1u]; k.velem = k.stride;
        compare_both(&k);
    }
    case_init(&k, "cull-left");
    for (i = 0u; i < 8u; i += 2u) k.quad[i] -= 100.0f;
    compare_both(&k);
    case_init(&k, "cull-right");
    for (i = 0u; i < 8u; i += 2u) k.quad[i] += 480.0f;
    compare_both(&k);
    case_init(&k, "cull-top");
    for (i = 1u; i < 8u; i += 2u) k.quad[i] -= 100.0f;
    compare_both(&k);
    case_init(&k, "cull-bottom");
    for (i = 1u; i < 8u; i += 2u) k.quad[i] += 270.0f;
    compare_both(&k);
    case_init(&k, "cull-edge-touch-right");
    for (i = 0u; i < 8u; i += 2u) k.quad[i] = 480.0f + (float)i;
    compare_both(&k);
    case_init(&k, "cull-edge-inside-by-one");
    k.quad[0] = -100.0f; k.quad[4] = -100.0f; k.quad[2] = 1.0f; k.quad[6] = 1.0f;
    compare_both(&k);
    case_init(&k, "degenerate-point");
    for (i = 0u; i < 8u; ++i) k.quad[i] = 100.0f;
    compare_both(&k);
    case_init(&k, "degenerate-line-x");
    k.quad[2] = 10.0f; k.quad[6] = 10.0f;
    compare_both(&k);
    case_init(&k, "target-negative-size");
    k.target_w = -5; k.target_h = -1;
    compare_both(&k);
    case_init(&k, "target-huge-size");
    k.target_w = (int32_t)0xfffffff0u; k.target_h = 0x7fffffff;
    compare_both(&k);
    case_init(&k, "base16-wrap");
    k.vcount = 65535u; k.vcap = 65539u; k.vbase = 0u;
    compare_both(&k);
    case_init(&k, "base16-wrap-plus-one");
    k.vcount = 65534u; k.vcap = 65538u; k.vbase = 0u;
    compare_both(&k);
    case_init(&k, "ring-index-1");
    k.vidx = 1u; k.iidx = 1u;
    compare_both(&k);
    case_init(&k, "ring-exact-capacity");
    k.vcount = 3u; k.vcap = 7u; k.icount = 5u; k.icap = 11u;
    compare_both(&k);
    case_init(&k, "already-dirty-flag");
    k.flags |= 0x4u;
    compare_both(&k);
    case_init(&k, "dirty-byte-set");
    k.dirty_byte = 1u;
    compare_both(&k);
    case_init(&k, "dirty-last-slot");
    k.dirty_used = ORACLE_DIRTY_SLOTS - 1u;
    compare_both(&k);
    case_init(&k, "depth-negative-max");
    k.depth = from_bits(0xff7fffffu);
    compare_both(&k);
    case_init(&k, "depth-nan");
    k.depth = from_bits(0x7fc00000u);
    compare_both(&k);
    case_init(&k, "snap-scale-zero");
    k.snap_scale = 0.0f;
    compare_both(&k);
    case_init(&k, "snap-scale-negative");
    k.snap_scale = -1.5f;
    compare_both(&k);
    case_init(&k, "snap-huge-coordinates");
    k.quad[0] = -2.5e9f; k.quad[4] = -2.5e9f; k.quad[1] = 3.0e9f;
    k.quad[7] = 3.0e9f; k.cull_enable = 0u;
    compare_both(&k);
    case_init(&k, "snap-inf-coordinates");
    k.quad[0] = from_bits(0xff800000u); k.quad[4] = from_bits(0xff800000u);
    k.quad[7] = from_bits(0x7f800000u); k.cull_enable = 0u;
    compare_both(&k);
    case_init(&k, "premultiplied-nan-colour");
    k.premultiply = 1u;
    k.colours[1][3] = from_bits(0x7fc00000u);
    k.colours[2][0] = from_bits(0xff800000u);
    compare_both(&k);
    case_init(&k, "uv-scale-zero");
    k.su = 0.0f; k.sv = from_bits(0x80000000u);
    compare_both(&k);
    case_init(&k, "deep-stack");
    k.esp_offset = ISAAC_VITA_KAGE_QUAD_DEPTH_BYTES;
    compare_both(&k);
    /* Element sizes the body does not check: the reserve computes with the
       ring element, the fill with the stride, as the x86 does. */
    case_init(&k, "vertex-element-mismatch");
    k.velem = 40u;
    compare_both(&k);
    case_init(&k, "index-element-4");
    k.ielem = 4u;
    compare_both(&k);
    case_init(&k, "vertex-element-0-count-0");
    k.count = 0u; k.stride = 0u; k.velem = 0u;
    compare_both(&k);
    case_init(&k, "null-vertex-buffer-count-0");
    k.count = 0u; k.stride = 0u; k.velem = 0x40u; k.vptr_null = 1;
    compare_both(&k);

    printf("=== ring and dirty-vector growth (translated callees) ===\n");
    case_init(&k, "index-ring-grows");
    k.icount = 43u; k.icap = 48u;
    compare_both(&k);
    case_init(&k, "index-ring-full");
    k.icount = 48u; k.icap = 48u;
    compare_both(&k);
    case_init(&k, "vertex-ring-grows");
    k.vcount = 61u; k.vcap = 64u;
    compare_both(&k);
    case_init(&k, "vertex-ring-empty-cap");
    k.vcount = 0u; k.vcap = 0u;
    compare_both(&k);
    case_init(&k, "both-rings-grow");
    k.icount = 48u; k.icap = 48u; k.vcount = 64u; k.vcap = 64u;
    compare_both(&k);
    case_init(&k, "both-rings-grow-count-0");
    k.count = 0u; k.stride = 0u; k.velem = 0x40u;
    k.icount = 48u; k.icap = 48u; k.vcount = 64u; k.vcap = 64u;
    compare_both(&k);
    case_init(&k, "ring-count-wraps");
    k.icount = 0xfffffffcu; k.icap = 0xffffffffu;
    compare_both(&k);
    case_init(&k, "fresh-vertex-record");
    k.fresh_v = 1;
    compare_both(&k);
    case_init(&k, "fresh-index-record");
    k.fresh_i = 1;
    compare_both(&k);
    case_init(&k, "fresh-both-records");
    k.fresh_v = 1; k.fresh_i = 1;
    compare_both(&k);
    case_init(&k, "fresh-both-records-count-0");
    k.count = 0u; k.stride = 0u; k.velem = 0x40u;
    k.fresh_v = 1; k.fresh_i = 1;
    compare_both(&k);
    case_init(&k, "dirty-vector-full-grows");
    k.dirty_used = ORACLE_DIRTY_SLOTS;
    compare_both(&k);
    case_init(&k, "dirty-vector-empty-grows");
    k.dirty_used = 0u; k.dirty_cap = 0u;
    compare_both(&k);
    case_init(&k, "dirty-grows-count-0");
    k.count = 0u; k.stride = 0u; k.velem = 0x40u;
    k.dirty_used = ORACLE_DIRTY_SLOTS;
    compare_both(&k);
    case_init(&k, "dirty-grows-rings-grow");
    k.dirty_used = ORACLE_DIRTY_SLOTS;
    k.icount = 48u; k.icap = 48u; k.vcount = 64u; k.vcap = 64u;
    compare_both(&k);
    case_init(&k, "dirty-full-already-dirty");
    k.dirty_used = ORACLE_DIRTY_SLOTS; k.flags |= 0x4u;
    compare_both(&k);
    case_init(&k, "dirty-full-dirty-byte");
    k.dirty_used = ORACLE_DIRTY_SLOTS; k.dirty_byte = 1u;
    compare_both(&k);

    printf("=== unbatched images (translated blend/batch select) ===\n");
    case_init(&k, "unbatched-translucent");
    unbatch(&k);
    compare_both(&k);
    case_init(&k, "unbatched-translucent-premul");
    unbatch(&k); k.premultiply = 1u;
    compare_both(&k);
    case_init(&k, "unbatched-opaque");
    unbatch(&k); opaque(&k);
    compare_both(&k);
    case_init(&k, "unbatched-alpha-above-4");
    unbatch(&k); opaque(&k); k.colours[2][3] = 1.5f;
    compare_both(&k);
    case_init(&k, "unbatched-alpha-nan");
    unbatch(&k); k.colours[1][3] = from_bits(0x7fc00000u);
    compare_both(&k);
    case_init(&k, "unbatched-alpha-inf");
    unbatch(&k); k.colours[3][3] = from_bits(0x7f800000u);
    compare_both(&k);
    case_init(&k, "unbatched-custom-blend");
    unbatch(&k); k.blend_custom = 1;
    k.blend[0] = 2u; k.blend[1] = 3u; k.blend[2] = 4u; k.blend[3] = 5u;
    compare_both(&k);
    case_init(&k, "unbatched-opaque-custom-blend");
    unbatch(&k); opaque(&k); k.blend_custom = 1;
    k.blend[0] = 1u; k.blend[1] = 0u; k.blend[2] = 1u; k.blend[3] = 1u;
    compare_both(&k);
    case_init(&k, "unbatched-blend-reset-byte-set");
    unbatch(&k); k.unbatched_byte = 1u;
    compare_both(&k);
    case_init(&k, "unbatched-custom-blend-reset-set");
    unbatch(&k); k.unbatched_byte = 1u; k.blend_custom = 1;
    k.blend[0] = 9u;
    compare_both(&k);
    for (i = 0u; i <= 4u; ++i) {
        uint32_t p;
        for (p = 0u; p < 4u; ++p) {
            case_init(&k, NAME("unbatched-group-%u-ptr-%u", i, p));
            unbatch(&k); k.group = i; k.group_ptr = (int)p;
            compare_both(&k);
        }
    }
    case_init(&k, "unbatched-prev-batch-same");
    unbatch(&k); k.prev_batch = 1;
    compare_both(&k);
    case_init(&k, "unbatched-prev-batch-null");
    unbatch(&k); k.prev_batch = 0;
    compare_both(&k);
    case_init(&k, "unbatched-prev-image-null");
    unbatch(&k); k.prev_image = 0;
    compare_both(&k);
    case_init(&k, "unbatched-prev-image-self");
    unbatch(&k); k.prev_image = 1;
    compare_both(&k);
    case_init(&k, "unbatched-pairs-empty");
    unbatch(&k); k.pairs_used = 0u;
    compare_both(&k);
    case_init(&k, "unbatched-pairs-at-capacity");
    unbatch(&k); k.pairs_used = ORACLE_PAIR_SLOTS;
    compare_both(&k);
    case_init(&k, "unbatched-pairs-empty-vector");
    unbatch(&k); k.pairs_used = 0u; k.pairs_cap = 0u;
    compare_both(&k);
    case_init(&k, "unbatched-renderer-getters");
    unbatch(&k); k.target_mode = 0;
    compare_both(&k);
    case_init(&k, "unbatched-renderer-negative-size");
    unbatch(&k); k.target_mode = 0; k.renderer_w = -3; k.renderer_h = 0;
    compare_both(&k);
    case_init(&k, "unbatched-size-override");
    unbatch(&k); k.size_override = 1u;
    compare_both(&k);
    case_init(&k, "unbatched-clobbering-getters");
    unbatch(&k); k.target_mode = 2;
    compare_both(&k);
    case_init(&k, "unbatched-cull-disabled");
    unbatch(&k); k.cull_enable = 0u;
    compare_both(&k);
    case_init(&k, "unbatched-culled");
    unbatch(&k);
    for (i = 0u; i < 8u; i += 2u) k.quad[i] += 480.0f;
    compare_both(&k);
    case_init(&k, "unbatched-snap-off");
    unbatch(&k); k.snap_enable = 0u;
    compare_both(&k);
    case_init(&k, "unbatched-snap-scale-from-select");
    unbatch(&k); k.snap_scale = 0.0f; k.target_w = 960; k.target_h = 540;
    compare_both(&k);
    case_init(&k, "unbatched-snap-words-set");
    unbatch(&k); k.snap_a = 1u; k.snap_b = 1u;
    compare_both(&k);
    case_init(&k, "unbatched-count-0");
    unbatch(&k); k.count = 0u; k.stride = 0u; k.velem = 0x40u;
    compare_both(&k);
    case_init(&k, "unbatched-already-dirty");
    unbatch(&k); k.flags |= 0x4u;
    compare_both(&k);
    case_init(&k, "unbatched-dirty-grows");
    unbatch(&k); opaque(&k); k.dirty_used = ORACLE_DIRTY_SLOTS;
    compare_both(&k);
    case_init(&k, "unbatched-dirty-grows-then-reset");
    unbatch(&k); opaque(&k); k.dirty_used = ORACLE_DIRTY_SLOTS;
    k.blend_custom = 1; k.blend[0] = 3u;
    compare_both(&k);
    case_init(&k, "unbatched-rings-grow");
    unbatch(&k); k.icount = 48u; k.icap = 48u; k.vcount = 64u; k.vcap = 64u;
    compare_both(&k);
    case_init(&k, "unbatched-fresh-batch");
    unbatch(&k); k.batch_select = 1;
    compare_both(&k);
    case_init(&k, "unbatched-fresh-batch-opaque");
    unbatch(&k); opaque(&k); k.batch_select = 1;
    compare_both(&k);
    case_init(&k, "unbatched-fresh-batch-count-0");
    unbatch(&k); k.batch_select = 1; k.count = 0u; k.stride = 0u;
    k.velem = 0x40u; k.b2_velem = 0x40u;
    compare_both(&k);
    case_init(&k, "unbatched-fresh-batch-dirty-grows");
    unbatch(&k); opaque(&k); k.batch_select = 1;
    k.dirty_used = ORACLE_DIRTY_SLOTS;
    compare_both(&k);
    case_init(&k, "unbatched-fresh-batch-element-0-count-0");
    unbatch(&k); k.batch_select = 1; k.count = 0u; k.stride = 0u;
    k.velem = 0u; k.b2_velem = 0u;
    compare_both(&k);
    case_init(&k, "unbatched-fresh-batch-prev-batch2");
    unbatch(&k); k.batch_select = 1; k.prev_batch = 2;
    compare_both(&k);
    case_init(&k, "unbatched-fresh-batch-partial-records");
    unbatch(&k); k.batch_select = 1;
    k.b2_irec.ptr = A(indices[0]) + 16u; k.b2_irec.count = 2u;
    k.b2_irec.capacity = 40u; k.b2_irec.base = 1u;
    k.b2_vrec.ptr = A(vertices[0]) + 512u; k.b2_vrec.count = 1u;
    k.b2_vrec.capacity = 16u; k.b2_vrec.base = 0u;
    compare_both(&k);
    case_init(&k, "unbatched-fresh-batch-vertex-only-grows");
    unbatch(&k); k.batch_select = 1;
    k.b2_irec.ptr = A(indices[0]) + 16u; k.b2_irec.count = 2u;
    k.b2_irec.capacity = 40u; k.b2_irec.base = 1u;
    compare_both(&k);
    case_init(&k, "unbatched-deep-stack");
    unbatch(&k); k.esp_offset = ISAAC_VITA_KAGE_QUAD_DEPTH_BYTES;
    compare_both(&k);

    printf("=== numeric cases (LCG) ===\n");
    for (seed = 1u; seed <= 160u; ++seed) {
        random_case(&k, NAME("lcg-%03u", seed), seed);
        compare_both(&k);
    }

    printf("=== hostile inputs decline to the translated body ===\n");
    case_init(&k, "decline-shallow-stack");
    k.esp_offset = ISAAC_VITA_KAGE_QUAD_DEPTH_BYTES - 4u;
    decline(&k, ISAAC_VITA_KAGE_QUAD_DECLINE_FRAME);
    compare_both(&k);
    case_init(&k, "decline-esp-below-floor");
    k.esp_below_floor = 1;
    decline(&k, ISAAC_VITA_KAGE_QUAD_DECLINE_FRAME);
    compare_both(&k);
    case_init(&k, "decline-frame-straddles");
    k.esp_straddle = 1;
    decline(&k, ISAAC_VITA_KAGE_QUAD_DECLINE_FRAME);
    compare_both(&k);
    case_init(&k, "decline-count-17");
    k.count = 17u;
    memcpy(k.formats, k_layouts[6], 16u); k.formats[16] = 5u;
    k.stride = layout_stride(&k); k.velem = k.stride;
    decline(&k, ISAAC_VITA_KAGE_QUAD_DECLINE_COUNT_BOUND);
    compare_both(&k);
    case_init(&k, "decline-count-255");
    k.count = 255u;
    decline(&k, ISAAC_VITA_KAGE_QUAD_DECLINE_COUNT_BOUND);
    compare_both(&k);
    case_init(&k, "decline-format-0");
    k.formats[1] = 0u;
    k.stride = 4u + 12u + 16u; k.velem = k.stride;
    decline(&k, ISAAC_VITA_KAGE_QUAD_DECLINE_FORMAT);
    compare_both(&k);
    case_init(&k, "decline-format-9");
    k.formats[2] = 9u;
    k.stride = 12u + 8u; k.velem = k.stride;
    decline(&k, ISAAC_VITA_KAGE_QUAD_DECLINE_FORMAT);
    compare_both(&k);
    case_init(&k, "decline-format-255");
    k.formats[0] = 255u;
    decline(&k, ISAAC_VITA_KAGE_QUAD_DECLINE_FORMAT);
    compare_both(&k);
    case_init(&k, "decline-stride-short");
    k.stride = 32u; k.velem = 32u;
    decline(&k, ISAAC_VITA_KAGE_QUAD_DECLINE_STRIDE);
    compare_both(&k);
    case_init(&k, "decline-stride-long");
    k.stride = 40u; k.velem = 40u;
    decline(&k, ISAAC_VITA_KAGE_QUAD_DECLINE_STRIDE);
    compare_both(&k);
    case_init(&k, "decline-unbatched-format-9");
    unbatch(&k); k.formats[2] = 9u;
    k.stride = 12u + 8u; k.velem = k.stride;
    decline(&k, ISAAC_VITA_KAGE_QUAD_DECLINE_FORMAT);
    compare_both(&k);
    case_init(&k, "decline-unbatched-stride-short");
    unbatch(&k); k.stride = 32u; k.velem = 32u;
    decline(&k, ISAAC_VITA_KAGE_QUAD_DECLINE_STRIDE);
    compare_both(&k);
    /* Element size 0 makes the ring reserve return a null pointer and the
       translated body then stores through it: a probe. */
    case_init(&k, "decline-index-element-0");
    k.ielem = 0u;
    decline(&k, ISAAC_VITA_KAGE_QUAD_DECLINE_OBJECT_BUFFER);
    probe_rejects(&k);
    case_init(&k, "decline-vertex-element-0");
    k.velem = 0u;
    decline(&k, ISAAC_VITA_KAGE_QUAD_DECLINE_OBJECT_BUFFER);
    probe_rejects(&k);
    case_init(&k, "decline-imports-not-ready");
    k.import_not_ready = 1;
    decline(&k, ISAAC_VITA_KAGE_QUAD_DECLINE_FLOOR);
    compare_both(&k);
    case_init(&k, "decline-floor-slot-foreign");
    k.slot_bad = 1;
    decline(&k, ISAAC_VITA_KAGE_QUAD_DECLINE_FLOOR);
    compare_both(&k);
    /* The snap predicate is evaluated after the select: the floor route is
       authenticated for every handled quad, snapping or not. */
    case_init(&k, "decline-imports-not-ready-no-snap");
    k.import_not_ready = 1; k.snap_enable = 0u;
    decline(&k, ISAAC_VITA_KAGE_QUAD_DECLINE_FLOOR);
    compare_both(&k);
    case_init(&k, "decline-unbatched-slot-foreign");
    unbatch(&k); k.slot_bad = 1;
    decline(&k, ISAAC_VITA_KAGE_QUAD_DECLINE_FLOOR);
    compare_both(&k);
    /* The translated body would read the vtable through a null pointer or
       guest_call a null slot: fast path probes only. */
    case_init(&k, "decline-null-vtable");
    k.target_mode = 3;
    decline(&k, ISAAC_VITA_KAGE_QUAD_DECLINE_GETTER);
    probe_rejects(&k);
    case_init(&k, "decline-null-width-slot");
    k.target_mode = 4;
    decline(&k, ISAAC_VITA_KAGE_QUAD_DECLINE_GETTER);
    probe_rejects(&k);
    case_init(&k, "decline-null-height-slot");
    k.target_mode = 5;
    decline(&k, ISAAC_VITA_KAGE_QUAD_DECLINE_GETTER);
    probe_rejects(&k);
    case_init(&k, "decline-unbatched-null-vtable");
    unbatch(&k); k.target_mode = 3;
    decline(&k, ISAAC_VITA_KAGE_QUAD_DECLINE_GETTER);
    probe_rejects(&k);
    case_init(&k, "handled-null-vtable-override");
    k.target_mode = 3; k.size_override = 1u;
    compare_both(&k);
    case_init(&k, "handled-null-vtable-cull-off");
    k.target_mode = 3; k.cull_enable = 0u;
    compare_both(&k);

    printf("=== probes: inputs the translated body cannot execute ===\n");
    case_init(&k, "probe-null-this");
    decline(&k, ISAAC_VITA_KAGE_QUAD_DECLINE_OBJECT_ARGUMENT);
    /* prepare() puts the Image address in ECX; the probe overrides it. */
    {
        uint32_t esp;
        prepare(&k, &s_cpu, &esp);
        s_cpu.ecx = 0u;
        describe(s_ref_state, sizeof s_ref_state, &s_cpu, esp, 0);
        isaac_vita_kage_quad_fastpath_take(NULL);
        {
            IsaacVitaKageQuadCounters counters;
            int verdict = oracle_kage_quad_impl(&s_cpu);
            isaac_vita_kage_quad_fastpath_take(&counters);
            describe(s_seam_state, sizeof s_seam_state, &s_cpu, esp, 0);
            ++s_probes;
            printf("probe %-32s verdict=%d reason=%d\n", k.name, verdict,
                   k.expect_reason);
            if (verdict != 0 || counters.declined[k.expect_reason] != 1u ||
                strcmp(s_ref_state, s_seam_state) != 0)
                fail(k.name, "probe", "null this did not decline cleanly");
        }
    }
    case_init(&k, "probe-null-quad");
    k.quad_null = 1;
    decline(&k, ISAAC_VITA_KAGE_QUAD_DECLINE_OBJECT_ARGUMENT);
    probe_rejects(&k);
    case_init(&k, "probe-null-formats");
    k.formats_null = 1;
    decline(&k, ISAAC_VITA_KAGE_QUAD_DECLINE_OBJECT_ARGUMENT);
    probe_rejects(&k);
    case_init(&k, "probe-null-uv");
    k.uv_null = 1;
    decline(&k, ISAAC_VITA_KAGE_QUAD_DECLINE_OBJECT_ARGUMENT);
    probe_rejects(&k);
    case_init(&k, "probe-null-colour");
    k.colour_null = 1;
    decline(&k, ISAAC_VITA_KAGE_QUAD_DECLINE_OBJECT_ARGUMENT);
    probe_rejects(&k);
    /* An unbatched image reads every colour's alpha before the select; a
       batched image without colour attributes never reads them. */
    case_init(&k, "probe-unbatched-null-colour-no-colour-format");
    unbatch(&k); k.colour_null = 1;
    k.count = 2u; k.formats[2] = 0u; k.stride = 20u; k.velem = 20u;
    decline(&k, ISAAC_VITA_KAGE_QUAD_DECLINE_OBJECT_ARGUMENT);
    probe_rejects(&k);
    case_init(&k, "handled-null-colour-no-colour-format");
    k.colour_null = 1;
    k.count = 2u; k.formats[2] = 0u; k.stride = 20u; k.velem = 20u;
    compare_both(&k);
    case_init(&k, "probe-null-batch");
    k.batch_null = 1;
    decline(&k, ISAAC_VITA_KAGE_QUAD_DECLINE_OBJECT_BATCH);
    probe_rejects(&k);
    case_init(&k, "probe-null-vertex-records");
    k.vrecords_null = 1;
    decline(&k, ISAAC_VITA_KAGE_QUAD_DECLINE_OBJECT_BATCH);
    probe_rejects(&k);
    case_init(&k, "probe-null-index-records");
    k.irecords_null = 1;
    decline(&k, ISAAC_VITA_KAGE_QUAD_DECLINE_OBJECT_BATCH);
    probe_rejects(&k);
    case_init(&k, "probe-null-index-buffer");
    k.iptr_null = 1;
    decline(&k, ISAAC_VITA_KAGE_QUAD_DECLINE_OBJECT_BUFFER);
    probe_rejects(&k);
    case_init(&k, "probe-null-vertex-buffer");
    k.vptr_null = 1;
    decline(&k, ISAAC_VITA_KAGE_QUAD_DECLINE_OBJECT_BUFFER);
    probe_rejects(&k);
    case_init(&k, "probe-null-index-buffer-count-0");
    k.count = 0u; k.stride = 0u; k.velem = 0x40u; k.iptr_null = 1;
    decline(&k, ISAAC_VITA_KAGE_QUAD_DECLINE_OBJECT_BUFFER);
    probe_rejects(&k);
    /* A batched image whose batch is null: the unbatched image reads the
       batch only after the select (a fault probe below). */
    case_init(&k, "probe-unbatched-null-records-not-read");
    unbatch(&k); k.batch_null = 1; k.batch_select = 0; k.vrecords_null = 1;
    k.irecords_null = 1;
    /* prepare() nulls the image's batch word; the select returns the batch
       object, whose null records the replay faults on after the select. */
    probe_fault(&k, "null vertex ring records");
    case_init(&k, "probe-unbound-cpu");
    {
        CPU foreign;
        IsaacVitaKageQuadCounters counters;
        int verdict;
        guest_cpu_init(&foreign);
        foreign.ecx = A(image[0]);
        foreign.esp = 0x1000u;
        isaac_vita_kage_quad_fastpath_take(NULL);
        verdict = oracle_kage_quad_impl(&foreign);
        isaac_vita_kage_quad_fastpath_take(&counters);
        ++s_probes;
        printf("probe %-32s verdict=%d reason=%d\n", k.name, verdict,
               ISAAC_VITA_KAGE_QUAD_DECLINE_FRAME);
        if (verdict != 0 ||
            counters.declined[ISAAC_VITA_KAGE_QUAD_DECLINE_FRAME] != 1u ||
            foreign.esp != 0x1000u || foreign.eax != 0u)
            fail(k.name, "probe", "unbound CPU did not decline cleanly");
        verdict = oracle_kage_quad_impl(NULL);
        isaac_vita_kage_quad_fastpath_take(&counters);
        ++s_probes;
        if (verdict != 0 ||
            counters.declined[ISAAC_VITA_KAGE_QUAD_DECLINE_FRAME] != 1u)
            fail(k.name, "probe", "NULL CPU did not decline cleanly");
    }

    printf("=== faults: states only the select produces ===\n");
    case_init(&k, "fault-select-null-batch");
    unbatch(&k); k.batch_select = 2;
    probe_fault(&k, "null batch");
    case_init(&k, "fault-fresh-batch-null-vertex-records");
    unbatch(&k); k.batch_select = 1; k.b2_vrecords_null = 1;
    probe_fault(&k, "null vertex ring records");
    case_init(&k, "fault-fresh-batch-null-index-records");
    unbatch(&k); k.batch_select = 1; k.b2_irecords_null = 1;
    probe_fault(&k, "ring records null");
    case_init(&k, "fault-fresh-batch-index-element-0");
    unbatch(&k); k.batch_select = 1; k.b2_ielem = 0u;
    probe_fault(&k, "ring element size 0");
    case_init(&k, "fault-fresh-batch-vertex-element-0");
    unbatch(&k); k.batch_select = 1; k.b2_velem = 0u;
    probe_fault(&k, "ring element size 0");
    case_init(&k, "fault-fresh-batch-index-record-no-buffer");
    unbatch(&k); k.batch_select = 1;
    k.b2_irec.capacity = 64u;                 /* room, no buffer */
    probe_fault(&k, "ring record without a buffer");
    case_init(&k, "fault-fresh-batch-vertex-record-no-buffer");
    unbatch(&k); k.batch_select = 1;
    k.b2_vrec.capacity = 64u; k.b2_vrec.count = 3u;
    probe_fault(&k, "ring record without a buffer");

    if (s_failures) {
        printf("Vita kage quad fast path oracle: FAIL; failures=%u\n",
               s_failures);
        return 1;
    }
    printf("Vita kage quad fast path oracle: PASS; cases=%u; handled=%u; "
           "declined=%u; culled=%u; snapped=%u; floors=%u; getters=%u; "
           "unbatched=%u; ring-growths=%u; dirty-growths=%u; faults=%u; "
           "probes=%u\n",
           s_cases, s_total_handled, s_total_declined, s_total_culled,
           s_total_snapped, s_total_floors, s_total_getters,
           s_total_unbatched, s_total_ring_growths, s_total_dirty_growths,
           s_faults, s_probes);
    return 0;
}
