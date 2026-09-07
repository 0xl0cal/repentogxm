/* Private native FILE sink algorithm, shared only with its host oracle.
 * The including TU supplies ISAAC_LOG_BATCH_OPEN/WRITE/CLOSE/DEBUG. The
 * payload has one writable trailing byte; lengths preserve record boundaries.
 * No state, allocation, persistent descriptor, or delayed records live here. */
#ifndef ISAAC_HOST_VITA_LOG_FILE_BATCH_PRIVATE_H
#define ISAAC_HOST_VITA_LOG_FILE_BATCH_PRIVATE_H

#include <stdint.h>

static void isaac_vita_log_file_batch_emit(char *payload,
                                          const uint16_t *lengths,
                                          unsigned records)
{
    unsigned total = 0u;
    unsigned accepted = 0u;
    unsigned begin;
    unsigned i;
    int fd;

    for (i = 0u; i < records; ++i)
        total += lengths[i];
    fd = ISAAC_LOG_BATCH_OPEN();
    if (fd >= 0) {
        while (accepted < total) {
            int written = ISAAC_LOG_BATCH_WRITE(fd, payload + accepted,
                                                total - accepted);
            if (written <= 0)
                break;
            accepted += (unsigned)written;
        }
        (void)ISAAC_LOG_BATCH_CLOSE(fd);
    }
    /* Preserve each later record's original append opportunity after an
     * aggregate failure. Never replay the prefix accepted by sceIoWrite.
     * Each suffix gets at most one original-style write, even if it is short.
     * A close error is not retried: accepted bytes may already be durable. */
    begin = 0u;
    for (i = 0u; accepted < total && i < records; ++i) {
        unsigned end = begin + lengths[i];
        if (end > accepted) {
            unsigned offset = begin > accepted ? begin : accepted;
            fd = ISAAC_LOG_BATCH_OPEN();
            if (fd >= 0) {
                (void)ISAAC_LOG_BATCH_WRITE(fd, payload + offset, end - offset);
                (void)ISAAC_LOG_BATCH_CLOSE(fd);
            }
        }
        begin = end;
    }
    /* One debug call per complete original record, including after I/O
     * failure. Temporarily terminate each record as the old pop path did;
     * interior NUL bytes retain the old printf("%s") behavior. */
    begin = 0u;
    for (i = 0u; i < records; ++i) {
        unsigned end = begin + lengths[i];
        char saved = payload[end];
        payload[end] = '\0';
        ISAAC_LOG_BATCH_DEBUG(payload + begin, lengths[i]);
        payload[end] = saved;
        begin = end;
    }
}

#endif
