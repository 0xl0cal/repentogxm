#include "guest.h"
#include "host_vita_anm2_scratch.h"

#if !defined(ISAAC_VITA_ANM2_POOL_INIT) || !ISAAC_VITA_ANM2_POOL_INIT
#error The ANM2 pool initializer is an opt-in runtime object
#endif

void __real_sub_00002120(CPU *__restrict c);

/* Frozen PE SHA256: 31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404.
 * Six external calls from 00009b40 (guest_0001.c) to 00002120 (guest_0000.c).
 * Only the first iteration is eligible. Preserve the original caller's final
 * byte store, stride step, SUB 1 flags and branch, rather than emulating its
 * exit in a different FLAGS_LOCAL/GPR_LOCAL domain. No generated-code changes.
 */
void __wrap_sub_00002120(CPU *__restrict c)
{
    static const uint32_t frame_offsets[6] = {
        0x266a8U, 0x266a0U, 0x26694U, 0x266a4U, 0x26678U, 0x26688U
    };
    uint32_t pool, bias, color_offset, stride, eax_bias, frame_slot;
    unsigned index;

    /* This constructor is also used outside ANM2. Reject by return site
     * before touching coverage state, frame locals or the scratch lock. */
    if (!c || !guest_stack_contains(c, c->esp, 4U))
        goto original;
    switch (ld32(c->esp)) {
    case 0x0000aa93U: index = 0U; break;
    case 0x0000aae3U: index = 1U; break;
    case 0x0000ab43U: index = 2U; break;
    case 0x0000ab93U: index = 3U; break;
    case 0x0000ac0fU: index = 4U; break;
    case 0x0000ac83U: index = 5U; break;
    default: goto original;
    }
    if (c->fault || c->esi != 5000U ||
        g_guest_coverage_functions || g_guest_coverage_cases ||
        c->ebp < frame_offsets[index])
        goto original;
    frame_slot = c->ebp - frame_offsets[index];
    if (frame_slot < 4U || c->esp > frame_slot - 4U ||
        !guest_stack_contains(c, frame_slot, 4U))
        goto original;
    pool = ld32(frame_slot);
    bias = index < 4U ? 4U : 0x14U;
    color_offset = index < 4U ? 0x18U : 0x30U;
    stride = index < 4U ? 84U : 108U;
    eax_bias = index == 2U ? 4U : index == 4U ? 0x14U : 0U;
    if (!pool || pool > UINT32_MAX - stride * 5000U ||
        c->edx != pool + bias || c->ecx != pool + color_offset ||
        c->eax != pool + eax_bias ||
        !isaac_vita_anm2_scratch_init_pool(
            index, pool, c->stack_floor, c->stack_ceiling))
        goto original;

    GUEST_PROFILE_NOTE_FUNCTION_ENTRY();
    c->esi = 1U;
    c->edx = pool + 4999U * stride + bias;
    c->ecx = c->eax = pool + 4999U * stride + color_offset;
    (void)gpop_generated(c); /* Same return word, original caller finishes. */
    GUEST_STACK_CALLSITE_BARRIER();
    return;

original:
    __real_sub_00002120(c);
}
