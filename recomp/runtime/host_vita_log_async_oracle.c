/* Host oracle for host_vita_log_async.c (ISAAC_VITA_LOG_ASYNC_ORACLE).
 *
 * Fake sinks capture every line with its sink tag, a fake clock drives the
 * drop-report rate bound, and isaac_vita_log_async_oracle_service() is one
 * pass of the logger thread.  Checks: synchronous fallback before start, FIFO
 * order across two interleaved producers on both sinks, byte identity, a
 * 512-byte and a maximal line, an over-long line, an exactly-full ring (last
 * byte used, next line dropped), the dropped=N report and its rate bound,
 * wake-ups only on the empty -> non-empty transition, flush draining
 * everything and forcing the report, and lock discipline.  Hostile cases
 * (review 2026-09-04): a maximal line parked across the ring end at every
 * distance from the end, drained by the logger step and by flush; binary
 * payload bytes (0x00, 0xff); exact-fit after a refused line; the position of
 * the sustained-overload dropped=N report inside a full drain; the over-long
 * printf output written synchronously with exact bytes; a mixed-size
 * two-producer fill to the last byte. */
#include "host_vita_log_async.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CAPTURE_BYTES (4u * 1024u * 1024u)

/* Captured and expected streams share one encoding: tag, length (LE16),
 * bytes.  Equality of the two streams is FIFO order + sink tags + bytes. */
static unsigned char s_got[CAPTURE_BYTES];
static size_t s_got_length;
static uint32_t s_got_lines;
static unsigned char s_expected[CAPTURE_BYTES];
static size_t s_expected_length;
static uint32_t s_expected_lines;

static uint32_t s_checks;
static uint32_t s_failures;

static void check(int condition, const char *what)
{
    ++s_checks;
    if (!condition) {
        ++s_failures;
        fprintf(stderr, "FAIL: %s\n", what);
    }
}

static void stream_append(unsigned char *stream, size_t *length,
                          uint32_t *lines, unsigned tag, const char *line,
                          unsigned count)
{
    if (*length + 3u + count > CAPTURE_BYTES) {
        fprintf(stderr, "capture overflow\n");
        exit(2);
    }
    stream[*length] = (unsigned char)tag;
    stream[*length + 1u] = (unsigned char)(count & 0xffu);
    stream[*length + 2u] = (unsigned char)(count >> 8);
    memcpy(stream + *length + 3u, line, count);
    *length += 3u + count;
    ++*lines;
}

static void file_sink(const char *line, unsigned length)
{
    stream_append(s_got, &s_got_length, &s_got_lines,
                  ISAAC_VITA_LOG_ASYNC_SINK_FILE, line, length);
}

static void printf_sink(const char *line, unsigned length)
{
    stream_append(s_got, &s_got_length, &s_got_lines,
                  ISAAC_VITA_LOG_ASYNC_SINK_PRINTF, line, length);
}

#if defined(ISAAC_VITA_LOG_ASYNC_FILE_BATCH)
/* Use the actual native append/recovery/splitting algorithm, replacing only
 * the kernel operations. The ordinary stream oracle still sees one record. */
static unsigned char s_file_bytes[ISAAC_VITA_LOG_ASYNC_FILE_BATCH_BYTES];
static unsigned s_file_size, s_io_opens, s_io_writes, s_io_closes;
static unsigned s_open_fail_mask, s_write_results_count;
static int s_write_results[16], s_close_fail, s_active_fd;
static unsigned s_batch_calls, s_batch_trace_count;
static unsigned s_batch_trace[256];
static int s_trace_batches;
static int s_batch_enqueue_after_sink;

static void batch_io_reset(void)
{
    s_file_size = s_io_opens = s_io_writes = s_io_closes = 0u;
    s_open_fail_mask = s_write_results_count = 0u;
    s_close_fail = 0;
    s_active_fd = -1;
}

static int batch_open(void)
{
    unsigned call = s_io_opens++;
    check(s_active_fd == -1, "batch: no descriptor overlap");
    if (call < 32u && ((s_open_fail_mask >> call) & 1u))
        return -1;
    s_active_fd = (int)call + 10;
    return s_active_fd;
}

static int batch_write(int fd, const char *data, unsigned length)
{
    unsigned call = s_io_writes++;
    int result = call < s_write_results_count ? s_write_results[call] :
        (int)length;
    check(fd == s_active_fd, "batch: write owns descriptor");
    if (result > (int)length)
        result = (int)length;
    if (result > 0) {
        check(s_file_size + (unsigned)result <= sizeof s_file_bytes,
              "batch: file capture bounded");
        if (s_file_size + (unsigned)result > sizeof s_file_bytes)
            exit(2);
        memcpy(s_file_bytes + s_file_size, data, (unsigned)result);
        s_file_size += (unsigned)result;
    }
    return result;
}

static int batch_close(int fd)
{
    check(fd == s_active_fd, "batch: close owns descriptor exactly once");
    s_active_fd = -1;
    ++s_io_closes;
    return s_close_fail ? -1 : 0;
}

static void batch_debug(const char *line, unsigned length)
{
    check(line[length] == '\0', "batch: each debug record terminated");
    file_sink(line, length);
}

#define ISAAC_LOG_BATCH_OPEN() batch_open()
#define ISAAC_LOG_BATCH_WRITE(fd, data, size) batch_write(fd, data, size)
#define ISAAC_LOG_BATCH_CLOSE(fd) batch_close(fd)
#define ISAAC_LOG_BATCH_DEBUG(line, size) batch_debug(line, size)
#include "host_vita_log_file_batch_private.h"
#undef ISAAC_LOG_BATCH_OPEN
#undef ISAAC_LOG_BATCH_WRITE
#undef ISAAC_LOG_BATCH_CLOSE
#undef ISAAC_LOG_BATCH_DEBUG

