#define _GNU_SOURCE
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#include "guest.h"
#include "host_vita_audio.h"
#include "host_vita_audio_cooperative.h"

#define TEST_BYTES 0x00800000U
#define TEST_STACK_FLOOR (GUEST_IMAGE_BASE + 0x007f0000U)
#define TEST_STACK_CEILING (GUEST_IMAGE_BASE + 0x007ff000U)
#define TEST_STREAM_OBJECT (GUEST_IMAGE_BASE + 0x00001000U)
#define TEST_STREAM_VECTOR (GUEST_IMAGE_BASE + 0x00002000U)
#define TEST_STREAM_SOURCE 73U

enum oracle_mode {
    ORACLE_NORMAL = 0,
    ORACLE_STREAM_QUEUE_EMPTY,
    ORACLE_STREAM_REFILL,
    ORACLE_GUEST_FAULT,
    ORACLE_BAD_ESP,
    ORACLE_BAD_JUMP_SITES
};

static uint64_t s_now;
static int s_manager_active;
static unsigned s_guest_calls;
static unsigned s_faults;
static unsigned s_logs;
static unsigned s_receipt_logs;
static int s_bad_log;
static int s_saw_queue_empty;
static int s_saw_empty_repeat;
static int s_saw_refill;
static int s_saw_guest_fault;
static int s_saw_esp_fault;
static int s_saw_jump_fault;
static enum oracle_mode s_mode;
static guest_jump_site s_original_jump_site;
static guest_jump_site s_rogue_jump_site;

uint64_t isaac_vita_get_process_time(void) { return s_now; }
int isaac_vita_audio_manager_is_active(void) { return s_manager_active; }

void isaac_vita_log(const char *format, ...)
{
    char line[768];
    va_list arguments;

    if (!format) {
        s_bad_log = 1;
        return;
    }
    va_start(arguments, format);
    (void)vsnprintf(line, sizeof line, format, arguments);
    va_end(arguments);

    if (strcmp(line,
               "KAGE VITA AUDIO COOPERATIVE: active interval_us=5000") == 0) {
        ++s_logs;
    } else if (strncmp(line, "KAGE VITA AUDIO STREAM RECEIPT: ",
                       sizeof "KAGE VITA AUDIO STREAM RECEIPT: " - 1U) == 0) {
        ++s_receipt_logs;
        if (strstr(line, "edge=queue-empty")) s_saw_queue_empty = 1;
        if (strstr(line, "edge=empty-repeat")) s_saw_empty_repeat = 1;
        if (strstr(line, "edge=refill")) s_saw_refill = 1;
        if (strstr(line, "result=guest-fault")) s_saw_guest_fault = 1;
        if (strstr(line, "result=esp-fault")) s_saw_esp_fault = 1;
        if (strstr(line, "result=jump-fault")) s_saw_jump_fault = 1;
    } else {
        s_bad_log = 1;
    }
}

void guest_fault(CPU *__restrict c, uint32_t address, const char *what)
{
    ++s_faults;
    c->fault_addr = address;
    c->fault = what;
    c->stop_kind = GUEST_RUN_FAULT;
}

void guest_call(CPU *__restrict c, uint32_t target)
{
    uint64_t nested_time;
    uint32_t returned_esp;
    unsigned index;

    ++s_guest_calls;
    if (target != GUEST_IMAGE_BASE + ISAAC_VITA_AUDIO_MANAGER_UPDATE_RVA ||
            c->ecx != GUEST_IMAGE_BASE + ISAAC_VITA_AUDIO_MANAGER_RVA ||
            ld32(c->esp) != ISAAC_VITA_AUDIO_COOPERATIVE_RETURN) {
        guest_fault(c, target, "oracle received the wrong audio call frame");
        return;
    }

    /* A nested safe point would be eligible by time alone.  The owner-only
     * recursion guard must still suppress it. */
    nested_time = s_now;
    s_now += 2000U;                 /* non-zero Manager::Update duration */
    isaac_vita_audio_cooperative_poll(c);
    if (s_now != nested_time + 2000U)
        guest_fault(c, target, "oracle update clock changed unexpectedly");

    (void)gpop(c);
    returned_esp = c->esp;
    if (s_mode == ORACLE_STREAM_QUEUE_EMPTY) {
        for (index = 0U; index < ISAAC_VITA_AUDIO_OGG_QUEUE_COUNT; ++index)
            st32(TEST_STREAM_OBJECT + ISAAC_VITA_AUDIO_OGG_QUEUE_OFFSET +
                     index * ISAAC_VITA_AUDIO_OGG_QUEUE_STRIDE,
                 0U);
    } else if (s_mode == ORACLE_STREAM_REFILL) {
        st32(TEST_STREAM_OBJECT + ISAAC_VITA_AUDIO_OGG_QUEUE_OFFSET, 3U);
    } else if (s_mode == ORACLE_GUEST_FAULT) {
        guest_fault(c, target + 1U, "oracle nested audio fault");
        return;
    }
    if (s_mode == ORACLE_BAD_ESP) {
        c->esp += 4U;
        return;
    }
    if (s_mode == ORACLE_BAD_JUMP_SITES) {
        c->jump_sites = &s_rogue_jump_site;
        return;
    }

    memset(c->r, 0xa5, sizeof c->r);
    c->esp = returned_esp;
    memset(c->x, 0x5a, sizeof c->x);
    memset(c->st, 0x3c, sizeof c->st);
    c->st_top = 6;
    c->fsw = 0x4500U;
    c->f_a = 1U; c->f_b = 2U; c->f_r = 3U;
    c->f_op = FLAG_PARTIAL; c->f_sz = 1U;
    c->f_cf = 1U; c->f_of = 1U; c->f_zf = 1U;
    c->f_sf = 1U; c->f_pf = 1U; c->df = 1U;
    c->last_error = 0xdeadbeefU;
    c->stack_low_water = c->esp - 0x40U;
}

