/* Host oracle for host_vita_native_vorbis.c (ISAAC_VITA_NATIVE_VORBIS).
 *
 * It drives the real wrapped entry (__wrap_sub_005bdf80) with a translated
 * style CPU whose stack, stb_vorbis state, alloc buffer, OGG bytes and KAGE
 * stream object all live in a 32-bit-addressable region, exactly as the
 * guest lays them out.  guest_call is implemented here as the KAGE stream
 * vtable (length/tell/seek/read, thiscall, callee cleans), and
 * __real_sub_005bdf80 serves a reference decode produced by the vendored
 * decoder in plain memory mode, i.e. what the translated body returns.
 *
 * Proofs:
 *   - the mirror path (clone + relocation + OGG/PCM rings + worker thread)
 *     reproduces the direct decode byte for byte, for stereo, mono,
 *     six-channel downmix and a 500 kbps stress file, across loop restarts
 *     (seek_start emulation), stale re-open detection and deinit;
 *   - the x86 convention is honoured: eax = frames, exactly one return word
 *     popped, callee-saved registers untouched, argument words left for the
 *     caller's `add esp, 8`;
 *   - the guest stream is read in ring-sized blocks through the frozen
 *     vtable slots with the frozen return markers;
 *   - fallbacks (push-mode state, no alloc buffer) use the translated path;
 *   - in the VERIFY build every slot logs result=MATCH.
 *
 * Usage: vita-native-vorbis-oracle <file.ogg> [more.ogg ...] */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "guest.h"
#include "host_vita_native_vorbis.h"
#include "guest_stack_legacy_oracle_stub.h"

#define STB_VORBIS_HEADER_ONLY
#include "third_party/stb_vorbis/stb_vorbis.c"
#undef STB_VORBIS_HEADER_ONLY

void __wrap_sub_005bdf80(CPU *__restrict c);
void __wrap_sub_005b7740(CPU *__restrict c);
void __wrap_sub_005bb260(CPU *__restrict c);
void __wrap_sub_005bd310(CPU *__restrict c);

/* ISAAC_VITA_NATIVE_VORBIS_ASYNC: a wrapped call may return any positive
 * count up to the request (partial slot); without it a slot is exact until
 * the stream ends. */
#if defined(ISAAC_VITA_NATIVE_VORBIS_ASYNC)
#define NV_ORACLE_ASYNC 1
#define SLOT_OK(n, slot) ((n) >= 1 && (n) <= (slot))
void __wrap_sub_005bd740(CPU *__restrict c);
#else
#define NV_ORACLE_ASYNC 0
#define SLOT_OK(n, slot) ((n) == (slot))
#endif

/* ------------------------------------------------------------ region --- */
#define REGION_BYTES     (24U * 1024U * 1024U)
#define STACK_OFFSET     0x00010000U
#define STACK_BYTES      0x00010000U
#define STREAM_OFFSET    0x00030000U   /* fake KAGE stream object */
#define VTABLE_OFFSET    0x00030100U
#define ALLOC_OFFSET     0x00040000U   /* first guest alloc buffer */
#define ALLOC_STRIDE     0x000a0000U
#define ALLOC_SLOTS      4U
#define OGG_OFFSET       0x00400000U   /* OGG bytes visible to the guest */
#define PCM_OFFSET       0x00800000U   /* guest PCM slot */

/* Fake translated code addresses for the vtable targets. */
#define FAKE_LENGTH 0x00ab0004U
#define FAKE_TELL   0x00ab0008U
#define FAKE_SEEK   0x00ab000cU
#define FAKE_READ   0x00ab0014U

static uint8_t *s_region;
static uint32_t s_region_base;
static unsigned s_failures;
static unsigned s_checks;

#define CHECK(condition, message) \
    do { \
        ++s_checks; \
        if (!(condition)) { \
            ++s_failures; \
            fprintf(stderr, "FAIL: %s (%s:%d)\n", message, __FILE__, __LINE__); \
        } \
    } while (0)

static uint32_t region_address(uint32_t offset)
{
    return s_region_base + offset;
}

/* Mirror memory for the module: page-granular 32-bit mappings. */
typedef struct oracle_block { void *pointer; size_t bytes; } oracle_block;
static oracle_block s_blocks[256];

/* Every mirror allocation ends exactly at a PROT_NONE guard page, so a
 * decoder write past the clone, the rings or the scratch faults instead of
 * corrupting a neighbour. */
void *isaac_nv_oracle_malloc(size_t bytes)
{
    size_t i;
    size_t rounded = (bytes + 4095U) & ~(size_t)4095U;
    uint8_t *p = (uint8_t *)mmap(NULL, rounded + 4096U, PROT_READ | PROT_WRITE,
                                 MAP_PRIVATE | MAP_ANONYMOUS
#ifdef MAP_32BIT
                                 | MAP_32BIT
#endif
                                 , -1, 0);
    if (p == (uint8_t *)MAP_FAILED)
        return NULL;
    mprotect(p + rounded, 4096U, PROT_NONE);
    for (i = 0U; i < sizeof s_blocks / sizeof s_blocks[0]; ++i)
        if (!s_blocks[i].pointer) {
            s_blocks[i].pointer = p;
            s_blocks[i].bytes = rounded + 4096U;
            return p + (rounded - bytes);
        }
    munmap(p, rounded + 4096U);
    return NULL;
}

void isaac_nv_oracle_free(void *pointer)
{
    size_t i;
    for (i = 0U; i < sizeof s_blocks / sizeof s_blocks[0]; ++i) {
        uint8_t *base = (uint8_t *)s_blocks[i].pointer;
        if (base && (uint8_t *)pointer >= base &&
                (uint8_t *)pointer < base + s_blocks[i].bytes) {
            munmap(base, s_blocks[i].bytes);
            s_blocks[i].pointer = NULL;
            return;
        }
    }
}

static void *region_pointer(uint32_t offset)
{
    return s_region + offset;
}

/* ------------------------------------------------------ fake stream --- */
typedef struct fake_stream {
    const uint8_t *data;
    uint32_t length;
    uint32_t position;
    uint32_t reads;
    uint32_t read_bytes;
    uint32_t small_reads;
    uint32_t seeks;
    uint32_t tells;
    uint32_t bad_markers;
    uint32_t bad_this;
} fake_stream;

static fake_stream s_stream;
/* position the guest state expects between calls (non-VERIFY: the clone
 * position; VERIFY: where the translated decode left it, unknown here) */
static uint32_t s_expect_position;
static int s_expect_position_valid;
static uint32_t s_open_seek_sample;   /* 0 = none: frames consumed before the
                                         state is handed over (mid-file clone
                                         with leftover channel-buffer samples) */
static int16_t s_skip_scratch[2048 * 8];

static int oracle_skip_frames(void *state, int channels, uint32_t frames)
{
    stb_vorbis *v = (stb_vorbis *)state;
    while (frames) {
        int want = frames > 2048U ? 2048 : (int)frames;
        int got = stb_vorbis_get_samples_short_interleaved(
            v, channels, s_skip_scratch, want * channels);
        if (got <= 0)
            return 0;
        frames -= (uint32_t)got;
    }
    return 1;
}

void guest_fault(CPU *__restrict c, uint32_t address, const char *message)
{
    c->fault_addr = address;
    c->fault = message;
    fprintf(stderr, "guest_fault %08x: %s\n", (unsigned)address, message);
    ++s_failures;
}

static uint32_t arg_at(const CPU *c, unsigned index)
{
    return ld32(c->esp + 4U + 4U * index);
}

void guest_call(CPU *__restrict c, uint32_t addr)
{
    uint32_t marker = ld32(c->esp);
    unsigned argc = 0U;

    if (c->ecx != region_address(STREAM_OFFSET))
        ++s_stream.bad_this;
    switch (addr) {
    case FAKE_LENGTH:
        c->eax = s_stream.length;
        break;
    case FAKE_TELL:
        ++s_stream.tells;
        if (marker != ISAAC_NV_SITE_TELL)
            ++s_stream.bad_markers;
        c->eax = s_stream.position;
        break;
    case FAKE_SEEK: {
        uint32_t offset = arg_at(c, 0U);
        uint32_t whence = arg_at(c, 1U);
        ++s_stream.seeks;
        if (marker != ISAAC_NV_SITE_SEEK)
            ++s_stream.bad_markers;
        if (whence == 0U && offset <= s_stream.length)
            s_stream.position = offset;
        c->eax = 0U;
        argc = 2U;
        break;
    }
    case FAKE_READ: {
        uint32_t buffer = arg_at(c, 0U);
        uint32_t size = arg_at(c, 1U);
        uint32_t count = arg_at(c, 2U);
        uint32_t want = size * count;
        uint32_t left = s_stream.length - s_stream.position;
        uint32_t got = want < left ? want : left;
        ++s_stream.reads;
        if (marker != ISAAC_NV_SITE_GETN_READ &&
                marker != ISAAC_NV_SITE_GET8_READ)
            ++s_stream.bad_markers;
        /* the translated get8 reads one byte per virtual call */
        if (want < 64U && got == want)
            ++s_stream.small_reads;
        memcpy((void *)(uintptr_t)buffer, s_stream.data + s_stream.position, got);
        s_stream.position += got;
        s_stream.read_bytes += got;
        c->eax = size ? got / size : 0U;
        argc = 3U;
        break;
    }
    default:
        guest_fault(c, addr, "unexpected guest_call target");
        return;
    }
    c->esp += 4U + 4U * argc;   /* ret imm: callee cleans */
}

/* --------------------------------------------- reference "translated" --- */
typedef struct reference {
    uint32_t guest_f;
    const int16_t *pcm;
    uint32_t frames;
    int channels;
    uint32_t cursor;
} reference;

static reference s_references[8];
static unsigned s_reference_count;
static unsigned s_real_calls;
static unsigned s_real_seek_calls;
static unsigned s_real_pump_calls;
static unsigned s_real_deinit_calls;

static reference *reference_for(uint32_t f)
{
    unsigned i;
    for (i = 0U; i < s_reference_count && i < 8U; ++i)
        if (s_references[i].guest_f == f)
            return &s_references[i];
    return NULL;
}

void __real_sub_005bdf80(CPU *__restrict c)
{
    uint32_t f = c->ecx;
    int channels = (int)c->edx;
    uint32_t buffer = ld32(c->esp + 4U);
    int num_shorts = (int)ld32(c->esp + 8U);
    int len = channels ? num_shorts / channels : 0;
    reference *r = reference_for(f);
    uint32_t n = 0U;

    ++s_real_calls;
    /* Hand-off contract: whenever the translated body runs on a stream-mode
     * state, the KAGE stream stands where that state expects it (the clone
     * position, or wherever the guest's own seek put it).  Between wrapped
     * calls the stream may rest at the read-ahead head; the runtime seeks it
     * back only here, at a re-clone, at an eviction and before a pump. */
    if (s_stream.data && s_expect_position_valid && channels > 0 && r)
        CHECK(s_stream.position == s_expect_position,
              "translated body sees the guest stream at the guest state's position");
    if (r && r->channels == channels) {
        uint32_t left = r->frames - r->cursor;
        n = (uint32_t)len < left ? (uint32_t)len : left;
        memcpy((void *)(uintptr_t)buffer, r->pcm + (size_t)r->cursor * channels,
               (size_t)n * channels * 2U);
        r->cursor += n;
    }
    c->eax = n;
    c->esp += 4U;   /* ret */
}

