#include "guest.h"
#include "host_vita_heap.h"

#if !defined(ISAAC_VITA_ARCHIVE_XOR_FASTPATH) || !ISAAC_VITA_ARCHIVE_XOR_FASTPATH
#error The archive XOR wrapper is an opt-in runtime object
#endif
#if !defined(ISAAC_VITA_HEAP_RANGE_LEASE)
#error The archive XOR wrapper requires exact requested-size heap leases
#endif
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error The frozen archive word transform requires a little-endian host
#endif

void __real_sub_005b06f0(CPU *__restrict c);
void sub_005c2fd0(CPU *__restrict c);

/* Frozen PE SHA256: 31846486979cfa07c8c968221553052c3ff603518681ca11912d445f96ca9404.
 * 0059c40c..0059c427 is the sole direct caller, after the mode-2 raw latch.
 * Object, decoder and ISAAC state allocations are proved by 0059c170 and
 * their exact-base deletes at 0059c103..0059c138. No Vorbis/ring policy changes.
 */
enum {
    XOR_RETURN = 0x0059c427U,
    XOR_REFRESH_RETURN = 0x005b072fU,
    XOR_OBJECT_BYTES = 0xc1cU,
    XOR_OUTPUT_OFFSET = 0x81cU,
    XOR_OUTPUT_BYTES = 0x400U,
    XOR_DECODER_BYTES = 0x14U,
    XOR_STATE_BYTES = 0x810U,
    XOR_STACK_BELOW = 36U,
    XOR_LEASES = 3U
};

static int xor_overlap(uint32_t a, uint32_t n, uint32_t b, uint32_t m)
{
    return (uint64_t)a < (uint64_t)b + m &&
           (uint64_t)b < (uint64_t)a + n;
}

static int xor_release(uint32_t tokens[XOR_LEASES])
{
    unsigned i;
    int ok = 1;
    for (i = XOR_LEASES; i != 0U; --i)
        if (tokens[i - 1U])
            ok = isaac_vita_guest_heap_lease_release(tokens[i - 1U]) && ok;
    return ok;
}

#if defined(ISAAC_VITA_ARCHIVE_XOR_NATIVE_REFRESH) && \
    ISAAC_VITA_ARCHIVE_XOR_NATIVE_REFRESH
static uint32_t xor_refresh_load(uint32_t address)
{
    uint32_t value;
    memcpy(&value, (const void *)(uintptr_t)address, sizeof value);
    return value;
}

static void xor_refresh_store(uint32_t address, uint32_t value)
{
    memcpy((void *)(uintptr_t)address, &value, sizeof value);
}

/* Called ONLY with the existing archive object's three leases held and its
 * full refresh stack window already checked. No global PRNG wrapper, decoder
 * replacement, thread policy or save-state PRNG change.
 *
 * Algorithm prior art: Bob Jenkins, readable.c (1996, public domain),
 * https://burtleburtle.net/bob/c/readable.c . The freshly emitted frozen
 * 005c2fd0 body is authoritative for field layout, stack words and CPU exits.
 * In particular edx is the final XOR-mixed a BEFORE its mm addition.
 */
