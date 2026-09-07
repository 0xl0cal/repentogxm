#include "host_vita_png_reuse.h"
#include <string.h>

static void np_reuse_drop(IsaacNpReuse *c, IsaacNpReuseEntry *e)
{
    if (e->valid) {
        e->valid = 0U;
        ++c->evictions;
    }
}

#if ISAAC_VITA_NATIVE_PNG_REUSE_TINFL
/* New admissions must not turn the old fitting strict working set into a
 * larger cyclic FIFO miss stream. Strict entries retain the original arena
 * cursor and slot sequence; tinfl entries only occupy its unused gaps. */
static void np_reuse_drop_tinfl(IsaacNpReuse *c)
{
    uint32_t i;
    for (i = 0U; i < ISAAC_NP_REUSE_SLOTS; ++i)
        if (c->entries[i].valid == 2U)
            np_reuse_drop(c, &c->entries[i]);
}

#if ISAAC_VITA_NATIVE_PNG_REUSE_LARGE
static void np_reuse_drop_large(IsaacNpReuse *c)
{
    uint32_t i;
    for (i = 0U; i < ISAAC_NP_REUSE_SLOTS; ++i)
        if (c->entries[i].valid == 3U)
            np_reuse_drop(c, &c->entries[i]);
}
#endif

static int np_reuse_gap(const IsaacNpReuse *c, uint32_t size, uint32_t *offset,
                        uint32_t *slot)
{
    uint32_t pass, i, end = c->bytes;
    *slot = ISAAC_NP_REUSE_SLOTS;
    for (i = 0U; i < ISAAC_NP_REUSE_SLOTS; ++i)
        if (!c->entries[i].valid) { *slot = i; break; }
    if (*slot == ISAAC_NP_REUSE_SLOTS)
        return 0;
    /* Every unsuccessful pass moves end below at least one live interval.
     * At most 32 live intervals exist; no sorting, allocation or raw scan. */
    for (pass = 0U; pass <= ISAAC_NP_REUSE_SLOTS && end >= size; ++pass) {
        uint32_t start = end - size;
        for (i = 0U; i < ISAAC_NP_REUSE_SLOTS; ++i) {
            const IsaacNpReuseEntry *e = &c->entries[i];
            if (e->valid && start < e->offset + e->bytes && e->offset < end) {
                end = e->offset;
                break;
            }
        }
        if (i == ISAAC_NP_REUSE_SLOTS) { *offset = start; return 1; }
    }
    return 0;
}
#endif

void isaac_np_reuse_init(IsaacNpReuse *c, uint8_t *arena, uint32_t bytes)
{
    memset(c, 0, sizeof(*c));
    c->arena = arena;
    c->bytes = bytes;
    c->cursor = bytes;
}

int isaac_np_reuse_disjoint(const IsaacNpReuse *c, const void *ptr, uint32_t bytes)
{
    uintptr_t a, r;
    if (!c || !c->arena || !c->bytes || !ptr || !bytes)
        return 0;
    a = (uintptr_t)c->arena;
    r = (uintptr_t)ptr;
    if (c->bytes > UINTPTR_MAX - a || bytes > UINTPTR_MAX - r ||
        (a < r + bytes && r < a + c->bytes))
        return 0;
    return 1;
}

int isaac_np_reuse_begin(IsaacNpReuse *c, uint8_t *raw, uint32_t bytes)
{
    if (!isaac_np_reuse_disjoint(c, raw, bytes))
        return 0;
    c->raw_bytes = bytes;
    return 1;
}

static int np_reuse_geometry(const IsaacNpReuseEntry *e,
                            const isaac_np_params *p, uint32_t n, uint32_t crc)
{
    return e->width == p->width && e->height == p->height &&
        e->channels == p->channels && e->color_type == p->color_type &&
        e->rowbytes == p->rowbytes && e->input_bytes == n &&
        e->crc_entry == p->crc_entry && e->crc_final == crc;
}

int isaac_np_reuse_load(IsaacNpReuse *c, const isaac_np_params *p,
    const uint8_t *input, uint32_t n, uint32_t crc,
    isaac_np_work *work, isaac_np_result *out)
{
    uint32_t i, gamma = p->gamma_table && p->color_type != 3U;
    if (!c || !work->last_row_pre_gamma)
        return 0;
    ++c->lookups;
    for (i = 0U; i < ISAAC_NP_REUSE_SLOTS; ++i) {
        const IsaacNpReuseEntry *e = &c->entries[i];
        const uint8_t *src;
        if (!e->valid || !np_reuse_geometry(e, p, n, crc) ||
            e->raw_bytes != c->raw_bytes || e->gamma != gamma ||
            (gamma && memcmp(e->gamma_table, p->gamma_table, 256U)))
            continue;
        src = c->arena + e->offset;
        if (memcmp(src, input, n))
            continue;
        /* Full input equality is mandatory even after CRC equality. */
        memcpy(work->raw, src + n, e->raw_bytes);
        memcpy(work->last_row_pre_gamma, src + n + e->raw_bytes, p->rowbytes);
        out->last_filter = e->last_filter;
        memcpy(out->filters, e->filters, sizeof(e->filters));
        out->crc_final = crc;
        out->status = ISAAC_NP_OK;
        ++c->hits;
        c->hit_kib += (e->raw_bytes + 1023U) / 1024U;
        return 1;
    }
    return 0;
}

