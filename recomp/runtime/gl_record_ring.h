#ifndef ISAAC_GL_RECORD_RING_H
#define ISAAC_GL_RECORD_RING_H

#include <stdint.h>

#define ISAAC_GL_RECORD_RING_BYTES (512u * 1024u)

typedef struct IsaacGlRecordHeader {
    uint32_t bytes;
    uint32_t opcode;
    uint32_t return_rva;
    uint32_t payload_bytes;
} IsaacGlRecordHeader;

typedef void (*IsaacGlRecordReplay)(void *context,
    const IsaacGlRecordHeader *header, const void *payload);

typedef struct IsaacGlRecordRing {
    /* 8-byte alignment permits copied doubles and native scalar records. */
    uint64_t storage[ISAAC_GL_RECORD_RING_BYTES / sizeof(uint64_t)];
    IsaacGlRecordReplay replay;
    void *context;
    uint32_t used;
    uint32_t high_water;
    uint32_t records;
    uint32_t copied_bytes;
    uint32_t overflow_drains;
    uint32_t draining;
    /* Worker mode only: monotonic byte positions, one producer, one consumer.
     * write: published by the producer; read/done: by the consumer. */
    volatile uint32_t write_pos, read_pos, done_pos;
    uint32_t producer_waits;
    volatile uint32_t producer_sleeping, consumer_sleeping; /* sleep flags */
    intptr_t producer_sema, consumer_sema;  /* Vita SceUIDs / Win32 HANDLEs */
    /* Wake policy: the producer signals the consumer only on a kick (its own
     * waits, PRESENT, ring full) or every ISAAC_GL_RECORD_KICK_BYTES. */
    uint32_t kick_pos;                      /* producer */
    uint32_t wakes;                         /* producer: signals sent */
    uint32_t consumer_sleeps, consumer_timeouts; /* consumer */
} IsaacGlRecordRing;

#define ISAAC_GL_RECORD_KICK_BYTES (64u * 1024u)

#define ISAAC_GL_RECORD_WRAP_OPCODE 0u

#if defined(ISAAC_VITA_GL_THREAD_WORKER) && ISAAC_VITA_GL_THREAD_WORKER
/* Producer: append (waits for space, never drops); returns the end position
 * of the record, 0 on a contract failure. Consumer: execute one record if
 * any is published; returns 0 when empty. */
uint32_t isaac_gl_record_spsc_append(IsaacGlRecordRing *ring, uint32_t opcode,
    uint32_t return_rva, const void *arguments, uint32_t argument_bytes,
    const void *client, uint32_t client_bytes,
    const void *client2, uint32_t client2_bytes);
int isaac_gl_record_spsc_consume_one(IsaacGlRecordRing *ring);
/* Producer: wait until every record ending at or before `position` ran. */
void isaac_gl_record_spsc_wait_done(IsaacGlRecordRing *ring, uint32_t position);
int isaac_gl_record_spsc_reached(IsaacGlRecordRing *ring, uint32_t position);
#if defined(ISAAC_VITA_GL_THREAD_ADMIT_WAIT) && ISAAC_VITA_GL_THREAD_ADMIT_WAIT == 2
/* Producer, GL admit wait: one wait_done step with an OS timeout of at most
 * timeout_us; 1 = position reached.  Callers loop on their own deadline. */
int isaac_gl_record_spsc_wait_done_for(IsaacGlRecordRing *ring, uint32_t position,
                                       uint32_t timeout_us);
#endif
/* Producer: make sure the consumer runs everything published so far
 * (PRESENT; wait_done and a full ring kick by themselves). */
void isaac_gl_record_spsc_kick(IsaacGlRecordRing *ring);
/* Consumer: sleep until kicked or the timeout (Vita 2 ms; host oracle 1 s,
 * so a missing kick shows up as a counted timeout). */
void isaac_gl_record_spsc_wait_work(IsaacGlRecordRing *ring);
#endif

void isaac_gl_record_ring_init(IsaacGlRecordRing *ring,
    IsaacGlRecordReplay replay, void *context);
/* Zero means no record was published; the caller must execute synchronously
 * or fault. The ring never truncates/drops an accepted record. */
int isaac_gl_record_ring_append(IsaacGlRecordRing *ring, uint32_t opcode,
    uint32_t return_rva, const void *payload, uint32_t payload_bytes);
/* Arguments and one client span go straight into the FIFO; no large scratch. */
int isaac_gl_record_ring_append_client(IsaacGlRecordRing *ring, uint32_t opcode,
    uint32_t return_rva, const void *arguments, uint32_t argument_bytes,
    const void *client, uint32_t client_bytes);
int isaac_gl_record_ring_append_client2(IsaacGlRecordRing *ring, uint32_t opcode,
    uint32_t return_rva, const void *arguments, uint32_t argument_bytes,
    const void *client, uint32_t client_bytes,
    const void *client2, uint32_t client2_bytes);
int isaac_gl_record_ring_drain(IsaacGlRecordRing *ring);

#endif
