/* Portable VCRUNTIME byte/string helpers for the Vita recompilation.
 *
 * The three memory movers intentionally use the identity-mapped guest address
 * as the native pointer, matching the established PC runtime.  The three
 * search helpers keep all traffic in ld8 so returned pointers remain guest
 * addresses and faults remain attributable at the foreign-API boundary.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "host_vita_memory.h"
#include "kage_vita_deep_profile.h"
#include "host_vita_import_id.h"
#include "host_vita_png_texel_init.h"

/* Full memblock queries are a diagnostic catastrophic-mover policy: they are
 * deliberately OFF unless the owning build enables this source explicitly,
 * and even then stay off the ordinary mover hot path.  Cheap null/address-
 * wrap rejection below remains unconditional. */
#ifndef ISAAC_VITA_MEMORY_RANGE_GUARD
#define ISAAC_VITA_MEMORY_RANGE_GUARD 0
#endif
#if ISAAC_VITA_MEMORY_RANGE_GUARD != 0 && \
    ISAAC_VITA_MEMORY_RANGE_GUARD != 1
#error ISAAC_VITA_MEMORY_RANGE_GUARD must be zero or one
#endif

/* This is a diagnostic tripwire, not a claimed maximum legitimate transfer.
 * The threshold keeps sysmem calls out of normal loading while retaining a
 * wide margin below the observed 0x5848f42a corrupt count. */
#define VITA_MEMORY_CATASTROPHIC_COUNT UINT32_C(0x01000000)

#if ISAAC_VITA_MEMORY_RANGE_GUARD
#if defined(__vita__)
#include <psp2/kernel/sysmem.h>
#define VITA_MEMORY_RANGE_ACCESS_READ  SCE_KERNEL_MEMORY_ACCESS_R
#define VITA_MEMORY_RANGE_ACCESS_WRITE SCE_KERNEL_MEMORY_ACCESS_W
#elif defined(ISAAC_VITA_MEMORY_RANGE_ORACLE)
/* Host execution uses the same post-query validation without pretending that
 * Vita's 32-bit SceKernelMemBlockInfo has the host's pointer layout. */
#define VITA_MEMORY_RANGE_ACCESS_READ  4U
#define VITA_MEMORY_RANGE_ACCESS_WRITE 2U
int isaac_vita_memory_range_oracle_query(
    uint32_t address, uint32_t count, uint32_t *mapped_base,
    uint32_t *mapped_size, uint32_t *access);
#else
#error The Vita memory range guard requires the Vita sysmem API
#endif

void isaac_vita_log(const char *format, ...);
#endif

typedef void (*vita_memory_import_fn)(CPU *__restrict);

typedef struct vita_memory_import_entry {
    const char *name;
    vita_memory_import_fn fn;
} vita_memory_import_entry;

static uint32_t vita_memory_arg(CPU *__restrict c, uint32_t index)
{
    /* ESP points at the x86 return address while a cdecl import is active. */
    return ld32(guest_stack_address(
        c, c->esp + 4U + index * 4U, 4U, 0U));
}

static void vita_memory_cdecl_return(CPU *__restrict c)
{
    (void)gpop(c);               /* translated caller removes arguments */
}

enum vita_memory_range_failure {
    VITA_MEMORY_RANGE_VALID = 0,
    VITA_MEMORY_RANGE_NULL,
    VITA_MEMORY_RANGE_WRAP,
    VITA_MEMORY_RANGE_QUERY,
    VITA_MEMORY_RANGE_MAPPING,
    VITA_MEMORY_RANGE_ACCESS
};

typedef struct vita_memory_range_probe {
    uint32_t failure;
    int32_t query_result;
    uint32_t mapped_base;
    uint32_t mapped_size;
    uint32_t access;
} vita_memory_range_probe;

