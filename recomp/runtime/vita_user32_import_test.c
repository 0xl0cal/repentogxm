/* Compile/link/source oracle for Vita's measured windowless USER32 policy. */
#include <stdint.h>
#include <string.h>

#include "guest.h"
#include "host_vita_user32.h"

#if UINTPTR_MAX != UINT32_MAX
#error Vita USER32 oracle requires a 32-bit identity-mapped target
#endif

#define SITE(value) value,
static const uint32_t s_window_calls[] = {
    ISAAC_VITA_USER32_GET_WINDOW_LONG_CALLS(SITE)
};
static const uint32_t s_load_calls[] = {
    ISAAC_VITA_USER32_LOAD_IMAGE_CALLS(SITE)
};
static const uint32_t s_send_calls[] = {
    ISAAC_VITA_USER32_SEND_MESSAGE_CALLS(SITE)
};
static const uint32_t s_set_window_pos_calls[] = {
    ISAAC_VITA_USER32_SET_WINDOW_POS_CALLS(SITE)
};
static const uint32_t s_metric_calls[] = {
    ISAAC_VITA_USER32_GET_SYSTEM_METRICS_CALLS(SITE)
};
static const uint32_t s_set_window_long_calls[] = {
    ISAAC_VITA_USER32_SET_WINDOW_LONG_CALLS(SITE)
};
#undef SITE

_Alignas(8) static uint32_t s_frame[16];

void guest_fault(CPU *__restrict c, uint32_t address, const char *what)
{
    c->fault_addr = address;
    c->fault = what;
}

int guest_stack_violation(CPU *__restrict c, uint32_t pc, uint32_t kind,
                          uint32_t address, uint32_t size)
{
    (void)pc;
    (void)kind;
    (void)size;
    guest_fault(c, address, "USER32 oracle guest-stack violation");
    return 0;
}

int guest_stack_owner_violation(CPU *__restrict c, uint32_t pc)
{
    guest_fault(c, pc, "USER32 oracle guest-stack owner violation");
    return 0;
}

static uint32_t pointer32(const void *p)
{
    return (uint32_t)(uintptr_t)p;
}

static uint64_t hash_sites(const uint32_t *sites, unsigned count)
{
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    unsigned i;

    for (i = 0U; i < count; ++i) {
        unsigned byte_index;
        for (byte_index = 0U; byte_index < 4U; ++byte_index) {
            hash ^= (uint8_t)(sites[i] >> (byte_index * 8U));
            hash *= UINT64_C(0x100000001b3);
        }
    }
    return hash;
}

static uint32_t prepare(CPU *c, const uint32_t *args, unsigned count)
{
    unsigned i;

    memset(c, 0, sizeof *c);
    memset(s_frame, 0xcc, sizeof s_frame);
    s_frame[2] = 0x0badc0deU;
    for (i = 0U; i < count; ++i)
        s_frame[3U + i] = args[i];
    c->esp = pointer32(&s_frame[2]);
    c->eax = 0x11223344U;
    return c->esp;
}

static int call_ok(CPU *c, const char *name, const uint32_t *args,
                   unsigned count, uint32_t expected, unsigned *calls)
{
    uint32_t esp = prepare(c, args, count);

    return isaac_vita_user32_import_counted(c, name, calls) &&
           !c->fault && c->eax == expected &&
           c->esp == esp + 4U + count * 4U;
}

static int call_ok_at(CPU *c, const char *name, uint32_t return_word,
                      const uint32_t *args, unsigned count,
                      uint32_t expected, unsigned *calls)
{
    uint32_t esp = prepare(c, args, count);

    s_frame[2] = return_word;
    return isaac_vita_user32_import_counted(c, name, calls) &&
           !c->fault && c->eax == expected &&
           c->esp == esp + 4U + count * 4U;
}