static int xor_refresh_native(CPU *__restrict c, uint32_t state)
{
    static const uint32_t destinations[4] = {
        0x005c2ffeU, 0x005c300bU, 0x005c3018U, 0x005c3027U
    };
    uint32_t a, b, counter, mixed = 0U;
    unsigned i;
    GUEST_FLAGS_DECL;

    /* All four entries are live mutable guest data. Check on EVERY refresh,
     * before incrementing c, changing bb, pushing saved words or any mutation.
     * A changed but valid switch destination must still execute the original.
     */
    for (i = 0U; i < 4U; ++i)
        if (xor_refresh_load((uint32_t)(GUEST_IMAGE_BASE + 0x005c309cU + i * 4U)) !=
                (uint32_t)(GUEST_IMAGE_BASE + destinations[i]))
            return 0;

    counter = xor_refresh_load(state + 0x80cU) + 1U;
    xor_refresh_store(state + 0x80cU, counter);
    b = xor_refresh_load(state + 0x808U) + counter;
    xor_refresh_store(state + 0x808U, b);
    a = xor_refresh_load(state + 0x804U);
    gpush_generated(c, c->ebx);
    gpush_generated(c, c->esi);
    gpush_generated(c, c->edi);
    for (i = 0U; i < 256U; ++i) {
        uint32_t x = xor_refresh_load(state + 0x404U + i * 4U);
        uint32_t shifted, y;
        switch (i & 3U) {
        case 0U: shifted = a << 13; break;
        case 1U: shifted = a >> 6; break;
        case 2U: shifted = a << 2; break;
        default: shifted = a >> 16; break;
        }
        mixed = shifted ^ a;
        SET_FLAGS(GUEST_FL, FLAG_LOGIC, shifted, a, mixed, 4);
        a = mixed + xor_refresh_load(state + 0x404U + ((i + 128U) & 255U) * 4U);
        xor_refresh_store(state + 0x804U, a);
        y = xor_refresh_load(state + 0x404U + ((x >> 2) & 255U) * 4U) + a + b;
        xor_refresh_store(state + 0x404U + i * 4U, y);
        b = xor_refresh_load(state + 0x404U + ((y >> 10) & 255U) * 4U) + x;
        xor_refresh_store(state + 0x808U, b);
        xor_refresh_store(state + 4U + i * 4U, b);
    }
    c->eax = b;
    c->edx = mixed;
    c->edi = gpop_generated(c);
    c->esi = gpop_generated(c);
    c->ebx = gpop_generated(c);
    (void)gpop_generated(c); /* Original refresh ret, no stack arguments. */
    return 1;
}

#if defined(ISAAC_VITA_ARCHIVE_XOR_REFRESH_ORACLE)
/* Existing host differential also compares the refresh's full CPU/stack exit,
 * which the enclosing XOR function would otherwise overwrite before return. */
int isaac_vita_archive_xor_refresh_oracle(CPU *__restrict c)
{
    return xor_refresh_native(c, c->ecx);
}
#endif
#endif