static int vita_memory_range_validate(uint32_t address, uint32_t count,
                                      int write,
                                      vita_memory_range_probe *probe)
{
    if (probe) {
        probe->failure = VITA_MEMORY_RANGE_VALID;
        probe->query_result = INT32_MIN;
        probe->mapped_base = 0U;
        probe->mapped_size = 0U;
        probe->access = 0U;
    }
    if (!count)
        return 1;
    if (!address) {
        if (probe)
            probe->failure = VITA_MEMORY_RANGE_NULL;
        return 0;
    }
    if (address > UINT32_MAX - (count - 1U)) {
        if (probe)
            probe->failure = VITA_MEMORY_RANGE_WRAP;
        return 0;
    }
#if ISAAC_VITA_MEMORY_RANGE_GUARD
    if (count <= VITA_MEMORY_CATASTROPHIC_COUNT)
        return 1;
    {
#if defined(__vita__)
        SceKernelMemBlockInfo info = { 0 };
#endif
        uint32_t mapped_base = 0U;
        uint32_t mapped_size = 0U;
        uint32_t access = 0U;
        uint32_t required_access = write
            ? VITA_MEMORY_RANGE_ACCESS_WRITE
            : VITA_MEMORY_RANGE_ACCESS_READ;
#if defined(__vita__)
        info.size = sizeof info;
        /* Query the block containing the first byte, then independently
         * require the complete range below.  The ByRange export is absent in
         * current emulator implementations even though VitaSDK declares it. */
        int result = sceKernelGetMemBlockInfoByAddr(
            (void *)(uintptr_t)address, &info);
        mapped_base = (uint32_t)(uintptr_t)info.mappedBase;
        mapped_size = (uint32_t)info.mappedSize;
        access = (uint32_t)info.access;
#else
        int result = isaac_vita_memory_range_oracle_query(
            address, count, &mapped_base, &mapped_size, &access);
#endif

        if (probe) {
            probe->query_result = (int32_t)result;
            probe->mapped_base = mapped_base;
            probe->mapped_size = mapped_size;
            probe->access = access;
        }
        if (result < 0) {
            if (probe)
                probe->failure = VITA_MEMORY_RANGE_QUERY;
            return 0;
        }

        if (!mapped_base || count > mapped_size ||
            address < mapped_base ||
            address - mapped_base > mapped_size - count) {
            if (probe)
                probe->failure = VITA_MEMORY_RANGE_MAPPING;
            return 0;
        }
        /* Hardware reports R/W here.  Vita3K currently leaves access zero;
         * treat only a non-zero contradictory mask as authoritative. */
        if (access && (access & required_access) != required_access) {
            if (probe)
                probe->failure = VITA_MEMORY_RANGE_ACCESS;
            return 0;
        }
    }
#else
    (void)write;
#endif
    return 1;
}

#if defined(ISAAC_VITA_MEMORY_RANGE_ORACLE)
/* Host-only seam for threshold/containment tests which must not actually copy
 * a 16 MiB diagnostic range.  Production callers still enter above only via
 * the guest import boundary. */
int isaac_vita_memory_range_oracle_validate(uint32_t address, uint32_t count,
                                            int write)
{
    vita_memory_range_probe probe;

    return vita_memory_range_validate(address, count, write, &probe)
        ? VITA_MEMORY_RANGE_VALID : (int)probe.failure;
}
#endif

static void vita_memory_range_fault(
    CPU *__restrict c, const char *operation, const char *side,
    const char *fault, uint32_t return_address, uint32_t destination,
    uint32_t source, uint32_t count, const vita_memory_range_probe *probe)
{
#if ISAAC_VITA_MEMORY_RANGE_GUARD
    isaac_vita_log(
        "guest mover reject: op=%s side=%s reason=%u query=0x%08x "
        "map=0x%08x+0x%08x access=0x%08x return=0x%08x "
        "dst=0x%08x src=0x%08x count=0x%08x "
        "cpu eax=%08x ecx=%08x edx=%08x ebx=%08x esp=%08x "
        "ebp=%08x esi=%08x edi=%08x",
        operation, side, (unsigned)probe->failure,
        (unsigned)probe->query_result, (unsigned)probe->mapped_base,
        (unsigned)probe->mapped_size, (unsigned)probe->access,
        (unsigned)return_address, (unsigned)destination, (unsigned)source,
        (unsigned)count, (unsigned)c->eax, (unsigned)c->ecx,
        (unsigned)c->edx, (unsigned)c->ebx, (unsigned)c->esp,
        (unsigned)c->ebp, (unsigned)c->esi, (unsigned)c->edi);
#else
    (void)operation;
    (void)side;
    (void)return_address;
    (void)destination;
    (void)source;
    (void)count;
    (void)probe;
#endif
    guest_fault(c, strcmp(side, "src") == 0 ? source : destination, fault);
}

static uint32_t vita_memory_return_address(CPU *__restrict c)
{
    return ld32(guest_stack_address(c, c->esp, 4U, 0U));
}

/* Copied from the already-oracled host_win32 implementation: the exact guest
 * range is scanned, and a zero count permits a null source without access. */