void __real_sub_005b7740(CPU *__restrict c)
{
    ++s_real_seek_calls;
    c->esp += 4U;
    c->eax = 1U;
}

void __real_sub_005bb260(CPU *__restrict c)
{
    ++s_real_pump_calls;
    c->esp += 4U;
}

void __real_sub_005bd310(CPU *__restrict c)
{
    ++s_real_deinit_calls;
    c->esp += 4U;
}

#if NV_ORACLE_ASYNC
static unsigned s_real_open_calls;
static uint32_t s_fake_open_result;

/* stb_vorbis_open_file: the oracle opened the state itself, so the
 * "translated body" only hands it back: eax = state (0 = failed), pop the
 * return word; the caller cleans the two argument words. */
void __real_sub_005bd740(CPU *__restrict c)
{
    ++s_real_open_calls;
    c->eax = s_fake_open_result;
    c->esp += 4U;
}
#endif

/* ---------------------------------------------------------- logging --- */
static unsigned s_log_lines;
static unsigned s_verify_match;
static unsigned s_verify_other;
static unsigned s_err_lines;
static unsigned s_expected_err_lines;   /* provoked on purpose */
static unsigned s_fallback_lines;
static unsigned s_close_lines;
static uint32_t s_last_close_run;
static char s_last_slot_line[1024];
#if NV_ORACLE_ASYNC
static unsigned s_early_lines;
#endif
static int s_log_echo;

void isaac_vita_log(const char *format, ...)
{
    char line[1024];
    va_list arguments;

    va_start(arguments, format);
    (void)vsnprintf(line, sizeof line, format, arguments);
    va_end(arguments);
    ++s_log_lines;
    if (strstr(line, "NATIVE VORBIS VERIFY:")) {
        if (strstr(line, "result=MATCH"))
            ++s_verify_match;
        else
            ++s_verify_other;
    }
    if (strstr(line, "NATIVE VORBIS ERR:") || strstr(line, "NATIVE VORBIS ASSERT:"))
        ++s_err_lines;
    if (strstr(line, "NATIVE VORBIS FALLBACK:"))
        ++s_fallback_lines;
    if (strstr(line, "NATIVE VORBIS SLOT:"))
        memcpy(s_last_slot_line, line, sizeof s_last_slot_line);
#if NV_ORACLE_ASYNC
    if (strstr(line, "NATIVE VORBIS EARLY:"))
        ++s_early_lines;
#endif
    if (strstr(line, "NATIVE VORBIS CLOSE:")) {
        const char *at = strstr(line, " run=");

        ++s_close_lines;
        s_last_close_run = at ? (uint32_t)strtoul(at + 5, NULL, 16) : 0U;
    }
    if (s_log_echo)
        printf("  log: %s\n", line);
}

/* ---------------------------------------------------------- helpers --- */
static uint8_t *read_file(const char *path, uint32_t *length)
{
    FILE *fp = fopen(path, "rb");
    uint8_t *data;
    long size;

    if (!fp)
        return NULL;
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (size <= 0) {
        fclose(fp);
        return NULL;
    }
    data = (uint8_t *)malloc((size_t)size);
    if (!data || fread(data, 1U, (size_t)size, fp) != (size_t)size) {
        free(data);
        fclose(fp);
        return NULL;
    }
    fclose(fp);
    *length = (uint32_t)size;
    return data;
}

/* Reference PCM: the vendored decoder in plain memory mode, malloc
 * allocation, requested layout, whole file. */
static int16_t *decode_reference(const uint8_t *ogg, uint32_t length,
                                 int channels, uint32_t *frames_out,
                                 stb_vorbis_info *info_out)
{
    int error = 0;
    stb_vorbis *v = stb_vorbis_open_memory(ogg, (int)length, &error, NULL);
    uint32_t capacity = 1U << 16;
    uint32_t frames = 0U;
    int16_t *pcm;

    if (!v)
        return NULL;
    *info_out = stb_vorbis_get_info(v);
    if (s_open_seek_sample && !oracle_skip_frames(v, channels, s_open_seek_sample)) {
        stb_vorbis_close(v);
        return NULL;
    }
    pcm = (int16_t *)malloc((size_t)capacity * channels * 2U);
    for (;;) {
        int n;
        if (frames + 8192U > capacity) {
            capacity *= 2U;
            pcm = (int16_t *)realloc(pcm, (size_t)capacity * channels * 2U);
        }
        n = stb_vorbis_get_samples_short_interleaved(
            v, channels, pcm + (size_t)frames * channels, 8192 * channels);
        if (n <= 0)
            break;
        frames += (uint32_t)n;
    }
    stb_vorbis_close(v);
    *frames_out = frames;
    return pcm;
}

typedef struct guest_state {
    uint32_t f;                 /* guest address of the stb_vorbis */
    uint32_t alloc_buffer;      /* guest address */
    uint32_t alloc_len;
    uint32_t ogg;               /* guest address of the OGG bytes */
    uint32_t ogg_len;
    uint8_t *snapshot_state;    /* post-open state for seek_start emulation */
    uint8_t *snapshot_alloc;
    uint32_t snapshot_alloc_len;
    uint32_t stream_position;   /* post-open position */
} guest_state;

/* Open a state the way StreamSourceOgg::Open does: alloc buffer of 0x4b000
 * bytes, state allocated inside it.  Memory mode first; stream mode is a
 * field swap that leaves the decoder state untouched. */
static int open_guest_state(guest_state *g, unsigned slot, const uint8_t *ogg,
                            uint32_t length, int stream_mode, int channels)
{
    stb_vorbis_alloc alloc;
    stb_vorbis *v;
    uint8_t *state_bytes;
    int error = 0;

    g->alloc_buffer = region_address(ALLOC_OFFSET + slot * ALLOC_STRIDE);
    /* the game's buffer; the 5.1 test file needs twice that */
    g->alloc_len = channels > 2 ? 2U * ISAAC_NV_GUEST_ALLOC_BYTES
                                : ISAAC_NV_GUEST_ALLOC_BYTES;
    g->ogg = region_address(OGG_OFFSET);
    g->ogg_len = length;
    memcpy(region_pointer(OGG_OFFSET), ogg, length);
    alloc.alloc_buffer = (char *)(uintptr_t)g->alloc_buffer;
    alloc.alloc_buffer_length_in_bytes = (int)g->alloc_len;
    v = stb_vorbis_open_memory((const unsigned char *)(uintptr_t)g->ogg,
                               (int)length, &error, &alloc);
    if (!v) {
        fprintf(stderr, "open_memory failed: %d\n", error);
        return 0;
    }
    g->f = (uint32_t)(uintptr_t)v;
    CHECK(g->f - g->alloc_buffer < g->alloc_len,
          "guest state lives inside its alloc buffer");
    state_bytes = (uint8_t *)v;
    if (s_open_seek_sample &&
            !oracle_skip_frames(v, channels, s_open_seek_sample)) {
        fprintf(stderr, "skip(%u) failed\n", (unsigned)s_open_seek_sample);
        return 0;
    }
    if (stream_mode) {
        g->stream_position = isaac_nv_oracle_to_stream_mode(
            v, region_address(STREAM_OFFSET));
#if !defined(NV_ORACLE_EXPECT_VERIFY)
        s_expect_position = g->stream_position;
        s_expect_position_valid = 1;
#endif
        s_stream.data = (const uint8_t *)region_pointer(OGG_OFFSET);
        s_stream.length = length;
        s_stream.position = g->stream_position;
    }
    /* snapshot for the loop-restart emulation */
    g->snapshot_alloc_len = g->alloc_len;
    g->snapshot_state = (uint8_t *)malloc(isaac_nv_oracle_state_bytes());
    g->snapshot_alloc = (uint8_t *)malloc(g->alloc_len);
    memcpy(g->snapshot_state, state_bytes, isaac_nv_oracle_state_bytes());
    memcpy(g->snapshot_alloc, (void *)(uintptr_t)g->alloc_buffer, g->alloc_len);
    return 1;
}

/* Loop restart: what the translated seek_start + pump leave behind (the
 * runtime's marker slot survives, as it does in the game). */
static void restore_guest_state(guest_state *g)
{
    void *state = (void *)(uintptr_t)g->f;
    uint32_t marker = isaac_nv_oracle_marker_get(state);

    /* the state lives inside the alloc buffer: restore both, keep the slot */
    memcpy((void *)(uintptr_t)g->alloc_buffer, g->snapshot_alloc, g->alloc_len);
    memcpy(state, g->snapshot_state, isaac_nv_oracle_state_bytes());
    isaac_nv_oracle_marker_set(state, marker);
    s_stream.position = g->stream_position;
    s_expect_position = g->stream_position;
}

/* Re-open: a brand new state at the same address, marker included. */
static void reopen_guest_state(guest_state *g)
{
    memcpy((void *)(uintptr_t)g->alloc_buffer, g->snapshot_alloc, g->alloc_len);
    memcpy((void *)(uintptr_t)g->f, g->snapshot_state, isaac_nv_oracle_state_bytes());
    s_stream.position = g->stream_position;
    s_expect_position = g->stream_position;
}

/* Call the wrapped entry exactly like sub_005a20e0 does and check the ABI. */
static int32_t call_wrapper(uint32_t f, int channels, uint32_t buffer,
                            int num_shorts)
{
    CPU c;
    uint32_t stack_top = region_address(STACK_OFFSET + STACK_BYTES - 0x100U);

    memset(&c, 0, sizeof c);
    c.ebx = 0x0b0b0b0bU;
    c.esi = 0x51515151U;
    c.edi = 0xd1d1d1d1U;
    c.ebp = 0xb9b9b9b9U;
    c.esp = stack_top;
    c.esp -= 4U; st32(c.esp, (uint32_t)num_shorts);   /* push eax */
    c.esp -= 4U; st32(c.esp, buffer);                  /* push [ebp+8] */
    c.ecx = f;
    c.edx = (uint32_t)channels;
    c.esp -= 4U; st32(c.esp, 0x005a214cU);             /* call: return word */
    memset((uint8_t *)(uintptr_t)buffer + (size_t)num_shorts * 2U, 0xa5, 64U);
    __wrap_sub_005bdf80(&c);
    {
        const uint8_t *tail = (const uint8_t *)(uintptr_t)buffer +
                              (size_t)num_shorts * 2U;
        int i, intact = 1;
        for (i = 0; i < 64; ++i)
            if (tail[i] != 0xa5)
                intact = 0;
        CHECK(intact, "no write past the guest PCM buffer");
    }
    /* The read-ahead may leave the stream past the state between calls (no
     * per-slot seek-back: KAGE streams are LZ4 entries, a backward seek
     * costs the whole position), never before it. */
    if (s_stream.data && s_expect_position_valid)
        CHECK(s_stream.position >= s_expect_position,
              "guest stream never rests before the guest state's position");
    CHECK(c.fault == NULL, "wrapper must not fault");
    CHECK(c.esp == stack_top - 8U, "exactly the return word is popped");
    CHECK(ld32(c.esp) == buffer && ld32(c.esp + 4U) == (uint32_t)num_shorts,
          "argument words are left for the caller's add esp, 8");
    CHECK(c.ebx == 0x0b0b0b0bU && c.esi == 0x51515151U &&
          c.edi == 0xd1d1d1d1U && c.ebp == 0xb9b9b9b9U,
          "callee-saved registers preserved");
    return (int32_t)c.eax;
}