static void initialize_cpu(CPU *c)
{
    unsigned i;

    memset(c, 0, sizeof *c);
    for (i = 0; i < 8U; ++i) {
        c->r[i] = 0x11110000U + i;
        c->st[i] = (double)i + 0.25;
    }
    memset(c->x, 0x27, sizeof c->x);
    c->esp = TEST_STACK_CEILING;
    c->st_top = 3;
    c->fsw = 0x4100U;
    c->f_a = 0x12345678U; c->f_b = 0x87654321U;
    c->f_r = 0x01020304U; c->f_op = FLAG_SUB; c->f_sz = 4U;
    c->f_cf = 1U; c->f_of = 0U; c->f_zf = 0U;
    c->f_sf = 1U; c->f_pf = 1U; c->df = 1U;
    c->stop_kind = GUEST_RUN_RETURNED;
    c->jump_sites = &s_original_jump_site;
    c->stack_owner = c;
    c->stack_floor = TEST_STACK_FLOOR;
    c->stack_ceiling = TEST_STACK_CEILING;
    c->stack_low_water = TEST_STACK_CEILING;
    c->last_error = 0x76543210U;
}

static void initialize_stream(uint32_t manager)
{
    unsigned index;

    st32(manager + ISAAC_VITA_AUDIO_MANAGER_ACTIVE_BEGIN_OFFSET,
         TEST_STREAM_VECTOR);
    st32(manager + ISAAC_VITA_AUDIO_MANAGER_ACTIVE_END_OFFSET,
         TEST_STREAM_VECTOR + 4U);
    st32(TEST_STREAM_VECTOR, TEST_STREAM_OBJECT);
    st32(TEST_STREAM_OBJECT,
         GUEST_IMAGE_BASE + ISAAC_VITA_AUDIO_OGG_VTABLE_RVA);
    st8(TEST_STREAM_OBJECT + ISAAC_VITA_AUDIO_OGG_STOPPED_OFFSET, 0U);
    st8(TEST_STREAM_OBJECT + ISAAC_VITA_AUDIO_OGG_LOOP_OFFSET, 1U);
    st32(TEST_STREAM_OBJECT + ISAAC_VITA_AUDIO_OGG_SOURCE_OFFSET,
         TEST_STREAM_SOURCE);
    st8(TEST_STREAM_OBJECT + ISAAC_VITA_AUDIO_OGG_EOF_OFFSET, 0U);
    for (index = 0U; index < ISAAC_VITA_AUDIO_OGG_QUEUE_COUNT; ++index)
        st32(TEST_STREAM_OBJECT + ISAAC_VITA_AUDIO_OGG_QUEUE_OFFSET +
                 index * ISAAC_VITA_AUDIO_OGG_QUEUE_STRIDE,
             index == 0U ? 3U : 0U);
}

static int expect_skip(CPU *c, unsigned calls)
{
    CPU saved = *c;
    isaac_vita_audio_cooperative_poll(c);
    return s_guest_calls == calls && memcmp(c, &saved, sizeof saved) == 0;
}

static int expect_save_skip(CPU *c, unsigned calls)
{
    CPU saved = *c;
    isaac_vita_audio_cooperative_save_poll(c);
    return s_guest_calls == calls && memcmp(c, &saved, sizeof saved) == 0;
}