static void vita_memory_memchr(CPU *__restrict c)
{
    uint32_t source = vita_memory_arg(c, 0U);
    uint8_t value = (uint8_t)vita_memory_arg(c, 1U);
    uint32_t count = vita_memory_arg(c, 2U);
    uint32_t index;

    if (count && (!source || source > UINT32_MAX - (count - 1U))) {
        guest_fault(c, source, "memchr received an invalid guest range");
        return;
    }
    c->eax = 0U;
    for (index = 0U; index < count; ++index) {
        if (ld8(source + index) == value) {
            c->eax = source + index;
            break;
        }
    }
    vita_memory_cdecl_return(c);
}

static void vita_memory_strchr(CPU *__restrict c)
{
    uint32_t cursor = vita_memory_arg(c, 0U);
    uint8_t needle = (uint8_t)vita_memory_arg(c, 1U);

    if (!cursor) {
        guest_fault(c, cursor, "strchr received a null guest pointer");
        return;
    }
    for (;;) {
        uint8_t value;
        if (cursor == UINT32_MAX) {
            guest_fault(c, cursor,
                        "strchr guest string wraps address space");
            return;
        }
        value = ld8(cursor);
        if (value == needle) {
            c->eax = cursor;
            break;
        }
        if (!value) {
            c->eax = 0U;
            break;
        }
        ++cursor;
    }
    vita_memory_cdecl_return(c);
}

static void vita_memory_strstr(CPU *__restrict c)
{
    uint32_t haystack = vita_memory_arg(c, 0U);
    uint32_t needle = vita_memory_arg(c, 1U);
    uint32_t needle_length = 0U;
    uint32_t cursor;

    if (!haystack || !needle) {
        guest_fault(c, !haystack ? haystack : needle,
                    "strstr received a null guest pointer");
        return;
    }
    for (;;) {
        if (needle > UINT32_MAX - needle_length) {
            guest_fault(c, needle, "strstr needle wraps address space");
            return;
        }
        if (!ld8(needle + needle_length))
            break;
        ++needle_length;
    }
    if (!needle_length) {
        c->eax = haystack;
        vita_memory_cdecl_return(c);
        return;
    }

    cursor = haystack;
    for (;;) {
        uint32_t index;
        if (!ld8(cursor)) {
            c->eax = 0U;
            break;
        }
        for (index = 0U; index < needle_length; ++index) {
            uint8_t haystack_byte;
            if (cursor > UINT32_MAX - index) {
                guest_fault(c, cursor,
                            "strstr haystack wraps address space");
                return;
            }
            haystack_byte = ld8(cursor + index);
            if (!haystack_byte || haystack_byte != ld8(needle + index))
                break;
        }
        if (index == needle_length) {
            c->eax = cursor;
            break;
        }
        if (cursor == UINT32_MAX) {
            guest_fault(c, cursor,
                        "strstr haystack wraps address space");
            return;
        }
        ++cursor;
    }
    vita_memory_cdecl_return(c);
}

static void vita_memory_memset(CPU *__restrict c)
{
    uint32_t destination = vita_memory_arg(c, 0U);
    int value = (int)vita_memory_arg(c, 1U);
    uint32_t count = vita_memory_arg(c, 2U);
    vita_memory_range_probe probe;

    if (count && !vita_memory_range_validate(
            destination, count, 1, &probe)) {
        vita_memory_range_fault(
            c, "memset", "dst", "memset received an invalid destination range",
            vita_memory_return_address(c), destination, 0U, count, &probe);
        return;
    }
#if ISAAC_VITA_PNG_TEXEL_INIT_ELISION
    if (count && value == 0 && vita_memory_return_address(c) ==
            ISAAC_VITA_PNG_TEXEL_INIT_RETURN_RVA) {
        int elide = isaac_vita_png_texel_init_try(c, destination, value, count);
        if (elide < 0)
            return; /* Terminal fault: no fallback, EAX change or cdecl pop. */
        if (elide > 0) {
            c->eax = destination;
            vita_memory_cdecl_return(c);
            return;
        }
    }
#endif
    if (count)
        memset((void *)(uintptr_t)destination, value, (size_t)count);
    c->eax = destination;
    vita_memory_cdecl_return(c);
}

