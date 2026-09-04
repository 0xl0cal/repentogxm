/* Runnable softfp ARM oracle for disabled Steam callback registration. */
#include <setjmp.h>
#include <stdint.h>
#include <string.h>

#include "guest.h"
#include "host_vita_steam.h"

#if UINTPTR_MAX != UINT32_MAX
#error Vita Steam oracle requires a 32-bit identity-mapped host
#endif

typedef struct steam_call_evidence {
    const char *name;
    uint32_t iat_rva;
    uint32_t iat_va;
    uint32_t call_rva;
    uint32_t return_rva;
    uint32_t ordinal;
} steam_call_evidence;

static const steam_call_evidence s_current = {
    ISAAC_VITA_STEAM_REGISTER_CALLBACK_NAME,
    ISAAC_VITA_STEAM_REGISTER_CALLBACK_IAT_RVA,
    ISAAC_VITA_STEAM_REGISTER_CALLBACK_IAT_VA,
    ISAAC_VITA_STEAM_REGISTER_CALLBACK_CALL_RVA,
    ISAAC_VITA_STEAM_REGISTER_CALLBACK_RETURN_RVA,
    ISAAC_VITA_STEAM_REGISTER_CALLBACK_ORDINAL
};

static const steam_call_evidence s_next = {
    ISAAC_VITA_STEAM_NEXT_NAME,
    ISAAC_VITA_STEAM_NEXT_IAT_RVA,
    ISAAC_VITA_STEAM_NEXT_IAT_VA,
    ISAAC_VITA_STEAM_NEXT_CALL_RVA,
    ISAAC_VITA_STEAM_NEXT_RETURN_RVA,
    ISAAC_VITA_STEAM_NEXT_ORDINAL
};

enum steam_family_policy {
    STEAM_FAMILY_LOCAL = 1,
    STEAM_FAMILY_EXTERNAL = 2,
    STEAM_FAMILY_LOUD = 3
};

typedef struct steam_family_evidence {
    const char *name;
    uint32_t iat_rva;
    uint32_t call_rva;
    uint32_t return_rva;
    uint32_t direct_call_count;
    uint64_t direct_call_fnv64;
    uint32_t policy;
} steam_family_evidence;

/* Exact IAT order from 0x6066ac through 0x6066d4.  These table fields are the
 * direct-call census; ContextInit's full physical census is pinned below.
 * Init is owned by host_vita_post_com; the three interface-producing exports
 * remain loud. */