static void file_batch_sink(char *payload, const uint16_t *lengths,
                             unsigned records)
{
    static char original[ISAAC_VITA_LOG_ASYNC_FILE_BATCH_BYTES + 1u];
    unsigned total = 0u;
    unsigned i;
    ++s_batch_calls;
    if (s_trace_batches) {
        check(s_batch_trace_count < 256u, "batch: bounded test trace");
        s_batch_trace[s_batch_trace_count++] = records;
    }
    for (i = 0u; i < records; ++i)
        total += lengths[i];
    check(records > 0u && records <= ISAAC_VITA_LOG_ASYNC_FILE_BATCH_RECORDS,
          "batch: record cap");
    check(total <= ISAAC_VITA_LOG_ASYNC_FILE_BATCH_BYTES, "batch: byte cap");
    memcpy(original, payload, total + 1u);
    batch_io_reset();
    isaac_vita_log_file_batch_emit(payload, lengths, records);
    check(s_io_opens == 1u && s_io_writes == 1u && s_io_closes == 1u,
          "batch: one successful file transaction");
    check(s_file_size == total && memcmp(s_file_bytes, original, total) == 0,
          "batch: exact durable payload including binary bytes");
    check(memcmp(payload, original, total + 1u) == 0,
          "batch: debug terminators restored");
    if (s_batch_enqueue_after_sink) {
        s_batch_enqueue_after_sink = 0;
        check(isaac_vita_log_async_enqueue(0u, "later", 5u) == 1,
              "batch: producer enqueue during sink");
    }
}
#endif

static void expect(unsigned tag, const char *line, unsigned length)
{
    stream_append(s_expected, &s_expected_length, &s_expected_lines, tag,
                  line, length);
}

static void expect_string(unsigned tag, const char *line)
{
    expect(tag, line, (unsigned)strlen(line));
}

static int streams_equal(void)
{
    return s_got_length == s_expected_length &&
        s_got_lines == s_expected_lines &&
        memcmp(s_got, s_expected, s_got_length) == 0;
}

static void streams_reset(void)
{
    s_got_length = 0u;
    s_got_lines = 0u;
    s_expected_length = 0u;
    s_expected_lines = 0u;
}

static IsaacVitaLogAsyncStats stats(void)
{
    IsaacVitaLogAsyncStats value;

    isaac_vita_log_async_get_stats(&value);
    return value;
}

/* Producer A: an isaac_vita_log style line (FILE sink, enqueue API). */
static int producer_a(unsigned index)
{
    char line[128];
    int length = snprintf(line, sizeof line,
                          "[%u.%03u thr 0x%x] audio SLOT %u refill=%u\n",
                          index / 1000u, index % 1000u, 0x40010003u, index,
                          index * 7u);
    int result = isaac_vita_log_async_enqueue(
        ISAAC_VITA_LOG_ASYNC_SINK_FILE, line, (unsigned)length);

    if (result == 1)
        expect(ISAAC_VITA_LOG_ASYNC_SINK_FILE, line, (unsigned)length);
    return result;
}

/* Producer B: a ph120 record through the printf path (PRINTF sink). */
static void producer_b(unsigned index)
{
    char reference[512];
    int length = snprintf(
        reference, sizeof reference,
        "[kage-vita] ph120.t bid=%.32s win=%u loops=%u us(50,95,max) "
        "svc=%u/%u/%u all=%u/%u/%u bad=%u clamp=%u\n",
        "perf:log-async-oracle-bid-longer-than-32", index, index * 120u,
        1u, 2u, 3u, 16000u + index, 17000u, 4294967295u, 0u, 0u);

    isaac_vita_log_async_printf(
        "[kage-vita] ph120.t bid=%.32s win=%u loops=%u us(50,95,max) "
        "svc=%u/%u/%u all=%u/%u/%u bad=%u clamp=%u\n",
        "perf:log-async-oracle-bid-longer-than-32", index, index * 120u,
        1u, 2u, 3u, 16000u + index, 17000u, 4294967295u, 0u, 0u);
    expect(ISAAC_VITA_LOG_ASYNC_SINK_PRINTF, reference, (unsigned)length);
}

/* ------------------------------------------------------------------------
 * Hostile cases. */
static uint32_t ring_offset(void)
{
    uint32_t head, tail, used;

    isaac_vita_log_async_oracle_state(&head, &tail, &used);
    if (used != 0u || head != tail)
        check(0, "ring empty with head == tail");
    return tail;
}

/* Queue one filler line of `length` bytes and drain it, so head == tail
 * advance together by HEADER + length. */
static void park_filler(uint32_t length, unsigned char *scratch)
{
    unsigned i;

    for (i = 0u; i < length; ++i)
        scratch[i] = (unsigned char)('a' + (i % 26u));
    /* Thousands of fillers per sweep: count only a failure. */
    if (isaac_vita_log_async_enqueue(ISAAC_VITA_LOG_ASYNC_SINK_FILE,
                                     (const char *)scratch, length) != 1)
        check(0, "filler queued");
    expect(ISAAC_VITA_LOG_ASYNC_SINK_FILE, (const char *)scratch, length);
    if (isaac_vita_log_async_oracle_service() != 1u)
        check(0, "filler drained");
}

/* Move the empty ring's cursor to exactly `target` (an absolute offset). */
static void park_at(uint32_t target, unsigned char *scratch)
{
    uint32_t guard = 0u;

    for (;;) {
        uint32_t offset = ring_offset();
        uint32_t remaining;
        uint32_t length;

        if (offset == target)
            return;
        remaining = (target + ISAAC_VITA_LOG_ASYNC_RING_BYTES - offset) %
            ISAAC_VITA_LOG_ASYNC_RING_BYTES;
        if (remaining >= ISAAC_VITA_LOG_ASYNC_HEADER_BYTES + 1u &&
                remaining <= ISAAC_VITA_LOG_ASYNC_HEADER_BYTES +
                    ISAAC_VITA_LOG_ASYNC_LINE_MAX)
            length = remaining - ISAAC_VITA_LOG_ASYNC_HEADER_BYTES;
        else
            length = ISAAC_VITA_LOG_ASYNC_LINE_MAX;
        park_filler(length, scratch);
        if (++guard > 4u * ISAAC_VITA_LOG_ASYNC_RING_BYTES / 1028u + 8u) {
            check(0, "park_at did not converge");
            return;
        }
    }
}

