#include "host_vita_archive_trail.h"

#include <stdio.h>
#include <string.h>

/* Not synchronized: the CRT holds its FILE-registry lock around every call.
 * Everything here is integer bookkeeping; no clock, no allocation, no log. */

typedef struct trail_key_state {
    int64_t last_offset;
    uint8_t used;
} trail_key_state;

static isaac_vita_archive_trail_window s_window;
static trail_key_state s_keys[ISAAC_VITA_ARCHIVE_TRAIL_KEYS];

static void trail_add_u32(uint32_t *value, uint32_t amount, uint32_t bit)
{
    if (amount > UINT32_MAX - *value) {
        *value = UINT32_MAX;
        s_window.sat |= bit;
    } else {
        *value += amount;
    }
}

static uint8_t trail_intern_key(const char *key)
{
    uint32_t i;
    size_t length = strlen(key);

    if (length >= ISAAC_VITA_ARCHIVE_TRAIL_KEY_CAPACITY) {
        s_window.sat |= ISAAC_VITA_ARCHIVE_TRAIL_SAT_KEYS;
        return ISAAC_VITA_ARCHIVE_TRAIL_KEY_NONE;
    }
    for (i = 0U; i < s_window.key_count; ++i) {
        if (strcmp(s_window.keys[i], key) == 0)
            return (uint8_t)i;
    }
    if (s_window.key_count >= ISAAC_VITA_ARCHIVE_TRAIL_KEYS) {
        s_window.sat |= ISAAC_VITA_ARCHIVE_TRAIL_SAT_KEYS;
        return ISAAC_VITA_ARCHIVE_TRAIL_KEY_NONE;
    }
    memcpy(s_window.keys[s_window.key_count], key, length + 1U);
    s_keys[s_window.key_count].last_offset = -1;
    s_keys[s_window.key_count].used = 1U;
    return (uint8_t)s_window.key_count++;
}

static void trail_clear_run(isaac_vita_archive_trail_token *token)
{
    token->at = 0U;
    token->run_offset = -1;
    token->run_bytes = 0U;
    token->run_freads = 0U;
    token->run_open = 0U;
    token->run_kind = 0U;
}

static void trail_record_run(isaac_vita_archive_trail_token *token,
                             uint8_t closed)
{
    isaac_vita_archive_trail_run *record;

    if (!token->run_open)
        return;
    token->run_open = 0U;
    /* The GetSize pattern (SEEK_END; ftell; SEEK_SET back) and any other
     * seek immediately followed by a seek would otherwise cost a record
     * each.  A seam seek is always a real entry access and is kept even
     * when the constructor failed before its first read. */
    if (!token->run_freads &&
        token->run_kind != ISAAC_VITA_ARCHIVE_TRAIL_KIND_SEAM) {
        trail_add_u32(&s_window.empty, 1U,
                      ISAAC_VITA_ARCHIVE_TRAIL_SAT_COUNTS);
        return;
    }
    trail_add_u32(&s_window.runs, 1U, ISAAC_VITA_ARCHIVE_TRAIL_SAT_COUNTS);
    trail_add_u32(&s_window.bytes, token->run_bytes,
                  ISAAC_VITA_ARCHIVE_TRAIL_SAT_BYTES);
    trail_add_u32(&s_window.freads, token->run_freads,
                  ISAAC_VITA_ARCHIVE_TRAIL_SAT_COUNTS);
    if (s_window.count >= ISAAC_VITA_ARCHIVE_READ_TRAIL_CAP) {
        trail_add_u32(&s_window.drop, 1U,
                      ISAAC_VITA_ARCHIVE_TRAIL_SAT_COUNTS);
        return;
    }
    record = &s_window.record[s_window.count++];
    record->at = token->at;
    record->offset = token->run_offset < 0 ||
                     token->run_offset > (int64_t)0xfffffffeU
        ? ISAAC_VITA_ARCHIVE_TRAIL_OFFSET_UNKNOWN
        : (uint32_t)token->run_offset;
    record->bytes = token->run_bytes;
    record->freads = token->run_freads;
    record->key = token->key;
    record->kind = token->run_kind;
    record->closed = closed;
    record->reserved = 0U;
}

static void trail_open_run(isaac_vita_archive_trail_token *token,
                           int64_t offset, uint8_t kind, uint64_t now)
{
    token->at = now;
    token->run_offset = offset;
    token->run_bytes = 0U;
    token->run_freads = 0U;
    token->run_open = 1U;
    token->run_kind = kind;
    if (offset >= 0 && token->key != ISAAC_VITA_ARCHIVE_TRAIL_KEY_NONE) {
        trail_key_state *state = &s_keys[token->key];

        if (state->last_offset >= 0 && offset < state->last_offset)
            trail_add_u32(&s_window.back, 1U,
                          ISAAC_VITA_ARCHIVE_TRAIL_SAT_COUNTS);
        state->last_offset = offset;
    }
}