static const steam_family_evidence
    s_family[ISAAC_VITA_STEAM_FAMILY_SLOT_COUNT] = {
    { ISAAC_VITA_STEAM_CONTEXT_INIT_NAME,
      ISAAC_VITA_STEAM_CONTEXT_INIT_IAT_RVA,
      ISAAC_VITA_STEAM_CONTEXT_INIT_CALL_RVA,
      ISAAC_VITA_STEAM_CONTEXT_INIT_RETURN_RVA,
      ISAAC_VITA_STEAM_CONTEXT_DIRECT_CALL_COUNT,
      ISAAC_VITA_STEAM_CONTEXT_DIRECT_CALL_FNV64, STEAM_FAMILY_LOCAL },
    { ISAAC_VITA_STEAM_GET_PIPE_NAME, ISAAC_VITA_STEAM_GET_PIPE_IAT_RVA,
      ISAAC_VITA_STEAM_GET_PIPE_CALL_RVA,
      ISAAC_VITA_STEAM_GET_PIPE_RETURN_RVA,
      ISAAC_VITA_STEAM_GET_PIPE_DIRECT_CALL_COUNT, UINT64_C(0),
      STEAM_FAMILY_LOUD },
    { ISAAC_VITA_STEAM_GET_USER_NAME, ISAAC_VITA_STEAM_GET_USER_IAT_RVA,
      ISAAC_VITA_STEAM_GET_USER_CALL_RVA,
      ISAAC_VITA_STEAM_GET_USER_RETURN_RVA,
      ISAAC_VITA_STEAM_GET_USER_DIRECT_CALL_COUNT, UINT64_C(0),
      STEAM_FAMILY_LOUD },
    { ISAAC_VITA_STEAM_CREATE_INTERFACE_NAME,
      ISAAC_VITA_STEAM_CREATE_INTERFACE_IAT_RVA,
      ISAAC_VITA_STEAM_CREATE_INTERFACE_CALL_RVA,
      ISAAC_VITA_STEAM_CREATE_INTERFACE_RETURN_RVA,
      ISAAC_VITA_STEAM_CREATE_INTERFACE_DIRECT_CALL_COUNT, UINT64_C(0),
      STEAM_FAMILY_LOUD },
    { ISAAC_VITA_STEAM_REGISTER_CALLBACK_NAME,
      ISAAC_VITA_STEAM_REGISTER_CALLBACK_IAT_RVA,
      ISAAC_VITA_STEAM_REGISTER_CALLBACK_CALL_RVA,
      ISAAC_VITA_STEAM_REGISTER_CALLBACK_RETURN_RVA,
      ISAAC_VITA_STEAM_REGISTER_CALLBACK_DIRECT_CALL_COUNT,
      ISAAC_VITA_STEAM_REGISTER_CALLBACK_DIRECT_CALL_FNV64,
      STEAM_FAMILY_LOCAL },
    { ISAAC_VITA_STEAM_REGISTER_CALL_RESULT_NAME,
      ISAAC_VITA_STEAM_REGISTER_CALL_RESULT_IAT_RVA,
      ISAAC_VITA_STEAM_REGISTER_CALL_RESULT_CALL_RVA,
      ISAAC_VITA_STEAM_REGISTER_CALL_RESULT_RETURN_RVA,
      ISAAC_VITA_STEAM_REGISTER_CALL_RESULT_DIRECT_CALL_COUNT,
      ISAAC_VITA_STEAM_REGISTER_CALL_RESULT_DIRECT_CALL_FNV64,
      STEAM_FAMILY_LOCAL },
    { ISAAC_VITA_STEAM_UNREGISTER_CALL_RESULT_NAME,
      ISAAC_VITA_STEAM_UNREGISTER_CALL_RESULT_IAT_RVA,
      ISAAC_VITA_STEAM_UNREGISTER_CALL_RESULT_CALL_RVA,
      ISAAC_VITA_STEAM_UNREGISTER_CALL_RESULT_RETURN_RVA,
      ISAAC_VITA_STEAM_UNREGISTER_CALL_RESULT_DIRECT_CALL_COUNT,
      ISAAC_VITA_STEAM_UNREGISTER_CALL_RESULT_DIRECT_CALL_FNV64,
      STEAM_FAMILY_LOCAL },
    { ISAAC_VITA_STEAM_RUN_CALLBACKS_NAME,
      ISAAC_VITA_STEAM_RUN_CALLBACKS_IAT_RVA,
      ISAAC_VITA_STEAM_RUN_CALLBACKS_CALL_RVA,
      ISAAC_VITA_STEAM_RUN_CALLBACKS_RETURN_RVA,
      ISAAC_VITA_STEAM_RUN_CALLBACKS_DIRECT_CALL_COUNT,
      ISAAC_VITA_STEAM_RUN_CALLBACKS_DIRECT_CALL_FNV64,
      STEAM_FAMILY_LOCAL },
    { ISAAC_VITA_STEAM_SHUTDOWN_NAME, ISAAC_VITA_STEAM_SHUTDOWN_IAT_RVA,
      ISAAC_VITA_STEAM_SHUTDOWN_CALL_RVA,
      ISAAC_VITA_STEAM_SHUTDOWN_RETURN_RVA,
      ISAAC_VITA_STEAM_SHUTDOWN_DIRECT_CALL_COUNT,
      ISAAC_VITA_STEAM_SHUTDOWN_DIRECT_CALL_FNV64, STEAM_FAMILY_LOCAL },
    { ISAAC_VITA_STEAM_INIT_NAME, ISAAC_VITA_STEAM_INIT_IAT_RVA,
      ISAAC_VITA_STEAM_INIT_CALL_RVA, ISAAC_VITA_STEAM_INIT_RETURN_RVA,
      ISAAC_VITA_STEAM_INIT_DIRECT_CALL_COUNT,
      ISAAC_VITA_STEAM_INIT_DIRECT_CALL_FNV64,
      STEAM_FAMILY_EXTERNAL },
    { ISAAC_VITA_STEAM_UNREGISTER_CALLBACK_NAME,
      ISAAC_VITA_STEAM_UNREGISTER_CALLBACK_IAT_RVA,
      ISAAC_VITA_STEAM_UNREGISTER_CALLBACK_CALL_RVA,
      ISAAC_VITA_STEAM_UNREGISTER_CALLBACK_RETURN_RVA,
      ISAAC_VITA_STEAM_UNREGISTER_CALLBACK_DIRECT_CALL_COUNT,
      ISAAC_VITA_STEAM_UNREGISTER_CALLBACK_DIRECT_CALL_FNV64,
      STEAM_FAMILY_LOCAL }
};

#define STEAM_SITE_VALUE(rva) rva,
static const uint32_t s_register_call_result_sites[] = {
    ISAAC_VITA_STEAM_REGISTER_CALL_RESULT_DIRECT_CALL_SITES(
        STEAM_SITE_VALUE)
};
static const uint32_t s_unregister_call_result_sites[] = {
    ISAAC_VITA_STEAM_UNREGISTER_CALL_RESULT_DIRECT_CALL_SITES(
        STEAM_SITE_VALUE)
};
#undef STEAM_SITE_VALUE

_Static_assert(GUEST_IMAGE_BASE +
               ISAAC_VITA_STEAM_REGISTER_CALLBACK_IAT_RVA ==
               ISAAC_VITA_STEAM_REGISTER_CALLBACK_IAT_VA,
               "Steam RegisterCallback IAT VA/base drifted");
_Static_assert(GUEST_IMAGE_BASE +
               ISAAC_VITA_STEAM_REGISTER_CALLBACK_OBJECT_RVA ==
               ISAAC_VITA_STEAM_REGISTER_CALLBACK_OBJECT_VA,
               "Steam callback object VA/base drifted");
_Static_assert(GUEST_IMAGE_BASE + ISAAC_VITA_STEAM_CONTEXT_INIT_IAT_RVA ==
               ISAAC_VITA_STEAM_CONTEXT_INIT_IAT_VA,
               "Steam ContextInit IAT VA/base drifted");
_Static_assert(GUEST_IMAGE_BASE +
               ISAAC_VITA_STEAM_CONTEXT_DESCRIPTOR_RVA ==
               ISAAC_VITA_STEAM_CONTEXT_DESCRIPTOR_VA,
               "Steam context descriptor VA/base drifted");
_Static_assert(GUEST_IMAGE_BASE + ISAAC_VITA_STEAM_CONTEXT_CALLBACK_RVA ==
               ISAAC_VITA_STEAM_CONTEXT_CALLBACK_VA,
               "Steam context callback VA/base drifted");
