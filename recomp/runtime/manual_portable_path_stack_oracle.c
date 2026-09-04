#include "guest.h"

#if !GUEST_STACK_REQUIRED
#error The manual portable stack oracle exercises the production stack ABI
#endif

static int stack_reject(CPU *__restrict c, uint32_t pc,
                        const char *message)
{
    guest_fault(c, pc, message);
    return 0;
}

static int stack_bound(const CPU *c)
{
    return c && c->stack_owner == c &&
           c->stack_floor < c->stack_ceiling;
}

int guest_stack_set(CPU *__restrict c, uint32_t value, uint32_t pc)
{
    if (!stack_bound(c) || value < c->stack_floor ||
        value > c->stack_ceiling)
        return stack_reject(c, pc, "path oracle stack-set rejection");
    c->esp = value;
    if (value < c->stack_low_water)
        c->stack_low_water = value;
    return 1;
}

uint32_t guest_stack_address(CPU *__restrict c, uint32_t address,
                             uint32_t size, uint32_t pc)
{
    uint32_t capacity;
    uint32_t offset;
    if (!stack_bound(c) || !size) {
        (void)stack_reject(c, pc, "path oracle stack-access rejection");
        return 0u;
    }
    capacity = c->stack_ceiling - c->stack_floor;
    offset = address - c->stack_floor;
    if (size > capacity || offset > capacity - size) {
        (void)stack_reject(c, pc, "path oracle stack-access rejection");
        return 0u;
    }
    if (address < c->stack_low_water)
        c->stack_low_water = address;
    return address;
}

uint32_t gpop_at(CPU *__restrict c, uint32_t pc)
{
    uint32_t address = guest_stack_address(c, c->esp, 4u, pc);
    uint32_t value;
    if (!address)
        return 0u;
    value = ld32(address);
    c->esp += 4u;
    return value;
}
