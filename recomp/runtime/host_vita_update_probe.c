/* Inclusive wall-clock probe over Game::Update's translated subtrees
 * (ISAAC_VITA_UPDATE_PROBE; CMake allows it only with ISAAC_VITA_PHASE_PROFILE).
 *
 * Why: the guest sampler names the innermost call site, so it gives self
 * time with stale-word bias and no inclusive time at all; the update phase
 * (~5.6-5.9 ms per tick in fights) could not be split into subtrees.  Each
 * listed function gets a GNU ld --wrap seam that counts entries and times
 * the translated body from entry to exit on sceKernelGetProcessTimeWide:
 * every entry while fewer than 64 have been seen this window, then 1 in 64.
 * A function re-entered while its own span is open is not timed again.
 * `in` counts probe spans completed inside a timed span, so the clock's
 * share can be taken out of the parent (each costs about one z).
 *
 * --wrap only sees references from other objects: calls from the
 * function's own generated unit go straight to the translated body and are
 * neither counted nor timed (CMake prints the owner of each symbol).
 * Guest state is never written; the seam only calls __real_.  The two
 * keyed probes (header) read their key from the guest registers/stack at
 * entry, time every entry and table it by key (ph120.uk).
 *
 * shortcut: counters are plain (not atomic); a listed function also run on
 * another thread could skew n/t by a race, never guest state. */
#include "host_vita_update_probe.h"

#include "guest.h"

#include <string.h>

uint64_t sceKernelGetProcessTimeWide(void);

typedef struct {
    uint32_t n, t, us, in, mx;
    int open;
} uprobe_slot;

static uprobe_slot s_slot[ISAAC_VITA_UPDATE_PROBES];
static uint32_t s_spans, s_entries, s_z_n, s_z_us;

/* The census exists exactly when guest.h declares it (same condition).
 * CMake sets ISAAC_VITA_UPDATE_PROBE_CENSUS when it gave this TU guest.c's
 * switches; then a missing census is a build error, not silent zeros. */
#if defined(ISAAC_VITA_UPDATE_PROBE_CENSUS) && \
    !(defined(ISAAC_VITA_PHASE_PROFILE) && \
      (defined(ISAAC_VITA_GUEST_DISPATCH_TABLE) || \
       defined(ISAAC_VITA_PROFILE_IMPORT_KINDS)))
# error "ISAAC_VITA_UPDATE_PROBE_CENSUS without the census switches"
#endif
#if defined(ISAAC_VITA_PHASE_PROFILE) && \
    (defined(ISAAC_VITA_GUEST_DISPATCH_TABLE) || \
     defined(ISAAC_VITA_PROFILE_IMPORT_KINDS))
# define UPROBE_CENSUS g_guest_phase_profile_import_calls
#else
static const uint32_t s_no_census[GUEST_PHASE_PROFILE_IMPORT_ID_SLOTS];
# define UPROBE_CENSUS s_no_census
#endif

static void uprobe_groups(uint32_t g[ISAAC_VITA_UPDATE_PROBE_GROUPS])
{
    const uint32_t *n = UPROBE_CENSUS;
    g[ISAAC_VITA_UPDATE_PROBE_GROUP_HEAP] = n[305] + n[306] + n[307] + n[309];
    g[ISAAC_VITA_UPDATE_PROBE_GROUP_ACCESS] = n[303];
    g[ISAAC_VITA_UPDATE_PROBE_GROUP_FILE] = n[368] + n[359] + n[361];
    g[ISAAC_VITA_UPDATE_PROBE_GROUP_LOCK] = n[61];
    g[ISAAC_VITA_UPDATE_PROBE_GROUP_STRING] = n[383] + n[385] + n[281] + n[284];
}
_Static_assert(GUEST_PHASE_PROFILE_IMPORT_ID_SLOTS > 385U, "census covers the IDs");

static IsaacVitaUpdateProbeComposed s_composed[ISAAC_VITA_UPDATE_PROBE_COMPOSED];
static const uint32_t k_composed_rva[ISAAC_VITA_UPDATE_PROBE_COMPOSED] = {
#define UPROBE_CRVA(r) 0x##r##U,
    ISAAC_VITA_UPDATE_PROBE_COMPOSED_LIST(UPROBE_CRVA)
#undef UPROBE_CRVA
};

static int uprobe_composed_index(uint32_t rva)
{
    int i;
    for (i = 0; i < ISAAC_VITA_UPDATE_PROBE_COMPOSED; ++i)
        if (k_composed_rva[i] == rva)
            return i;
    return -1;
}

typedef struct {
    IsaacVitaUpdateProbeKey slot[ISAAC_VITA_UPDATE_PROBE_KEY_SLOTS];
    uint32_t overflow;
} uprobe_keys;
static uprobe_keys s_keys[ISAAC_VITA_UPDATE_PROBE_KEYED];
static const uint32_t k_keyed_rva[ISAAC_VITA_UPDATE_PROBE_KEYED] = {
    0x0040eef0U, 0x001b79c0U
};

/* -1: not keyed.  The key is read before the body runs. */
static int uprobe_key(uint32_t rva, const CPU *c, uint32_t *key)
{
    if (rva == 0x0040eef0U) {
        *key = ld32(c->ecx + 4U);
        return 0;
    }
    if (rva == 0x001b79c0U) {
        *key = (ld32(c->esp + 8U) << 16) | (ld32(c->esp + 12U) & 0xffffU);
        return 1;
    }
    return -1;
}