static void call_seek_start(uint32_t f, uint32_t first_page)
{
    CPU c;

    memset(&c, 0, sizeof c);
    c.esp = region_address(STACK_OFFSET + STACK_BYTES - 0x100U);
    c.ecx = f;
    c.edx = first_page;
    c.esp -= 4U; st32(c.esp, 0x005a2185U);
    __wrap_sub_005b7740(&c);
    c.ecx = f;
    c.esp -= 4U; st32(c.esp, 0x005a21a0U);
    __wrap_sub_005bb260(&c);
}

static void call_deinit(uint32_t f)
{
    CPU c;

    memset(&c, 0, sizeof c);
    c.esp = region_address(STACK_OFFSET + STACK_BYTES - 0x100U);
    c.ecx = f;
    c.esp -= 4U; st32(c.esp, 0x005a3790U);
    __wrap_sub_005bd310(&c);
}

#if NV_ORACLE_ASYNC
/* Call the wrapped stb_vorbis_open_file exactly like StreamSourceOgg::Open
 * at 0x5a2346 (ecx = KAGE stream, push alloc, push &error, call) with the
 * translated body returning `result`, and check the ABI: the state pointer
 * comes back in eax, only the return word is popped, callee-saved registers
 * are untouched. */
static void call_open_hook(uint32_t result)
{
    CPU c;
    uint32_t stack_top = region_address(STACK_OFFSET + STACK_BYTES - 0x100U);
    unsigned real_before = s_real_open_calls;

    memset(&c, 0, sizeof c);
    c.ebx = 0x0b0b0b0bU;
    c.esi = 0x51515151U;
    c.edi = 0xd1d1d1d1U;
    c.ebp = 0xb9b9b9b9U;
    c.esp = stack_top;
    c.esp -= 4U; st32(c.esp, region_address(STACK_OFFSET));       /* alloc */
    c.esp -= 4U; st32(c.esp, region_address(STACK_OFFSET + 4U));  /* &error */
    c.ecx = region_address(STREAM_OFFSET);
    c.esp -= 4U; st32(c.esp, 0x005a234bU);
    s_fake_open_result = result;
    __wrap_sub_005bd740(&c);
    CHECK(s_real_open_calls == real_before + 1U,
          "open hook runs the translated body first");
    CHECK(c.fault == NULL, "open hook must not fault");
    CHECK(c.esp == stack_top - 8U,
          "open hook pops exactly the return word (the caller cleans the arguments)");
    CHECK(c.eax == result,
          "open hook returns the translated body's state pointer in eax");
    CHECK(c.ebx == 0x0b0b0b0bU && c.esi == 0x51515151U &&
          c.edi == 0xd1d1d1d1U && c.ebp == 0xb9b9b9b9U,
          "open hook preserves callee-saved registers");
}
#endif

/* Decode the whole stream through the wrapper in slot-sized requests. */
static uint32_t drain(uint32_t f, int channels, int slot_frames,
                      int16_t *out, uint32_t out_capacity_frames,
                      int pace_us, uint32_t *calls_out)
{
    uint32_t frames = 0U;
    uint32_t calls = 0U;
    uint32_t buffer = region_address(PCM_OFFSET);   /* guest PCM slot */

    for (;;) {
        int32_t n;
        if (frames + (uint32_t)slot_frames > out_capacity_frames)
            break;
        n = call_wrapper(f, channels, buffer, slot_frames * channels);
        ++calls;
        CHECK(n >= 0 && n <= slot_frames, "frame count in range");
        if (n < 0)
            break;
        memcpy(out + (size_t)frames * channels, region_pointer(PCM_OFFSET),
               (size_t)n * channels * 2U);
        frames += (uint32_t)n;
#if NV_ORACLE_ASYNC
        /* partial slots are ordinary; the guest sees the end as 0 frames */
        if (n == 0)
            break;
#else
        if (n < slot_frames)
            break;
#endif
        if (pace_us)
            usleep((useconds_t)pace_us);
        if (calls > 100000U)
            break;
    }
    *calls_out = calls;
    return frames;
}

static int same_pcm(const int16_t *a, const int16_t *b, uint32_t frames,
                    int channels, uint32_t *first_diff)
{
    uint32_t i;
    for (i = 0U; i < frames * (uint32_t)channels; ++i)
        if (a[i] != b[i]) {
            *first_diff = i;
            return 0;
        }
    return 1;
}

/* ------------------------------------------------------------ tests --- */
typedef struct scenario {
    const char *name;
    int channels;       /* requested layout */
    int stream_mode;
    int worker;
    int slot_frames;
    int loops;
    int pace_us;
} scenario;

static void run_scenario(const char *path, const uint8_t *ogg, uint32_t length,
                         const scenario *sc)
{
    guest_state g;
    stb_vorbis_info info;
    uint32_t ref_frames = 0U;
    int16_t *ref;
    int16_t *out;
    uint32_t pass;
    isaac_nv_oracle_stats before, after;
    reference *r;
    uint32_t first_page = 0U;
    uint32_t run_expected = 0U;
    unsigned close_before = s_close_lines;

    memset(&g, 0, sizeof g);
    memset(&s_stream, 0, sizeof s_stream);
    ref = decode_reference(ogg, length, sc->channels, &ref_frames, &info);
    if (!ref) {
        fprintf(stderr, "SKIP %s: reference decode failed\n", path);
        return;
    }
    printf("%s: %s sr=%u ch=%u -> arg_ch=%d frames=%u %s %s\n", sc->name, path,
           (unsigned)info.sample_rate, (unsigned)info.channels, sc->channels,
           (unsigned)ref_frames, sc->stream_mode ? "stream" : "memory",
           sc->worker ? "worker" : "sync");
    if (!open_guest_state(&g, 0U, ogg, length, sc->stream_mode, info.channels)) {
        free(ref);
        return;
    }
    first_page = isaac_nv_oracle_first_page((void *)(uintptr_t)g.f);
    r = &s_references[s_reference_count++ % 8U];
    r->guest_f = g.f;
    r->pcm = ref;
    r->frames = ref_frames;
    r->channels = sc->channels;
    r->cursor = 0U;
    out = (int16_t *)malloc(((size_t)ref_frames + 65536U) * sc->channels * 2U);
    isaac_nv_oracle_stats_get(&before);
    if (sc->worker)
        CHECK(isaac_nv_oracle_worker_start(), "worker starts");
    for (pass = 0U; pass < (uint32_t)sc->loops; ++pass) {
        uint32_t calls = 0U;
        uint32_t first_diff = 0U;
        uint32_t got;

        r->cursor = 0U;
        got = drain(g.f, sc->channels, sc->slot_frames, out,
                    ref_frames + 65536U, sc->pace_us, &calls);
        run_expected = isaac_nv_crc32(run_expected, ref,
                                      ref_frames * (uint32_t)sc->channels * 2U);
        CHECK(got == ref_frames, "mirror returns the reference frame count");
        if (got == ref_frames) {
            int equal = same_pcm(out, ref, got, sc->channels, &first_diff);
            CHECK(equal, "mirror PCM equals the direct decode byte for byte");
            if (!equal)
                fprintf(stderr, "  first differing sample index %u\n",
                        (unsigned)first_diff);
        } else {
            fprintf(stderr, "  got %u frames, reference %u, calls %u\n",
                    (unsigned)got, (unsigned)ref_frames, (unsigned)calls);
        }
        printf("  pass %u: frames=%u calls=%u reads=%u read_bytes=%u "
               "small_reads=%u seeks=%u tells=%u\n", (unsigned)pass,
               (unsigned)got, (unsigned)calls, (unsigned)s_stream.reads,
               (unsigned)s_stream.read_bytes, (unsigned)s_stream.small_reads,
               (unsigned)s_stream.seeks, (unsigned)s_stream.tells);
        if (sc->stream_mode) {
            CHECK(s_stream.bad_markers == 0U,
                  "guest stream calls carry the frozen return markers");
            CHECK(s_stream.bad_this == 0U, "guest stream calls use ecx=this");
            CHECK(s_stream.small_reads == 0U,
                  "OGG bytes are read in blocks, never byte by byte");
        }
        /* loop restart exactly as Decode does: translated seek_start */
        restore_guest_state(&g);
        call_seek_start(g.f, first_page);
    }
    isaac_nv_oracle_stats_get(&after);
    CHECK(after.fallbacks == before.fallbacks, "no translated fallback");
    CHECK(after.asserts == before.asserts, "no decoder assertion");
    CHECK(after.underruns == before.underruns, "no ring underrun");
    if (sc->stream_mode) {
        CHECK(after.mirrors_created == before.mirrors_created + 1U,
              "one mirror for the stream");
        if (sc->loops > 1)
            CHECK(after.reclones - before.reclones == (uint32_t)sc->loops - 1U,
                  "each seek_start followed by a request re-clones once");
        if (sc->worker && sc->pace_us)
            CHECK(after.frames_worker > before.frames_worker,
                  "the worker produced frames ahead of the game thread");
        if (!sc->worker)
            CHECK(after.frames_worker == before.frames_worker,
                  "no worker frames in sync mode");
    }
    printf("  stats: worker_frames=%u sync_frames=%u waits=%u reclones=%u "
           "fallbacks=%u rejected=%u\n",
           (unsigned)(after.frames_worker - before.frames_worker),
           (unsigned)(after.frames_sync - before.frames_sync),
           (unsigned)(after.waits - before.waits),
           (unsigned)(after.reclones - before.reclones),
           (unsigned)(after.fallbacks - before.fallbacks),
           (unsigned)(after.clone_rejected - before.clone_rejected));
    /* teardown as the whole-sample path does; the CLOSE receipt's running
     * CRC covers every pass (RECLONE keeps it) and must equal the CRC of
     * the delivered PCM -- with ASYNC it is folded in by the producers */
    call_deinit(g.f);
    CHECK(s_close_lines == close_before + 1U, "deinit logs one CLOSE receipt");
    CHECK(s_last_close_run == run_expected,
          "CLOSE run= equals the CRC of the delivered PCM");
    if (sc->worker)
        isaac_nv_oracle_worker_stop();
    r->guest_f = 0U;
    free(out);
    free(ref);
    free(g.snapshot_state);
    free(g.snapshot_alloc);
}

