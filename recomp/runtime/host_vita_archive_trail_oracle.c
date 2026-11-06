/* Host oracle for the packed-archive read trail accumulator
 * (ISAAC_VITA_ARCHIVE_READ_TRAIL).  Drives host_vita_archive_trail.c with
 * the exact call shapes the CRT issues and pins the record stream, the
 * per-window totals, the bounded record store, key interning, cursor
 * tracking and the take/reset contract.  Built twice by
 * vita/test_archive_trail.sh: with ISAAC_VITA_ARCHIVE_READ_TRAIL_CAP=8 (the
 * store overflows inside a scenario) and at the header default the shipped
 * configure uses; every cap-dependent expectation is derived from the macro. */
#include <stdio.h>
#include <string.h>

#include "host_vita_archive_trail.h"

static unsigned s_failures;

#define CHECK(expression) \
    do { \
        if (!(expression)) { \
            fprintf(stderr, "archive-trail oracle failed at line %u: %s\n", \
                    (unsigned)__LINE__, #expression); \
            ++s_failures; \
        } \
    } while (0)

/* Provided by the CRT on the device; the oracle owns no lock. */
int isaac_vita_crt_archive_trail_take(isaac_vita_archive_trail_window *out)
{
    isaac_vita_archive_trail_take(out);
    return 1;
}

static void drain(isaac_vita_archive_trail_window *w)
{
    isaac_vita_archive_trail_take(w);
}

static void archived_file_entry(isaac_vita_archive_trail_token *t,
                                const char *key, int32_t offset,
                                uint32_t blocks, uint64_t at)
{
    uint32_t i;

    isaac_vita_archive_trail_open(t, key, at);
    isaac_vita_archive_trail_seek(t, offset, SEEK_SET, 0, offset, 1, at + 1U);
    for (i = 0U; i < blocks; ++i) {
        CHECK(!isaac_vita_archive_trail_needs_clock(t));
        isaac_vita_archive_trail_read(t, 4U, 0U);
        isaac_vita_archive_trail_read(t, 0x800U, 0U);
    }
    isaac_vita_archive_trail_close(t);
}

static void scenario_seam_entry(void)
{
    isaac_vita_archive_trail_token t;
    isaac_vita_archive_trail_window w;

    archived_file_entry(&t, "afterbirth.a", 5000, 3U, 100U);
    drain(&w);
    CHECK(w.valid == 1U && w.abi == ISAAC_VITA_ARCHIVE_TRAIL_ABI);
    CHECK(w.count == 1U && w.runs == 1U && w.drop == 0U);
    CHECK(w.opens == 1U && w.closes == 1U && w.seeks == 1U);
    CHECK(w.bytes == 3U * 0x804U && w.freads == 6U);
    CHECK(w.implicit == 0U && w.empty == 0U && w.fails == 0U);
    CHECK(w.record[0].at == 101U);
    CHECK(w.record[0].offset == 5000U);
    CHECK(w.record[0].bytes == 3U * 0x804U && w.record[0].freads == 6U);
    CHECK(w.record[0].kind == 'S' && w.record[0].closed == 1U);
    CHECK(w.key_count == 1U && strcmp(w.keys[w.record[0].key], "afterbirth.a") == 0);
    /* back: first offset of this archive can never be backward. */
    CHECK(w.back == 0U);
    CHECK(!t.keyed && !t.run_open);
}

static void scenario_mount_header_and_table(void)
{
    isaac_vita_archive_trail_token t;
    isaac_vita_archive_trail_window w;

    isaac_vita_archive_trail_open(&t, "afterbirth.a", 200U);
    CHECK(isaac_vita_archive_trail_needs_clock(&t));
    isaac_vita_archive_trail_read(&t, 14U, 205U);      /* header at 0 */
    CHECK(!isaac_vita_archive_trail_needs_clock(&t));
    isaac_vita_archive_trail_seek(&t, 145266573, SEEK_SET, 0, -1, 0, 210U);
    isaac_vita_archive_trail_read(&t, 52940U, 0U);     /* the table */
    isaac_vita_archive_trail_close(&t);
    drain(&w);
    CHECK(w.count == 2U && w.runs == 2U && w.implicit == 1U);
    CHECK(w.record[0].kind == 'I' && w.record[0].offset == 0U &&
          w.record[0].bytes == 14U && w.record[0].freads == 1U &&
          w.record[0].closed == 0U && w.record[0].at == 205U);
    CHECK(w.record[1].kind == 'O' && w.record[1].offset == 145266573U &&
          w.record[1].bytes == 52940U && w.record[1].closed == 1U &&
          w.record[1].at == 210U);
    /* 5000 (previous scenario) -> 0 is backward, 0 -> table is forward. */
    CHECK(w.back == 1U);
    CHECK(w.key_count == 1U);
}

static void scenario_cursor_tracking(void)
{
    isaac_vita_archive_trail_token t;
    isaac_vita_archive_trail_window w;

    isaac_vita_archive_trail_open(&t, "graphics.a", 300U);
    isaac_vita_archive_trail_seek(&t, 1000, SEEK_SET, 0, -1, 0, 301U);
    isaac_vita_archive_trail_read(&t, 100U, 0U);
    /* SEEK_CUR from a known cursor: 1000 + 100 + 50. */
    isaac_vita_archive_trail_seek(&t, 50, SEEK_CUR, 0, -1, 0, 302U);
    isaac_vita_archive_trail_read(&t, 8U, 0U);
    /* SEEK_END: unknown offset; a read still lands in the run. */
    isaac_vita_archive_trail_seek(&t, 0, SEEK_END, 0, -1, 0, 303U);
    isaac_vita_archive_trail_read(&t, 4U, 0U);
    /* SEEK_CUR from an unknown cursor stays unknown. */
    isaac_vita_archive_trail_seek(&t, -4, SEEK_CUR, 0, -1, 0, 304U);
    isaac_vita_archive_trail_read(&t, 4U, 0U);
    /* Empty non-seam runs are counted, not recorded. */
    isaac_vita_archive_trail_seek(&t, 3000, SEEK_SET, 0, -1, 0, 305U);
    isaac_vita_archive_trail_seek(&t, 4000, SEEK_SET, 0, -1, 0, 306U);
    isaac_vita_archive_trail_read(&t, 1U, 0U);
    /* The CRT's own post-seek position wins over the arithmetic. */
    isaac_vita_archive_trail_seek(&t, 7, SEEK_SET, 0, 4242, 0, 307U);
    isaac_vita_archive_trail_read(&t, 2U, 0U);
    /* A failed seek: counted, cursor and open run untouched. */
    isaac_vita_archive_trail_seek(&t, 9, SEEK_SET, -1, -1, 0, 308U);
    isaac_vita_archive_trail_read(&t, 3U, 0U);
    isaac_vita_archive_trail_close(&t);
    drain(&w);
    CHECK(w.seeks == 8U && w.fails == 1U && w.empty == 1U);
    CHECK(w.runs == 6U && w.count == 6U);
    CHECK(w.record[0].kind == 'O' && w.record[0].offset == 1000U && w.record[0].bytes == 100U);
    CHECK(w.record[1].kind == 'C' && w.record[1].offset == 1150U && w.record[1].bytes == 8U);
    CHECK(w.record[2].kind == 'E' && w.record[2].offset == ISAAC_VITA_ARCHIVE_TRAIL_OFFSET_UNKNOWN);
    CHECK(w.record[3].kind == 'C' && w.record[3].offset == ISAAC_VITA_ARCHIVE_TRAIL_OFFSET_UNKNOWN);
    CHECK(w.record[4].kind == 'O' && w.record[4].offset == 4000U && w.record[4].bytes == 1U);
    CHECK(w.record[5].kind == 'O' && w.record[5].offset == 4242U &&
          w.record[5].bytes == 5U && w.record[5].freads == 2U && w.record[5].closed == 1U);
    CHECK(w.record[5].at == 307U);
    /* graphics.a is a new key: 1000 -> 1150 -> 3000 -> 4000 -> 4242 forward. */
    CHECK(w.back == 0U && w.key_count == 2U);
    CHECK(strcmp(w.keys[w.record[0].key], "graphics.a") == 0);
}

static void scenario_backward_and_seam_without_read(void)
{
    isaac_vita_archive_trail_token t;
    isaac_vita_archive_trail_window w;

    /* graphics.a last known offset is 4242: 1000 is backward. */
    isaac_vita_archive_trail_open(&t, "graphics.a", 400U);
    isaac_vita_archive_trail_seek(&t, 1000, SEEK_SET, 0, 1000, 1, 401U);
    isaac_vita_archive_trail_close(&t);           /* ctor failed, no read */
    drain(&w);
    CHECK(w.back == 1U);
    CHECK(w.runs == 1U && w.count == 1U && w.empty == 0U);
    CHECK(w.record[0].kind == 'S' && w.record[0].freads == 0U &&
          w.record[0].bytes == 0U && w.record[0].closed == 1U);
}

static void scenario_cap_overflow(void)
{
    isaac_vita_archive_trail_token t;
    isaac_vita_archive_trail_window w;
    uint32_t i;

    for (i = 0U; i < ISAAC_VITA_ARCHIVE_READ_TRAIL_CAP + 3U; ++i)
        archived_file_entry(&t, "afterbirthp.a",
                            (int32_t)(1000U + 0x1000U * i), 1U, 500U + i);
    drain(&w);
    CHECK(w.runs == ISAAC_VITA_ARCHIVE_READ_TRAIL_CAP + 3U);
    CHECK(w.count == ISAAC_VITA_ARCHIVE_READ_TRAIL_CAP);
    CHECK(w.drop == 3U);
    CHECK(w.bytes == (ISAAC_VITA_ARCHIVE_READ_TRAIL_CAP + 3U) * 0x804U);
    CHECK(w.opens == ISAAC_VITA_ARCHIVE_READ_TRAIL_CAP + 3U);
    CHECK(w.record[ISAAC_VITA_ARCHIVE_READ_TRAIL_CAP - 1U].at ==
          500U + ISAAC_VITA_ARCHIVE_READ_TRAIL_CAP);
    CHECK(w.back == 0U);
    CHECK(w.key_count == 3U);
}

static void scenario_take_reset_and_persistence(void)
{
    isaac_vita_archive_trail_window w;
    isaac_vita_archive_trail_token t;

    drain(&w);
    CHECK(w.valid == 1U && w.count == 0U && w.runs == 0U && w.drop == 0U &&
          w.bytes == 0U && w.freads == 0U && w.seeks == 0U && w.opens == 0U &&
          w.closes == 0U && w.back == 0U && w.implicit == 0U &&
          w.empty == 0U && w.fails == 0U);
    CHECK(w.key_count == 3U);
    CHECK(strcmp(w.keys[0], "afterbirth.a") == 0);
    CHECK(strcmp(w.keys[1], "graphics.a") == 0);
    CHECK(strcmp(w.keys[2], "afterbirthp.a") == 0);
    /* The interned index is stable across takes. */
    archived_file_entry(&t, "graphics.a", 9000, 1U, 700U);
    drain(&w);
    CHECK(w.count == 1U && w.record[0].key == 1U);
}

static void scenario_unkeyed_token(void)
{
    isaac_vita_archive_trail_token t;
    isaac_vita_archive_trail_window w;

    isaac_vita_archive_trail_open(&t, "", 800U);
    CHECK(!t.keyed && !isaac_vita_archive_trail_needs_clock(&t));
    isaac_vita_archive_trail_read(&t, 100U, 801U);
    isaac_vita_archive_trail_seek(&t, 5, SEEK_SET, 0, -1, 0, 802U);
    isaac_vita_archive_trail_close(&t);
    isaac_vita_archive_trail_open(&t, NULL, 803U);
    isaac_vita_archive_trail_read(&t, 100U, 804U);
    isaac_vita_archive_trail_close(&t);
    drain(&w);
    CHECK(w.runs == 0U && w.opens == 0U && w.closes == 0U && w.seeks == 0U);
    /* Slot reuse: a stale run of the previous keyed owner never leaks. */
    isaac_vita_archive_trail_open(&t, "sfx.a", 810U);
    isaac_vita_archive_trail_seek(&t, 10, SEEK_SET, 0, -1, 1, 811U);
    isaac_vita_archive_trail_read(&t, 1U, 0U);
    isaac_vita_archive_trail_open(&t, "", 812U);   /* republished unkeyed */
    isaac_vita_archive_trail_read(&t, 1U, 0U);
    isaac_vita_archive_trail_close(&t);
    drain(&w);
    CHECK(w.runs == 0U && w.opens == 1U && w.closes == 0U);
}

static void scenario_key_table_saturation(void)
{
    isaac_vita_archive_trail_token t;
    isaac_vita_archive_trail_window w;
    char name[ISAAC_VITA_ARCHIVE_TRAIL_KEY_CAPACITY];
    /* One more archive than the key table holds.  The store keeps the first
     * CAP runs of the window whichever cap is compiled in (8 in the oracle
     * script's first build, the header default in its second), so every
     * expectation below is derived from the macros, not from cap=8. */
    const uint32_t runs = ISAAC_VITA_ARCHIVE_TRAIL_KEYS + 1U;
    const uint32_t kept = runs < ISAAC_VITA_ARCHIVE_READ_TRAIL_CAP
        ? runs : ISAAC_VITA_ARCHIVE_READ_TRAIL_CAP;
    uint32_t base, i;

    /* The keys interned by the earlier scenarios persist across takes. */
    drain(&w);
    base = w.key_count;
    CHECK(base == 4U && base < ISAAC_VITA_ARCHIVE_TRAIL_KEYS);
    for (i = 0U; i < runs; ++i) {
        snprintf(name, sizeof name, "extra%02u.a", (unsigned)i);
        archived_file_entry(&t, name, 100, 1U, 900U + i);
    }
    drain(&w);
    CHECK(w.key_count == ISAAC_VITA_ARCHIVE_TRAIL_KEYS);
    CHECK((w.sat & ISAAC_VITA_ARCHIVE_TRAIL_SAT_KEYS) != 0U);
    CHECK(w.runs == runs && w.count == kept && w.drop == runs - kept);
    /* Runs opened while the table had room carry their interned key; only
     * the runs after it filled carry the NONE key, and are still recorded. */
    for (i = 0U; i < kept; ++i) {
        uint32_t expected = base + i < ISAAC_VITA_ARCHIVE_TRAIL_KEYS
            ? base + i : ISAAC_VITA_ARCHIVE_TRAIL_KEY_NONE;
        CHECK((uint32_t)w.record[i].key == expected);
        CHECK(w.record[i].at == 901U + i);
    }
    /* Saturation is sticky across takes. */
    drain(&w);
    CHECK((w.sat & ISAAC_VITA_ARCHIVE_TRAIL_SAT_KEYS) != 0U);
}

static void scenario_byte_saturation(void)
{
    isaac_vita_archive_trail_token t;
    isaac_vita_archive_trail_window w;

    isaac_vita_archive_trail_open(&t, "afterbirth.a", 1000U);
    isaac_vita_archive_trail_seek(&t, 0, SEEK_SET, 0, -1, 1, 1001U);
    isaac_vita_archive_trail_read(&t, 0xffffff00U, 0U);
    isaac_vita_archive_trail_read(&t, 0x200U, 0U);
    isaac_vita_archive_trail_close(&t);
    drain(&w);
    CHECK(w.count == 1U && w.record[0].bytes == UINT32_MAX);
    CHECK((w.sat & ISAAC_VITA_ARCHIVE_TRAIL_SAT_BYTES) != 0U);
    CHECK(w.record[0].freads == 2U);
}

int main(void)
{
    scenario_seam_entry();
    scenario_mount_header_and_table();
    scenario_cursor_tracking();
    scenario_backward_and_seam_without_read();
    scenario_cap_overflow();
    scenario_take_reset_and_persistence();
    scenario_unkeyed_token();
    scenario_key_table_saturation();
    scenario_byte_saturation();
    if (s_failures) {
        fprintf(stderr, "archive-trail oracle: %u failure(s)\n", s_failures);
        return 1;
    }
    printf("archive-trail oracle: PASS (cap=%u keys=%u record=%u bytes)\n",
           (unsigned)ISAAC_VITA_ARCHIVE_READ_TRAIL_CAP,
           (unsigned)ISAAC_VITA_ARCHIVE_TRAIL_KEYS,
           (unsigned)sizeof(isaac_vita_archive_trail_run));
    return 0;
}