void isaac_np_reuse_store(IsaacNpReuse *c, const isaac_np_params *p,
    const uint8_t *input, uint32_t n, const isaac_np_work *work,
    const isaac_np_result *out)
{
    uint32_t i, offset, size, slot = ISAAC_NP_REUSE_SLOTS;
    uint64_t need;
    IsaacNpReuseEntry *e;
    uint8_t *dst;
#if ISAAC_VITA_NATIVE_PNG_REUSE_TINFL
    uint32_t strict = out->strict_successes == 1U && out->strict_refusals == 0U;
#endif
#if ISAAC_VITA_NATIVE_PNG_REUSE_LARGE
    uint32_t large = n > 65536U;
#endif
    if (!c || !work->last_row_pre_gamma || out->status != ISAAC_NP_OK ||
        !((out->strict_successes == 1U && out->strict_refusals == 0U)
#if ISAAC_VITA_NATIVE_PNG_REUSE_TINFL
          || out->reuse_history_safe
#endif
          ) ||
        out->chunks != 1U || n == 0U || n > ISAAC_NP_REUSE_INPUT_BYTES ||
        n != p->first_remaining ||
#if ISAAC_VITA_NATIVE_PNG_REUSE_TINFL
        n > work->staging_bytes ||
#endif
        out->idat_bytes != n || out->stream_bytes != n)
        return;
#if ISAAC_VITA_NATIVE_PNG_REUSE_LARGE
    /* The strict adapter keeps its original <=64KiB domain. Larger entries
     * require a complete original-tinfl success and its history observation. */
    if (large && (!out->reuse_history_safe || out->strict_attempts ||
                  out->strict_successes || out->strict_refusals))
        return;
#endif
    need = (uint64_t)n + c->raw_bytes + p->rowbytes;
    if (need > c->bytes) {
        ++c->skipped;
        return;
    }
    size = (uint32_t)need;
#if ISAAC_VITA_NATIVE_PNG_REUSE_LARGE
    if (large) {
        /* Do not evict even older optional entries to admit this population.
         * No slot/cursor movement; lack of a fitting gap simply refuses it. */
        if (!np_reuse_gap(c, size, &offset, &slot)) {
            ++c->skipped;
            return;
        }
    } else {
        /* Existing strict AND small-tinfl insertions see their original
         * placements, not a new cyclic miss stream caused by larger rows. */
        np_reuse_drop_large(c);
#endif
#if ISAAC_VITA_NATIVE_PNG_REUSE_TINFL
    if (!strict) {
        if (!np_reuse_gap(c, size, &offset, &slot)) {
            np_reuse_drop_tinfl(c);
            if (!np_reuse_gap(c, size, &offset, &slot)) {
                ++c->skipped;
                return;
            }
        }
    } else {
        /* Restore exactly the strict-only slot/placement state before its
         * next insertion. Neither fallback allocation nor hits move cursor. */
        np_reuse_drop_tinfl(c);
#endif
    if (size > c->cursor)
        c->cursor = c->bytes;
    offset = c->cursor - size;
    for (i = 0U; i < ISAAC_NP_REUSE_SLOTS; ++i) {
        e = &c->entries[i];
        if (e->valid && offset < e->offset + e->bytes &&
            e->offset < offset + size)
            np_reuse_drop(c, e);
        if (!e->valid && slot == ISAAC_NP_REUSE_SLOTS)
            slot = i;
    }
    if (slot == ISAAC_NP_REUSE_SLOTS) {
        slot = c->next_slot;
        np_reuse_drop(c, &c->entries[slot]);
    }
    c->next_slot = (slot + 1U) % ISAAC_NP_REUSE_SLOTS;
#if ISAAC_VITA_NATIVE_PNG_REUSE_TINFL
    }
#endif
#if ISAAC_VITA_NATIVE_PNG_REUSE_LARGE
    }
#endif
    e = &c->entries[slot];
    e->valid = 0U; /* publish only after all payload/metadata writes */
    dst = c->arena + offset;
    memcpy(dst, input, n);
    memcpy(dst + n, work->raw, c->raw_bytes);
    memcpy(dst + n + c->raw_bytes, work->last_row_pre_gamma, p->rowbytes);
    e->offset = offset;
    e->bytes = size;
    e->raw_bytes = c->raw_bytes;
    e->width = p->width;
    e->height = p->height;
    e->channels = p->channels;
    e->color_type = p->color_type;
    e->rowbytes = p->rowbytes;
    e->input_bytes = n;
    e->crc_entry = p->crc_entry;
    e->crc_final = out->crc_final;
    e->gamma = p->gamma_table && p->color_type != 3U;
    if (e->gamma)
        memcpy(e->gamma_table, p->gamma_table, 256U);
    e->last_filter = out->last_filter;
    memcpy(e->filters, out->filters, sizeof(e->filters));
#if ISAAC_VITA_NATIVE_PNG_REUSE_TINFL
#if ISAAC_VITA_NATIVE_PNG_REUSE_LARGE
    e->valid = large ? 3U : (strict ? 1U : 2U);
#else
    e->valid = strict ? 1U : 2U;
#endif
    if (strict)
#else
    e->valid = 1U;
#endif
        c->cursor = offset;
    ++c->stores;
}