/* A re-open at the same address must be detected and restart the mirror. */
static void run_stale_test(const char *path, const uint8_t *ogg, uint32_t length)
{
    guest_state g;
    stb_vorbis_info info;
    uint32_t ref_frames = 0U;
    int16_t *ref;
    int16_t *out;
    uint32_t buffer = region_address(PCM_OFFSET);
    int32_t n1, n2, n3;
    reference *r;
    isaac_nv_oracle_stats before, after;
    uint32_t first_diff = 0U;

    memset(&g, 0, sizeof g);
    memset(&s_stream, 0, sizeof s_stream);
    ref = decode_reference(ogg, length, 2, &ref_frames, &info);
    if (!ref || info.channels != 2 || ref_frames < 3U * 4096U) {
        free(ref);
        return;
    }
    printf("stale-reopen: %s\n", path);
    if (!open_guest_state(&g, 1U, ogg, length, 1, 2)) {
        free(ref);
        return;
    }
    r = &s_references[s_reference_count++ % 8U];
    r->guest_f = g.f;
    r->pcm = ref;
    r->frames = ref_frames;
    r->channels = 2;
    r->cursor = 0U;
    out = (int16_t *)malloc(4096U * 2U * 2U);
    isaac_nv_oracle_stats_get(&before);
    n1 = call_wrapper(g.f, 2, buffer, 4096 * 2);
    n2 = call_wrapper(g.f, 2, buffer, 4096 * 2);
    CHECK(SLOT_OK(n1, 4096) && SLOT_OK(n2, 4096), "two slots decode");
    /* the guest closes and re-opens the same file at the same addresses */
    reopen_guest_state(&g);
    r->cursor = 0U;
    n3 = call_wrapper(g.f, 2, buffer, 4096 * 2);
    memcpy(out, region_pointer(PCM_OFFSET), 4096U * 4U);
    CHECK(SLOT_OK(n3, 4096), "slot after re-open decodes");
    CHECK(n3 > 0 && same_pcm(out, ref, (uint32_t)n3, 2, &first_diff),
          "re-opened stream restarts from the beginning");
    isaac_nv_oracle_stats_get(&after);
    CHECK(after.mirrors_created == before.mirrors_created + 2U,
          "a stale mirror is replaced by a new one");
    call_deinit(g.f);
    r->guest_f = 0U;
    free(out);
    free(ref);
    free(g.snapshot_state);
    free(g.snapshot_alloc);
}

/* Push-mode and alloc-less states must fall back to the translated body. */
static void run_fallback_test(const char *path, const uint8_t *ogg, uint32_t length)
{
    guest_state g;
    stb_vorbis_info info;
    uint32_t ref_frames = 0U;
    int16_t *ref;
    uint32_t buffer = region_address(PCM_OFFSET);
    int32_t n;
    reference *r;
    isaac_nv_oracle_stats before, after;
    void *state;
    void *saved_alloc;

    memset(&g, 0, sizeof g);
    memset(&s_stream, 0, sizeof s_stream);
    ref = decode_reference(ogg, length, 2, &ref_frames, &info);
    if (!ref || info.channels != 2) {
        free(ref);
        return;
    }
    printf("fallback: %s\n", path);
    if (!open_guest_state(&g, 2U, ogg, length, 1, 2)) {
        free(ref);
        return;
    }
    r = &s_references[s_reference_count++ % 8U];
    r->guest_f = g.f;
    r->pcm = ref;
    r->frames = ref_frames;
    r->channels = 2;
    r->cursor = 0U;
    state = (void *)(uintptr_t)g.f;
    isaac_nv_oracle_stats_get(&before);
    /* push mode */
    isaac_nv_oracle_set_push_mode(state, 1);
    n = call_wrapper(g.f, 2, buffer, 1024 * 2);
    CHECK(n == 1024 && s_real_calls > 0U, "push-mode state uses the translated body");
    isaac_nv_oracle_set_push_mode(state, 0);
    /* no alloc buffer */
    saved_alloc = isaac_nv_oracle_swap_alloc_buffer(state, NULL);
    n = call_wrapper(g.f, 2, buffer, 1024 * 2);
    CHECK(n == 1024, "alloc-less state uses the translated body");
    (void)isaac_nv_oracle_swap_alloc_buffer(state, saved_alloc);
    /* channels 0: translated body handles the fault path itself */
    {
        unsigned real_before = s_real_calls;
        n = call_wrapper(g.f, 0, buffer, 1024 * 2);
        CHECK(s_real_calls == real_before + 1U, "channels=0 goes to the translated body");
        (void)n;
    }
    isaac_nv_oracle_stats_get(&after);
    /* channels=0 is routed to the translated body without a receipt: the
     * translated idiv fault is the authoritative behaviour there */
    CHECK(after.fallbacks == before.fallbacks + 2U, "two clone fallbacks counted");
    CHECK(after.mirrors_created == before.mirrors_created, "no mirror created");
    call_deinit(g.f);
    r->guest_f = 0U;
    free(ref);
    free(g.snapshot_state);
    free(g.snapshot_alloc);
}

/* Wait until the worker is inside a frame of the mirror for f. */
static int wait_worker_midframe(uint32_t f)
{
    int i;
    for (i = 0; i < 4000; ++i) {
        if (isaac_nv_oracle_mirror_busy(f) == 2)
            return 1;
        usleep(50);
    }
    return 0;
}

/* Lifecycle races: the guest re-opens the same state (same address, marker
 * gone) or deinits it while the worker is inside a frame; a retired slot is
 * then re-used for a whole-sample decode while the worker is still active;
 * the guest frees a stream's alloc buffer without deinit. */
static void run_midframe_tests(const char *path, const uint8_t *ogg, uint32_t length)
{
    guest_state g;
    stb_vorbis_info info;
    uint32_t ref_frames = 0U;
    int16_t *ref;
    int16_t *out;
    uint32_t buffer = region_address(PCM_OFFSET);
    reference *r;
    isaac_nv_oracle_stats before, after;
    uint32_t first_diff = 0U;
    int slot_frames = 4096;
    int n;
    unsigned round;

    memset(&g, 0, sizeof g);
    memset(&s_stream, 0, sizeof s_stream);
    ref = decode_reference(ogg, length, 2, &ref_frames, &info);
    if (!ref || info.channels != 2 || ref_frames < 40U * 4096U) {
        free(ref);
        return;
    }
    printf("midframe: %s\n", path);
    out = (int16_t *)malloc((size_t)slot_frames * 2U * 2U);
    CHECK(isaac_nv_oracle_worker_start(), "worker starts");
    isaac_nv_oracle_set_worker_frame_delay_us(3000U);

    /* a) re-open while the worker is inside a frame */
    if (!open_guest_state(&g, 3U, ogg, length, 1, 2)) {
        free(ref);
        free(out);
        return;
    }
    r = &s_references[s_reference_count++ % 8U];
    r->guest_f = g.f; r->pcm = ref; r->frames = ref_frames; r->channels = 2;
    r->cursor = 0U;
    isaac_nv_oracle_stats_get(&before);
    n = call_wrapper(g.f, 2, buffer, slot_frames * 2);
    CHECK(SLOT_OK(n, slot_frames), "first slot decodes");
    n = call_wrapper(g.f, 2, buffer, slot_frames * 2);
    CHECK(SLOT_OK(n, slot_frames), "second slot decodes");
    CHECK(wait_worker_midframe(g.f), "worker is mid-frame on the stream");
    reopen_guest_state(&g);
    r->cursor = 0U;
    n = call_wrapper(g.f, 2, buffer, slot_frames * 2);
    memcpy(out, region_pointer(PCM_OFFSET), (size_t)slot_frames * 4U);
    CHECK(SLOT_OK(n, slot_frames), "re-opened stream decodes while worker was mid-frame");
    CHECK(n > 0 && same_pcm(out, ref, (uint32_t)n, 2, &first_diff),
          "re-opened stream restarts from the beginning (mid-frame)");
    /* the rest of the stream must still be exact */
    {
        uint32_t frames = (uint32_t)(n > 0 ? n : 0);
        int32_t k;
        for (;;) {
            k = call_wrapper(g.f, 2, buffer, slot_frames * 2);
            CHECK(k >= 0 && k <= slot_frames, "frame count in range (mid-frame)");
            if (k <= 0)
                break;
            if (frames + (uint32_t)k <= ref_frames)
                CHECK(same_pcm((const int16_t *)region_pointer(PCM_OFFSET),
                               ref + (size_t)frames * 2U, (uint32_t)k, 2,
                               &first_diff),
                      "PCM after mid-frame re-open equals the reference");
            frames += (uint32_t)k;
#if !NV_ORACLE_ASYNC
            if (k < slot_frames)
                break;
#endif
        }
        CHECK(frames == ref_frames, "whole stream delivered after mid-frame re-open");
    }

    /* b) deinit while the worker is inside a frame, then re-use the slot
     * for a whole-sample decode while the worker keeps running */
    reopen_guest_state(&g);
    r->cursor = 0U;
    n = call_wrapper(g.f, 2, buffer, slot_frames * 2);
    CHECK(SLOT_OK(n, slot_frames), "stream decodes before deinit");
    CHECK(wait_worker_midframe(g.f), "worker is mid-frame before deinit");
    call_deinit(g.f);
    CHECK(isaac_nv_oracle_mirror_busy(g.f) == -1, "deinit retired the mirror synchronously");
    r->guest_f = 0U;
    free(g.snapshot_state);
    free(g.snapshot_alloc);
    for (round = 0U; round < 12U; ++round) {
        guest_state mem;
        reference *rm;
        isaac_nv_oracle_stats w0, w1;
        uint32_t frames = 0U;
        int32_t k;

        memset(&mem, 0, sizeof mem);
        if (!open_guest_state(&mem, 3U, ogg, length, 0, 2))
            break;
        rm = &s_references[s_reference_count++ % 8U];
        rm->guest_f = mem.f; rm->pcm = ref; rm->frames = ref_frames;
        rm->channels = 2; rm->cursor = 0U;
        isaac_nv_oracle_stats_get(&w0);
        for (;;) {
            k = call_wrapper(mem.f, 2, buffer, 777 * 2);
            CHECK(k >= 0 && k <= 777, "memory frame count in range");
            if (k <= 0)
                break;
            if (frames + (uint32_t)k <= ref_frames)
                CHECK(same_pcm((const int16_t *)region_pointer(PCM_OFFSET),
                               ref + (size_t)frames * 2U, (uint32_t)k, 2,
                               &first_diff),
                      "whole-sample PCM equals the reference under worker churn");
            frames += (uint32_t)k;
            if (k < 777 || frames > 40U * 4096U)
                break;
        }
        isaac_nv_oracle_stats_get(&w1);
        CHECK(w1.frames_worker == w0.frames_worker,
              "the worker never decodes a whole-sample mirror");
        call_deinit(mem.f);
        rm->guest_f = 0U;
        free(mem.snapshot_state);
        free(mem.snapshot_alloc);
        /* interleave a short-lived stream on the same slot */
        memset(&mem, 0, sizeof mem);
        memset(&s_stream, 0, sizeof s_stream);
        if (!open_guest_state(&mem, 3U, ogg, length, 1, 2))
            break;
        rm = &s_references[s_reference_count++ % 8U];
        rm->guest_f = mem.f; rm->pcm = ref; rm->frames = ref_frames;
        rm->channels = 2; rm->cursor = 0U;
        k = call_wrapper(mem.f, 2, buffer, slot_frames * 2);
        CHECK(SLOT_OK(k, slot_frames), "churned stream decodes its first slot");
        CHECK(k > 0 && same_pcm((const int16_t *)region_pointer(PCM_OFFSET), ref,
                                (uint32_t)k, 2, &first_diff),
              "churned stream PCM equals the reference");
        if (round & 1U) {
            (void)wait_worker_midframe(mem.f);
            call_deinit(mem.f);
        } else {
            /* the game closes a stream by freeing its alloc buffer */
            (void)wait_worker_midframe(mem.f);
            isaac_nv_guest_buffer_freed((void *)(uintptr_t)mem.alloc_buffer);
        }
        CHECK(isaac_nv_oracle_mirror_busy(mem.f) == -1,
              "closed stream mirror is retired before its memory is reused");
        rm->guest_f = 0U;
        free(mem.snapshot_state);
        free(mem.snapshot_alloc);
    }
    isaac_nv_oracle_stats_get(&after);
    CHECK(after.asserts == before.asserts, "no decoder assertion (midframe)");
    CHECK(after.underruns == before.underruns, "no ring underrun (midframe)");
    CHECK(after.fallbacks == before.fallbacks, "no translated fallback (midframe)");
    CHECK(after.freed_retires - before.freed_retires == 6U,
          "six stream mirrors retired through the free observer");
    printf("  midframe stats: worker_refused=%u freed_retires=%u waits=%u\n",
           (unsigned)(after.worker_refused - before.worker_refused),
           (unsigned)(after.freed_retires - before.freed_retires),
           (unsigned)(after.waits - before.waits));
    isaac_nv_oracle_set_worker_frame_delay_us(0U);
    isaac_nv_oracle_worker_stop();
    free(out);
    free(ref);
}

