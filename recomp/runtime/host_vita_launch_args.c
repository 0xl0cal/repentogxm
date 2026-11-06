/* Launch-time guest options from a file; see host_vita_launch_args.h. */
#include "host_vita_launch_args.h"

#include <string.h>

#if !defined(ISAAC_VITA_LAUNCH_ARGS_ORACLE)
#include <psp2/io/fcntl.h>
#include "guest.h"
#endif

/* SCE_ERROR_ERRNO_ENOENT, as in host_vita_runtime_settings.c. */
#define LAUNCH_ARGS_SCE_ENOENT 0x80010002u

/* One byte past the cap (the too-large signal) plus the terminating NUL. */
static char s_text[ISAAC_VITA_LAUNCH_ARGS_BYTES + 2u];

/* --vita-save-slot= state: the parsed slot (0 = none), whether the first
 * store happened, the calls that found a pointer still zero, the restores. */
static unsigned s_slot;
static int s_slot_applied;
static unsigned s_slot_waited;
static unsigned s_slot_restored;

#if defined(ISAAC_VITA_LAUNCH_ARGS_ORACLE)
#define launch_args_open(p) isaac_vita_launch_args_oracle_open(p)
#define launch_args_read(d, b, n) isaac_vita_launch_args_oracle_read(d, b, n)
#define launch_args_close(d) isaac_vita_launch_args_oracle_close(d)
#define launch_args_ld32(a) isaac_vita_launch_args_oracle_ld32(a)
#define launch_args_st32(a, v) isaac_vita_launch_args_oracle_st32(a, v)
#else
#define launch_args_open(p) sceIoOpen(p, SCE_O_RDONLY, 0)
#define launch_args_read(d, b, n) sceIoRead(d, b, n)
#define launch_args_close(d) sceIoClose(d)
/* guest.h accessors: the Vita guest is identity-mapped. */
#define launch_args_ld32(a) ld32(a)
#define launch_args_st32(a, v) st32(a, v)
#endif

static int launch_args_space(char ch)
{
    return ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n';
}

unsigned isaac_vita_launch_args_split(char *text, unsigned length,
                                      char **out, unsigned cap)
{
    unsigned i = 0u;
    unsigned count = 0u;

    while (i < length) {
        if (launch_args_space(text[i]) || text[i] == '\0') {
            text[i++] = '\0';
            continue;
        }
        if (text[i] == '#') {
            while (i < length && text[i] != '\n')
                text[i++] = '\0';
            continue;
        }
        if (count < cap)
            out[count] = text + i;
        ++count;
        while (i < length && !launch_args_space(text[i]) && text[i] != '#' &&
               text[i] != '\0')
            ++i;
    }
    return count;
}

static int launch_args_prefix(const char *token, const char *prefix)
{
    return strncmp(token, prefix, strlen(prefix)) == 0;
}

/* Consumes every --vita-save-slot= token of out[0..count) (compacting the
 * rest) and applies the slot rule.  Returns the status string ("ok" when
 * accepted); *slot gets the slot (0 = none or rejected), *count the guest
 * token count (0 when rejected). */
static const char *launch_args_take_slot(char **out, unsigned *count,
                                         unsigned *slot)
{
    const size_t prefix = strlen(ISAAC_VITA_LAUNCH_ARGS_SLOT_TOKEN);
    unsigned kept = 0u;
    unsigned seen = 0u;
    unsigned value = 0u;
    int bad = 0;
    int set_stage = 0;
    unsigned i;

    for (i = 0u; i < *count; ++i) {
        const char *token = out[i];

        if (launch_args_prefix(token, ISAAC_VITA_LAUNCH_ARGS_SLOT_TOKEN)) {
            const char *digit = token + prefix;

            ++seen;
            if (digit[0] >= '1' && digit[0] <= '3' && digit[1] == '\0')
                value = (unsigned)(digit[0] - '0');
            else
                bad = 1;
            continue;
        }
        if (launch_args_prefix(token, ISAAC_VITA_LAUNCH_ARGS_SET_STAGE_TOKEN))
            set_stage = 1;
        out[kept++] = out[i];
    }
    *slot = 0u;
    if (bad || seen > 1u) {
        *count = 0u;
        return "bad-slot";
    }
    if (set_stage && seen == 0u) {
        *count = 0u;
        return "no-slot";
    }
    *slot = value;
    *count = kept;
    return "ok";
}