int main(void)
{
    CPU cpu;
    CPU saved;
    CPU expected;
    uint32_t manager = GUEST_IMAGE_BASE + ISAAC_VITA_AUDIO_MANAGER_RVA;
    void *wanted = (void *)(uintptr_t)GUEST_IMAGE_BASE;
#if defined(_WIN32)
    void *mapped = VirtualAlloc(wanted, TEST_BYTES,
                                MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
#else
    void *mapped = mmap(wanted, TEST_BYTES, PROT_READ | PROT_WRITE,
                        MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
#endif

    if (mapped != wanted)
        return 2;
    initialize_cpu(&cpu);
    st8(manager + ISAAC_VITA_AUDIO_MANAGER_RUNNING_OFFSET, 1U);
    st8(manager + ISAAC_VITA_AUDIO_MANAGER_SUSPENDED_OFFSET, 0U);
    initialize_stream(manager);

    s_now = 100U;
    if (!expect_skip(&cpu, 0U)) return 3;              /* inactive */
    s_manager_active = 1;
    cpu.stop_kind = GUEST_RUN_EXIT;
    if (!expect_skip(&cpu, 0U)) return 4;              /* stopped */
    cpu.stop_kind = GUEST_RUN_RETURNED;
    st8(manager + ISAAC_VITA_AUDIO_MANAGER_RUNNING_OFFSET, 0U);
    if (!expect_skip(&cpu, 0U)) return 5;              /* not running */
    st8(manager + ISAAC_VITA_AUDIO_MANAGER_RUNNING_OFFSET, 1U);
    st8(manager + ISAAC_VITA_AUDIO_MANAGER_SUSPENDED_OFFSET, 1U);
    if (!expect_skip(&cpu, 0U)) return 6;              /* suspended */
    st8(manager + ISAAC_VITA_AUDIO_MANAGER_SUSPENDED_OFFSET, 0U);

    saved = cpu;
    isaac_vita_audio_cooperative_poll(&cpu);
    expected = saved;
    expected.stack_low_water = saved.esp - 0x40U;
    if (s_guest_calls != 1U || s_faults != 0U || s_logs != 1U || s_bad_log ||
            memcmp(&cpu, &expected, sizeof cpu) != 0)
        return 7;                                      /* state + recursion */

    s_now = 7099U;
    if (!expect_skip(&cpu, 1U)) return 8;              /* 4999 us */
    s_now = 7100U;
    s_mode = ORACLE_STREAM_QUEUE_EMPTY;
    isaac_vita_audio_cooperative_poll(&cpu);
    if (s_guest_calls != 2U || s_faults != 0U || s_logs != 1U)
        return 9;                                      /* exactly 5 ms */

    s_mode = ORACLE_NORMAL;
    s_now = 59099U;
    if (!expect_save_skip(&cpu, 2U)) return 10;         /* 49,999 us */
    s_now = 59100U;
    isaac_vita_audio_cooperative_save_poll(&cpu);
    if (s_guest_calls != 3U || s_faults != 0U || s_logs != 1U)
        return 11;                                     /* exactly 50 ms */

    s_mode = ORACLE_STREAM_REFILL;
    s_now = 66100U;
    isaac_vita_audio_cooperative_poll(&cpu);
    if (s_guest_calls != 4U || s_faults != 0U || s_logs != 1U)
        return 12;                                     /* empty -> queued */

    initialize_cpu(&cpu);
    s_mode = ORACLE_GUEST_FAULT;
    s_now = 73100U;
    isaac_vita_audio_cooperative_poll(&cpu);
    if (s_guest_calls != 5U || s_faults != 1U || !cpu.fault ||
            cpu.fault_addr != GUEST_IMAGE_BASE +
                              ISAAC_VITA_AUDIO_MANAGER_UPDATE_RVA + 1U)
        return 13;

    initialize_cpu(&cpu);
    s_mode = ORACLE_BAD_ESP;
    s_now = 78100U;
    isaac_vita_audio_cooperative_poll(&cpu);
    if (s_guest_calls != 6U || s_faults != 2U ||
            cpu.fault_addr != GUEST_IMAGE_BASE +
                              ISAAC_VITA_AUDIO_MANAGER_UPDATE_RVA ||
            !cpu.fault || strstr(cpu.fault, "ESP") == NULL)
        return 14;

    initialize_cpu(&cpu);
    s_mode = ORACLE_BAD_JUMP_SITES;
    s_now = 83100U;
    isaac_vita_audio_cooperative_poll(&cpu);
    if (s_guest_calls != 7U || s_faults != 3U || !cpu.fault ||
            strstr(cpu.fault, "jump-site") == NULL)
        return 15;

    if (s_bad_log || s_receipt_logs < 7U || !s_saw_queue_empty ||
            !s_saw_empty_repeat || !s_saw_refill || !s_saw_guest_fault ||
            !s_saw_esp_fault || !s_saw_jump_fault)
        return 16;

    puts("Vita audio cooperative host oracle: PASS; gating/room+save-throttle/recursion/state/OGG-receipt/faults");
    return 0;
}