/* Mid-file opens: the game seeks a layer to the main track's position
 * (translated stb_vorbis_seek), so the state is cloned mid-page with
 * leftover samples in its channel buffers; it is then freed after a single
 * slot and re-opened at another position while the worker runs. */
static void run_seek_tests(const char *path, const uint8_t *ogg, uint32_t length)
{
    static const uint32_t seeks[] = { 12345U, 70000U, 130001U };
    stb_vorbis_info info;
    uint32_t buffer = region_address(PCM_OFFSET);
    int slot_frames = 4096;
    unsigned round;
    isaac_nv_oracle_stats before, after;
    uint32_t first_diff = 0U;

    isaac_nv_oracle_stats_get(&before);
    CHECK(isaac_nv_oracle_worker_start(), "worker starts (seek)");
    isaac_nv_oracle_set_worker_frame_delay_us(2000U);
    for (round = 0U; round < 9U; ++round) {
        guest_state g;
        reference *r;
        int16_t *ref;
        uint32_t ref_frames = 0U;
        uint32_t frames = 0U;
        int32_t k;
        int calls = 0;

        memset(&g, 0, sizeof g);
        memset(&s_stream, 0, sizeof s_stream);
        s_open_seek_sample = seeks[round % 3U];
        ref = decode_reference(ogg, length, 2, &ref_frames, &info);
        if (!ref) {
            printf("  skip %u: SKIP (reference failed)\n", (unsigned)s_open_seek_sample);
            s_open_seek_sample = 0U;
            continue;
        }
        if (info.channels != 2 || !open_guest_state(&g, 3U, ogg, length, 1, 2)) {
            free(ref);
            s_open_seek_sample = 0U;
            continue;
        }
        if (round == 0U)
            printf("mid-file: %s (frames after skipping %u=%u)\n", path,
                   (unsigned)s_open_seek_sample, (unsigned)ref_frames);
        r = &s_references[s_reference_count++ % 8U];
        r->guest_f = g.f; r->pcm = ref; r->frames = ref_frames; r->channels = 2;
        r->cursor = 0U;
        for (;;) {
            k = call_wrapper(g.f, 2, buffer, slot_frames * 2);
            ++calls;
            CHECK(k >= 0 && k <= slot_frames, "seek: frame count in range");
            if (k <= 0)
                break;
            if (frames + (uint32_t)k <= ref_frames)
                CHECK(same_pcm((const int16_t *)region_pointer(PCM_OFFSET),
                               ref + (size_t)frames * 2U, (uint32_t)k, 2,
                               &first_diff),
                      "PCM after a mid-file seek equals the reference (leftover included)");
            frames += (uint32_t)k;
            /* free after one or two slots in most rounds, drain in others */
            if (round % 3U != 2U && calls >= 1 + (int)(round & 1U))
                break;
#if !NV_ORACLE_ASYNC
            if (k < slot_frames)
                break;
#endif
        }
        if (round % 3U == 2U)
            CHECK(frames == ref_frames, "seek: whole remainder delivered");
        (void)wait_worker_midframe(g.f);
        /* the game frees the alloc buffer (state inside) right away */
        isaac_nv_guest_buffer_freed((void *)(uintptr_t)g.alloc_buffer);
        CHECK(isaac_nv_oracle_mirror_busy(g.f) == -1, "seek: freed mirror retired");
        r->guest_f = 0U;
        free(ref);
        free(g.snapshot_state);
        free(g.snapshot_alloc);
        s_open_seek_sample = 0U;
    }
    isaac_nv_oracle_set_worker_frame_delay_us(0U);
    isaac_nv_oracle_worker_stop();
    isaac_nv_oracle_stats_get(&after);
    CHECK(after.asserts == before.asserts, "seek: no decoder assertion");
    CHECK(after.zero_fallbacks == before.zero_fallbacks, "seek: no zero fallback");
    printf("  seek stats: freed_retires=%u worker_refused=%u fallbacks=%u\n",
           (unsigned)(after.freed_retires - before.freed_retires),
           (unsigned)(after.worker_refused - before.worker_refused),
           (unsigned)(after.fallbacks - before.fallbacks));
}

/* A stream whose position does not match its state (what the false
 * free-retire produced on device) must not report end-of-stream: the
 * mirror is dropped and the translated body answers. */
static void run_garbage_position_test(const char *path, const uint8_t *ogg, uint32_t length)
{
    guest_state g;
    stb_vorbis_info info;
    uint32_t ref_frames = 0U;
    int16_t *ref;
    uint32_t buffer = region_address(PCM_OFFSET);
    reference *r;
    isaac_nv_oracle_stats before, after;
    int32_t n;
    int saved_expect_valid;

    memset(&g, 0, sizeof g);
    memset(&s_stream, 0, sizeof s_stream);
    ref = decode_reference(ogg, length, 2, &ref_frames, &info);
    if (!ref || info.channels != 2) {
        free(ref);
        return;
    }
    printf("garbage-position: %s\n", path);
    if (!open_guest_state(&g, 3U, ogg, length, 1, 2)) {
        free(ref);
        return;
    }
    r = &s_references[s_reference_count++ % 8U];
    r->guest_f = g.f; r->pcm = ref; r->frames = ref_frames; r->channels = 2;
    r->cursor = 0U;
    CHECK(isaac_nv_oracle_worker_start(), "worker starts (garbage)");
    isaac_nv_oracle_stats_get(&before);
    /* the state expects a page boundary next (as after a translated
     * seek_start) but the stream stands 777 bytes away from it */
    isaac_nv_oracle_force_page_resync((void *)(uintptr_t)g.f);
    s_stream.position = g.stream_position + 777U;
    saved_expect_valid = s_expect_position_valid;
    s_expect_position_valid = 0;
    n = call_wrapper(g.f, 2, buffer, 4096 * 2);
    CHECK(n == 4096, "garbage position: translated body answers the request");
    CHECK(s_real_calls > 0U, "garbage position: translated body was called");
    isaac_nv_oracle_stats_get(&after);
    CHECK(after.zero_fallbacks - before.zero_fallbacks == 1U &&
          after.fallbacks - before.fallbacks == 1U,
          "garbage position: counted as a zero fallback, not an end of stream");
    CHECK(isaac_nv_oracle_mirror_busy(g.f) == -1, "garbage position: mirror dropped");
    s_expect_position_valid = saved_expect_valid;
    isaac_nv_oracle_worker_stop();
    call_deinit(g.f);
    r->guest_f = 0U;
    free(ref);
    free(g.snapshot_state);
    free(g.snapshot_alloc);
}

/* Hand-off under a live mirror: after stereo slots have moved the stream
 * ahead of the frozen state, the guest asks for another layout
 * (RECLONE-channels; four channels of a stereo file is the copy-and-zero
 * branch of convert_channels_short_interleaved, the only other layout the
 * decoder accepts for a stereo file).  The re-clone reads the stream
 * position, so the runtime must first seek the stream back to the clone
 * position -- and must not have done so after every ordinary slot (the
 * read-ahead is expected to stand past the state before the change). */
static void run_channels_reclone_test(const char *path, const uint8_t *ogg,
                                      uint32_t length)
{
    guest_state g;
    stb_vorbis_info info;
    uint32_t ref2_frames = 0U;
    uint32_t ref4_frames = 0U;
    int16_t *ref2;
    int16_t *ref4;
    int16_t *out;
    uint32_t buffer = region_address(PCM_OFFSET);
    reference *r;
    isaac_nv_oracle_stats before, after;
    uint32_t seeks_before;
    uint32_t got;
    uint32_t calls = 0U;
    uint32_t first_diff = 0U;
    int i;

    memset(&g, 0, sizeof g);
    memset(&s_stream, 0, sizeof s_stream);
    ref2 = decode_reference(ogg, length, 2, &ref2_frames, &info);
    ref4 = decode_reference(ogg, length, 4, &ref4_frames, &info);
    if (!ref2 || !ref4 || info.channels != 2) {
        free(ref2);
        free(ref4);
        return;
    }
    printf("channels-reclone: %s\n", path);
    if (!open_guest_state(&g, 3U, ogg, length, 1, 2)) {
        free(ref2);
        free(ref4);
        return;
    }
    r = &s_references[s_reference_count++ % 8U];
    r->guest_f = g.f; r->pcm = ref2; r->frames = ref2_frames; r->channels = 2;
    r->cursor = 0U;
    CHECK(isaac_nv_oracle_worker_start(), "worker starts (channels)");
    isaac_nv_oracle_stats_get(&before);
    for (i = 0; i < 3; ++i) {
        int32_t n = call_wrapper(g.f, 2, buffer, 4096 * 2);
        CHECK(SLOT_OK(n, 4096), "channels: stereo slot served");
        usleep(5000);
    }
    /* The VERIFY build runs the translated body after every call and seeks
     * the stream back for it (s_expect_position_valid stays 0 there). */
    if (s_expect_position_valid)
        CHECK(s_stream.position > s_expect_position,
              "channels: the read-ahead leaves the stream past the state between slots");
    seeks_before = s_stream.seeks;
    r->channels = 4; r->pcm = ref4; r->frames = ref4_frames; r->cursor = 0U;
    out = (int16_t *)malloc(((size_t)ref4_frames + 65536U) * 4U * 2U);
    got = drain(g.f, 4, 2048, out, ref4_frames + 65536U, 0, &calls);
    CHECK(s_stream.seeks > seeks_before,
          "channels: the stream was seeked back before the re-clone read it");
    CHECK(got == ref4_frames,
          "channels: the re-cloned mirror returns the reference frame count");
    if (got == ref4_frames)
        CHECK(same_pcm(out, ref4, got, 4, &first_diff),
              "channels: re-cloned PCM equals the direct four-channel decode");
    isaac_nv_oracle_stats_get(&after);
    CHECK(after.reclones - before.reclones == 1U, "channels: exactly one re-clone");
    CHECK(after.fallbacks == before.fallbacks, "channels: no translated fallback");
    CHECK(after.asserts == before.asserts &&
          after.zero_fallbacks == before.zero_fallbacks,
          "channels: no assertion, no zero fallback");
    printf("  channels stats: frames=%u calls=%u seeks=%u reclones=%u\n",
           (unsigned)got, (unsigned)calls, (unsigned)s_stream.seeks,
           (unsigned)(after.reclones - before.reclones));
    isaac_nv_oracle_worker_stop();
    call_deinit(g.f);
    r->guest_f = 0U;
    free(out);
    free(ref2);
    free(ref4);
    free(g.snapshot_state);
    free(g.snapshot_alloc);
}