static int evidence_ok(void)
{
    return sizeof s_window_calls / sizeof s_window_calls[0] == 5U &&
           sizeof s_load_calls / sizeof s_load_calls[0] == 3U &&
           sizeof s_send_calls / sizeof s_send_calls[0] == 2U &&
           sizeof s_set_window_pos_calls /
                   sizeof s_set_window_pos_calls[0] == 5U &&
           sizeof s_metric_calls / sizeof s_metric_calls[0] == 10U &&
           sizeof s_set_window_long_calls /
                   sizeof s_set_window_long_calls[0] == 6U &&
           hash_sites(s_window_calls, 5U) ==
               ISAAC_VITA_USER32_GET_WINDOW_LONG_CALL_FNV64 &&
           hash_sites(s_load_calls, 3U) ==
               ISAAC_VITA_USER32_LOAD_IMAGE_CALL_FNV64 &&
           hash_sites(s_send_calls, 2U) ==
               ISAAC_VITA_USER32_SEND_MESSAGE_CALL_FNV64 &&
           hash_sites(s_set_window_pos_calls, 5U) ==
               ISAAC_VITA_USER32_SET_WINDOW_POS_CALL_FNV64 &&
           hash_sites(s_metric_calls, 10U) ==
               ISAAC_VITA_USER32_GET_SYSTEM_METRICS_CALL_FNV64 &&
           hash_sites(s_set_window_long_calls, 6U) ==
               ISAAC_VITA_USER32_SET_WINDOW_LONG_CALL_FNV64 &&
           ISAAC_VITA_USER32_GET_WINDOW_LONG_IAT_RVA == 0x006063b8U &&
           ISAAC_VITA_USER32_LOAD_IMAGE_IAT_RVA == 0x006063bcU &&
           ISAAC_VITA_USER32_SEND_MESSAGE_IAT_RVA == 0x006063c0U &&
           ISAAC_VITA_USER32_SET_WINDOW_POS_IAT_RVA == 0x006063c4U &&
           ISAAC_VITA_USER32_GET_SYSTEM_METRICS_IAT_RVA == 0x006063c8U &&
           ISAAC_VITA_USER32_SET_WINDOW_LONG_IAT_RVA == 0x006063ccU;
}