_Static_assert(ISAAC_VITA_STEAM_CONTEXT_DIRECT_CALL_COUNT == 71U &&
               ISAAC_VITA_STEAM_CONTEXT_DIRECT_CALL_FNV64 ==
                   UINT64_C(0xc6d821ad60ab1e85) &&
               ISAAC_VITA_STEAM_CONTEXT_REGISTER_LOAD_COUNT == 24U &&
               ISAAC_VITA_STEAM_CONTEXT_REGISTER_LOAD_FNV64 ==
                   UINT64_C(0x6ca3efba9cdf0795) &&
               ISAAC_VITA_STEAM_CONTEXT_REGISTER_CALL_COUNT == 67U &&
               ISAAC_VITA_STEAM_CONTEXT_REGISTER_CALL_FNV64 ==
                   UINT64_C(0x34aa83223fda02c7) &&
               ISAAC_VITA_STEAM_CONTEXT_PHYSICAL_CALL_COUNT == 138U &&
               ISAAC_VITA_STEAM_CONTEXT_PHYSICAL_CALL_FNV64 ==
                   UINT64_C(0x10c1bf5a3766bc87) &&
               ISAAC_VITA_STEAM_CONTEXT_DIRECT_CALL_COUNT +
                   ISAAC_VITA_STEAM_CONTEXT_REGISTER_CALL_COUNT ==
                   ISAAC_VITA_STEAM_CONTEXT_PHYSICAL_CALL_COUNT,
               "Steam ContextInit full physical census drifted");
_Static_assert(ISAAC_VITA_STEAM_FAMILY_LAST_IAT_RVA -
               ISAAC_VITA_STEAM_FAMILY_FIRST_IAT_RVA ==
               (ISAAC_VITA_STEAM_FAMILY_SLOT_COUNT - 1U) * 4U,
               "Steam family is no longer an 11-slot contiguous IAT run");
_Static_assert(ISAAC_VITA_STEAM_FAMILY_LOCAL_COUNT +
               ISAAC_VITA_STEAM_FAMILY_EXTERNAL_COUNT ==
               ISAAC_VITA_STEAM_FAMILY_HANDLED_COUNT,
               "Steam handled-owner denominator drifted");
_Static_assert(ISAAC_VITA_STEAM_FAMILY_HANDLED_COUNT +
               ISAAC_VITA_STEAM_FAMILY_LOUD_COUNT ==
               ISAAC_VITA_STEAM_FAMILY_SLOT_COUNT,
               "Steam 8/11 disabled-lifecycle policy drifted");
_Static_assert(sizeof s_register_call_result_sites /
               sizeof s_register_call_result_sites[0] ==
               ISAAC_VITA_STEAM_REGISTER_CALL_RESULT_DIRECT_CALL_COUNT,
               "Steam RegisterCallResult physical census drifted");
_Static_assert(sizeof s_unregister_call_result_sites /
               sizeof s_unregister_call_result_sites[0] ==
               ISAAC_VITA_STEAM_UNREGISTER_CALL_RESULT_DIRECT_CALL_COUNT,
               "Steam UnregisterCallResult physical census drifted");
_Static_assert(GUEST_IMAGE_BASE + ISAAC_VITA_STEAM_NEXT_IAT_RVA ==
               ISAAC_VITA_STEAM_NEXT_IAT_VA,
               "post-initterm IAT VA/base drifted");
_Static_assert(ISAAC_VITA_STEAM_REGISTER_CALLBACK_ORDINAL +
               ISAAC_VITA_STEAM_POST_REGISTER_ATEXIT_CALL_COUNT ==
               ISAAC_VITA_STEAM_POST_REGISTER_LAST_ORDINAL,
               "post-Steam atexit denominator drifted");
_Static_assert(ISAAC_VITA_STEAM_POST_REGISTER_LAST_ORDINAL + 1U ==
               ISAAC_VITA_STEAM_NEXT_ORDINAL,
               "post-initterm frontier is no longer consecutive");
_Static_assert(ISAAC_VITA_STEAM_IMPORT_COUNT == 7U &&
               ISAAC_VITA_STEAM_BOOT_CALL_COUNT == 1U,
               "isolated Steam import surface drifted");

_Alignas(8) static uint32_t s_frame[12];
static CPU s_jump_cpu;
static jmp_buf s_fault_jump;
static unsigned s_jump_count;
static int s_fault_must_jump;
static int s_checked_read_must_fault;
static uint32_t s_checked_fault_address;
static CPU *s_checked_cpu;
static CPU *s_context_cpu;
static uint32_t
    s_context_words[ISAAC_VITA_STEAM_CONTEXT_DWORD_COUNT + 2U];

static uint32_t pointer32(const void *pointer)
{
    return (uint32_t)(uintptr_t)pointer;
}

static int pointer_fits(const void *pointer)
{
    return (uintptr_t)pointer <= UINT32_MAX;
}

static uint32_t prepare_call3(CPU *c, uint32_t argument0,
                              uint32_t argument1, uint32_t argument2)
{
    uint32_t esp;

    memset(c, 0, sizeof *c);
    memset(s_frame, 0xcc, sizeof s_frame);
    esp = pointer32(&s_frame[4]);
    s_frame[4] = 0x0badc0deU;
    s_frame[5] = argument0;
    s_frame[6] = argument1;
    s_frame[7] = argument2;
    c->esp = esp;
    c->eax = 0x13579bdfU;
    return esp;
}

static uint32_t prepare_call(CPU *c, uint32_t object, uint32_t callback_id)
{
    return prepare_call3(c, object, callback_id, 0U);
}

void guest_fault(CPU *__restrict c, uint32_t address, const char *what)
{
    c->fault_addr = address;
    c->fault = what;
    if (s_fault_must_jump)
        longjmp(s_fault_jump, 1);
}

void guest_check(uint32_t address, uint32_t size, int write)
{
    (void)size;
    if (s_checked_read_must_fault && !write &&
        (s_checked_fault_address == 0U ||
         s_checked_fault_address == address)) {
        s_checked_read_must_fault = 0;
        guest_fault(s_checked_cpu, address,
                    "oracle checked Steam argument fault");
    }
}