/* 9. A maximal line whose header and/or payload straddle the ring end, at
 *    every distance k (1..HEADER+LINE_MAX+3) from the end; followed by a
 *    1-byte line and a binary line (0x00 and 0xff inside) so the wrap of the
 *    *next* header is covered too.  Even k: logger step; odd k: flush. */
static void hostile_straddle_sweep(const char *big)
{
    static unsigned char scratch[ISAAC_VITA_LOG_ASYNC_LINE_MAX];
    static char binary[700];
    uint32_t k;
    unsigned i;

    for (i = 0u; i < sizeof binary; ++i)
        binary[i] = (char)(i % 256u);
    binary[0] = '\0';
    binary[1] = (char)0xff;
    binary[sizeof binary - 1u] = '\n';

    streams_reset();
    for (k = 1u; k <= ISAAC_VITA_LOG_ASYNC_HEADER_BYTES +
                     ISAAC_VITA_LOG_ASYNC_LINE_MAX + 3u; ++k) {
        uint32_t head, tail, used;

        park_at(ISAAC_VITA_LOG_ASYNC_RING_BYTES - k, scratch);
        streams_reset();
        check(isaac_vita_log_async_enqueue(ISAAC_VITA_LOG_ASYNC_SINK_PRINTF,
                                           big,
                                           ISAAC_VITA_LOG_ASYNC_LINE_MAX)
                  == 1,
              "straddle: maximal line queued");
        expect(ISAAC_VITA_LOG_ASYNC_SINK_PRINTF, big,
               ISAAC_VITA_LOG_ASYNC_LINE_MAX);
        check(isaac_vita_log_async_enqueue(ISAAC_VITA_LOG_ASYNC_SINK_FILE,
                                           "\n", 1u) == 1,
              "straddle: 1-byte line queued");
        expect(ISAAC_VITA_LOG_ASYNC_SINK_FILE, "\n", 1u);
        check(isaac_vita_log_async_enqueue(ISAAC_VITA_LOG_ASYNC_SINK_FILE,
                                           binary, (unsigned)sizeof binary)
                  == 1,
              "straddle: binary line queued");
        expect(ISAAC_VITA_LOG_ASYNC_SINK_FILE, binary,
               (unsigned)sizeof binary);
        isaac_vita_log_async_oracle_state(&head, &tail, &used);
        check(used == 3u * ISAAC_VITA_LOG_ASYNC_HEADER_BYTES +
                  ISAAC_VITA_LOG_ASYNC_LINE_MAX + 1u + sizeof binary,
              "straddle: used counts the three lines");
        check(tail < head, "straddle: tail wrapped past the ring end");
        if (k & 1u)
            isaac_vita_log_async_flush();
        else
            check(isaac_vita_log_async_oracle_service() == 3u,
                  "straddle: service drained three lines");
        if (!streams_equal()) {
            char what[96];

            snprintf(what, sizeof what,
                     "straddle: bytes/order at distance %u from the ring end",
                     (unsigned)k);
            check(0, what);
        } else {
            check(1, "straddle ok");
        }
        check(stats().used == 0u, "straddle: ring empty");
    }
    streams_reset();
}

/* 10. Exact fit after a refused line: fill to RING - (HEADER + L); a line of
 *     L + 1 bytes is refused and counted, a line of L bytes then fills the
 *     last byte; the drain returns every accepted byte in order. */
static void hostile_exact_fit_after_drop(void)
{
    static unsigned char scratch[ISAAC_VITA_LOG_ASYNC_LINE_MAX + 1u];
    const uint32_t L = 300u;
    uint32_t target = ISAAC_VITA_LOG_ASYNC_RING_BYTES -
        (ISAAC_VITA_LOG_ASYNC_HEADER_BYTES + L);
    uint32_t dropped_before = stats().dropped;
    uint32_t lines = 0u;
    unsigned i;

    for (i = 0u; i < sizeof scratch; ++i)
        scratch[i] = (unsigned char)(0x20u + (i % 90u));
    streams_reset();
    /* Start from an odd cursor so the fill wraps the ring end. */
    park_at(7u, scratch);
    streams_reset();
    while (stats().used + ISAAC_VITA_LOG_ASYNC_HEADER_BYTES +
               ISAAC_VITA_LOG_ASYNC_LINE_MAX <= target) {
        check(isaac_vita_log_async_enqueue(ISAAC_VITA_LOG_ASYNC_SINK_FILE,
                                           (const char *)scratch,
                                           ISAAC_VITA_LOG_ASYNC_LINE_MAX)
                  == 1,
              "exact-fit: fill line queued");
        expect(ISAAC_VITA_LOG_ASYNC_SINK_FILE, (const char *)scratch,
               ISAAC_VITA_LOG_ASYNC_LINE_MAX);
        ++lines;
    }
    if (stats().used < target) {
        uint32_t rest = target - stats().used -
            ISAAC_VITA_LOG_ASYNC_HEADER_BYTES;

        check(rest >= 1u && rest <= ISAAC_VITA_LOG_ASYNC_LINE_MAX,
              "exact-fit: remainder is one line");
        check(isaac_vita_log_async_enqueue(ISAAC_VITA_LOG_ASYNC_SINK_PRINTF,
                                           (const char *)scratch, rest) == 1,
              "exact-fit: remainder queued");
        expect(ISAAC_VITA_LOG_ASYNC_SINK_PRINTF, (const char *)scratch, rest);
        ++lines;
    }
    check(stats().used == target, "exact-fit: HEADER + L bytes free");
    check(isaac_vita_log_async_enqueue(ISAAC_VITA_LOG_ASYNC_SINK_FILE,
                                       (const char *)scratch, L + 1u) == -1,
          "exact-fit: one byte too many is refused");
    check(stats().dropped == dropped_before + 1u,
          "exact-fit: the refusal is counted once");
    check(stats().used == target, "exact-fit: a refusal consumes nothing");
    check(isaac_vita_log_async_enqueue(ISAAC_VITA_LOG_ASYNC_SINK_FILE,
                                       (const char *)scratch, L) == 1,
          "exact-fit: L bytes fill the last byte");
    expect(ISAAC_VITA_LOG_ASYNC_SINK_FILE, (const char *)scratch, L);
    ++lines;
    check(stats().used == ISAAC_VITA_LOG_ASYNC_RING_BYTES,
          "exact-fit: ring full to the last byte");
    check(isaac_vita_log_async_enqueue(ISAAC_VITA_LOG_ASYNC_SINK_PRINTF,
                                       "\n", 1u) == -1,
          "exact-fit: full ring refuses one byte");
    check(isaac_vita_log_async_oracle_service() == lines,
          "exact-fit: drained every accepted line");
    /* Two refusals since the last report and the clock has not advanced:
     * the report waits for the rate bound. */
    check(streams_equal(), "exact-fit: bytes and order intact");
    g_isaac_vita_log_async_oracle_now_us += ISAAC_VITA_LOG_ASYNC_DROP_REPORT_US;
    check(isaac_vita_log_async_oracle_service() == 0u, "exact-fit: idle");
    {
        char report[64];

        snprintf(report, sizeof report, "[kage-vita] log-async dropped=%u\n",
                 (unsigned)(dropped_before + 2u));
        expect_string(ISAAC_VITA_LOG_ASYNC_SINK_PRINTF, report);
    }
    check(streams_equal(), "exact-fit: one report for two refusals");
    streams_reset();
}