#if NV_ORACLE_ASYNC
static int s_async_ran;

/* Through the wrapper from `cursor` to the end of the stream: every call is
 * in range, its PCM equals the reference at the cursor, and 0 frames arrive
 * only at the real end.  pace_us == 0 is the hostile caller that never
 * yields between calls; the game's own cadence is one call per Update. */
static uint32_t drain_checked(uint32_t f, int channels, int slot_frames,
                              const int16_t *ref, uint32_t ref_frames,
                              uint32_t cursor, uint32_t pace_us,
                              const char *what)
{
    uint32_t buffer = region_address(PCM_OFFSET);
    uint32_t calls = 0U;
    uint32_t first_diff = 0U;

    for (;;) {
        int32_t k = call_wrapper(f, channels, buffer, slot_frames * channels);

        ++calls;
        CHECK(k >= 0 && k <= slot_frames, what);
        if (k <= 0) {
            CHECK(cursor == ref_frames,
                  "async: 0 frames only at the real end of stream");
            break;
        }
        CHECK(cursor + (uint32_t)k <= ref_frames,
              "async: never more frames than the reference");
        if (cursor + (uint32_t)k <= ref_frames)
            CHECK(same_pcm((const int16_t *)region_pointer(PCM_OFFSET),
                           ref + (size_t)cursor * (size_t)channels,
                           (uint32_t)k, channels, &first_diff),
                  "async: partial slot PCM equals the reference");
        cursor += (uint32_t)k;
        if (pace_us)
            usleep((useconds_t)pace_us);
        if (calls > 100000U)
            break;
    }
    return cursor;
}

/* Wait (up to 1 s) until the worker has `frames` ready for f. */
static int wait_pcm_ahead(uint32_t f, int32_t frames)
{
    int i;
    for (i = 0; i < 20000; ++i) {
        if (isaac_nv_oracle_pcm_ahead_frames(f) >= frames)
            return 1;
        usleep(50);
    }
    return 0;
}

/* ISAAC_VITA_NATIVE_VORBIS_ASYNC: partial slots, the sync budget, the early
 * clone at stb_vorbis_open_file, and the lifecycle edges under partial
 * serving, with the production defaults of the knobs. */
