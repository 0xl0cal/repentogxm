/* Stage-1 same-thread transport. Consumer executes before storage is reused.
 * No allocator, worker, timer, implicit fallback or dropped command. */
#include "gl_record_ring.h"
#include <string.h>

void isaac_gl_record_ring_init(IsaacGlRecordRing *ring,
    IsaacGlRecordReplay replay, void *context)
{
    memset(ring, 0, sizeof *ring);
    ring->replay = replay;
    ring->context = context;
}

int isaac_gl_record_ring_drain(IsaacGlRecordRing *ring)
{
    uint32_t cursor = 0u;
    if (!ring || !ring->replay || ring->draining) return 0;
    ring->draining = 1u;
    while (cursor < ring->used) {
        const IsaacGlRecordHeader *header =
            (const IsaacGlRecordHeader *)((const uint8_t *)ring->storage + cursor);
        ring->replay(ring->context, header, header + 1);
        cursor += header->bytes;
    }
    ring->used = 0u;
    ring->draining = 0u;
    return 1;
}

int isaac_gl_record_ring_append(IsaacGlRecordRing *ring, uint32_t opcode,
    uint32_t return_rva, const void *payload, uint32_t payload_bytes)
{
    return isaac_gl_record_ring_append_client(ring, opcode, return_rva,
        payload, payload_bytes, NULL, 0u);
}

int isaac_gl_record_ring_append_client(IsaacGlRecordRing *ring, uint32_t opcode,
    uint32_t return_rva, const void *arguments, uint32_t argument_bytes,
    const void *client, uint32_t client_bytes)
{
    return isaac_gl_record_ring_append_client2(ring, opcode, return_rva,
        arguments, argument_bytes, client, client_bytes, NULL, 0u);
}

/* Record size and layout; zero when the parts can never fit. */
static uint32_t ring_layout(uint32_t argument_bytes, uint32_t client_bytes,
    uint32_t client2_bytes, uint32_t *second, uint32_t *payload_bytes)
{
    uint32_t bytes;
    if (argument_bytes > ISAAC_GL_RECORD_RING_BYTES / 2u ||
        client_bytes > ISAAC_GL_RECORD_RING_BYTES / 2u ||
        client2_bytes > ISAAC_GL_RECORD_RING_BYTES / 2u)
        return 0u;
    /* The second span starts 8-aligned after the first (zero padding). */
    *second = (argument_bytes + client_bytes + 7u) & ~7u;
    *payload_bytes = client2_bytes ? *second + client2_bytes : argument_bytes + client_bytes;
    if (*payload_bytes > ISAAC_GL_RECORD_RING_BYTES - sizeof(IsaacGlRecordHeader)) return 0u;
    bytes = ((uint32_t)sizeof(IsaacGlRecordHeader) + *payload_bytes + 7u) & ~7u;
    return bytes > ISAAC_GL_RECORD_RING_BYTES ? 0u : bytes;
}

static void ring_fill(IsaacGlRecordHeader *header, uint32_t bytes, uint32_t opcode,
    uint32_t return_rva, uint32_t second, uint32_t payload_bytes,
    const void *arguments, uint32_t argument_bytes,
    const void *client, uint32_t client_bytes,
    const void *client2, uint32_t client2_bytes)
{
    header->bytes = bytes;
    header->opcode = opcode;
    header->return_rva = return_rva;
    header->payload_bytes = payload_bytes;
    if (argument_bytes) memcpy(header + 1, arguments, argument_bytes);
    if (client_bytes) memcpy((uint8_t *)(header + 1) + argument_bytes, client, client_bytes);
    if (client2_bytes) {
        memset((uint8_t *)(header + 1) + argument_bytes + client_bytes, 0,
            second - argument_bytes - client_bytes);
        memcpy((uint8_t *)(header + 1) + second, client2, client2_bytes);
    }
    memset((uint8_t *)(header + 1) + payload_bytes, 0,
        bytes - (uint32_t)sizeof *header - payload_bytes);
}