/* 11. Sustained overload: the ring is full, a line was refused, the rate
 *     bound has elapsed.  One logger pass must place the dropped=N report
 *     after exactly 64 sunk lines (the in-loop report) and not repeat it at
 *     the end; a later refusal inside the bound stays silent until the clock
 *     advances. */
static void hostile_overload_report_position(void)
{
    uint32_t entries = ISAAC_VITA_LOG_ASYNC_RING_BYTES / 64u;
    uint32_t dropped_before = stats().dropped;
    char line[61];
    char report[64];
    unsigned i;

    streams_reset();
    for (i = 0u; i < entries; ++i) {
        int length = snprintf(line, sizeof line,
                              "[kage-vita] load %08u %033u\n", i, i);

        check(length == 60, "overload: fill line is 60 bytes");
        check(isaac_vita_log_async_enqueue(ISAAC_VITA_LOG_ASYNC_SINK_PRINTF,
                                           line, 60u) == 1,
              "overload: fill line queued");
        expect(ISAAC_VITA_LOG_ASYNC_SINK_PRINTF, line, 60u);
        if (i == 63u) {
            snprintf(report, sizeof report,
                     "[kage-vita] log-async dropped=%u\n",
                     (unsigned)(dropped_before + 1u));
            expect_string(ISAAC_VITA_LOG_ASYNC_SINK_PRINTF, report);
        }
    }
    check(stats().used == ISAAC_VITA_LOG_ASYNC_RING_BYTES,
          "overload: ring full");
    check(isaac_vita_log_async_enqueue(ISAAC_VITA_LOG_ASYNC_SINK_FILE,
                                       "late\n", 5u) == -1,
          "overload: refused");
    g_isaac_vita_log_async_oracle_now_us += ISAAC_VITA_LOG_ASYNC_DROP_REPORT_US;
    check(isaac_vita_log_async_oracle_service() == entries,
          "overload: one pass drained the ring");
    check(entries > 64u, "overload: the ring holds more than 64 lines");
    check(streams_equal(),
          "overload: report after exactly 64 lines and not repeated");
    streams_reset();
    /* Inside the bound: silent.  After it: reported once with the new count. */
    check(isaac_vita_log_async_enqueue(ISAAC_VITA_LOG_ASYNC_SINK_FILE,
                                       "x\n", 2u) == 1,
          "overload: line queued after the drain");
    expect(ISAAC_VITA_LOG_ASYNC_SINK_FILE, "x\n", 2u);
    check(isaac_vita_log_async_enqueue(7u, "x\n", 2u) == -1,
          "overload: refused inside the bound");
    check(isaac_vita_log_async_oracle_service() == 1u, "overload: drained 1");
    check(streams_equal(), "overload: no report inside the rate bound");
    g_isaac_vita_log_async_oracle_now_us += ISAAC_VITA_LOG_ASYNC_DROP_REPORT_US;
    check(isaac_vita_log_async_oracle_service() == 0u, "overload: idle");
    snprintf(report, sizeof report, "[kage-vita] log-async dropped=%u\n",
             (unsigned)(dropped_before + 2u));
    expect_string(ISAAC_VITA_LOG_ASYNC_SINK_PRINTF, report);
    check(streams_equal(), "overload: reported once after the bound");
    streams_reset();
}

/* 12. printf output that does not fit one queued line is written
 *     synchronously with exact bytes after the queued lines (program order
 *     holds; not queued, not truncated, not counted as a drop); output of
 *     exactly PRINTF_BYTES - 1 bytes is queued intact with its newline. */