uint32_t isaac_vita_steam_context_test_read32(uint32_t address)
{
    uint32_t offset;
    uint32_t index;

    guest_check(address, 4U, 0);
    if (address < ISAAC_VITA_STEAM_CONTEXT_DESCRIPTOR_VA) {
        guest_fault(s_context_cpu, address,
                    "oracle context read below descriptor");
        return 0U;
    }
    offset = address - ISAAC_VITA_STEAM_CONTEXT_DESCRIPTOR_VA;
    if ((offset & 3U) != 0U ||
        offset >= sizeof s_context_words) {
        guest_fault(s_context_cpu, address,
                    "oracle context read outside descriptor");
        return 0U;
    }
    index = offset / 4U;
    return s_context_words[index];
}

static uint64_t hash_byte(uint64_t hash, uint8_t value)
{
    return (hash ^ value) * UINT64_C(0x100000001b3);
}

static uint64_t hash_text(uint64_t hash, const char *text)
{
    const unsigned char *cursor = (const unsigned char *)text;

    do {
        hash = hash_byte(hash, *cursor);
    } while (*cursor++ != 0U);
    return hash;
}

static uint64_t hash_u32(uint64_t hash, uint32_t value)
{
    uint32_t shift;

    for (shift = 0U; shift < 32U; shift += 8U)
        hash = hash_byte(hash, (uint8_t)(value >> shift));
    return hash;
}

static uint64_t hash_u64(uint64_t hash, uint64_t value)
{
    uint32_t shift;

    for (shift = 0U; shift < 64U; shift += 8U)
        hash = hash_byte(hash, (uint8_t)(value >> shift));
    return hash;
}

static uint64_t rva_sequence_fnv64(const uint32_t *sites, uint32_t count)
{
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    uint32_t index;

    for (index = 0U; index < count; ++index)
        hash = hash_u32(hash, sites[index]);
    return hash;
}

static uint64_t hash_record(uint64_t hash,
                            const steam_call_evidence *record)
{
    hash = hash_text(hash, record->name);
    hash = hash_u32(hash, record->iat_rva);
    hash = hash_u32(hash, record->iat_va);
    hash = hash_u32(hash, record->call_rva);
    hash = hash_u32(hash, record->return_rva);
    return hash_u32(hash, record->ordinal);
}

static uint64_t hash_family_record(uint64_t hash,
                                   const steam_family_evidence *record)
{
    hash = hash_text(hash, record->name);
    hash = hash_u32(hash, record->iat_rva);
    hash = hash_u32(hash, record->call_rva);
    hash = hash_u32(hash, record->return_rva);
    hash = hash_u32(hash, record->direct_call_count);
    hash = hash_u64(hash, record->direct_call_fnv64);
    return hash_u32(hash, record->policy);
}

static uint64_t evidence_hash(void)
{
    uint64_t hash = UINT64_C(0xcbf29ce484222325);
    uint32_t index;

    hash = hash_record(hash, &s_current);
    hash = hash_u32(hash, ISAAC_VITA_STEAM_REGISTER_CALLBACK_OBJECT_RVA);
    hash = hash_u32(hash, ISAAC_VITA_STEAM_REGISTER_CALLBACK_OBJECT_VA);
    hash = hash_u32(hash, ISAAC_VITA_STEAM_REGISTER_CALLBACK_ID);
    hash = hash_u32(hash, ISAAC_VITA_STEAM_POST_REGISTER_ATEXIT_IAT_RVA);
    hash = hash_u32(hash, ISAAC_VITA_STEAM_POST_REGISTER_ATEXIT_CALL_RVA);
    hash = hash_u32(hash, ISAAC_VITA_STEAM_POST_REGISTER_ATEXIT_RETURN_RVA);
    hash = hash_u32(hash,
                    ISAAC_VITA_STEAM_POST_REGISTER_FIRST_CALLBACK_VA);
    hash = hash_u32(hash,
                    ISAAC_VITA_STEAM_POST_REGISTER_ATEXIT_CALL_COUNT);
    hash = hash_u32(hash, ISAAC_VITA_STEAM_INIT_TABLE_CURRENT_ENTRY_RVA);
    hash = hash_u32(hash, ISAAC_VITA_STEAM_INIT_TABLE_CURRENT_FUNCTION_RVA);
    hash = hash_u32(hash, ISAAC_VITA_STEAM_INIT_TABLE_LAST_ENTRY_RVA);
    hash = hash_u32(hash, ISAAC_VITA_STEAM_INIT_TABLE_LAST_FUNCTION_RVA);
    hash = hash_u32(hash, ISAAC_VITA_STEAM_INIT_TABLE_TERMINATOR_RVA);
    hash = hash_record(hash, &s_next);
    for (index = 0U; index < ISAAC_VITA_STEAM_FAMILY_SLOT_COUNT; ++index)
        hash = hash_family_record(hash, &s_family[index]);
    hash = hash_u32(hash, ISAAC_VITA_STEAM_CONTEXT_DESCRIPTOR_RVA);
    hash = hash_u32(hash, ISAAC_VITA_STEAM_CONTEXT_DESCRIPTOR_VA);
    hash = hash_u32(hash, ISAAC_VITA_STEAM_CONTEXT_CALLBACK_RVA);
    hash = hash_u32(hash, ISAAC_VITA_STEAM_CONTEXT_CALLBACK_VA);
    hash = hash_u32(hash, ISAAC_VITA_STEAM_CONTEXT_GENERATION);
    hash = hash_u32(hash, ISAAC_VITA_STEAM_CONTEXT_DWORD_COUNT);
    hash = hash_u32(hash, ISAAC_VITA_STEAM_FAMILY_HANDLED_COUNT);
    hash = hash_u32(hash, ISAAC_VITA_STEAM_FAMILY_LOCAL_COUNT);
    hash = hash_u32(hash, ISAAC_VITA_STEAM_FAMILY_EXTERNAL_COUNT);
    hash = hash_u32(hash, ISAAC_VITA_STEAM_FAMILY_LOUD_COUNT);
    hash = hash_u32(hash, ISAAC_VITA_STEAM_IMPORT_COUNT);
    hash = hash_u32(hash, ISAAC_VITA_STEAM_BOOT_CALL_COUNT);
    return hash_u32(hash, ISAAC_VITA_STEAM_CALLBACK_CAPACITY);
}