static void run_async_tests(const char *path, const uint8_t *ogg, uint32_t length)
{
    guest_state g;
    stb_vorbis_info info;
    uint32_t ref_frames = 0U;
    int16_t *ref;
    uint32_t buffer = region_address(PCM_OFFSET);
    reference *r;
    isaac_nv_oracle_stats s0, s1, s2;
    uint32_t first_diff = 0U;
    const int slot_frames = 0x10000 / 4;   /* the game's 64 KiB stereo slot */
    const int32_t min_frames = (int32_t)ISAAC_NV_SLOT_MIN_FRAMES;
    uint32_t sync_bound;
    int32_t n;
    uint32_t got;
    uint32_t reads_before;
    unsigned fallback_lines_before;

    memset(&g, 0, sizeof g);
    memset(&s_stream, 0, sizeof s_stream);
    ref = decode_reference(ogg, length, 2, &ref_frames, &info);
    if (!ref || info.channels != 2 || ref_frames < 8U * 16384U) {
        free(ref);
        return;
    }
    s_async_ran = 1;
    sync_bound = (uint32_t)min_frames + (uint32_t)info.max_frame_size;
    printf("async: %s (min_frames=%d budget_us=%u early_chunks=%u)\n", path,
           (int)min_frames, (unsigned)ISAAC_NV_SYNC_BUDGET_US,
           (unsigned)ISAAC_NV_EARLY_TOPUP_CHUNKS);
    CHECK(isaac_nv_oracle_worker_start(), "worker starts (async)");

    /* 1. Fresh mirror, slow worker (3 ms per frame), frozen clock: the first
     * call returns at least MIN_FRAMES, the game thread decoded at most
     * MIN_FRAMES plus one frame, no call returns 0 before the end and the
     * concatenated PCM equals the direct decode. */
    isaac_nv_oracle_set_worker_frame_delay_us(3000U);
    isaac_nv_oracle_set_fake_clock(1, 0U);
    if (!open_guest_state(&g, 3U, ogg, length, 1, 2)) {
        free(ref);
        return;
    }
    r = &s_references[s_reference_count++ % 8U];
    r->guest_f = g.f; r->pcm = ref; r->frames = ref_frames; r->channels = 2;
    r->cursor = 0U;
    isaac_nv_oracle_stats_get(&s0);
    n = call_wrapper(g.f, 2, buffer, slot_frames * 2);
    isaac_nv_oracle_stats_get(&s1);
    CHECK(n >= min_frames && n <= slot_frames,
          "async: first slot of a fresh mirror returns at least MIN_FRAMES");
    CHECK(s1.frames_sync - s0.frames_sync <= sync_bound,
          "async: the game thread decoded at most MIN_FRAMES plus one frame");
    CHECK(n > 0 && same_pcm((const int16_t *)region_pointer(PCM_OFFSET), ref,
                            (uint32_t)n, 2, &first_diff),
          "async: first partial slot PCM equals the reference");
    got = (uint32_t)(n > 0 ? n : 0);
    /* second call: after two top-ups the OGG ring stands above the guard
     * (2 x 24 KiB minus what two partial slots consumed, ring 64 KiB), so
     * the worker must resume on its own as soon as the game thread has left
     * the bounded path (game_waiting cleared, worker signalled).  The tight
     * caller loop below legitimately starves it again, so the resume is
     * checked here, where it is deterministic. */
    n = call_wrapper(g.f, 2, buffer, slot_frames * 2);
    CHECK(n >= min_frames && n <= slot_frames,
          "async: second slot of a fresh mirror returns at least MIN_FRAMES");
    CHECK(n > 0 && got + (uint32_t)n <= ref_frames &&
              same_pcm((const int16_t *)region_pointer(PCM_OFFSET),
                       ref + (size_t)got * 2U, (uint32_t)n, 2, &first_diff),
          "async: second partial slot PCM continues the reference");
    got += (uint32_t)(n > 0 ? n : 0);
    CHECK(wait_pcm_ahead(g.f, 1),
          "async: the worker resumes once the game thread leaves the bounded path");
    got = drain_checked(g.f, 2, slot_frames, ref, ref_frames, got, 0U,
                        "async: frame count in range (fresh)");
    CHECK(got == ref_frames, "async: fresh mirror delivers the whole stream");
    isaac_nv_oracle_stats_get(&s2);
    CHECK(s2.partial_slots > s0.partial_slots, "async: partial slots were served");
    CHECK(s2.zero_fallbacks == s0.zero_fallbacks && s2.fallbacks == s0.fallbacks,
          "async: no fallback under partial serving");
    CHECK(s2.frames_worker > s0.frames_worker,
          "async: the worker produced frames while partial slots were served");
    /* loop restart under partial serving */
    restore_guest_state(&g);
    call_seek_start(g.f, isaac_nv_oracle_first_page((void *)(uintptr_t)g.f));
    r->cursor = 0U;
    got = drain_checked(g.f, 2, slot_frames, ref, ref_frames, 0U, 0U,
                        "async: frame count in range (loop restart)");
    CHECK(got == ref_frames, "async: loop restart delivers the whole stream again");
    isaac_nv_oracle_stats_get(&s1);
    CHECK(s1.reclones == s2.reclones + 1U, "async: the loop restart re-cloned once");

    /* 2. Budget: the clock jumps SYNC_BUDGET_US per read and the worker is
     * quick (0.5 ms per frame), so the bounded path stops after its first
     * decoded frame -- and still returns at least one frame. */
    isaac_nv_oracle_set_worker_frame_delay_us(500U);
    isaac_nv_oracle_set_fake_clock(1, (uint32_t)ISAAC_NV_SYNC_BUDGET_US);
    reopen_guest_state(&g);
    r->cursor = 0U;
    isaac_nv_oracle_stats_get(&s0);
    n = call_wrapper(g.f, 2, buffer, slot_frames * 2);
    isaac_nv_oracle_stats_get(&s1);
    CHECK(n >= 1 && n <= slot_frames,
          "async: budget-bounded first slot still returns at least one frame");
    CHECK(s1.budget_hits > s0.budget_hits || n >= min_frames,
          "async: the sync budget stopped the game thread");
    CHECK(s1.frames_sync - s0.frames_sync <= sync_bound,
          "async: budget path decoded at most MIN_FRAMES plus one frame");
    CHECK(n > 0 && same_pcm((const int16_t *)region_pointer(PCM_OFFSET), ref,
                            (uint32_t)n, 2, &first_diff),
          "async: budget-bounded slot PCM equals the reference");
    got = drain_checked(g.f, 2, slot_frames, ref, ref_frames,
                        (uint32_t)(n > 0 ? n : 0), 0U,
                        "async: frame count in range (budget)");
    CHECK(got == ref_frames, "async: whole stream after a budget-bounded first slot");
    isaac_nv_oracle_set_fake_clock(0, 0U);
    isaac_nv_oracle_set_worker_frame_delay_us(0U);
    call_deinit(g.f);
    CHECK(isaac_nv_oracle_mirror_busy(g.f) == -1, "async: deinit retired the mirror");
    r->guest_f = 0U;
    free(g.snapshot_state);
    free(g.snapshot_alloc);

    /* 3. Open hook: the mirror is cloned and its OGG ring primed at
     * stb_vorbis_open_file; by the first Decode the worker has PCM ready, so
     * slot 1 is served from the ring with sync_frames == 0. */
    memset(&g, 0, sizeof g);
    memset(&s_stream, 0, sizeof s_stream);
    if (!open_guest_state(&g, 3U, ogg, length, 1, 2)) {
        free(ref);
        return;
    }
    r = &s_references[s_reference_count++ % 8U];
    r->guest_f = g.f; r->pcm = ref; r->frames = ref_frames; r->channels = 2;
    r->cursor = 0U;
    reads_before = s_stream.reads;
    isaac_nv_oracle_stats_get(&s0);
    call_open_hook(g.f);
    isaac_nv_oracle_stats_get(&s1);
    CHECK(s1.early_clones == s0.early_clones + 1U &&
              s1.mirrors_created == s0.mirrors_created + 1U,
          "open hook cloned the mirror early");
    CHECK(s_stream.reads > reads_before, "open hook primed the OGG ring");
    CHECK(s_stream.bad_markers == 0U && s_stream.bad_this == 0U,
          "open hook stream calls carry the frozen markers and ecx=this");
    CHECK(wait_pcm_ahead(g.f, min_frames),
          "worker produced MIN_FRAMES of lead before the first Decode");
    isaac_nv_oracle_stats_get(&s0);
    n = call_wrapper(g.f, 2, buffer, slot_frames * 2);
    isaac_nv_oracle_stats_get(&s1);
    CHECK(n >= min_frames && n <= slot_frames,
          "open hook: first slot returns at least MIN_FRAMES");
    CHECK(s1.frames_sync == s0.frames_sync,
          "open hook: first slot needs no game-thread decoding (sync_frames == 0)");
    CHECK(strstr(s_last_slot_line, " n=1 ") != NULL &&
              strstr(s_last_slot_line, " src=worker ") != NULL &&
              strstr(s_last_slot_line, " sync_frames=0 ") != NULL,
          "open hook: SLOT n=1 receipt says src=worker sync_frames=0");
    CHECK(n > 0 && same_pcm((const int16_t *)region_pointer(PCM_OFFSET), ref,
                            (uint32_t)n, 2, &first_diff),
          "open hook: first slot PCM equals the reference");
    got = drain_checked(g.f, 2, slot_frames, ref, ref_frames,
                        (uint32_t)(n > 0 ? n : 0), 0U,
                        "async: frame count in range (open hook)");
    CHECK(got == ref_frames, "open hook: whole stream delivered");
    call_deinit(g.f);
    r->guest_f = 0U;
    free(g.snapshot_state);
    free(g.snapshot_alloc);

    /* 4. Open hook, then the guest seeks before the first Decode (a layer
     * synchronised to the main track): the early mirror is dirty and the
     * first Decode re-clones from the guest's own position. */
    memset(&g, 0, sizeof g);
    memset(&s_stream, 0, sizeof s_stream);
    if (!open_guest_state(&g, 3U, ogg, length, 1, 2)) {
        free(ref);
        return;
    }
    r = &s_references[s_reference_count++ % 8U];
    r->guest_f = g.f; r->pcm = ref; r->frames = ref_frames; r->channels = 2;
    r->cursor = 0U;
    call_open_hook(g.f);
    (void)wait_pcm_ahead(g.f, min_frames);
    restore_guest_state(&g);
    call_seek_start(g.f, isaac_nv_oracle_first_page((void *)(uintptr_t)g.f));
    isaac_nv_oracle_stats_get(&s0);
    got = drain_checked(g.f, 2, slot_frames, ref, ref_frames, 0U, 0U,
                        "async: frame count in range (open hook + seek)");
    isaac_nv_oracle_stats_get(&s1);
    CHECK(got == ref_frames,
          "open hook + guest seek: the whole stream from the guest's position");
    CHECK(s1.reclones == s0.reclones + 1U,
          "open hook + guest seek: exactly one RECLONE-seek");
    CHECK(s1.fallbacks == s0.fallbacks && s1.zero_fallbacks == s0.zero_fallbacks,
          "open hook + guest seek: no fallback");
    call_deinit(g.f);
    r->guest_f = 0U;
    free(g.snapshot_state);
    free(g.snapshot_alloc);

    /* 5. Open hook rejections: a failed open (eax = 0) and a push-mode state
     * clone nothing and log no fallback (the first Decode decides that). */
    memset(&g, 0, sizeof g);
    memset(&s_stream, 0, sizeof s_stream);
    if (!open_guest_state(&g, 3U, ogg, length, 1, 2)) {
        free(ref);
        return;
    }
    r = &s_references[s_reference_count++ % 8U];
    r->guest_f = g.f; r->pcm = ref; r->frames = ref_frames; r->channels = 2;
    r->cursor = 0U;
    fallback_lines_before = s_fallback_lines;
    isaac_nv_oracle_stats_get(&s0);
    call_open_hook(0U);
    isaac_nv_oracle_stats_get(&s1);
    CHECK(s1.mirrors_created == s0.mirrors_created &&
              s1.early_clones == s0.early_clones &&
              s1.early_rejected == s0.early_rejected,
          "open hook: a failed open clones nothing");
    isaac_nv_oracle_set_push_mode((void *)(uintptr_t)g.f, 1);
    call_open_hook(g.f);
    isaac_nv_oracle_set_push_mode((void *)(uintptr_t)g.f, 0);
    isaac_nv_oracle_stats_get(&s1);
    CHECK(s1.mirrors_created == s0.mirrors_created &&
              s1.early_rejected == s0.early_rejected + 1U &&
              s_fallback_lines == fallback_lines_before,
          "open hook: a rejected clone is counted, not logged as a fallback");
    CHECK(isaac_nv_oracle_mirror_busy(g.f) == -1,
          "open hook: no mirror after the rejections");

    /* 6. deinit under partial serving while the slow worker is mid-frame */
    isaac_nv_oracle_set_worker_frame_delay_us(3000U);
    n = call_wrapper(g.f, 2, buffer, slot_frames * 2);
    CHECK(n >= 1 && n <= slot_frames, "async: partial slot before deinit");
    CHECK(wait_worker_midframe(g.f), "async: worker is mid-frame before deinit");
    call_deinit(g.f);
    CHECK(isaac_nv_oracle_mirror_busy(g.f) == -1,
          "async: deinit retired the mirror while the worker was mid-frame");
    isaac_nv_oracle_set_worker_frame_delay_us(0U);
    r->guest_f = 0U;
    free(g.snapshot_state);
    free(g.snapshot_alloc);

    /* 7. The game's cadence: one call per Update with the worker at full
     * speed.  The whole stream is delivered exactly and the worker, not the
     * game thread, decodes the bulk of it. */
    memset(&g, 0, sizeof g);
    memset(&s_stream, 0, sizeof s_stream);
    if (!open_guest_state(&g, 3U, ogg, length, 1, 2)) {
        free(ref);
        return;
    }
    r = &s_references[s_reference_count++ % 8U];
    r->guest_f = g.f; r->pcm = ref; r->frames = ref_frames; r->channels = 2;
    r->cursor = 0U;
    isaac_nv_oracle_stats_get(&s0);
    got = drain_checked(g.f, 2, slot_frames, ref, ref_frames, 0U, 2000U,
                        "async: frame count in range (paced)");
    CHECK(got == ref_frames, "async: paced drain delivers the whole stream");
    isaac_nv_oracle_stats_get(&s1);
    CHECK(s1.frames_worker - s0.frames_worker > s1.frames_sync - s0.frames_sync,
          "async: with the game's cadence the worker decodes most frames");
    CHECK(s1.fallbacks == s0.fallbacks && s1.zero_fallbacks == s0.zero_fallbacks,
          "async: paced drain needs no fallback");
    call_deinit(g.f);
    r->guest_f = 0U;
    free(g.snapshot_state);
    free(g.snapshot_alloc);

    /* 8. Worker timeout under ASYNC: the mirror is cloned at the open and
     * the worker stalls inside its first frame for longer than
     * ISAAC_NV_ACQUIRE_TIMEOUT_US, so the first Decode finds nothing ready
     * and cannot take the decoder role.  The whole (short) file already sits
     * in the OGG ring (ogg_eof), which must not turn the timeout into an end
     * of stream: the call hands 0 frames to the guest only at a real end.
     * Expected: the mirror is dropped (zero fallback, one ERR line) and the
     * translated body answers from the guest state's position. */
    memset(&g, 0, sizeof g);
    memset(&s_stream, 0, sizeof s_stream);
    if (!open_guest_state(&g, 3U, ogg, length, 1, 2)) {
        free(ref);
        return;
    }
    r = &s_references[s_reference_count++ % 8U];
    r->guest_f = g.f; r->pcm = ref; r->frames = ref_frames; r->channels = 2;
    r->cursor = 0U;
    isaac_nv_oracle_set_worker_frame_delay_us(ISAAC_NV_ACQUIRE_TIMEOUT_US +
                                              50000U);
    call_open_hook(g.f);
    CHECK(wait_worker_midframe(g.f), "async: worker stalls inside its first frame");
    CHECK(isaac_nv_oracle_pcm_ahead_frames(g.f) == 0,
          "async: nothing is ready while the worker stalls");
    {
        unsigned err_before = s_err_lines;
        unsigned real_before = s_real_calls;

        isaac_nv_oracle_stats_get(&s0);
        n = call_wrapper(g.f, 2, buffer, slot_frames * 2);
        isaac_nv_oracle_stats_get(&s1);
        ++s_expected_err_lines;
        CHECK(n > 0, "async: a worker timeout never returns 0 frames before EOF");
        CHECK(s1.zero_fallbacks == s0.zero_fallbacks + 1U &&
                  s1.fallbacks == s0.fallbacks + 1U,
              "async: the stalled mirror is dropped through the zero fallback");
        CHECK(s_err_lines == err_before + 1U,
              "async: the worker timeout logs one ERR line");
        CHECK(s_real_calls == real_before + 1U,
              "async: the translated body served the call after the timeout");
        CHECK(n > 0 && same_pcm((const int16_t *)region_pointer(PCM_OFFSET),
                                ref, (uint32_t)n, 2, &first_diff),
              "async: translated PCM after the timeout starts at the guest state");
        CHECK(isaac_nv_oracle_mirror_busy(g.f) == -1,
              "async: no mirror remains after the worker timeout");
        CHECK(s1.asserts == s0.asserts && s1.underruns == s0.underruns,
              "async: the timeout is neither an assertion nor an underrun");
    }
    isaac_nv_oracle_set_worker_frame_delay_us(0U);
    call_deinit(g.f);
    r->guest_f = 0U;
    free(g.snapshot_state);
    free(g.snapshot_alloc);

    /* 9. Early clone, then the guest frees the state's buffer before any
     * Decode (StreamSource::Open failed after stb_vorbis_open_file) while
     * the worker is mid-frame; a new state at the same address is cloned
     * early again and decodes from the start. */
    memset(&g, 0, sizeof g);
    memset(&s_stream, 0, sizeof s_stream);
    if (!open_guest_state(&g, 3U, ogg, length, 1, 2)) {
        free(ref);
        return;
    }
    r = &s_references[s_reference_count++ % 8U];
    r->guest_f = g.f; r->pcm = ref; r->frames = ref_frames; r->channels = 2;
    r->cursor = 0U;
    isaac_nv_oracle_set_worker_frame_delay_us(3000U);
    isaac_nv_oracle_stats_get(&s0);
    call_open_hook(g.f);
    CHECK(wait_worker_midframe(g.f), "early clone: worker is mid-frame after the open");
    isaac_nv_guest_buffer_freed((void *)(uintptr_t)g.alloc_buffer);
    isaac_nv_oracle_stats_get(&s1);
    CHECK(isaac_nv_oracle_mirror_busy(g.f) == -1,
          "early clone: freed before the first Decode retires the mirror");
    CHECK(s1.freed_retires == s0.freed_retires + 1U &&
              s1.early_clones == s0.early_clones + 1U,
          "early clone: the free is accounted as a freed retire");
    isaac_nv_oracle_set_worker_frame_delay_us(0U);
    reopen_guest_state(&g);
    r->cursor = 0U;
    call_open_hook(g.f);
    isaac_nv_oracle_stats_get(&s2);
    CHECK(s2.early_clones == s1.early_clones + 1U &&
              s2.mirrors_created == s1.mirrors_created + 1U,
          "early clone: a new state at the freed address is cloned early again");
    CHECK(wait_pcm_ahead(g.f, min_frames),
          "early clone: lead time after the re-open");
    got = drain_checked(g.f, 2, slot_frames, ref, ref_frames, 0U, 0U,
                        "async: frame count in range (freed + re-open)");
    CHECK(got == ref_frames, "early clone: whole stream after the free and re-open");
    call_deinit(g.f);
    r->guest_f = 0U;
    free(g.snapshot_state);
    free(g.snapshot_alloc);
    isaac_nv_oracle_stats_get(&s1);
    printf("  async stats: partial_slots=%u budget_hits=%u early=%u "
           "early_rejected=%u\n", (unsigned)s1.partial_slots,
           (unsigned)s1.budget_hits, (unsigned)s1.early_clones,
           (unsigned)s1.early_rejected);
    isaac_nv_oracle_worker_stop();
    free(ref);
}
#endif