static void hostile_printf_overflow(void)
{
    static char text[2048];
    static char reference[2048];
    uint32_t enqueued_before = stats().enqueued;
    uint32_t dropped_before = stats().dropped;
    unsigned i;
    int length;

    for (i = 0u; i < sizeof text - 1u; ++i)
        text[i] = (char)('A' + (i % 26u));
    text[sizeof text - 1u] = '\0';
    streams_reset();
    /* A queued line first: it must reach the sink before the long one. */
    check(isaac_vita_log_async_enqueue(ISAAC_VITA_LOG_ASYNC_SINK_FILE,
                                       "[1.000 thr 0x1] before\n", 23u) == 1,
          "overflow: earlier line queued");
    expect(ISAAC_VITA_LOG_ASYNC_SINK_FILE, "[1.000 thr 0x1] before\n", 23u);
    ++enqueued_before;
    /* 1500 characters + newline: longer than any queued line. */
    text[1500] = '\0';
    length = snprintf(reference, sizeof reference,
                      "[kage-vita] long=%s end\n", text);
    isaac_vita_log_async_printf("[kage-vita] long=%s end\n", text);
    expect(ISAAC_VITA_LOG_ASYNC_SINK_PRINTF, reference, (unsigned)length);
    check(s_got_lines == 2u,
          "overflow: queued line then the long line, both written at once");
    check(streams_equal(), "overflow: exact bytes in program order");
    check(stats().enqueued == enqueued_before, "overflow: not queued");
    check(stats().dropped == dropped_before, "overflow: not a drop");
    check(stats().used == 0u, "overflow: ring untouched");
    streams_reset();
    /* Exactly PRINTF_BYTES - 1 bytes: the largest queued printf line. */
    text[ISAAC_VITA_LOG_ASYNC_PRINTF_BYTES - 1u - 1u] = '\0';
    length = snprintf(reference, sizeof reference, "%s\n", text);
    check(length == (int)ISAAC_VITA_LOG_ASYNC_PRINTF_BYTES - 1,
          "overflow: boundary line is PRINTF_BYTES - 1 bytes");
    isaac_vita_log_async_printf("%s\n", text);
    expect(ISAAC_VITA_LOG_ASYNC_SINK_PRINTF, reference, (unsigned)length);
    check(s_got_lines == 0u, "overflow: boundary line queued, not written");
    check(stats().enqueued == enqueued_before + 1u,
          "overflow: boundary line counted as queued");
    check(isaac_vita_log_async_oracle_service() == 1u,
          "overflow: boundary line drained");
    check(streams_equal(), "overflow: boundary line intact with newline");
    /* One byte more: synchronous again. */
    text[ISAAC_VITA_LOG_ASYNC_PRINTF_BYTES - 1u - 1u] = 'Z';
    text[ISAAC_VITA_LOG_ASYNC_PRINTF_BYTES - 1u] = '\0';
    length = snprintf(reference, sizeof reference, "%s\n", text);
    check(length == (int)ISAAC_VITA_LOG_ASYNC_PRINTF_BYTES,
          "overflow: the next line is PRINTF_BYTES bytes");
    isaac_vita_log_async_printf("%s\n", text);
    expect(ISAAC_VITA_LOG_ASYNC_SINK_PRINTF, reference, (unsigned)length);
    check(s_got_lines == 2u, "overflow: PRINTF_BYTES bytes written at once");
    check(stats().used == 0u, "overflow: nothing left queued");
    check(streams_equal(), "overflow: PRINTF_BYTES bytes exact");
    check(stats().enqueued == enqueued_before + 1u,
          "overflow: PRINTF_BYTES bytes not queued");
    streams_reset();
}

/* 13. Two producers with extreme sizes (1-byte PRINTF records and maximal
 *     FILE lines) alternate until the ring refuses; exactly the refused line
 *     is missing and every accepted line comes back in program order. */
static void hostile_mixed_fill(void)
{
    static char maximal[ISAAC_VITA_LOG_ASYNC_LINE_MAX];
    uint32_t dropped_before = stats().dropped;
    uint32_t accepted = 0u;
    unsigned i;

    for (i = 0u; i < sizeof maximal; ++i)
        maximal[i] = (char)(0xff - (i % 255u));
    streams_reset();
    for (i = 0u; ; ++i) {
        char tiny = (char)('0' + (i % 10u));
        int result;

        if (i & 1u) {
            result = isaac_vita_log_async_enqueue(
                ISAAC_VITA_LOG_ASYNC_SINK_FILE, maximal,
                (unsigned)sizeof maximal);
            if (result == 1)
                expect(ISAAC_VITA_LOG_ASYNC_SINK_FILE, maximal,
                       (unsigned)sizeof maximal);
        } else {
            result = isaac_vita_log_async_enqueue(
                ISAAC_VITA_LOG_ASYNC_SINK_PRINTF, &tiny, 1u);
            if (result == 1)
                expect(ISAAC_VITA_LOG_ASYNC_SINK_PRINTF, &tiny, 1u);
        }
        if (result != 1)
            break;
        ++accepted;
        if (i > 4u * ISAAC_VITA_LOG_ASYNC_RING_BYTES) {
            check(0, "mixed: ring never refused");
            break;
        }
    }
    check(stats().dropped == dropped_before + 1u, "mixed: one refusal");
    check(stats().used + ISAAC_VITA_LOG_ASYNC_HEADER_BYTES +
              ISAAC_VITA_LOG_ASYNC_LINE_MAX > ISAAC_VITA_LOG_ASYNC_RING_BYTES,
          "mixed: the refusal was for a maximal line that did not fit");
    check(isaac_vita_log_async_enqueue(ISAAC_VITA_LOG_ASYNC_SINK_PRINTF,
                                       "\n", 1u) == 1,
          "mixed: a 1-byte line still fits after the refusal");
    expect(ISAAC_VITA_LOG_ASYNC_SINK_PRINTF, "\n", 1u);
    ++accepted;
    isaac_vita_log_async_flush();
    check(s_got_lines == accepted + 1u,
          "mixed: flush wrote every accepted line plus the forced report");
    {
        char report[64];

        snprintf(report, sizeof report, "[kage-vita] log-async dropped=%u\n",
                 (unsigned)(dropped_before + 1u));
        expect_string(ISAAC_VITA_LOG_ASYNC_SINK_PRINTF, report);
    }
    check(streams_equal(), "mixed: program order and bytes across producers");
    streams_reset();
}