/* 0: no guest mutation, replay the original. 1: completed or terminal fault. */
int isaac_vita_archive_xor_try(CPU *__restrict c)
{
    uint32_t tokens[XOR_LEASES] = { 0U, 0U, 0U };
    uint32_t object, decoder, state, output, count, index;
    uint32_t offset, key = 0U, last_before = 0U, last_key = 0U;
    uint32_t stack, frame;
    GUEST_FLAGS_DECL;

    if (!c || c->fault || g_guest_coverage_functions || g_guest_coverage_cases ||
        c->esp < XOR_STACK_BELOW ||
        !guest_stack_contains(c, c->esp - XOR_STACK_BELOW,
                              XOR_STACK_BELOW + 12U) ||
        ld32(c->esp) != XOR_RETURN)
        return 0;
    output = ld32(c->esp + 4U);
    count = ld32(c->esp + 8U);
    object = c->edi;
    decoder = c->ecx;
    stack = c->esp;
    if (!count || count > XOR_OUTPUT_BYTES || count != c->esi ||
        !object || object > UINT32_MAX - XOR_OBJECT_BYTES ||
        output != object + XOR_OUTPUT_OFFSET || output != c->ebx ||
        !decoder || decoder > UINT32_MAX - XOR_DECODER_BYTES ||
        xor_overlap(object, XOR_OBJECT_BYTES, decoder, XOR_DECODER_BYTES) ||
        xor_overlap(object, XOR_OBJECT_BYTES, c->stack_floor,
                    c->stack_ceiling - c->stack_floor) ||
        xor_overlap(decoder, XOR_DECODER_BYTES, c->stack_floor,
                    c->stack_ceiling - c->stack_floor))
        return 0;

    tokens[0] = isaac_vita_guest_heap_lease_exact_range(
        (const void *)(uintptr_t)object, (const void *)(uintptr_t)object,
        XOR_OBJECT_BYTES);
    if (!tokens[0])
        return 0;
    if (ld32(object + 0x14U) != decoder)
        goto reject;
    tokens[1] = isaac_vita_guest_heap_lease_exact_range(
        (const void *)(uintptr_t)decoder, (const void *)(uintptr_t)decoder,
        XOR_DECODER_BYTES);
    if (!tokens[1])
        goto reject;
    state = ld32(decoder);
    if (ld8(decoder + 0x10U) != 1U || !state ||
        state > UINT32_MAX - XOR_STATE_BYTES ||
        xor_overlap(state, XOR_STATE_BYTES, object, XOR_OBJECT_BYTES) ||
        xor_overlap(state, XOR_STATE_BYTES, decoder, XOR_DECODER_BYTES) ||
        xor_overlap(state, XOR_STATE_BYTES, c->stack_floor,
                    c->stack_ceiling - c->stack_floor))
        goto reject;
    tokens[2] = isaac_vita_guest_heap_lease_exact_range(
        (const void *)(uintptr_t)state, (const void *)(uintptr_t)state,
        XOR_STATE_BYTES);
    if (!tokens[2])
        goto reject;
    index = ld32(state);
    if (index >= 256U)
        goto reject;
    GUEST_PROFILE_NOTE_FUNCTION_ENTRY();

    /* Retain the original guest stack image and the CPU state at the refresh
     * call. The word-XOR uses memcpy-sized accesses (no alignment/aliasing
     * assumption and no access beyond a partial tail); the separate native
     * refresh remains guarded at that original call boundary.
     */
    gpush_generated(c, c->ebp);
    frame = c->esp;
    c->ebp = frame;
    gpush_generated(c, decoder);
    gpush_generated(c, c->ebx);
    gpush_generated(c, c->esi);
    c->esi = count;
    gpush_generated(c, c->edi);
    c->edi = output;
    for (offset = 0U; offset < count; ) {
        uint32_t bytes = count - offset;
        uint32_t value;
        if (bytes > 4U)
            bytes = 4U;
        key = ld32(state + 4U + index * 4U);
        ++index;
        st32(stack + 8U, key); /* original overwrites its count argument */
        st32(state, index);
        if (index == 256U) {
            /* Fetch the OLD word, then refresh BEFORE XORing even its first
             * byte. Default retains the original PRNG. The separate native
             * mode still falls back at a changed live jump table. */
            c->eax = index;
            c->ecx = state;
            c->edx = key;
            c->ebx = offset;
            gpush_generated(c, XOR_REFRESH_RETURN);
#if defined(ISAAC_VITA_ARCHIVE_XOR_NATIVE_REFRESH) && \
    ISAAC_VITA_ARCHIVE_XOR_NATIVE_REFRESH
            if (!xor_refresh_native(c, state))
                sub_005c2fd0(c);
#else
            sub_005c2fd0(c);
#endif
            if (c->fault) {
                (void)xor_release(tokens);
                return 1;
            }
            index = 0U;
            st32(state, index);
        }
        if (bytes == 4U) {
            memcpy(&value, (const void *)(uintptr_t)(output + offset),
                   sizeof value);
            last_before = value >> 24;
            last_key = key >> 24;
            value ^= key;
            memcpy((void *)(uintptr_t)(output + offset), &value, sizeof value);
        } else {
            uint32_t byte;
            for (byte = 0U; byte < bytes; ++byte) {
                last_before = ld8(output + offset + byte);
                last_key = (uint8_t)(key >> (8U * byte));
                st8(output + offset + byte, (uint8_t)(last_before ^ last_key));
            }
        }
        /* Match the last byte's lazy producer even at a subsequent original
         * refresh boundary. FLAGS_LOCAL builds compile this bookkeeping out. */
        SET_FLAGS(GUEST_FL, FLAG_LOGIC, last_before, last_key,
                  last_before ^ last_key, 1);
        offset += bytes;
    }
    c->eax = decoder;
    c->ecx = state;
    c->edx = key >> (8U * ((count - 1U) & 3U));
    c->edi = gpop_generated(c);
    c->esi = gpop_generated(c);
    c->ebx = gpop_generated(c);
    if (!guest_stack_set_generated(c, frame))
        goto fault_release;
    c->ebp = gpop_generated(c);
    (void)gpop_generated(c);
    if (!guest_stack_adjust_generated(c, 8U))
        goto fault_release;
    if (!xor_release(tokens))
        guest_fault(c, object, "native archive XOR lease release failed");
    return 1;

fault_release:
    (void)xor_release(tokens);
    return 1;
reject:
    if (xor_release(tokens))
        return 0;
    guest_fault(c, object, "native archive XOR lease release failed");
    (void)gpop_generated(c);
    (void)guest_stack_adjust_generated(c, 8U);
    return 1;
}

void __wrap_sub_005b06f0(CPU *__restrict c)
{
    if (!isaac_vita_archive_xor_try(c))
        __real_sub_005b06f0(c);
}