unsigned isaac_vita_launch_args_load(const char *argv0, char **out)
{
    char cmdline[ISAAC_VITA_LAUNCH_ARGS_BYTES + 64u];
    const char *status = "ok";
    unsigned io_error = 0u;
    unsigned total = 0u;
    unsigned count = 0u;
    unsigned found = 0u;
    unsigned i;
    size_t used;
    int descriptor = launch_args_open(ISAAC_VITA_LAUNCH_ARGS_PATH);

    s_slot = 0u;
    if (descriptor < 0) {
        if ((unsigned)descriptor == LAUNCH_ARGS_SCE_ENOENT) {
            status = "absent";
        } else {
            status = "io-error";
            io_error = (unsigned)descriptor;
        }
    } else {
        for (;;) {
            unsigned room = ISAAC_VITA_LAUNCH_ARGS_BYTES + 1u - total;
            int got;

            if (room == 0u)
                break;
            got = launch_args_read(descriptor, s_text + total, room);
            if (got < 0) {
                status = "io-error";
                io_error = (unsigned)got;
                break;
            }
            if (got == 0)
                break;
            total += (unsigned)got > room ? room : (unsigned)got;
        }
        (void)launch_args_close(descriptor);
        if (io_error == 0u && total > ISAAC_VITA_LAUNCH_ARGS_BYTES) {
            status = "too-large";
        } else if (io_error == 0u) {
            s_text[total] = '\0';
            found = isaac_vita_launch_args_split(
                s_text, total, out, ISAAC_VITA_LAUNCH_ARGS_MAX);
            if (found > ISAAC_VITA_LAUNCH_ARGS_MAX) {
                status = "too-many";
            } else {
                count = found;
                status = launch_args_take_slot(out, &count, &s_slot);
            }
        }
    }

    used = strlen(argv0);
    if (used > sizeof cmdline - 1u)
        used = sizeof cmdline - 1u;
    memcpy(cmdline, argv0, used);
    for (i = 0u; i < count; ++i) {
        size_t len = strlen(out[i]);

        if (used + 1u + len > sizeof cmdline - 1u)
            break;
        cmdline[used++] = ' ';
        memcpy(cmdline + used, out[i], len);
        used += len;
    }
    cmdline[used] = '\0';
    isaac_vita_log("ISAAC VITA LAUNCH ARGS: path=%s status=%s io=0x%08x "
                   "bytes=%u tokens=%u slot=%u argc=%u cmdline=%s",
                   ISAAC_VITA_LAUNCH_ARGS_PATH, status, io_error, total,
                   found, s_slot, count + 1u, cmdline);
    return count;
}

unsigned isaac_vita_launch_args_save_slot(void)
{
    return s_slot;
}

int isaac_vita_launch_args_slot_apply(uint32_t manager_global,
                                      uint32_t game_global,
                                      const char *trigger)
{
    uint32_t manager;
    uint32_t field;
    uint32_t was;

    if (s_slot == 0u)
        return 0;
    manager = launch_args_ld32(manager_global);
    if (!s_slot_applied) {
        /* Manager::Init clears the slot (004ad7fa) before it stores the Game
         * pointer (004ad841): a non-zero Game pointer means the clear ran. */
        if (manager == 0u || launch_args_ld32(game_global) == 0u) {
            ++s_slot_waited;
            return 0;
        }
        field = manager + ISAAC_VITA_LAUNCH_ARGS_MANAGER_SLOT;
        was = launch_args_ld32(field);
        launch_args_st32(field, (uint32_t)s_slot);
        s_slot_applied = 1;
        isaac_vita_log("ISAAC VITA LAUNCH SAVE SLOT: event=applied slot=%u "
                       "was=0x%08x manager=0x%08x waited=%u trigger=%s",
                       s_slot, (unsigned)was, (unsigned)manager,
                       s_slot_waited, trigger ? trigger : "(null)");
        return 1;
    }
    /* Manager teardown (0050c3a6 stores 0) ends the bench run anyway. */
    if (manager == 0u)
        return 0;
    field = manager + ISAAC_VITA_LAUNCH_ARGS_MANAGER_SLOT;
    was = launch_args_ld32(field);
    if (was == (uint32_t)s_slot)
        return 0;
    launch_args_st32(field, (uint32_t)s_slot);
    ++s_slot_restored;
    if (s_slot_restored <= ISAAC_VITA_LAUNCH_ARGS_SLOT_LOG_MAX)
        isaac_vita_log("ISAAC VITA LAUNCH SAVE SLOT: event=restored slot=%u "
                       "was=0x%08x manager=0x%08x restored=%u trigger=%s",
                       s_slot, (unsigned)was, (unsigned)manager,
                       s_slot_restored, trigger ? trigger : "(null)");
    return 1;
}

#if defined(ISAAC_VITA_LAUNCH_ARGS_ORACLE)
void isaac_vita_launch_args_oracle_reset(void)
{
    s_slot = 0u;
    s_slot_applied = 0;
    s_slot_waited = 0u;
    s_slot_restored = 0u;
}
#endif