static void uprobe_key_add(uprobe_keys *t, uint32_t key, uint32_t us)
{
    uint32_t i = (key * 2654435761U) >> 26, k;
    for (k = 0; k < 16U; ++k, i = (i + 1U) & (ISAAC_VITA_UPDATE_PROBE_KEY_SLOTS - 1U)) {
        IsaacVitaUpdateProbeKey *e = &t->slot[i];
        if (e->n == 0U || e->key == key) {
            e->key = key;
            ++e->n;
            e->us += us;
            return;
        }
    }
    ++t->overflow;
}

static const uint32_t k_rva[ISAAC_VITA_UPDATE_PROBES] = {
#define UPROBE_RVA(r) 0x##r##U,
    ISAAC_VITA_UPDATE_PROBE_LIST(UPROBE_RVA)
#undef UPROBE_RVA
};

static void uprobe_run(unsigned i, CPU *c, void (*body)(CPU *))
{
    uprobe_slot *p = &s_slot[i];
    const uint32_t ord = p->n++;
    uint32_t key = 0;
    const int keyed = uprobe_key(k_rva[i], c, &key);

    if ((++s_entries & 1023U) == 512U) {
        const uint64_t z0 = sceKernelGetProcessTimeWide();
        s_z_us += (uint32_t)(sceKernelGetProcessTimeWide() - z0);
        ++s_z_n;
    }
    if (!p->open && (keyed >= 0 || ord < 64U || (ord & 63U) == 0U)) {
        const uint32_t spans0 = s_spans;
        uint64_t t0;
        uint32_t dt;

        const int comp = uprobe_composed_index(k_rva[i]);
        uint32_t g0[ISAAC_VITA_UPDATE_PROBE_GROUPS], g1[ISAAC_VITA_UPDATE_PROBE_GROUPS];

        if (comp >= 0)
            uprobe_groups(g0);
        p->open = 1;
        t0 = sceKernelGetProcessTimeWide();
        body(c);
        dt = (uint32_t)(sceKernelGetProcessTimeWide() - t0);
        p->open = 0;
        if (comp >= 0) {
            IsaacVitaUpdateProbeComposed *o = &s_composed[comp];
            unsigned k;
            uprobe_groups(g1);
            ++o->t;
            o->us += dt;
            for (k = 0; k < ISAAC_VITA_UPDATE_PROBE_GROUPS; ++k)
                o->calls[k] += g1[k] - g0[k];
        }
        ++p->t;
        p->us += dt;
        p->in += s_spans - spans0;
        if (dt > p->mx)
            p->mx = dt;
        if (keyed >= 0)
            uprobe_key_add(&s_keys[keyed], key, dt);
        ++s_spans;
        return;
    }
    body(c);
}

#define UPROBE_INDEX(r) UPROBE_I_##r,
enum { ISAAC_VITA_UPDATE_PROBE_LIST(UPROBE_INDEX) UPROBE_I_END };
#undef UPROBE_INDEX

#define UPROBE_WRAP(r)                                                       \
    void __real_sub_##r(CPU *c);                                             \
    void __wrap_sub_##r(CPU *c);                                             \
    void __wrap_sub_##r(CPU *c)                                              \
    {                                                                        \
        uprobe_run(UPROBE_I_##r, c, __real_sub_##r);                         \
    }
ISAAC_VITA_UPDATE_PROBE_LIST(UPROBE_WRAP)
#undef UPROBE_WRAP

/* distinct keys and the top by us (ties keep slot order). */
static void uprobe_keys_take(const uprobe_keys *t, uint32_t rva,
                             IsaacVitaUpdateProbeKeyed *o)
{
    uint32_t i, j;
    memset(o, 0, sizeof *o);
    o->rva = rva;
    o->overflow = t->overflow;
    for (i = 0; i < ISAAC_VITA_UPDATE_PROBE_KEY_SLOTS; ++i) {
        const IsaacVitaUpdateProbeKey *e = &t->slot[i];
        if (e->n == 0U)
            continue;
        ++o->distinct;
        for (j = ISAAC_VITA_UPDATE_PROBE_TOP_KEYS; j > 0U && (o->top[j - 1U].n == 0U || o->top[j - 1U].us < e->us); --j)
            if (j < ISAAC_VITA_UPDATE_PROBE_TOP_KEYS)
                o->top[j] = o->top[j - 1U];
        if (j < ISAAC_VITA_UPDATE_PROBE_TOP_KEYS)
            o->top[j] = *e;
    }
}

void isaac_vita_update_probe_take(IsaacVitaUpdateProbe *out)
{
    unsigned i;

    if (out) {
        for (i = 0; i < ISAAC_VITA_UPDATE_PROBES; ++i) {
            out->row[i].rva = k_rva[i];
            out->row[i].n = s_slot[i].n;
            out->row[i].t = s_slot[i].t;
            out->row[i].us = s_slot[i].us;
            out->row[i].in = s_slot[i].in;
            out->row[i].mx = s_slot[i].mx;
        }
        out->z_n = s_z_n;
        out->z_us = s_z_us;
        for (i = 0; i < ISAAC_VITA_UPDATE_PROBE_KEYED; ++i)
            uprobe_keys_take(&s_keys[i], k_keyed_rva[i], &out->keyed[i]);
        for (i = 0; i < ISAAC_VITA_UPDATE_PROBE_COMPOSED; ++i) {
            out->composed[i] = s_composed[i];
            out->composed[i].rva = k_composed_rva[i];
        }
    }
    memset(s_composed, 0, sizeof s_composed);
    memset(s_keys, 0, sizeof s_keys);
    /* Also closes a span a guest longjmp may have left open. */
    memset(s_slot, 0, sizeof s_slot);
    s_spans = s_entries = s_z_n = s_z_us = 0;
}