int isaac_gl_record_ring_append_client2(IsaacGlRecordRing *ring, uint32_t opcode,
    uint32_t return_rva, const void *arguments, uint32_t argument_bytes,
    const void *client, uint32_t client_bytes,
    const void *client2, uint32_t client2_bytes)
{
    uint32_t bytes, payload_bytes = 0u, second = 0u;
    IsaacGlRecordHeader *header;
    if (!ring || !ring->replay || ring->draining ||
        (argument_bytes && !arguments) || (client_bytes && !client) ||
        (client2_bytes && !client2))
        return 0;
    bytes = ring_layout(argument_bytes, client_bytes, client2_bytes, &second, &payload_bytes);
    if (!bytes) return 0;
    if (ring->used > ISAAC_GL_RECORD_RING_BYTES - bytes) {
        ++ring->overflow_drains;
        if (!isaac_gl_record_ring_drain(ring)) return 0;
    }
    header = (IsaacGlRecordHeader *)((uint8_t *)ring->storage + ring->used);
    ring_fill(header, bytes, opcode, return_rva, second, payload_bytes,
        arguments, argument_bytes, client, client_bytes, client2, client2_bytes);
    ring->used += bytes;
    if (ring->used > ring->high_water) ring->high_water = ring->used;
    ++ring->records;
    ring->copied_bytes += payload_bytes;
    return 1;
}

#if defined(ISAAC_VITA_GL_RECORD_CONSUMER_TIMEOUT_US)
/* R60-EXT-SYNTH T2: the render thread's (consumer's) idle wait times out
 * after this many us instead of 2000; every producer wait keeps 2000 us.
 * CMake defines it on this unit only, from a decimal 100..2000. */
# if !defined(__vita__) || !defined(ISAAC_VITA_GL_THREAD_WORKER) || !ISAAC_VITA_GL_THREAD_WORKER
#  error "ISAAC_VITA_GL_RECORD_CONSUMER_TIMEOUT_US needs the Vita GL render thread (ISAAC_VITA_GL_THREAD_WORKER)"
# endif
# if ISAAC_VITA_GL_RECORD_CONSUMER_TIMEOUT_US < 100 || ISAAC_VITA_GL_RECORD_CONSUMER_TIMEOUT_US > 2000
#  error "ISAAC_VITA_GL_RECORD_CONSUMER_TIMEOUT_US must be 100..2000"
# endif

#endif
#if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
/* Positions are monotonic uint32 byte counts; compare by difference.  Both
 * sides sleep on a semaphore behind a flag (Dekker: flag, re-check, sleep),
 * and a side only signals when the other announced it sleeps.  The producer
 * does NOT signal per record (measured on the device: ~700 SignalSema per
 * frame against an 8 %-busy consumer); it kicks on its own waits, on PRESENT,
 * on a full ring and every ISAAC_GL_RECORD_KICK_BYTES.  The consumer's sleep
 * times out, so records below the threshold are picked up within 2 ms on the
 * device; on the host the timeout is 1 s and a timeout that finds work is
 * counted, which is how the oracle proves no kick is missing. */