static void vita_memory_memcpy(CPU *__restrict c)
{
    KAGE_VITA_DEEP_SCOPE_BYTES(KVD_MEMCPY, vita_memory_arg(c, 2U));
    uint32_t destination = vita_memory_arg(c, 0U);
    uint32_t source = vita_memory_arg(c, 1U);
    uint32_t count = vita_memory_arg(c, 2U);
    vita_memory_range_probe probe;

    if (count) {
        if (!vita_memory_range_validate(destination, count, 1, &probe)) {
            vita_memory_range_fault(
                c, "memcpy", "dst",
                "memcpy received an invalid destination range",
                vita_memory_return_address(c), destination, source, count,
                &probe);
            return;
        }
        if (!vita_memory_range_validate(source, count, 0, &probe)) {
            vita_memory_range_fault(
                c, "memcpy", "src",
                "memcpy received an invalid source range",
                vita_memory_return_address(c), destination, source, count,
                &probe);
            return;
        }
        memcpy((void *)(uintptr_t)destination,
               (const void *)(uintptr_t)source, (size_t)count);
    }
    c->eax = destination;
    vita_memory_cdecl_return(c);
}

static void vita_memory_memmove(CPU *__restrict c)
{
    KAGE_VITA_DEEP_SCOPE_BYTES(KVD_MEMMOVE, vita_memory_arg(c, 2U));
    uint32_t destination = vita_memory_arg(c, 0U);
    uint32_t source = vita_memory_arg(c, 1U);
    uint32_t count = vita_memory_arg(c, 2U);
    vita_memory_range_probe probe;

    if (count) {
        if (!vita_memory_range_validate(destination, count, 1, &probe)) {
            vita_memory_range_fault(
                c, "memmove", "dst",
                "memmove received an invalid destination range",
                vita_memory_return_address(c), destination, source, count,
                &probe);
            return;
        }
        if (!vita_memory_range_validate(source, count, 0, &probe)) {
            vita_memory_range_fault(
                c, "memmove", "src",
                "memmove received an invalid source range",
                vita_memory_return_address(c), destination, source, count,
                &probe);
            return;
        }
        memmove((void *)(uintptr_t)destination,
                (const void *)(uintptr_t)source, (size_t)count);
    }
    c->eax = destination;
    vita_memory_cdecl_return(c);
}

/* Import-table order, not alphabetical order.  This makes an accidental IAT
 * reassignment visible in both the header evidence and the runnable oracle. */
static const vita_memory_import_entry s_vita_memory_imports[] = {
    { ISAAC_VITA_MEMORY_STRCHR_NAME,  vita_memory_strchr },
    { ISAAC_VITA_MEMORY_MEMCHR_NAME,  vita_memory_memchr },
    { ISAAC_VITA_MEMORY_MEMSET_NAME,  vita_memory_memset },
    { ISAAC_VITA_MEMORY_MEMCPY_NAME,  vita_memory_memcpy },
    { ISAAC_VITA_MEMORY_STRSTR_NAME,  vita_memory_strstr },
    { ISAAC_VITA_MEMORY_MEMMOVE_NAME, vita_memory_memmove }
};

_Static_assert(sizeof s_vita_memory_imports /
               sizeof s_vita_memory_imports[0] ==
               ISAAC_VITA_MEMORY_IMPORT_COUNT,
               "Vita memory import count drifted from exact PE batch");

const char *isaac_vita_memory_import_name(uint32_t index)
{
    return index < ISAAC_VITA_MEMORY_IMPORT_COUNT
        ? s_vita_memory_imports[index].name : NULL;
}

int isaac_vita_memory_import_indexed(CPU *__restrict c, uint32_t index,
                                     unsigned *call_count)
{
    if (index >= ISAAC_VITA_MEMORY_IMPORT_COUNT)
        return 0;
    if (call_count)
        ++*call_count; /* guest_fault may not return */
    s_vita_memory_imports[index].fn(c);
    return 1;
}

static int vita_memory_dispatch(CPU *__restrict c, const char *name,
                                unsigned *call_count)
{
    uint32_t i;

    for (i = 0U; i < ISAAC_VITA_MEMORY_IMPORT_COUNT; ++i) {
        if (strcmp(s_vita_memory_imports[i].name, name) == 0)
            return isaac_vita_memory_import_indexed(c, i, call_count);
    }
    return 0;
}

int isaac_vita_memory_import(CPU *__restrict c, const char *name)
{
    return vita_memory_dispatch(c, name, NULL);
}

int isaac_vita_memory_import_counted(CPU *__restrict c, const char *name,
                                     unsigned *call_count)
{
    return vita_memory_dispatch(c, name, call_count);
}