#if defined(ISAAC_VITA_LOG_ASYNC_FILE_BATCH)
static void batch_boundaries(void)
{
    char line[ISAAC_VITA_LOG_ASYNC_LINE_MAX];
    unsigned i;
    uint32_t calls;
    uint32_t dropped;
    memset(line, 'B', sizeof line);
    streams_reset();
    s_trace_batches = 1;
    s_batch_trace_count = 0u;
    /* Eight-record cap, then a PRINTF barrier and the exact 4096-byte cap. */
    for (i = 0u; i < 9u; ++i) {
        check(isaac_vita_log_async_enqueue(0u, "x", 1u) == 1,
              "batch: short queued");
        expect(0u, "x", 1u);
    }
    check(isaac_vita_log_async_enqueue(1u, "P", 1u) == 1, "batch: barrier");
    expect(1u, "P", 1u);
    for (i = 0u; i < 5u; ++i) {
        check(isaac_vita_log_async_enqueue(0u, line, sizeof line) == 1,
              "batch: maximal queued");
        expect(0u, line, sizeof line);
    }
    check(isaac_vita_log_async_oracle_service() == 15u, "batch: immediate drain");
    check(streams_equal(), "batch: caps and PRINTF FIFO");
    check(s_batch_trace_count == 4u && s_batch_trace[0] == 8u &&
          s_batch_trace[1] == 1u && s_batch_trace[2] == 4u &&
          s_batch_trace[3] == 1u, "batch: exact record/byte boundaries");
    streams_reset();
    s_batch_trace_count = 0u;
    /* The drop-report check is after record 64, even within a FILE run. */
    dropped = stats().dropped;
    expect(1u, "P", 1u);
    check(isaac_vita_log_async_enqueue(1u, "P", 1u) == 1, "batch: offset");
    for (i = 0u; i < 66u; ++i) {
        check(isaac_vita_log_async_enqueue(0u, "F", 1u) == 1, "batch: long run");
        expect(0u, "F", 1u);
        if (i == 62u) {
            char report[64];
            snprintf(report, sizeof report, "[kage-vita] log-async dropped=%u\n",
                     (unsigned)(dropped + 1u));
            expect_string(1u, report);
        }
    }
    check(isaac_vita_log_async_enqueue(7u, "bad", 3u) == -1,
          "batch: refused record before drain");
    g_isaac_vita_log_async_oracle_now_us += ISAAC_VITA_LOG_ASYNC_DROP_REPORT_US;
    isaac_vita_log_async_flush();
    check(streams_equal(), "batch: flush exact FILE run");
    check(s_batch_trace_count == 9u && s_batch_trace[7] == 7u &&
          s_batch_trace[8] == 3u, "batch: never straddles drop-report boundary");
    streams_reset();
    s_batch_trace_count = 0u;
    check(isaac_vita_log_async_enqueue(0u, "first", 5u) == 1,
          "batch: one queued record does not wait");
    expect(0u, "first", 5u);
    expect(0u, "later", 5u);
    s_batch_enqueue_after_sink = 1;
    check(isaac_vita_log_async_oracle_service() == 2u,
          "batch: enqueue during sink drained without delayed pending state");
    check(streams_equal() && s_batch_trace_count == 2u &&
          s_batch_trace[0] == 1u && s_batch_trace[1] == 1u,
          "batch: only already queued records in each group");
    streams_reset();
    s_trace_batches = 0;
    /* Exceptional no-lock fallback remains the original per-record sink. */
    calls = s_batch_calls;
    for (i = 0u; i < 3u; ++i) {
        check(isaac_vita_log_async_enqueue(i & 1u, "F", 1u) == 1,
              "batch: fallback queued");
        expect(i & 1u, "F", 1u);
    }
    g_isaac_vita_log_async_oracle_sink_lock_fail = 1;
    isaac_vita_log_async_flush();
    g_isaac_vita_log_async_oracle_sink_lock_fail = 0;
    check(s_batch_calls == calls && streams_equal(), "batch: original no-lock path");
    streams_reset();
}

static void batch_failures(void)
{
    static const uint16_t lengths[] = {5u, 4u, 6u};
    unsigned mode;
    for (mode = 0u; mode < 12u; ++mode) {
        char payload[] = "ABCDEfghiJKLMNO";
        const char *expected_file = payload;
        unsigned expected_size = 15u;
        unsigned expected_opens = 1u, expected_writes = 1u, expected_closes = 1u;
        unsigned i;
        streams_reset();
        batch_io_reset();
        if (mode == 1u) { /* Every positive short write progresses. */
            for (i = 0u; i < 8u; ++i) s_write_results[i] = 2;
            s_write_results_count = 8u;
            expected_writes = 8u;
        } else if (mode == 2u || mode == 3u || mode == 8u ||
                   mode == 10u || mode == 11u) {
            int at_boundary = mode == 3u || mode == 11u;
            s_write_results[0] = at_boundary ? 5 : 2;
            s_write_results[1] = mode >= 10u ? 0 : -1;
            s_write_results_count = 2u;
            expected_opens = at_boundary ? 3u : 4u;
            expected_writes = at_boundary ? 4u : 5u;
            expected_closes = expected_opens;
            if (mode == 8u) { /* A fallback suffix gets only one attempt. */
                s_write_results[2] = 1;
                s_write_results_count = 3u;
                expected_file = "ABCfghiJKLMNO";
                expected_size = 13u;
            }
        } else if (mode == 4u || mode == 5u) {
            s_open_fail_mask = mode == 4u ? 1u : 0xffffffffu;
            expected_opens = 4u;
            expected_writes = expected_closes = mode == 4u ? 3u : 0u;
            if (mode == 5u) expected_size = 0u;
        } else if (mode == 6u || mode == 7u) {
            s_write_results_count = mode == 6u ? 1u : 4u;
            for (i = 0u; i < s_write_results_count; ++i)
                s_write_results[i] = mode == 6u ? 0 : -1;
            expected_opens = expected_writes = expected_closes = 4u;
            if (mode == 7u) expected_size = 0u;
        } else if (mode == 9u) {
            s_close_fail = 1;
        }
        expect(0u, "ABCDE", 5u);
        expect(0u, "fghi", 4u);
        expect(0u, "JKLMNO", 6u);
        isaac_vita_log_file_batch_emit(payload, lengths, 3u);
        check(s_file_size == expected_size &&
              memcmp(s_file_bytes, expected_file, expected_size) == 0,
              "batch failure: accepted prefix never replayed");
        check(s_io_opens == expected_opens && s_io_writes == expected_writes &&
              s_io_closes == expected_closes && s_active_fd == -1,
              "batch failure: bounded recovery and balanced close");
        check(streams_equal(), "batch failure: every debug record exactly once");
        check(memcmp(payload, "ABCDEfghiJKLMNO", sizeof payload) == 0,
              "batch failure: payload restored");
    }
    streams_reset();
}
#endif