int main(void)
{
    static const uint32_t selectors[] = { 0U, 1U, 11U, 12U, 49U, 50U };
    static const uint32_t values[] = { 960U, 544U, 32U, 32U, 16U, 16U };
    CPU c;
    CPU snapshot;
    uint32_t args[7];
    uint32_t esp;
    unsigned calls = 0U;
    unsigned i;

    if (!evidence_ok())
        return 10;
    for (i = 0U; i < 6U; ++i) {
        args[0] = selectors[i];
        if (!call_ok(&c, ISAAC_VITA_USER32_GET_SYSTEM_METRICS_NAME,
                     args, 1U, values[i], &calls))
            return 20 + (int)i;
    }
    args[0] = 0U;
    args[1] = ISAAC_VITA_USER32_GWL_STYLE;
    if (!call_ok(&c, ISAAC_VITA_USER32_GET_WINDOW_LONG_NAME,
                 args, 2U, 0U, &calls))
        return 30;

    args[0] = 0U;
    args[1] = ISAAC_VITA_USER32_GWL_STYLE;
    args[2] = 0x80000000U;
    if (!call_ok(&c, ISAAC_VITA_USER32_SET_WINDOW_LONG_NAME,
                 args, 3U, 0U, &calls))
        return 35;

    args[0] = 0U;
    args[1] = 0U;
    args[2] = 0U;
    args[3] = 0U;
    args[4] = ISAAC_VITA_USER32_FULLSCREEN_WIDTH;
    args[5] = ISAAC_VITA_USER32_FULLSCREEN_HEIGHT;
    args[6] = ISAAC_VITA_USER32_SWP_FRAMECHANGED;
    if (!call_ok_at(&c, ISAAC_VITA_USER32_SET_WINDOW_POS_NAME,
                    ISAAC_VITA_USER32_FULLSCREEN_RETURN_RVA,
                    args, 7U, 1U, &calls))
        return 36;
    if (!call_ok_at(&c, ISAAC_VITA_USER32_SET_WINDOW_POS_NAME,
                    (uint32_t)GUEST_IMAGE_BASE +
                        ISAAC_VITA_USER32_FULLSCREEN_RETURN_RVA,
                    args, 7U, 1U, &calls))
        return 37;

    args[0] = GUEST_IMAGE_BASE;
    args[1] = 101U;
    args[2] = ISAAC_VITA_USER32_IMAGE_ICON;
    args[3] = ISAAC_VITA_USER32_ICON_WIDTH;
    args[4] = ISAAC_VITA_USER32_ICON_HEIGHT;
    args[5] = 0U;
    if (!call_ok(&c, ISAAC_VITA_USER32_LOAD_IMAGE_NAME, args, 6U,
                 ISAAC_VITA_USER32_HICON_ISAAC, &calls))
        return 31;
    args[1] = 104U;
    args[3] = ISAAC_VITA_USER32_SMALL_ICON_WIDTH;
    args[4] = ISAAC_VITA_USER32_SMALL_ICON_HEIGHT;
    if (!call_ok(&c, ISAAC_VITA_USER32_LOAD_IMAGE_NAME, args, 6U,
                 ISAAC_VITA_USER32_HICON_ISAAC_ALT +
                     ISAAC_VITA_USER32_HICON_SMALL_BIAS, &calls))
        return 32;

    args[0] = 0U;
    args[1] = ISAAC_VITA_USER32_WM_SETICON;
    args[2] = ISAAC_VITA_USER32_ICON_BIG;
    args[3] = ISAAC_VITA_USER32_HICON_ISAAC;
    if (!call_ok(&c, ISAAC_VITA_USER32_SEND_MESSAGE_NAME,
                 args, 4U, 0U, &calls))
        return 33;
    args[2] = ISAAC_VITA_USER32_ICON_SMALL;
    args[3] = ISAAC_VITA_USER32_HICON_ISAAC_ALT +
              ISAAC_VITA_USER32_HICON_SMALL_BIAS;
    if (!call_ok(&c, ISAAC_VITA_USER32_SEND_MESSAGE_NAME,
                 args, 4U, 0U, &calls) || calls != 14U)
        return 34;

    esp = prepare(&c, args, 0U);
    snapshot = c;
    if (isaac_vita_user32_import_counted(
            &c, "USER32.dll!unowned", &calls) ||
        memcmp(&c, &snapshot, sizeof c) != 0 || calls != 14U)
        return 40;

    args[0] = 999U;
    esp = prepare(&c, args, 1U);
    if (!isaac_vita_user32_import_counted(
            &c, ISAAC_VITA_USER32_GET_SYSTEM_METRICS_NAME, &calls) ||
        !c.fault || c.esp != esp || calls != 15U)
        return 41;

    args[0] = 0U;
    args[1] = 0U;
    args[2] = 0x12345678U;
    esp = prepare(&c, args, 3U);
    if (!isaac_vita_user32_import_counted(
            &c, ISAAC_VITA_USER32_SET_WINDOW_LONG_NAME, &calls) ||
        !c.fault || c.esp != esp || calls != 16U)
        return 42;

    args[0] = 0U;
    args[1] = 0U;
    args[2] = 0U;
    args[3] = 0U;
    args[4] = ISAAC_VITA_USER32_FULLSCREEN_WIDTH;
    args[5] = ISAAC_VITA_USER32_FULLSCREEN_HEIGHT;
    args[6] = ISAAC_VITA_USER32_SWP_FRAMECHANGED;
    esp = prepare(&c, args, 7U);
    s_frame[2] = 0x0056122bU;
    if (!isaac_vita_user32_import_counted(
            &c, ISAAC_VITA_USER32_SET_WINDOW_POS_NAME, &calls) ||
        !c.fault || c.esp != esp || calls != 17U)
        return 43;

    esp = prepare(&c, args, 7U);
    s_frame[2] = ISAAC_VITA_USER32_FULLSCREEN_RETURN_RVA;
    s_frame[3U + 4U] = ISAAC_VITA_USER32_SCREEN_WIDTH;
    if (!isaac_vita_user32_import_counted(
            &c, ISAAC_VITA_USER32_SET_WINDOW_POS_NAME, &calls) ||
        !c.fault || c.esp != esp || calls != 18U)
        return 44;
    return 0;
}
