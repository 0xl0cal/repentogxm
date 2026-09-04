#include "guest.h"
#include "guest_stack_legacy_oracle_stub.h"

static unsigned s_stack_violation_calls;

/* Copied from the established standalone seam in
 * kage_vita_loading_override_oracle.c.  These cold helpers are unreachable in
 * the legacy-stack oracle path, but the link must still satisfy guest.h. */
int guest_stack_violation(CPU *__restrict c, uint32_t pc, uint32_t kind,
                          uint32_t address, uint32_t size)
{
    (void)c;
    (void)pc;
    (void)kind;
    (void)address;
    (void)size;
    ++s_stack_violation_calls;
    return 0;
}

int guest_stack_owner_violation(CPU *__restrict c, uint32_t pc)
{
    (void)c;
    (void)pc;
    ++s_stack_violation_calls;
    return 0;
}

unsigned guest_stack_legacy_oracle_violation_calls(void)
{
    return s_stack_violation_calls;
}