#if defined(_WIN32)
#include <windows.h>
static uint32_t spsc_load(volatile uint32_t *p)
{ return (uint32_t)InterlockedCompareExchange((volatile LONG *)p, 0, 0); }
static void spsc_store(volatile uint32_t *p, uint32_t v)
{ (void)InterlockedExchange((volatile LONG *)p, (LONG)v); }
#define spsc_publish spsc_store /* Interlocked is a full barrier already */
static uint32_t spsc_take(volatile uint32_t *p)
{ return (uint32_t)InterlockedExchange((volatile LONG *)p, 0); }
static int spsc_sleep_os(intptr_t sema)
{
# if defined(ISAAC_GL_RECORD_ORACLE_MUTATE_NO_KICK)
    return WaitForSingleObject((HANDLE)sema, 20u) == WAIT_OBJECT_0; /* fast mutant */
# else
    return WaitForSingleObject((HANDLE)sema, 1000u) == WAIT_OBJECT_0;
# endif
}
static void spsc_signal_os(intptr_t sema) { (void)ReleaseSemaphore((HANDLE)sema, 1, NULL); }
#elif defined(__vita__)
#include <psp2/kernel/threadmgr.h>
static uint32_t spsc_load(volatile uint32_t *p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
/* Sleep flags need seq_cst (Dekker: flag store, then re-check the other
 * side's position); positions only need release: the seq_cst flag RMW/store
 * that follows a publish on the same thread orders it on ARMv7 (dmb). */
static void spsc_store(volatile uint32_t *p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_SEQ_CST); }
static void spsc_publish(volatile uint32_t *p, uint32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
static uint32_t spsc_take(volatile uint32_t *p) { return __atomic_exchange_n(p, 0u, __ATOMIC_SEQ_CST); }
static int spsc_sleep_os(intptr_t sema)
{
    SceUInt timeout = 2000u;
    return sceKernelWaitSema((int)sema, 1, &timeout) >= 0;
}
#if defined(ISAAC_VITA_GL_RECORD_CONSUMER_TIMEOUT_US)
/* The consumer's sleep only (isaac_gl_record_spsc_wait_work); the producer
 * keeps spsc_sleep_os above. */
static int spsc_sleep_os_consumer(intptr_t sema)
{
    SceUInt timeout = (SceUInt)ISAAC_VITA_GL_RECORD_CONSUMER_TIMEOUT_US;
    return sceKernelWaitSema((int)sema, 1, &timeout) >= 0;
}
void isaac_vita_log(const char *format, ...);
#endif
static void spsc_signal_os(intptr_t sema) { (void)sceKernelSignalSema((int)sema, 1); }
#else
#error "GL record worker: no platform primitives for this target"
#endif

static int spsc_room(IsaacGlRecordRing *ring, uint32_t write, uint32_t need)
{
    return ISAAC_GL_RECORD_RING_BYTES - (write - spsc_load(&ring->read_pos)) >= need;
}

void isaac_gl_record_spsc_kick(IsaacGlRecordRing *ring)
{
    ring->kick_pos = ring->write_pos;
    if (spsc_take(&ring->consumer_sleeping)) {
        spsc_signal_os(ring->consumer_sema);
        ++ring->wakes;
    }
}

void isaac_gl_record_spsc_wait_work(IsaacGlRecordRing *ring)
{
#if defined(ISAAC_VITA_GL_RECORD_CONSUMER_TIMEOUT_US)
    /* Boot receipt, once, from the thread that waits: the render thread is
     * the only caller, and its first idle wait follows its start. */
    static int consumer_timeout_logged;
    if (!consumer_timeout_logged) {
        consumer_timeout_logged = 1;
        isaac_vita_log("KAGE VITA GL RING: consumer_timeout_us=%u producer_timeout_us=2000 record=ph120.rw",
                       (unsigned)ISAAC_VITA_GL_RECORD_CONSUMER_TIMEOUT_US);
    }
#endif
    spsc_store(&ring->consumer_sleeping, 1u);
    if (spsc_load(&ring->read_pos) != spsc_load(&ring->write_pos)) {
        spsc_store(&ring->consumer_sleeping, 0u);
        return;
    }
    ++ring->consumer_sleeps;
#if defined(ISAAC_VITA_GL_RECORD_CONSUMER_TIMEOUT_US)
    if (!spsc_sleep_os_consumer(ring->consumer_sema) &&
#else
    if (!spsc_sleep_os(ring->consumer_sema) &&
#endif
            spsc_load(&ring->read_pos) != spsc_load(&ring->write_pos))
        ++ring->consumer_timeouts;  /* work arrived without a kick */
    spsc_store(&ring->consumer_sleeping, 0u);
}

uint32_t isaac_gl_record_spsc_append(IsaacGlRecordRing *ring, uint32_t opcode,
    uint32_t return_rva, const void *arguments, uint32_t argument_bytes,
    const void *client, uint32_t client_bytes,
    const void *client2, uint32_t client2_bytes)
{
    uint32_t bytes, payload_bytes = 0u, second = 0u, write, offset, tail, need;
    if (!ring || (argument_bytes && !arguments) || (client_bytes && !client) ||
        (client2_bytes && !client2))
        return 0u;
    bytes = ring_layout(argument_bytes, client_bytes, client2_bytes, &second, &payload_bytes);
    if (!bytes) return 0u;
    /* 16-byte records: a non-empty tail always holds a whole wrap header. */
    bytes = (bytes + 15u) & ~15u;
    write = ring->write_pos;
    offset = write % ISAAC_GL_RECORD_RING_BYTES;
    tail = ISAAC_GL_RECORD_RING_BYTES - offset;
    need = tail < bytes ? tail + bytes : bytes; /* a wrap marker fills the tail */
    while (!spsc_room(ring, write, need)) {
        ++ring->producer_waits;
        isaac_gl_record_spsc_kick(ring);       /* the consumer must drain */
        spsc_store(&ring->producer_sleeping, 1u);
        if (spsc_room(ring, write, need)) {
            spsc_store(&ring->producer_sleeping, 0u);
            break;
        }
        (void)spsc_sleep_os(ring->producer_sema);
        spsc_store(&ring->producer_sleeping, 0u);
    }
    if (tail < bytes) {
        IsaacGlRecordHeader *wrap = (IsaacGlRecordHeader *)((uint8_t *)ring->storage + offset);
        wrap->bytes = tail;
        wrap->opcode = ISAAC_GL_RECORD_WRAP_OPCODE;
        wrap->return_rva = 0u;
        wrap->payload_bytes = 0u;
        write += tail;
        offset = 0u;
    }
    ring_fill((IsaacGlRecordHeader *)((uint8_t *)ring->storage + offset), bytes, opcode,
        return_rva, second, payload_bytes, arguments, argument_bytes,
        client, client_bytes, client2, client2_bytes);
    write += bytes;
    ++ring->records;
    ring->copied_bytes += argument_bytes + client_bytes + client2_bytes;
    {
        uint32_t outstanding = write - spsc_load(&ring->read_pos);
        if (outstanding > ring->high_water) ring->high_water = outstanding;
    }
    spsc_publish(&ring->write_pos, write); /* publish after the bytes */
    if (write - ring->kick_pos >= ISAAC_GL_RECORD_KICK_BYTES)
        isaac_gl_record_spsc_kick(ring);
    return write;
}

int isaac_gl_record_spsc_consume_one(IsaacGlRecordRing *ring)
{
    uint32_t read = ring->read_pos;
    const IsaacGlRecordHeader *header;
    if (read == spsc_load(&ring->write_pos)) return 0;
    header = (const IsaacGlRecordHeader *)((const uint8_t *)ring->storage +
        read % ISAAC_GL_RECORD_RING_BYTES);
    if (header->opcode != ISAAC_GL_RECORD_WRAP_OPCODE)
        ring->replay(ring->context, header, header + 1);
    read += header->bytes;
    spsc_publish(&ring->read_pos, read);
    spsc_publish(&ring->done_pos, read);
    if (spsc_take(&ring->producer_sleeping))
        spsc_signal_os(ring->producer_sema);
    return 1;
}

int isaac_gl_record_spsc_reached(IsaacGlRecordRing *ring, uint32_t position)
{
    return (int32_t)(spsc_load(&ring->done_pos) - position) >= 0;
}

void isaac_gl_record_spsc_wait_done(IsaacGlRecordRing *ring, uint32_t position)
{
    while (!isaac_gl_record_spsc_reached(ring, position)) {
#if !defined(ISAAC_GL_RECORD_ORACLE_MUTATE_NO_KICK) /* oracle self-test: lost wake-up */
        isaac_gl_record_spsc_kick(ring);
#endif
        spsc_store(&ring->producer_sleeping, 1u);
        if (isaac_gl_record_spsc_reached(ring, position)) {
            spsc_store(&ring->producer_sleeping, 0u);
            break;
        }
        (void)spsc_sleep_os(ring->producer_sema);
        spsc_store(&ring->producer_sleeping, 0u);
    }
}
#if defined(ISAAC_VITA_GL_THREAD_ADMIT_WAIT) && ISAAC_VITA_GL_THREAD_ADMIT_WAIT == 2

/* One bounded producer sleep (GL admit wait, R60-DRYADMIT-SPEC-20261028.md):
 * wait_done's Dekker steps with a caller timeout; 1 = position reached.  A
 * consumer signal that lands after the timeout leaves one count on
 * producer_sema; every producer wait loops on done_pos, so it costs at most
 * one spurious wake-up. */
static int spsc_sleep_os_for(intptr_t sema, uint32_t timeout_us)
{
# if defined(_WIN32)
    return WaitForSingleObject((HANDLE)sema, (DWORD)((timeout_us + 999u) / 1000u)) == WAIT_OBJECT_0;
# else
    SceUInt timeout = timeout_us;
    return sceKernelWaitSema((int)sema, 1, &timeout) >= 0;
# endif
}

int isaac_gl_record_spsc_wait_done_for(IsaacGlRecordRing *ring, uint32_t position,
                                       uint32_t timeout_us)
{
    if (isaac_gl_record_spsc_reached(ring, position))
        return 1;
# if !defined(ISAAC_GL_RECORD_ORACLE_MUTATE_NO_KICK) /* oracle self-test: lost wake-up */
    isaac_gl_record_spsc_kick(ring);
# endif
    spsc_store(&ring->producer_sleeping, 1u);
    if (!isaac_gl_record_spsc_reached(ring, position))
        (void)spsc_sleep_os_for(ring->producer_sema, timeout_us);
    spsc_store(&ring->producer_sleeping, 0u);
    return isaac_gl_record_spsc_reached(ring, position);
}
# endif
#endif