#if NV_ORACLE_ASYNC
/* The early clone on every test layout: mono (the file is shorter than the
 * two priming chunks, so the ring hits its end at the open), 5.1 (a 12-byte
 * frame, 5461-frame slots) and the two stereo files.  By the first Decode
 * the worker has MIN_FRAMES ready and the slot needs no game-thread decode;
 * the whole stream then arrives exactly, 0 only at the end. */
static void run_async_early_tests(const char *path, const uint8_t *ogg,
                                  uint32_t length)
{
    guest_state g;
    stb_vorbis_info info;
    uint32_t ref_frames = 0U;
    int16_t *ref;
    uint32_t buffer = region_address(PCM_OFFSET);
    reference *r;
    isaac_nv_oracle_stats s0, s1;
    uint32_t first_diff = 0U;
    int channels;
    int slot_frames;
    int32_t lead;
    int32_t n;
    uint32_t got;
    unsigned early_lines_before;

    memset(&g, 0, sizeof g);
    memset(&s_stream, 0, sizeof s_stream);
    {
        stb_vorbis *probe = stb_vorbis_open_memory(ogg, (int)length, NULL, NULL);
        if (!probe)
            return;
        channels = stb_vorbis_get_info(probe).channels;
        stb_vorbis_close(probe);
    }
    ref = decode_reference(ogg, length, channels, &ref_frames, &info);
    if (!ref || ref_frames < 2U * ISAAC_NV_SLOT_MIN_FRAMES) {
        free(ref);
        return;
    }
    slot_frames = 0x10000 / (channels * 2);
    lead = (int32_t)ISAAC_NV_SLOT_MIN_FRAMES;
    if (lead > slot_frames)
        lead = slot_frames;
    printf("async-early: %s ch=%d slot=%d frames=%u\n", path, channels,
           slot_frames, (unsigned)ref_frames);
    CHECK(isaac_nv_oracle_worker_start(), "worker starts (async-early)");
    if (!open_guest_state(&g, 3U, ogg, length, 1, channels)) {
        free(ref);
        return;
    }
    r = &s_references[s_reference_count++ % 8U];
    r->guest_f = g.f; r->pcm = ref; r->frames = ref_frames;
    r->channels = channels; r->cursor = 0U;
    early_lines_before = s_early_lines;
    isaac_nv_oracle_stats_get(&s0);
    call_open_hook(g.f);
    isaac_nv_oracle_stats_get(&s1);
    CHECK(s1.early_clones == s0.early_clones + 1U,
          "async-early: the open hook cloned the mirror");
    CHECK(s_early_lines == early_lines_before + 1U,
          "async-early: one EARLY receipt line per early clone");
    CHECK(wait_pcm_ahead(g.f, lead),
          "async-early: the worker has MIN_FRAMES ready before the first Decode");
    isaac_nv_oracle_stats_get(&s0);
    n = call_wrapper(g.f, channels, buffer, slot_frames * channels);
    isaac_nv_oracle_stats_get(&s1);
    CHECK(n >= lead && n <= slot_frames,
          "async-early: first slot returns at least MIN_FRAMES");
    CHECK(s1.frames_sync == s0.frames_sync,
          "async-early: first slot needs no game-thread decoding");
    CHECK(n > 0 && same_pcm((const int16_t *)region_pointer(PCM_OFFSET), ref,
                            (uint32_t)n, channels, &first_diff),
          "async-early: first slot PCM equals the reference");
    got = drain_checked(g.f, channels, slot_frames, ref, ref_frames,
                        (uint32_t)(n > 0 ? n : 0), 0U,
                        "async-early: frame count in range");
    CHECK(got == ref_frames, "async-early: whole stream delivered");
    isaac_nv_oracle_stats_get(&s1);
    CHECK(s1.fallbacks == s0.fallbacks && s1.zero_fallbacks == s0.zero_fallbacks &&
              s1.asserts == s0.asserts && s1.underruns == s0.underruns,
          "async-early: no fallback, assertion or underrun");
    call_deinit(g.f);
    CHECK(isaac_nv_oracle_mirror_busy(g.f) == -1, "async-early: deinit retired the mirror");
    r->guest_f = 0U;
    free(g.snapshot_state);
    free(g.snapshot_alloc);
    isaac_nv_oracle_worker_stop();
    free(ref);
}
#endif

static void test_crc32(void)
{
    static const char vector[] = "123456789";
    CHECK(isaac_nv_crc32(0U, vector, 9U) == 0xcbf43926U,
          "CRC-32 check value");
    CHECK(isaac_nv_crc32(isaac_nv_crc32(0U, vector, 4U), vector + 4, 5U) ==
              0xcbf43926U,
          "CRC-32 is resumable");
}

int main(int argc, char **argv)
{
    int i;
    void *mapped;

    s_log_echo = getenv("ISAAC_NV_ORACLE_ECHO") != NULL;
    mapped = mmap(NULL, REGION_BYTES, PROT_READ | PROT_WRITE,
                  MAP_PRIVATE | MAP_ANONYMOUS
#ifdef MAP_32BIT
                  | MAP_32BIT
#endif
                  , -1, 0);
    if (mapped == MAP_FAILED || (uintptr_t)mapped + REGION_BYTES > 0xffffffffULL) {
        fprintf(stderr, "cannot map a 32-bit-addressable region\n");
        return 2;
    }
    s_region = (uint8_t *)mapped;
    s_region_base = (uint32_t)(uintptr_t)mapped;
    /* fake KAGE stream object: vtable pointer + vtable slots */
    st32(region_address(STREAM_OFFSET), region_address(VTABLE_OFFSET));
    st32(region_address(VTABLE_OFFSET + ISAAC_NV_STREAM_VT_LENGTH), FAKE_LENGTH);
    st32(region_address(VTABLE_OFFSET + ISAAC_NV_STREAM_VT_TELL), FAKE_TELL);
    st32(region_address(VTABLE_OFFSET + ISAAC_NV_STREAM_VT_SEEK), FAKE_SEEK);
    st32(region_address(VTABLE_OFFSET + ISAAC_NV_STREAM_VT_READ), FAKE_READ);

    test_crc32();
    if (argc < 2) {
        fprintf(stderr, "usage: %s file.ogg [...]\n", argv[0]);
        return s_failures ? 1 : 0;
    }
    for (i = 1; i < argc; ++i) {
        uint32_t length = 0U;
        uint8_t *ogg = read_file(argv[i], &length);
        stb_vorbis_info info;
        uint32_t frames = 0U;
        int16_t *probe;
        int file_channels;
        int is_big;

        if (!ogg) {
            fprintf(stderr, "cannot read %s\n", argv[i]);
            ++s_failures;
            continue;
        }
        probe = decode_reference(ogg, length, 2, &frames, &info);
        if (!probe) {
            fprintf(stderr, "cannot decode %s\n", argv[i]);
            ++s_failures;
            free(ogg);
            continue;
        }
        free(probe);
        file_channels = info.channels;
        is_big = length > 512U * 1024U;
        {
            scenario sc;
            /* the game's shape: 64 KiB slots, worker ahead, paced calls */
            sc.name = "stream-worker"; sc.channels = file_channels;
            sc.stream_mode = 1; sc.worker = 1;
            sc.slot_frames = 0x10000 / (2 * file_channels);
            sc.loops = is_big ? 1 : 3; sc.pace_us = is_big ? 20000 : 5000;
            run_scenario(argv[i], ogg, length, &sc);
            /* synchronous native path (model a) */
            sc.name = "stream-sync"; sc.worker = 0; sc.pace_us = 0;
            sc.loops = 2;
            run_scenario(argv[i], ogg, length, &sc);
            /* unpaced worker: game thread and worker race for frames */
            sc.name = "stream-worker-race"; sc.worker = 1; sc.pace_us = 0;
            sc.slot_frames = 1000; sc.loops = 1;
            run_scenario(argv[i], ogg, length, &sc);
            /* stereo downmix / upmix through compute_stereo_samples */
            if (file_channels != 2 && file_channels <= 6) {
                sc.name = "stream-worker-stereo-arg"; sc.channels = 2;
                sc.slot_frames = 0x10000 / 4; sc.pace_us = 5000; sc.loops = 1;
                run_scenario(argv[i], ogg, length, &sc);
            }
            /* whole-sample path: memory mode, odd request sizes */
            sc.name = "memory-sync"; sc.channels = file_channels;
            sc.stream_mode = 0; sc.worker = 0; sc.slot_frames = 777;
            sc.loops = 1; sc.pace_us = 0;
            run_scenario(argv[i], ogg, length, &sc);
        }
        if (file_channels == 2 && !is_big) {
            run_stale_test(argv[i], ogg, length);
            run_fallback_test(argv[i], ogg, length);
            run_midframe_tests(argv[i], ogg, length);
            run_seek_tests(argv[i], ogg, length);
            run_garbage_position_test(argv[i], ogg, length);
            run_channels_reclone_test(argv[i], ogg, length);
#if NV_ORACLE_ASYNC
            run_async_tests(argv[i], ogg, length);
#endif
        }
#if NV_ORACLE_ASYNC
        /* every layout, including the mono file shorter than the priming
         * chunks, the 5.1 file and the big noise file */
        run_async_early_tests(argv[i], ogg, length);
#endif
        free(ogg);
    }
#if NV_ORACLE_ASYNC
    if (s_async_ran) {
        isaac_nv_oracle_stats st;

        isaac_nv_oracle_stats_get(&st);
        printf("async totals: slots=%u frames=%u partial_slots=%u budget_hits=%u "
               "early=%u early_rejected=%u\n", (unsigned)st.slots,
               (unsigned)st.frames, (unsigned)st.partial_slots,
               (unsigned)st.budget_hits, (unsigned)st.early_clones,
               (unsigned)st.early_rejected);
        CHECK(st.early_clones > 0U && st.partial_slots > 0U,
              "async: the open hook and partial serving were exercised");
    }
#endif
    printf("verify lines: match=%u other=%u; err/assert lines=%u; "
           "fallback lines=%u; real calls=%u seek=%u pump=%u deinit=%u\n",
           s_verify_match, s_verify_other, s_err_lines, s_fallback_lines,
           s_real_calls, s_real_seek_calls, s_real_pump_calls,
           s_real_deinit_calls);
#if defined(NV_ORACLE_EXPECT_VERIFY)
    CHECK(s_verify_match > 0U && s_verify_other == 0U,
          "VERIFY build logs only result=MATCH");
#else
    CHECK(s_verify_match == 0U && s_verify_other == 0U,
          "non-VERIFY build logs no VERIFY lines");
#endif
    CHECK(s_err_lines == s_expected_err_lines, "no unexpected ERR/ASSERT receipts");
    printf("%u checks, %u failures\n", s_checks, s_failures);
    return s_failures ? 1 : 0;
}