void isaac_vita_archive_trail_open(isaac_vita_archive_trail_token *token,
                                   const char *key, uint64_t now)
{
    if (!token)
        return;
    memset(token, 0, sizeof *token);
    token->cursor = -1;
    token->run_offset = -1;
    token->key = ISAAC_VITA_ARCHIVE_TRAIL_KEY_NONE;
    if (!key || !key[0])
        return;
    token->keyed = 1U;
    token->key = trail_intern_key(key);
    token->cursor = 0;
    token->at = now;
    trail_add_u32(&s_window.opens, 1U, ISAAC_VITA_ARCHIVE_TRAIL_SAT_COUNTS);
}

int isaac_vita_archive_trail_needs_clock(
    const isaac_vita_archive_trail_token *token)
{
    return token && token->keyed && !token->run_open;
}

void isaac_vita_archive_trail_read(isaac_vita_archive_trail_token *token,
                                   uint32_t bytes, uint64_t now)
{
    if (!token || !token->keyed)
        return;
    if (!token->run_open) {
        trail_open_run(token, token->cursor,
                       ISAAC_VITA_ARCHIVE_TRAIL_KIND_IMPLICIT, now);
        trail_add_u32(&s_window.implicit, 1U,
                      ISAAC_VITA_ARCHIVE_TRAIL_SAT_COUNTS);
    }
    if (bytes > UINT32_MAX - token->run_bytes) {
        token->run_bytes = UINT32_MAX;
        s_window.sat |= ISAAC_VITA_ARCHIVE_TRAIL_SAT_BYTES;
    } else {
        token->run_bytes += bytes;
    }
    if (token->run_freads != UINT32_MAX)
        ++token->run_freads;
    else
        s_window.sat |= ISAAC_VITA_ARCHIVE_TRAIL_SAT_COUNTS;
    if (token->cursor >= 0) {
        if (token->cursor > INT64_MAX - (int64_t)bytes)
            token->cursor = -1;
        else
            token->cursor += (int64_t)bytes;
    }
}

void isaac_vita_archive_trail_seek(isaac_vita_archive_trail_token *token,
                                   int32_t offset, int origin, int result,
                                   int64_t known_position, int seam,
                                   uint64_t now)
{
    int64_t target;
    uint8_t kind;

    if (!token || !token->keyed)
        return;
    trail_add_u32(&s_window.seeks, 1U, ISAAC_VITA_ARCHIVE_TRAIL_SAT_COUNTS);
    if (result != 0) {
        /* newlib leaves the cursor where it was; the run stays open. */
        trail_add_u32(&s_window.fails, 1U,
                      ISAAC_VITA_ARCHIVE_TRAIL_SAT_COUNTS);
        return;
    }
    trail_record_run(token, 0U);
    if (known_position >= 0) {
        target = known_position;
    } else if (origin == SEEK_SET) {
        target = offset < 0 ? -1 : (int64_t)offset;
    } else if (origin == SEEK_CUR) {
        target = token->cursor < 0 ? -1 : token->cursor + (int64_t)offset;
        if (target < 0)
            target = -1;
    } else {
        target = -1;
    }
    if (seam)
        kind = ISAAC_VITA_ARCHIVE_TRAIL_KIND_SEAM;
    else if (origin == SEEK_SET)
        kind = ISAAC_VITA_ARCHIVE_TRAIL_KIND_SET;
    else if (origin == SEEK_CUR)
        kind = ISAAC_VITA_ARCHIVE_TRAIL_KIND_CUR;
    else
        kind = ISAAC_VITA_ARCHIVE_TRAIL_KIND_END;
    token->cursor = target;
    trail_open_run(token, target, kind, now);
}

void isaac_vita_archive_trail_close(isaac_vita_archive_trail_token *token)
{
    if (!token || !token->keyed)
        return;
    trail_record_run(token, 1U);
    trail_add_u32(&s_window.closes, 1U, ISAAC_VITA_ARCHIVE_TRAIL_SAT_COUNTS);
    trail_clear_run(token);
    token->keyed = 0U;
    token->key = ISAAC_VITA_ARCHIVE_TRAIL_KEY_NONE;
    token->cursor = -1;
}

void isaac_vita_archive_trail_take(isaac_vita_archive_trail_window *out)
{
    if (!out)
        return;
    s_window.abi = ISAAC_VITA_ARCHIVE_TRAIL_ABI;
    s_window.valid = 1U;
    *out = s_window;
    s_window.count = 0U;
    s_window.drop = 0U;
    s_window.runs = 0U;
    s_window.bytes = 0U;
    s_window.freads = 0U;
    s_window.seeks = 0U;
    s_window.fails = 0U;
    s_window.empty = 0U;
    s_window.back = 0U;
    s_window.opens = 0U;
    s_window.closes = 0U;
    s_window.implicit = 0U;
    /* keys, key_count and sat persist: names stay stable across windows and
     * a saturated table is a process-lifetime fact. */
}