static int counted_call(CPU *c, unsigned *calls, uint32_t object,
                        uint32_t callback_id)
{
    uint32_t esp = prepare_call(c, object, callback_id);
    unsigned expected = *calls + 1U;

    if (!isaac_vita_steam_import_counted(
            c, ISAAC_VITA_STEAM_REGISTER_CALLBACK_NAME, calls))
        return 0;
    return !c->fault && *calls == expected && c->esp == esp + 4U &&
           c->eax == 0x13579bdfU && s_frame[5] == object &&
           s_frame[6] == callback_id;
}

int main(void)
{
    CPU cpu;
    CPU snapshot;
    uint32_t esp;
    unsigned calls = 0U;
    unsigned index;
    unsigned local_count = 0U;
    unsigned external_count = 0U;
    unsigned loud_count = 0U;
    int32_t callback_id = 0;

    if (!pointer_fits(s_frame))
        return 1;
    if (evidence_hash() != UINT64_C(0x6bef938096319b04))
        return 2;
    if (strcmp(s_current.name,
               "steam_api.dll!SteamAPI_RegisterCallback") != 0 ||
        s_current.iat_rva != 0x006066bcU ||
        s_current.iat_va != 0x986066bcU ||
        s_current.call_rva != 0x00001deaU ||
        s_current.return_rva != 0x00001df0U ||
        s_current.ordinal != 255U ||
        ISAAC_VITA_STEAM_REGISTER_CALLBACK_OBJECT_VA != 0x987e815cU ||
        ISAAC_VITA_STEAM_REGISTER_CALLBACK_ID != 0x44dU ||
        ISAAC_VITA_STEAM_POST_REGISTER_ATEXIT_CALL_COUNT != 7U ||
        ISAAC_VITA_STEAM_POST_REGISTER_FIRST_ORDINAL != 256U ||
        ISAAC_VITA_STEAM_POST_REGISTER_LAST_ORDINAL != 262U ||
        strcmp(s_next.name,
               "api-ms-win-crt-runtime-l1-1-0.dll!"
               "_get_initial_narrow_environment") != 0 ||
        s_next.iat_rva != 0x00606574U ||
        s_next.iat_va != 0x98606574U ||
        s_next.call_rva != 0x005eb79aU ||
        s_next.return_rva != 0x005eb79fU ||
        s_next.ordinal != 263U ||
        ISAAC_VITA_STEAM_CONTEXT_INIT_IAT_VA != 0x986066acU ||
        ISAAC_VITA_STEAM_CONTEXT_INIT_CALL_RVA != 0x004ad6b5U ||
        ISAAC_VITA_STEAM_CONTEXT_INIT_RETURN_RVA != 0x004ad6bbU ||
        ISAAC_VITA_STEAM_CONTEXT_DESCRIPTOR_VA != 0x987aa3c8U ||
        ISAAC_VITA_STEAM_CONTEXT_CALLBACK_VA != 0x980189a0U ||
        ISAAC_VITA_STEAM_CONTEXT_DWORD_COUNT != 21U ||
        ISAAC_VITA_STEAM_CONTEXT_DIRECT_CALL_COUNT != 71U ||
        ISAAC_VITA_STEAM_CONTEXT_REGISTER_LOAD_COUNT != 24U ||
        ISAAC_VITA_STEAM_CONTEXT_REGISTER_CALL_COUNT != 67U ||
        ISAAC_VITA_STEAM_CONTEXT_PHYSICAL_CALL_COUNT != 138U)
        return 3;
    for (index = 0U; index < ISAAC_VITA_STEAM_FAMILY_SLOT_COUNT; ++index) {
        if (s_family[index].iat_rva !=
            ISAAC_VITA_STEAM_FAMILY_FIRST_IAT_RVA + index * 4U)
            return 3;
        if (s_family[index].policy == STEAM_FAMILY_LOCAL)
            ++local_count;
        else if (s_family[index].policy == STEAM_FAMILY_EXTERNAL)
            ++external_count;
        else if (s_family[index].policy == STEAM_FAMILY_LOUD)
            ++loud_count;
        else
            return 3;
    }
    if (local_count != ISAAC_VITA_STEAM_FAMILY_LOCAL_COUNT ||
        external_count != ISAAC_VITA_STEAM_FAMILY_EXTERNAL_COUNT ||
        loud_count != ISAAC_VITA_STEAM_FAMILY_LOUD_COUNT ||
        rva_sequence_fnv64(
            s_register_call_result_sites,
            ISAAC_VITA_STEAM_REGISTER_CALL_RESULT_DIRECT_CALL_COUNT) !=
            ISAAC_VITA_STEAM_REGISTER_CALL_RESULT_DIRECT_CALL_FNV64 ||
        rva_sequence_fnv64(
            s_unregister_call_result_sites,
            ISAAC_VITA_STEAM_UNREGISTER_CALL_RESULT_DIRECT_CALL_COUNT) !=
            ISAAC_VITA_STEAM_UNREGISTER_CALL_RESULT_DIRECT_CALL_FNV64)
        return 3;

    isaac_vita_steam_callback_clear();
    if (!counted_call(&cpu, &calls,
                      ISAAC_VITA_STEAM_REGISTER_CALLBACK_OBJECT_VA,
                      ISAAC_VITA_STEAM_REGISTER_CALLBACK_ID) ||
        isaac_vita_steam_callback_count() != 1U ||
        !isaac_vita_steam_callback_lookup(
            ISAAC_VITA_STEAM_REGISTER_CALLBACK_OBJECT_VA, &callback_id) ||
        callback_id != (int32_t)ISAAC_VITA_STEAM_REGISTER_CALLBACK_ID)
        return 4;

    /* NULL, externally-owned Init and all three loud interface slots are
     * exact mutation-free dispatcher handoffs. */
    esp = prepare_call(&cpu, 0x81234000U, 9U);
    memcpy(&snapshot, &cpu, sizeof snapshot);
    if (isaac_vita_steam_import_counted(&cpu, NULL, &calls) != 0 ||
        calls != 1U || memcmp(&snapshot, &cpu, sizeof cpu) != 0 ||
        isaac_vita_steam_import_counted(
            &cpu, ISAAC_VITA_STEAM_INIT_NAME, &calls) != 0 ||
        calls != 1U || memcmp(&snapshot, &cpu, sizeof cpu) != 0 ||
        isaac_vita_steam_import_counted(
            &cpu, ISAAC_VITA_STEAM_GET_PIPE_NAME, &calls) != 0 ||
        calls != 1U || memcmp(&snapshot, &cpu, sizeof cpu) != 0 ||
        isaac_vita_steam_import_counted(
            &cpu, ISAAC_VITA_STEAM_GET_USER_NAME, &calls) != 0 ||
        calls != 1U || memcmp(&snapshot, &cpu, sizeof cpu) != 0 ||
        isaac_vita_steam_import_counted(
            &cpu, ISAAC_VITA_STEAM_CREATE_INTERFACE_NAME, &calls) != 0 ||
        calls != 1U || cpu.esp != esp ||
        memcmp(&snapshot, &cpu, sizeof cpu) != 0 ||
        isaac_vita_steam_callback_count() != 1U)
        return 5;

    /* Same object is one listener.  A re-registration changes its observable
     * id but never claims that a Steam backend will deliver it. */
    if (!counted_call(&cpu, &calls,
                      ISAAC_VITA_STEAM_REGISTER_CALLBACK_OBJECT_VA,
                      ISAAC_VITA_STEAM_REGISTER_CALLBACK_ID) ||
        !counted_call(&cpu, &calls,
                      ISAAC_VITA_STEAM_REGISTER_CALLBACK_OBJECT_VA,
                      0x76543210U) ||
        isaac_vita_steam_callback_count() != 1U ||
        !isaac_vita_steam_callback_lookup(
            ISAAC_VITA_STEAM_REGISTER_CALLBACK_OBJECT_VA, &callback_id) ||
        callback_id != (int32_t)0x76543210U)
        return 6;

    /* Fill the bounded registry with distinct listeners. */
    for (index = 1U; index < ISAAC_VITA_STEAM_CALLBACK_CAPACITY; ++index) {
        if (!counted_call(&cpu, &calls, 0x81000000U + index * 0x20U,
                          0x400U + index))
            return 7;
    }
    if (isaac_vita_steam_callback_count() !=
        ISAAC_VITA_STEAM_CALLBACK_CAPACITY)
        return 8;

    /* Capacity failure is loud, counted before the handler and leaves the
     * guest call frame/state intact through production's nonlocal fault. */
    (void)prepare_call(&s_jump_cpu, 0x82000000U, 0x500U);
    s_jump_count = calls;
    s_fault_must_jump = 1;
    if (setjmp(s_fault_jump) == 0) {
        (void)isaac_vita_steam_import_counted(
            &s_jump_cpu, ISAAC_VITA_STEAM_REGISTER_CALLBACK_NAME,
            &s_jump_count);
        return 9;
    }
    s_fault_must_jump = 0;
    if (s_jump_count != calls + 1U || !s_jump_cpu.fault ||
        strcmp(s_jump_cpu.fault, "Steam callback registry is full") != 0 ||
        isaac_vita_steam_callback_count() !=
            ISAAC_VITA_STEAM_CALLBACK_CAPACITY)
        return 10;

    /* Unregister compacts safely, then a new listener can reuse capacity. */
    if (!isaac_vita_steam_callback_unregister(0x81000020U) ||
        isaac_vita_steam_callback_unregister(0x81000020U) ||
        isaac_vita_steam_callback_lookup(0x81000020U, NULL) ||
        isaac_vita_steam_callback_count() !=
            ISAAC_VITA_STEAM_CALLBACK_CAPACITY - 1U ||
        !counted_call(&cpu, &calls, 0x82000000U, 0x500U) ||
        isaac_vita_steam_callback_count() !=
            ISAAC_VITA_STEAM_CALLBACK_CAPACITY)
        return 11;

    /* A checked argument fault commits the dynamic denominator but no state. */
    isaac_vita_steam_callback_clear();
    (void)prepare_call(&s_jump_cpu, 0x83000000U, 0x600U);
    s_jump_count = calls;
    s_checked_cpu = &s_jump_cpu;
    s_checked_read_must_fault = 1;
    s_fault_must_jump = 1;
    if (setjmp(s_fault_jump) == 0) {
        (void)isaac_vita_steam_import_counted(
            &s_jump_cpu, ISAAC_VITA_STEAM_REGISTER_CALLBACK_NAME,
            &s_jump_count);
        return 12;
    }
    s_fault_must_jump = 0;
    if (s_jump_count != calls + 1U || !s_jump_cpu.fault ||
        strcmp(s_jump_cpu.fault,
               "oracle checked Steam argument fault") != 0 ||
        isaac_vita_steam_callback_count() != 0U)
        return 13;

    /* NULL is rejected without creating a fake listener. */
    (void)prepare_call(&s_jump_cpu, 0U, 0x700U);
    s_jump_count = calls;
    s_fault_must_jump = 1;
    if (setjmp(s_fault_jump) == 0) {
        (void)isaac_vita_steam_import_counted(
            &s_jump_cpu, ISAAC_VITA_STEAM_REGISTER_CALLBACK_NAME,
            &s_jump_count);
        return 14;
    }
    s_fault_must_jump = 0;
    if (s_jump_count != calls + 1U || !s_jump_cpu.fault ||
        strcmp(s_jump_cpu.fault,
               "SteamAPI_RegisterCallback received a NULL object") != 0 ||
        isaac_vita_steam_callback_count() != 0U)
        return 15;

    /* Uncounted production wrapper has identical cdecl/state semantics. */
    esp = prepare_call(&cpu,
                       ISAAC_VITA_STEAM_REGISTER_CALLBACK_OBJECT_VA,
                       ISAAC_VITA_STEAM_REGISTER_CALLBACK_ID);
    if (!isaac_vita_steam_import(
            &cpu, ISAAC_VITA_STEAM_REGISTER_CALLBACK_NAME) ||
        cpu.fault || cpu.esp != esp + 4U || cpu.eax != 0x13579bdfU ||
        isaac_vita_steam_callback_count() != 1U ||
        !isaac_vita_steam_callback_lookup(
            ISAAC_VITA_STEAM_REGISTER_CALLBACK_OBJECT_VA, &callback_id) ||
        callback_id != (int32_t)ISAAC_VITA_STEAM_REGISTER_CALLBACK_ID)
        return 16;

    /* Lifecycle unregister observes the bounded callback model instead of
     * becoming a second, contradictory no-op implementation. */
    esp = prepare_call(&cpu,
                       ISAAC_VITA_STEAM_REGISTER_CALLBACK_OBJECT_VA, 0U);
    if (!isaac_vita_steam_import_counted(
            &cpu, ISAAC_VITA_STEAM_UNREGISTER_CALLBACK_NAME, &calls) ||
        cpu.fault || cpu.esp != esp + 4U || cpu.eax != 0x13579bdfU ||
        isaac_vita_steam_callback_count() != 0U)
        return 17;

    if (!counted_call(&cpu, &calls,
                      ISAAC_VITA_STEAM_REGISTER_CALLBACK_OBJECT_VA,
                      ISAAC_VITA_STEAM_REGISTER_CALLBACK_ID))
        return 18;
    esp = prepare_call3(&cpu, 0U, 0U, 0U);
    if (!isaac_vita_steam_import_counted(
            &cpu, ISAAC_VITA_STEAM_RUN_CALLBACKS_NAME, &calls) ||
        cpu.fault || cpu.esp != esp + 4U || cpu.eax != 0x13579bdfU ||
        isaac_vita_steam_callback_count() != 1U)
        return 19;

    /* Both call-result helpers consume object + uint64 handle as three cdecl
     * dwords but retain no promise of delivery on a disabled backend. */
    esp = prepare_call3(&cpu, 0x81234000U, 0x89abcdefU, 0x01234567U);
    if (!isaac_vita_steam_import_counted(
            &cpu, ISAAC_VITA_STEAM_REGISTER_CALL_RESULT_NAME, &calls) ||
        cpu.fault || cpu.esp != esp + 4U || cpu.eax != 0x13579bdfU ||
        s_frame[5] != 0x81234000U || s_frame[6] != 0x89abcdefU ||
        s_frame[7] != 0x01234567U ||
        isaac_vita_steam_callback_count() != 1U)
        return 20;
    esp = prepare_call3(&cpu, 0x81234000U, 0x89abcdefU, 0x01234567U);
    if (!isaac_vita_steam_import_counted(
            &cpu, ISAAC_VITA_STEAM_UNREGISTER_CALL_RESULT_NAME, &calls) ||
        cpu.fault || cpu.esp != esp + 4U || cpu.eax != 0x13579bdfU ||
        s_frame[5] != 0x81234000U || s_frame[6] != 0x89abcdefU ||
        s_frame[7] != 0x01234567U ||
        isaac_vita_steam_callback_count() != 1U)
        return 21;

    esp = prepare_call3(&cpu, 0U, 0U, 0U);
    if (!isaac_vita_steam_import_counted(
            &cpu, ISAAC_VITA_STEAM_SHUTDOWN_NAME, &calls) ||
        cpu.fault || cpu.esp != esp + 4U || cpu.eax != 0x13579bdfU ||
        isaac_vita_steam_callback_count() != 0U)
        return 22;

    /* The standalone oracle models only the exact mapped descriptor. */
    memset(s_context_words, 0, sizeof s_context_words);
    s_context_words[0] = ISAAC_VITA_STEAM_CONTEXT_CALLBACK_VA;
    esp = prepare_call(&cpu, ISAAC_VITA_STEAM_CONTEXT_DESCRIPTOR_VA, 0U);
    s_context_cpu = &cpu;
    if (!isaac_vita_steam_import_counted(
            &cpu, ISAAC_VITA_STEAM_CONTEXT_INIT_NAME, &calls) ||
        cpu.fault || cpu.esp != esp + 4U ||
        cpu.eax != ISAAC_VITA_STEAM_CONTEXT_DESCRIPTOR_VA + 8U)
        return 23;
    for (index = 0U;
         index < ISAAC_VITA_STEAM_CONTEXT_DWORD_COUNT + 2U; ++index) {
        uint32_t expected =
            index == 0U ? ISAAC_VITA_STEAM_CONTEXT_CALLBACK_VA : 0U;

        if (s_context_words[index] != expected)
            return 23;
    }

    /* Unknown descriptor/callback/generation states fail before returning a
     * pointer that would turn into a later NULL-vtable mystery. */
    esp = prepare_call(&cpu,
                       ISAAC_VITA_STEAM_CONTEXT_DESCRIPTOR_VA + 4U, 0U);
    if (!isaac_vita_steam_import_counted(
            &cpu, ISAAC_VITA_STEAM_CONTEXT_INIT_NAME, &calls) ||
        !cpu.fault ||
        strcmp(cpu.fault,
               "SteamInternal_ContextInit unknown descriptor state") != 0 ||
        cpu.fault_addr != ISAAC_VITA_STEAM_CONTEXT_DESCRIPTOR_VA + 4U ||
        cpu.esp != esp || cpu.eax != 0x13579bdfU)
        return 24;

    memset(s_context_words, 0, sizeof s_context_words);
    s_context_words[0] = ISAAC_VITA_STEAM_CONTEXT_CALLBACK_VA ^ 4U;
    esp = prepare_call(&cpu, ISAAC_VITA_STEAM_CONTEXT_DESCRIPTOR_VA, 0U);
    s_context_cpu = &cpu;
    if (!isaac_vita_steam_import_counted(
            &cpu, ISAAC_VITA_STEAM_CONTEXT_INIT_NAME, &calls) ||
        !cpu.fault ||
        strcmp(cpu.fault,
               "SteamInternal_ContextInit unknown descriptor state") != 0 ||
        cpu.fault_addr != ISAAC_VITA_STEAM_CONTEXT_DESCRIPTOR_VA ||
        cpu.esp != esp || cpu.eax != 0x13579bdfU)
        return 25;

    memset(s_context_words, 0, sizeof s_context_words);
    s_context_words[0] = ISAAC_VITA_STEAM_CONTEXT_CALLBACK_VA;
    s_context_words[1] = 1U;
    esp = prepare_call(&cpu, ISAAC_VITA_STEAM_CONTEXT_DESCRIPTOR_VA, 0U);
    s_context_cpu = &cpu;
    if (!isaac_vita_steam_import_counted(
            &cpu, ISAAC_VITA_STEAM_CONTEXT_INIT_NAME, &calls) ||
        !cpu.fault ||
        strcmp(cpu.fault,
               "SteamInternal_ContextInit unknown descriptor state") != 0 ||
        cpu.fault_addr != ISAAC_VITA_STEAM_CONTEXT_DESCRIPTOR_VA ||
        cpu.esp != esp || cpu.eax != 0x13579bdfU)
        return 26;

    /* Every one of the 21 context slots is part of the zero-state contract. */
    for (index = 0U; index < ISAAC_VITA_STEAM_CONTEXT_DWORD_COUNT; ++index) {
        uint32_t slot = ISAAC_VITA_STEAM_CONTEXT_DESCRIPTOR_VA +
                        8U + index * 4U;

        memset(s_context_words, 0, sizeof s_context_words);
        s_context_words[0] = ISAAC_VITA_STEAM_CONTEXT_CALLBACK_VA;
        s_context_words[index + 2U] = 0x40000000U + index;
        esp = prepare_call(&cpu,
                           ISAAC_VITA_STEAM_CONTEXT_DESCRIPTOR_VA, 0U);
        s_context_cpu = &cpu;
        if (!isaac_vita_steam_import_counted(
                &cpu, ISAAC_VITA_STEAM_CONTEXT_INIT_NAME, &calls) ||
            !cpu.fault ||
            strcmp(cpu.fault,
                   "Steam-disabled context unexpectedly initialized") != 0 ||
            cpu.fault_addr != slot || cpu.esp != esp ||
            cpu.eax != 0x13579bdfU)
            return 27;
    }

    /* A checked descriptor read faults after the dynamic denominator commits
     * and before ContextInit can return or mutate guest-visible state. */
    memset(s_context_words, 0, sizeof s_context_words);
    s_context_words[0] = ISAAC_VITA_STEAM_CONTEXT_CALLBACK_VA;
    (void)prepare_call(&s_jump_cpu,
                       ISAAC_VITA_STEAM_CONTEXT_DESCRIPTOR_VA, 0U);
    s_context_cpu = &s_jump_cpu;
    s_checked_cpu = &s_jump_cpu;
    s_checked_fault_address =
        ISAAC_VITA_STEAM_CONTEXT_DESCRIPTOR_VA + 8U + 10U * 4U;
    s_checked_read_must_fault = 1;
    s_jump_count = calls;
    s_fault_must_jump = 1;
    if (setjmp(s_fault_jump) == 0) {
        (void)isaac_vita_steam_import_counted(
            &s_jump_cpu, ISAAC_VITA_STEAM_CONTEXT_INIT_NAME,
            &s_jump_count);
        return 28;
    }
    s_fault_must_jump = 0;
    s_checked_fault_address = 0U;
    if (s_jump_count != calls + 1U || !s_jump_cpu.fault ||
        strcmp(s_jump_cpu.fault,
               "oracle checked Steam argument fault") != 0 ||
        s_jump_cpu.fault_addr !=
            ISAAC_VITA_STEAM_CONTEXT_DESCRIPTOR_VA + 8U + 10U * 4U)
        return 29;

    return 0;
}