int main(void)
{
    static char big[ISAAC_VITA_LOG_ASYNC_LINE_MAX + 1u];
    IsaacVitaLogAsyncStats s;
    /* Producer pairs for the FIFO test: about 210 bytes per pair, so a
     * small ring (the CMake knob) still holds every line. */
    uint32_t pairs = ISAAC_VITA_LOG_ASYNC_RING_BYTES / 1024u;
    uint32_t wakes;
    uint32_t sunk;
    unsigned i;

    if (pairs > 200u)
        pairs = 200u;

    g_isaac_vita_log_async_oracle_file_sink = file_sink;
    g_isaac_vita_log_async_oracle_printf_sink = printf_sink;
#if defined(ISAAC_VITA_LOG_ASYNC_FILE_BATCH)
    g_isaac_vita_log_async_oracle_file_batch_sink = file_batch_sink;
#endif
    g_isaac_vita_log_async_oracle_now_us = 5000000u;

    /* 1. Before start: enqueue refuses (caller writes synchronously) and the
     *    printf path writes synchronously itself; nothing is queued. */
    check(!isaac_vita_log_async_started(), "not started initially");
    check(isaac_vita_log_async_enqueue(ISAAC_VITA_LOG_ASYNC_SINK_FILE,
                                       "early\n", 6u) == 0,
          "enqueue before start returns 0 (synchronous fallback)");
    isaac_vita_log_async_printf("[kage-vita] boot %u\n", 42u);
    expect_string(ISAAC_VITA_LOG_ASYNC_SINK_PRINTF, "[kage-vita] boot 42\n");
    check(streams_equal(), "printf before start reaches the sink at once");
    check(s_got_lines == 1u, "exactly one synchronous line before start");
    isaac_vita_log_async_flush();
    check(s_got_lines == 1u, "flush before start is a no-op");
    check(g_isaac_vita_log_async_oracle_wakes == 0u, "no wake before start");
    s = stats();
    check(s.started == 0u && s.enqueued == 0u && s.dropped == 0u,
          "stats before start");
    streams_reset();

    /* 2. Start (idempotent). */
    check(isaac_vita_log_async_start() == 1, "start");
    check(isaac_vita_log_async_start() == 1, "start twice");
    check(isaac_vita_log_async_started(), "started");
    s = stats();
    check(s.started == 1u && s.start_result == 0u &&
              s.thread_priority == ISAAC_VITA_LOG_ASYNC_THREAD_PRIORITY,
          "start stats");

    /* 3. Two interleaved producers on both sinks: nothing reaches a sink
     *    until the logger runs; then the exact bytes in program order. */
    wakes = g_isaac_vita_log_async_oracle_wakes;
    for (i = 0u; i < pairs; ++i) {
        check(producer_a(i) == 1, "producer A queued");
        producer_b(i);
    }
    check(s_got_lines == 0u, "queued lines are not written by producers");
    check(g_isaac_vita_log_async_oracle_wakes == wakes + 1u,
          "one wake for the empty -> non-empty transition");
    check(stats().used > 0u, "ring holds the queued bytes");
    sunk = isaac_vita_log_async_oracle_service();
    check(sunk == 2u * pairs, "service drained both producers");
    check(streams_equal(), "FIFO order, sink tags and bytes across producers");
    check(stats().used == 0u, "ring empty after service");
    check(isaac_vita_log_async_oracle_service() == 0u, "idle service");
    streams_reset();

    /* 4. A 512-byte line, a maximal line and an over-long line (dropped and
     *    counted, then reported once by the logger). */
    for (i = 0u; i < sizeof big; ++i)
        big[i] = (char)(1u + (i * 7u) % 250u);
    check(isaac_vita_log_async_enqueue(ISAAC_VITA_LOG_ASYNC_SINK_FILE, big,
                                       512u) == 1,
          "512-byte line queued");
    expect(ISAAC_VITA_LOG_ASYNC_SINK_FILE, big, 512u);
    check(isaac_vita_log_async_enqueue(ISAAC_VITA_LOG_ASYNC_SINK_PRINTF, big,
                                       ISAAC_VITA_LOG_ASYNC_LINE_MAX) == 1,
          "maximal line queued");
    expect(ISAAC_VITA_LOG_ASYNC_SINK_PRINTF, big,
           ISAAC_VITA_LOG_ASYNC_LINE_MAX);
    check(isaac_vita_log_async_enqueue(ISAAC_VITA_LOG_ASYNC_SINK_FILE, big,
                                       ISAAC_VITA_LOG_ASYNC_LINE_MAX + 1u)
              == -1,
          "over-long line dropped");
    check(isaac_vita_log_async_enqueue(7u, "x\n", 2u) == -1,
          "unknown sink dropped");
    check(isaac_vita_log_async_enqueue(ISAAC_VITA_LOG_ASYNC_SINK_FILE, NULL,
                                       0u) == 1,
          "empty line accepted as a no-op");
    check(stats().dropped == 2u, "two drops counted");
    check(isaac_vita_log_async_oracle_service() == 2u, "service drained 2");
    expect_string(ISAAC_VITA_LOG_ASYNC_SINK_PRINTF,
                  "[kage-vita] log-async dropped=2\n");
    check(streams_equal(), "512/maximal bytes intact, first drop report");
    streams_reset();

    /* 5. Exactly-full ring: 4-byte header + 60-byte line = 64 bytes per
     *    entry, so RING/64 entries fill the last byte (head is already past
     *    offset 0 after 3 and 4, so the fill also wraps the ring end).  The
     *    next line of any size is dropped; the count changes but the report
     *    is rate bounded until the clock advances. */
    {
        uint32_t entries = ISAAC_VITA_LOG_ASYNC_RING_BYTES / 64u;
        char line[61];
        int all_queued = 1;
        int all_expected = 1;

        wakes = g_isaac_vita_log_async_oracle_wakes;
        for (i = 0u; i < entries; ++i) {
            /* 17 + 8 + 1 + 33 + 1 = 60 bytes. */
            int length = snprintf(line, sizeof line,
                                  "[kage-vita] fill %08u %033u\n", i, i);
            if (length != 60)
                all_expected = 0;
            if (isaac_vita_log_async_enqueue(
                    ISAAC_VITA_LOG_ASYNC_SINK_PRINTF, line, 60u) != 1)
                all_queued = 0;
            expect(ISAAC_VITA_LOG_ASYNC_SINK_PRINTF, line, 60u);
        }
        check(all_expected, "fill line is exactly 60 bytes");
        check(all_queued, "every fill line queued");
        s = stats();
        check(s.used == ISAAC_VITA_LOG_ASYNC_RING_BYTES,
              "ring exactly full");
        check(s.high_water == ISAAC_VITA_LOG_ASYNC_RING_BYTES,
              "high water is the full ring");
        check(g_isaac_vita_log_async_oracle_wakes == wakes + 1u,
              "one wake for the whole fill");
        check(isaac_vita_log_async_enqueue(ISAAC_VITA_LOG_ASYNC_SINK_FILE,
                                           "x\n", 2u) == -1,
              "line into a full ring dropped");
        check(stats().dropped == 3u, "third drop counted");
        check(isaac_vita_log_async_oracle_service() == entries,
              "service drained the full ring");
        check(streams_equal(),
              "full-ring lines in order, no report within the rate bound");
        g_isaac_vita_log_async_oracle_now_us +=
            ISAAC_VITA_LOG_ASYNC_DROP_REPORT_US;
        check(isaac_vita_log_async_oracle_service() == 0u,
              "idle service after the clock advanced");
        expect_string(ISAAC_VITA_LOG_ASYNC_SINK_PRINTF,
                      "[kage-vita] log-async dropped=3\n");
        check(streams_equal(), "report once the rate bound elapsed");
        check(stats().used == 0u, "ring empty after the full drain");
        streams_reset();
    }

    /* 6. Flush drains everything on the caller thread and forces the report
     *    of a changed count regardless of the clock. */
    wakes = g_isaac_vita_log_async_oracle_wakes;
    for (i = 0u; i < 10u; ++i)
        check(producer_a(1000u + i) == 1, "flush producer queued");
    check(g_isaac_vita_log_async_oracle_wakes == wakes + 1u,
          "one wake before flush");
    check(isaac_vita_log_async_enqueue(ISAAC_VITA_LOG_ASYNC_SINK_FILE, big,
                                       ISAAC_VITA_LOG_ASYNC_LINE_MAX + 1u)
              == -1,
          "over-long line dropped before flush");
    isaac_vita_log_async_flush();
    expect_string(ISAAC_VITA_LOG_ASYNC_SINK_PRINTF,
                  "[kage-vita] log-async dropped=4\n");
    check(streams_equal(), "flush drained 10 lines and forced the report");
    check(stats().used == 0u, "ring empty after flush");
    isaac_vita_log_async_flush();
    check(streams_equal(), "second flush writes nothing");
    streams_reset();

    /* 7. Enqueue into a non-empty ring does not wake; after a drain the
     *    next line does. */
    wakes = g_isaac_vita_log_async_oracle_wakes;
    check(producer_a(1u) == 1, "wake test line 1");
    check(producer_a(2u) == 1, "wake test line 2");
    check(g_isaac_vita_log_async_oracle_wakes == wakes + 1u,
          "second line into a non-empty ring does not wake");
    check(isaac_vita_log_async_oracle_service() == 2u, "wake test drained");
    check(producer_a(3u) == 1, "wake test line 3");
    check(g_isaac_vita_log_async_oracle_wakes == wakes + 2u,
          "first line after a drain wakes");
    check(isaac_vita_log_async_oracle_service() == 1u, "wake test drained 2");
    check(streams_equal(), "wake test bytes");
    streams_reset();

    /* 8. Totals and lock discipline. */
    s = stats();
    check(s.enqueued == s.sunk, "every queued line was sunk");
    check(s.enqueued == 2u * pairs + 2u +
              ISAAC_VITA_LOG_ASYNC_RING_BYTES / 64u + 10u + 3u,
          "enqueued total");
    check(s.dropped == 4u, "dropped total");
    check(g_isaac_vita_log_async_oracle_lock_faults == 0u,
          "no lock misuse (nested or unbalanced)");

    hostile_straddle_sweep(big);
    hostile_exact_fit_after_drop();
    hostile_overload_report_position();
    hostile_printf_overflow();
    hostile_mixed_fill();
#if defined(ISAAC_VITA_LOG_ASYNC_FILE_BATCH)
    batch_boundaries();
    batch_failures();
#endif

    /* Final totals: every accepted line reached a sink exactly once. */
    s = stats();
    check(s.enqueued == s.sunk, "hostile: every queued line was sunk");
    check(s.used == 0u, "hostile: ring empty at the end");
    check(g_isaac_vita_log_async_oracle_lock_faults == 0u,
          "hostile: no lock misuse");

    if (s_failures) {
        printf("host_vita_log_async oracle: FAIL checks=%u failures=%u\n",
               (unsigned)s_checks, (unsigned)s_failures);
        return 1;
    }
    printf("host_vita_log_async oracle: PASS checks=%u ring_kb=%u "
           "line_max=%u\n",
           (unsigned)s_checks, (unsigned)ISAAC_VITA_LOG_ASYNC_RING_KB,
           (unsigned)ISAAC_VITA_LOG_ASYNC_LINE_MAX);
    return 0;
}
