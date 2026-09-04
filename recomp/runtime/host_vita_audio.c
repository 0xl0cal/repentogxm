/* Direct frozen-x86 OpenAL ABI adapters for Vita OpenAL Soft. */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifdef ISAAC_VITA_AUDIO_ORACLE
#include "vita_audio_test_openal.h"
#else
#ifndef AL_LIBTYPE_STATIC
#define AL_LIBTYPE_STATIC
#endif
#include <AL/al.h>
#include <AL/alc.h>
#endif

#include "host_vita_audio.h"
#include "host_vita_import_id.h"
#if defined(ISAAC_VITA_AUDIO_REFILL_HOOKS)
/* Stream refill pacing/receipts.  Off in every oracle and in ordinary
 * production builds; see host_vita_audio_refill.h for the measured rationale. */
#include "host_vita_audio_refill.h"
#endif
#ifdef ISAAC_VITA_OPENAL_POOL_ROUTER
#include "guest_pe.h"
#include "host_vita_openal_pool.h"
#endif

/* platform.h is deliberately not required by the standalone ABI link probe.
 * Production provides this logger; focused oracles provide a no-allocation
 * capture stub. */
void isaac_vita_log(const char *format, ...);

#ifdef ISAAC_VITA_OPENAL_POOL_ROUTER
static int s_vita_audio_pool_init_attempted;
static int s_vita_audio_pool_final_logged;

static void vita_audio_openal_pool_log(const char *phase)
{
    isaac_vita_openal_pool_snapshot snapshot;

    if (!isaac_vita_openal_pool_snapshot_get(&snapshot)) {
        isaac_vita_log("KAGE VITA OPENAL POOL: phase=%s snapshot=unavailable",
                       phase);
        return;
    }
    isaac_vita_log(
        "KAGE VITA OPENAL POOL: phase=%s state=%u init=%u backing=%u "
        "live=%u bytes=%u peak=%u/%u ops=%u/%u/%u pool_fail=%u "
        "stranded=%u corrupt=%u pre=%u",
        phase, (unsigned)snapshot.state, (unsigned)snapshot.init_failure,
        (unsigned)snapshot.backing_bytes, (unsigned)snapshot.live_count,
        (unsigned)snapshot.live_requested_bytes,
        (unsigned)snapshot.peak_live_count,
        (unsigned)snapshot.peak_requested_bytes,
        (unsigned)snapshot.allocations, (unsigned)snapshot.frees,
        (unsigned)snapshot.reallocations, (unsigned)snapshot.failures,
        (unsigned)snapshot.stranded_count, (unsigned)snapshot.corruption,
        (unsigned)snapshot.preinit_native_allocations);
}

static void vita_audio_openal_pool_ensure(void)
{
    if (s_vita_audio_pool_init_attempted)
        return;
    s_vita_audio_pool_init_attempted = 1;
    (void)isaac_vita_openal_pool_initialize(
        GUEST_PE_VITA_TARGET_BASE,
        GUEST_PE_VITA_TARGET_BASE + GUEST_PE_EXPECTED_IMAGE_SIZE);
    vita_audio_openal_pool_log("init");
}

static void vita_audio_openal_pool_log_final(void)
{
    if (s_vita_audio_pool_final_logged)
        return;
    s_vita_audio_pool_final_logged = 1;
    vita_audio_openal_pool_log("final");
}

static int vita_audio_openal_pool_manager_failure(int status)
{
    vita_audio_openal_pool_log("manager-fail");
    return status;
}

int isaac_vita_audio_openal_pool_reserve(void)
{
    isaac_vita_openal_pool_snapshot snapshot;

    vita_audio_openal_pool_ensure();
    if (!isaac_vita_openal_pool_snapshot_get(&snapshot))
        return 0;
    return snapshot.state == ISAAC_VITA_OPENAL_POOL_READY;
}
#else
static void vita_audio_openal_pool_ensure(void) { }
static void vita_audio_openal_pool_log(const char *phase) { (void)phase; }
static void vita_audio_openal_pool_log_final(void) { }
static int vita_audio_openal_pool_manager_failure(int status)
{
    return status;
}

int isaac_vita_audio_openal_pool_reserve(void)
{
    return 0;
}
#endif

typedef void (*vita_audio_handler)(CPU *__restrict c);

typedef struct vita_audio_import_entry {
    const char *name;
    vita_audio_handler handler;
} vita_audio_import_entry;

#define ISAAC_VITA_AUDIO_INDEX_ENTRY(id, name, iat_rva) \
    VITA_AUDIO_INDEX_##id,
enum vita_audio_import_index {
    ISAAC_VITA_AUDIO_IMPORTS(ISAAC_VITA_AUDIO_INDEX_ENTRY)
    VITA_AUDIO_INDEX_COUNT
};
#undef ISAAC_VITA_AUDIO_INDEX_ENTRY

typedef struct vita_audio_a005_attribution {
    uint32_t sequence;
    uint32_t last_sequence;
    uint32_t last_index;
    uint32_t last_arguments[5];
    uint32_t allocation_sequence;
    uint32_t allocation_index;
    uint32_t allocation_arguments[5];
    uint32_t total;
    uint32_t is_playing_total;
    uint32_t set_volume_total;
    uint32_t other_total;
} vita_audio_a005_attribution;

typedef struct vita_audio_manager_owner {
    uint32_t manager;
    ALCdevice *device;
    ALCcontext *context;
    uint8_t sources_live;
    uint8_t buffers_live;
} vita_audio_manager_owner;

/* Sound::Manager initialization, its imported OpenAL calls, and the host
 * fault boundary all execute on the same translated-guest owner thread.  The
 * native mixer never enters this adapter, so this compound owner transition
 * deliberately needs no half-atomic fields.  Native pointers live here rather
 * than in mutable guest memory; a direct imported close can therefore retire
 * exactly its owned handle without making final teardown guess or double-close.
 */
static vita_audio_manager_owner s_vita_audio_owner;
static vita_audio_a005_attribution s_vita_audio_a005 = {
    .last_index = UINT32_MAX,
    .allocation_index = UINT32_MAX
};

static const uint8_t s_vita_audio_argument_counts[VITA_AUDIO_INDEX_COUNT] = {
    [VITA_AUDIO_INDEX_source_play] = 1U,
    [VITA_AUDIO_INDEX_get_source_i] = 3U,
    [VITA_AUDIO_INDEX_source_stop] = 1U,
    [VITA_AUDIO_INDEX_gen_buffers] = 2U,
    [VITA_AUDIO_INDEX_source_queue_buffers] = 3U,
    [VITA_AUDIO_INDEX_source_pause] = 1U,
    [VITA_AUDIO_INDEX_source_unqueue_buffers] = 3U,
    [VITA_AUDIO_INDEX_make_context_current] = 1U,
    [VITA_AUDIO_INDEX_destroy_context] = 1U,
    [VITA_AUDIO_INDEX_open_device] = 1U,
    [VITA_AUDIO_INDEX_create_context] = 2U,
    [VITA_AUDIO_INDEX_close_device] = 1U,
    [VITA_AUDIO_INDEX_listener_3f] = 4U,
    [VITA_AUDIO_INDEX_source_i] = 3U,
    [VITA_AUDIO_INDEX_process_context] = 1U,
    [VITA_AUDIO_INDEX_get_source_f] = 3U,
    [VITA_AUDIO_INDEX_gen_sources] = 2U,
    [VITA_AUDIO_INDEX_get_error] = 0U,
    [VITA_AUDIO_INDEX_delete_buffers] = 2U,
    [VITA_AUDIO_INDEX_listener_fv] = 2U,
    [VITA_AUDIO_INDEX_delete_sources] = 2U,
    [VITA_AUDIO_INDEX_source_3f] = 5U,
    [VITA_AUDIO_INDEX_source_f] = 3U,
    [VITA_AUDIO_INDEX_buffer_data] = 5U
};

_Static_assert(VITA_AUDIO_INDEX_COUNT == ISAAC_VITA_AUDIO_IMPORT_COUNT,
               "OpenAL diagnostic index count drifted");
_Static_assert(sizeof s_vita_audio_argument_counts /
                   sizeof s_vita_audio_argument_counts[0] ==
                   ISAAC_VITA_AUDIO_IMPORT_COUNT,
               "OpenAL diagnostic argument inventory drifted");

/* Keep the frozen materialisation source contract while making the value an
 * alias of the single owner record, not a second latch. */
#define s_vita_audio_active_manager                                      \
    (s_vita_audio_owner.manager && s_vita_audio_owner.device &&          \
     s_vita_audio_owner.context)

#if defined(__vita__)
_Static_assert(sizeof(void *) == 4U,
               "guest/native OpenAL pointer seam requires a 32-bit Vita ABI");
#endif
_Static_assert(sizeof(ALuint) == 4U && sizeof(ALint) == 4U &&
                   sizeof(ALenum) == 4U && sizeof(ALsizei) == 4U &&
                   sizeof(ALfloat) == 4U,
               "OpenAL scalar widths no longer match the frozen x86 ABI");
_Static_assert(ISAAC_VITA_AUDIO_DEVICE_SOURCE_COUNT >=
                   ISAAC_VITA_AUDIO_MANAGER_SOURCE_COUNT,
               "OpenAL device capacity cannot be below the guest manager pool");
_Static_assert(ISAAC_VITA_AUDIO_STREAM_SOURCE_HEADROOM == 16U,
               "frozen music/jingle/Theora source headroom drifted");

static uint32_t vita_audio_arg(CPU *__restrict c, uint32_t index)
{
    return ld32(guest_stack_address(
        c, c->esp + 4U + index * 4U, 4U, 0U));
}

static uint32_t vita_audio_counter_increment(uint32_t *counter)
{
    if (*counter != UINT32_MAX)
        ++*counter;
    return *counter;
}

static int vita_audio_power_of_two(uint32_t value)
{
    return value != 0U && (value & (value - 1U)) == 0U;
}

static int vita_audio_allocation_affecting(uint32_t index)
{
    return index == VITA_AUDIO_INDEX_source_play ||
           index == VITA_AUDIO_INDEX_gen_buffers ||
           index == VITA_AUDIO_INDEX_source_queue_buffers ||
           index == VITA_AUDIO_INDEX_gen_sources ||
           index == VITA_AUDIO_INDEX_buffer_data;
}

static void vita_audio_record_operation(CPU *__restrict c, uint32_t index,
                                        uint32_t sequence)
{
    uint32_t argument;
    uint32_t count = s_vita_audio_argument_counts[index];

    s_vita_audio_a005.last_sequence = sequence;
    s_vita_audio_a005.last_index = index;
    memset(s_vita_audio_a005.last_arguments, 0,
           sizeof s_vita_audio_a005.last_arguments);
    for (argument = 0U; argument < count; ++argument)
        s_vita_audio_a005.last_arguments[argument] =
            vita_audio_arg(c, argument);

    if (vita_audio_allocation_affecting(index)) {
        s_vita_audio_a005.allocation_sequence = sequence;
        s_vita_audio_a005.allocation_index = index;
        memcpy(s_vita_audio_a005.allocation_arguments,
               s_vita_audio_a005.last_arguments,
               sizeof s_vita_audio_a005.allocation_arguments);
    }
}

static void vita_audio_clear_observation_interval(void)
{
    s_vita_audio_a005.last_sequence = 0U;
    s_vita_audio_a005.last_index = UINT32_MAX;
    memset(s_vita_audio_a005.last_arguments, 0,
           sizeof s_vita_audio_a005.last_arguments);
    s_vita_audio_a005.allocation_sequence = 0U;
    s_vita_audio_a005.allocation_index = UINT32_MAX;
    memset(s_vita_audio_a005.allocation_arguments, 0,
           sizeof s_vita_audio_a005.allocation_arguments);
}

static void vita_audio_observe_error(CPU *__restrict c,
                                     uint32_t return_address,
                                     uint32_t observation_sequence)
{
    uint32_t count;
    uint32_t *site_counter;
    int fold_duplicate = 0;
    int emit;

    if (c->eax != UINT32_C(0x0000a005)) {
        vita_audio_clear_observation_interval();
        return;
    }

    (void)vita_audio_counter_increment(&s_vita_audio_a005.total);
    if (return_address == ISAAC_VITA_AUDIO_A005_IS_PLAYING_RETURN) {
        site_counter = &s_vita_audio_a005.is_playing_total;
        fold_duplicate = 1;
    } else if (return_address == ISAAC_VITA_AUDIO_A005_SET_VOLUME_RETURN) {
        site_counter = &s_vita_audio_a005.set_volume_total;
        fold_duplicate = 1;
    } else {
        site_counter = &s_vita_audio_a005.other_total;
    }
    count = vita_audio_counter_increment(site_counter);
    emit = vita_audio_power_of_two(count);

    if (emit) {
        isaac_vita_log(
            "KAGE VITA OPENAL A005: n=%u site=%08x seq=%u "
            "prev=%u/%u args=%08x,%08x,%08x,%08x,%08x "
            "alloc=%u/%u aargs=%08x,%08x,%08x,%08x,%08x",
            (unsigned)count, (unsigned)return_address,
            (unsigned)observation_sequence,
            (unsigned)s_vita_audio_a005.last_index,
            (unsigned)s_vita_audio_a005.last_sequence,
            (unsigned)s_vita_audio_a005.last_arguments[0],
            (unsigned)s_vita_audio_a005.last_arguments[1],
            (unsigned)s_vita_audio_a005.last_arguments[2],
            (unsigned)s_vita_audio_a005.last_arguments[3],
            (unsigned)s_vita_audio_a005.last_arguments[4],
            (unsigned)s_vita_audio_a005.allocation_index,
            (unsigned)s_vita_audio_a005.allocation_sequence,
            (unsigned)s_vita_audio_a005.allocation_arguments[0],
            (unsigned)s_vita_audio_a005.allocation_arguments[1],
            (unsigned)s_vita_audio_a005.allocation_arguments[2],
            (unsigned)s_vita_audio_a005.allocation_arguments[3],
            (unsigned)s_vita_audio_a005.allocation_arguments[4]);
        vita_audio_openal_pool_log("a005");
    } else if (fold_duplicate) {
        /* Both frozen callers only log the error and then continue.  Preserve
         * first/power-of-two evidence while removing synchronous log floods. */
        c->eax = 0U;
    }
    vita_audio_clear_observation_interval();
}

#ifdef ISAAC_VITA_AUDIO_ORACLE
void isaac_vita_audio_a005_test_reset(void)
{
    memset(&s_vita_audio_a005, 0, sizeof s_vita_audio_a005);
    vita_audio_clear_observation_interval();
#ifdef ISAAC_VITA_OPENAL_POOL_ROUTER
    s_vita_audio_pool_init_attempted = 0;
    s_vita_audio_pool_final_logged = 0;
#endif
}
#endif

static float vita_audio_float_arg(CPU *__restrict c, uint32_t index)
{
    return ldf(guest_stack_address(
        c, c->esp + 4U + index * 4U, 4U, 0U));
}

static void *vita_audio_pointer_arg(CPU *__restrict c, uint32_t index)
{
    return (void *)(uintptr_t)vita_audio_arg(c, index);
}

static void vita_audio_cdecl_return(CPU *__restrict c)
{
    (void)gpop(c);
}

static uint32_t vita_audio_native_pointer(CPU *__restrict c,
                                          const void *pointer)
{
    uintptr_t value = (uintptr_t)pointer;

    if (value > UINT32_MAX) {
        guest_fault(c, 0U,
                    "native OpenAL returned a pointer outside the x86 space");
        return 0U;
    }
    return (uint32_t)value;
}

static int vita_audio_manager_pointer(const void *pointer,
                                      uint32_t *guest_pointer)
{
    uintptr_t value = (uintptr_t)pointer;

    if (value > UINT32_MAX)
        return 0;
    *guest_pointer = (uint32_t)value;
    return 1;
}

static int vita_audio_device_has_required_sources(ALCdevice *device)
{
    ALCint mono_sources = -1;
    ALCint stereo_sources = -1;

    /* The patched 1.19.1 allocator sizes its initial voice block from the
     * device's configured source ceiling.  Query the two public components
     * before alcCreateContext so a missing app0:/alsoft.conf cannot silently
     * recreate the old 256-voice, 8.13 MiB reservation. */
    alcGetIntegerv(device, ALC_MONO_SOURCES, 1, &mono_sources);
    alcGetIntegerv(device, ALC_STEREO_SOURCES, 1, &stereo_sources);
    if (mono_sources < 0 || stereo_sources < 0)
        return 0;
    return (uint32_t)mono_sources + (uint32_t)stereo_sources ==
           ISAAC_VITA_AUDIO_DEVICE_SOURCE_COUNT;
}

static void vita_audio_manager_zero_names(uint32_t manager)
{
    uint32_t index;

    for (index = 0U; index < ISAAC_VITA_AUDIO_MANAGER_SOURCE_COUNT; ++index)
        st32(manager + ISAAC_VITA_AUDIO_MANAGER_SOURCES_OFFSET + index * 4U,
             0U);
    for (index = 0U; index < ISAAC_VITA_AUDIO_MANAGER_BUFFER_COUNT; ++index)
        st32(manager + ISAAC_VITA_AUDIO_MANAGER_BUFFERS_OFFSET + index * 4U,
             0U);
}

static void vita_audio_owner_retire_if_empty(void)
{
    uint32_t manager = s_vita_audio_owner.manager;

    if (!manager || s_vita_audio_owner.context || s_vita_audio_owner.device)
        return;
    st8(manager + ISAAC_VITA_AUDIO_MANAGER_RUNNING_OFFSET, 0U);
    st8(manager + ISAAC_VITA_AUDIO_MANAGER_SUSPENDED_OFFSET, 0U);
    st32(manager + ISAAC_VITA_AUDIO_MANAGER_CONTEXT_OFFSET, 0U);
    st32(manager + ISAAC_VITA_AUDIO_MANAGER_DEVICE_OFFSET, 0U);
    vita_audio_manager_zero_names(manager);
    memset(&s_vita_audio_owner, 0, sizeof s_vita_audio_owner);
}

static void vita_audio_owner_context_destroyed(ALCcontext *context)
{
    uint32_t manager = s_vita_audio_owner.manager;

    if (!manager || !context || context != s_vita_audio_owner.context)
        return;
    s_vita_audio_owner.context = NULL;
    s_vita_audio_owner.sources_live = 0U;
    s_vita_audio_owner.buffers_live = 0U;
    st32(manager + ISAAC_VITA_AUDIO_MANAGER_CONTEXT_OFFSET, 0U);
    vita_audio_manager_zero_names(manager);
    vita_audio_owner_retire_if_empty();
}

static void vita_audio_owner_device_closed(ALCdevice *device)
{
    uint32_t manager = s_vita_audio_owner.manager;

    if (!manager || !device || device != s_vita_audio_owner.device)
        return;
    s_vita_audio_owner.device = NULL;
    st32(manager + ISAAC_VITA_AUDIO_MANAGER_DEVICE_OFFSET, 0U);
    vita_audio_owner_retire_if_empty();
}

static void vita_audio_manager_release(uint32_t manager)
{
    ALCcontext *context;
    ALCdevice *device;

    if (!manager || manager != s_vita_audio_owner.manager)
        return;

    st8(manager + ISAAC_VITA_AUDIO_MANAGER_RUNNING_OFFSET, 0U);
    st8(manager + ISAAC_VITA_AUDIO_MANAGER_SUSPENDED_OFFSET, 0U);
    context = s_vita_audio_owner.context;
    if (context) {
        if (s_vita_audio_owner.sources_live) {
            alDeleteSources(
                (ALsizei)ISAAC_VITA_AUDIO_MANAGER_SOURCE_COUNT,
                (const ALuint *)(uintptr_t)(
                    manager + ISAAC_VITA_AUDIO_MANAGER_SOURCES_OFFSET));
            s_vita_audio_owner.sources_live = 0U;
        }
        if (s_vita_audio_owner.buffers_live) {
            alDeleteBuffers(
                (ALsizei)ISAAC_VITA_AUDIO_MANAGER_BUFFER_COUNT,
                (const ALuint *)(uintptr_t)(
                    manager + ISAAC_VITA_AUDIO_MANAGER_BUFFERS_OFFSET));
            s_vita_audio_owner.buffers_live = 0U;
        }
        (void)alcMakeContextCurrent(NULL);
        alcDestroyContext(context);
        s_vita_audio_owner.context = NULL;
        st32(manager + ISAAC_VITA_AUDIO_MANAGER_CONTEXT_OFFSET, 0U);
    } else {
        s_vita_audio_owner.sources_live = 0U;
        s_vita_audio_owner.buffers_live = 0U;
    }

    vita_audio_manager_zero_names(manager);
    device = s_vita_audio_owner.device;
    if (device && alcCloseDevice(device)) {
        s_vita_audio_owner.device = NULL;
        st32(manager + ISAAC_VITA_AUDIO_MANAGER_DEVICE_OFFSET, 0U);
    }
    vita_audio_owner_retire_if_empty();
}

int isaac_vita_audio_manager_initialize(uint32_t manager,
                                        uint32_t *al_error)
{
    ALCdevice *device;
    ALCcontext *context;
    uint32_t guest_pointer;
    ALenum error;
    static const ALfloat orientation[6] = {
        0.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f
    };

    if (al_error)
        *al_error = (uint32_t)AL_NO_ERROR;
    if (!manager)
        return ISAAC_VITA_AUDIO_MANAGER_INVALID_ADDRESS;
    if (s_vita_audio_owner.manager)
        return ISAAC_VITA_AUDIO_MANAGER_ALREADY_ACTIVE;
    vita_audio_openal_pool_ensure();

    st8(manager + ISAAC_VITA_AUDIO_MANAGER_RUNNING_OFFSET, 0U);
    st8(manager + ISAAC_VITA_AUDIO_MANAGER_SUSPENDED_OFFSET, 0U);
    st32(manager + ISAAC_VITA_AUDIO_MANAGER_CONTEXT_OFFSET, 0U);
    st32(manager + ISAAC_VITA_AUDIO_MANAGER_DEVICE_OFFSET, 0U);
    vita_audio_manager_zero_names(manager);

    device = alcOpenDevice(NULL);
    if (!device)
        return vita_audio_openal_pool_manager_failure(
            ISAAC_VITA_AUDIO_MANAGER_OPEN_DEVICE_FAILED);
    s_vita_audio_owner.manager = manager;
    s_vita_audio_owner.device = device;
    s_vita_audio_owner.context = NULL;
    s_vita_audio_owner.sources_live = 0U;
    s_vita_audio_owner.buffers_live = 0U;
    if (!vita_audio_manager_pointer(device, &guest_pointer)) {
        vita_audio_manager_release(manager);
        return vita_audio_openal_pool_manager_failure(
            ISAAC_VITA_AUDIO_MANAGER_POINTER_OUT_OF_RANGE);
    }
    st32(manager + ISAAC_VITA_AUDIO_MANAGER_DEVICE_OFFSET, guest_pointer);

    if (!vita_audio_device_has_required_sources(device)) {
        vita_audio_manager_release(manager);
        return vita_audio_openal_pool_manager_failure(
            ISAAC_VITA_AUDIO_MANAGER_SOURCE_CAPACITY_MISMATCH);
    }

    context = alcCreateContext(device, NULL);
    if (!context) {
        vita_audio_manager_release(manager);
        return vita_audio_openal_pool_manager_failure(
            ISAAC_VITA_AUDIO_MANAGER_CREATE_CONTEXT_FAILED);
    }
    s_vita_audio_owner.context = context;
    if (!vita_audio_manager_pointer(context, &guest_pointer)) {
        vita_audio_manager_release(manager);
        return vita_audio_openal_pool_manager_failure(
            ISAAC_VITA_AUDIO_MANAGER_POINTER_OUT_OF_RANGE);
    }
    st32(manager + ISAAC_VITA_AUDIO_MANAGER_CONTEXT_OFFSET, guest_pointer);

    if (!alcMakeContextCurrent(context)) {
        vita_audio_manager_release(manager);
        return vita_audio_openal_pool_manager_failure(
            ISAAC_VITA_AUDIO_MANAGER_MAKE_CURRENT_FAILED);
    }
    alcProcessContext(context);
    alGenSources(
        (ALsizei)ISAAC_VITA_AUDIO_MANAGER_SOURCE_COUNT,
        (ALuint *)(uintptr_t)(manager +
                              ISAAC_VITA_AUDIO_MANAGER_SOURCES_OFFSET));
    s_vita_audio_owner.sources_live = 1U;
    alGenBuffers(
        (ALsizei)ISAAC_VITA_AUDIO_MANAGER_BUFFER_COUNT,
        (ALuint *)(uintptr_t)(manager +
                              ISAAC_VITA_AUDIO_MANAGER_BUFFERS_OFFSET));
    s_vita_audio_owner.buffers_live = 1U;
    alListener3f(AL_POSITION, 0.0f, 0.0f, 0.0f);
    alListenerfv(AL_ORIENTATION, orientation);
    error = alGetError();
    if (al_error)
        *al_error = (uint32_t)error;
    if (error != AL_NO_ERROR) {
        vita_audio_manager_release(manager);
        return vita_audio_openal_pool_manager_failure(
            ISAAC_VITA_AUDIO_MANAGER_AL_ERROR);
    }
#if defined(ISAAC_VITA_AUDIO_REFILL_HOOKS)
    isaac_vita_audio_refill_manager_ready();
#endif
    return ISAAC_VITA_AUDIO_MANAGER_OK;
}

void isaac_vita_audio_manager_close(uint32_t manager)
{
    if (!manager)
        return;
    vita_audio_manager_release(manager);
}

int isaac_vita_audio_manager_is_active(void)
{
    return s_vita_audio_active_manager != 0U;
}

int isaac_vita_audio_shutdown_active(void)
{
    uint32_t manager = s_vita_audio_owner.manager;
    int status;

    if (!manager) {
        status = ISAAC_VITA_AUDIO_SHUTDOWN_NONE;
    } else {
        isaac_vita_audio_manager_close(manager);
        status = s_vita_audio_owner.manager
            ? ISAAC_VITA_AUDIO_SHUTDOWN_INCOMPLETE
            : ISAAC_VITA_AUDIO_SHUTDOWN_COMPLETE;
    }
    return status;
}

void isaac_vita_audio_log_final(void)
{
    vita_audio_openal_pool_log_final();
}

const char *isaac_vita_audio_manager_status_name(int status)
{
    switch (status) {
    case ISAAC_VITA_AUDIO_MANAGER_OK:
        return "ready";
    case ISAAC_VITA_AUDIO_MANAGER_INVALID_ADDRESS:
        return "invalid-manager";
    case ISAAC_VITA_AUDIO_MANAGER_OPEN_DEVICE_FAILED:
        return "open-device-failed";
    case ISAAC_VITA_AUDIO_MANAGER_POINTER_OUT_OF_RANGE:
        return "native-pointer-out-of-range";
    case ISAAC_VITA_AUDIO_MANAGER_CREATE_CONTEXT_FAILED:
        return "create-context-failed";
    case ISAAC_VITA_AUDIO_MANAGER_MAKE_CURRENT_FAILED:
        return "make-current-failed";
    case ISAAC_VITA_AUDIO_MANAGER_AL_ERROR:
        return "al-error";
    case ISAAC_VITA_AUDIO_MANAGER_SOURCE_CAPACITY_MISMATCH:
        return "source-capacity-mismatch";
    case ISAAC_VITA_AUDIO_MANAGER_ALREADY_ACTIVE:
        return "already-active";
    default:
        return "unknown";
    }
}

static void vita_audio_source_play(CPU *__restrict c)
{
#if defined(ISAAC_VITA_AUDIO_REFILL_HOOKS) && defined(ISAAC_VITA_AUDIO_REFILL_REPLAYS)
    ALuint source = (ALuint)vita_audio_arg(c, 0U);
    uint32_t return_address = ld32(guest_stack_address(c, c->esp, 4U, 0U));
    ALint state = 0;
    ALint queued = 0;

    /* The state before the play tells a restart of a source that ran dry
     * (AL_STOPPED) from a fresh stream (AL_INITIAL); only the QueueData
     * site is asked, every other caller costs nothing extra. */
    if (ISAAC_VITA_AUDIO_REFILL_IS_SITE(
            return_address, ISAAC_VITA_AUDIO_STREAM_PLAY_RETURN)) {
        alGetSourcei(source, ISAAC_VITA_AUDIO_AL_SOURCE_STATE, &state);
        alGetSourcei(source, ISAAC_VITA_AUDIO_AL_BUFFERS_QUEUED, &queued);
    }
    alSourcePlay(source);
    isaac_vita_audio_refill_note_play_state(
        (uint32_t)source, return_address, (int32_t)state, (int32_t)queued);
#else
    alSourcePlay((ALuint)vita_audio_arg(c, 0U));
#if defined(ISAAC_VITA_AUDIO_REFILL_HOOKS)
    isaac_vita_audio_refill_note_play();
#endif
#endif
    vita_audio_cdecl_return(c);
}

static void vita_audio_get_source_i(CPU *__restrict c)
{
    ALuint source = (ALuint)vita_audio_arg(c, 0U);
    ALenum parameter = (ALenum)vita_audio_arg(c, 1U);
    ALint *value = (ALint *)vita_audio_pointer_arg(c, 2U);

    alGetSourcei(source, parameter, value);
#if defined(ISAAC_VITA_AUDIO_REFILL_HOOKS)
    if (parameter == ISAAC_VITA_AUDIO_AL_BUFFERS_PROCESSED && value &&
            *value > 0 &&
            ISAAC_VITA_AUDIO_REFILL_IS_SITE(
                ld32(guest_stack_address(c, c->esp, 4U, 0U)),
                ISAAC_VITA_AUDIO_STREAM_PROCESSED_RETURN)) {
        ALint queued = 0;

        alGetSourcei(source, ISAAC_VITA_AUDIO_AL_BUFFERS_QUEUED, &queued);
        *value = (ALint)isaac_vita_audio_refill_filter_processed(
            (uint32_t)source, (int32_t)*value, (int32_t)queued);
    }
#endif
    vita_audio_cdecl_return(c);
}

static void vita_audio_source_stop(CPU *__restrict c)
{
    alSourceStop((ALuint)vita_audio_arg(c, 0U));
#if defined(ISAAC_VITA_AUDIO_REFILL_HOOKS) && defined(ISAAC_VITA_AUDIO_REFILL_REPLAYS)
    isaac_vita_audio_refill_note_stop((uint32_t)vita_audio_arg(c, 0U));
#endif
    vita_audio_cdecl_return(c);
}

static void vita_audio_gen_buffers(CPU *__restrict c)
{
    alGenBuffers((ALsizei)vita_audio_arg(c, 0U),
                 (ALuint *)vita_audio_pointer_arg(c, 1U));
    vita_audio_cdecl_return(c);
}

static void vita_audio_source_queue_buffers(CPU *__restrict c)
{
    ALuint source = (ALuint)vita_audio_arg(c, 0U);

    alSourceQueueBuffers(source,
                         (ALsizei)vita_audio_arg(c, 1U),
                         (const ALuint *)vita_audio_pointer_arg(c, 2U));
#if defined(ISAAC_VITA_AUDIO_REFILL_HOOKS)
    {
        uint32_t return_address = ld32(guest_stack_address(
            c, c->esp, 4U, 0U));
        ALint state = 0;

        if (ISAAC_VITA_AUDIO_REFILL_IS_SITE(
                return_address, ISAAC_VITA_AUDIO_STREAM_QUEUE_RETURN))
            alGetSourcei(source, ISAAC_VITA_AUDIO_AL_SOURCE_STATE, &state);
        isaac_vita_audio_refill_note_queue(
            (uint32_t)source, return_address, (int32_t)state);
    }
#endif
    vita_audio_cdecl_return(c);
}

static void vita_audio_source_pause(CPU *__restrict c)
{
    alSourcePause((ALuint)vita_audio_arg(c, 0U));
    vita_audio_cdecl_return(c);
}

static void vita_audio_source_unqueue_buffers(CPU *__restrict c)
{
    ALuint source = (ALuint)vita_audio_arg(c, 0U);

    alSourceUnqueueBuffers(source,
                           (ALsizei)vita_audio_arg(c, 1U),
                           (ALuint *)vita_audio_pointer_arg(c, 2U));
#if defined(ISAAC_VITA_AUDIO_REFILL_HOOKS)
    isaac_vita_audio_refill_note_unqueue((uint32_t)source);
#endif
    vita_audio_cdecl_return(c);
}

static void vita_audio_make_context_current(CPU *__restrict c)
{
    c->eax = (uint32_t)alcMakeContextCurrent(
        (ALCcontext *)vita_audio_pointer_arg(c, 0U));
    vita_audio_cdecl_return(c);
}

static void vita_audio_destroy_context(CPU *__restrict c)
{
    ALCcontext *context = (ALCcontext *)vita_audio_pointer_arg(c, 0U);

    alcDestroyContext(context);
    vita_audio_owner_context_destroyed(context);
    vita_audio_cdecl_return(c);
}

static void vita_audio_open_device(CPU *__restrict c)
{
    ALCdevice *device = alcOpenDevice(
        (const ALCchar *)vita_audio_pointer_arg(c, 0U));

    c->eax = vita_audio_native_pointer(c, device);
    if (!c->fault)
        vita_audio_cdecl_return(c);
}

static void vita_audio_create_context(CPU *__restrict c)
{
    ALCcontext *context = alcCreateContext(
        (ALCdevice *)vita_audio_pointer_arg(c, 0U),
        (const ALCint *)vita_audio_pointer_arg(c, 1U));

    c->eax = vita_audio_native_pointer(c, context);
    if (!c->fault)
        vita_audio_cdecl_return(c);
}

static void vita_audio_close_device(CPU *__restrict c)
{
    ALCdevice *device = (ALCdevice *)vita_audio_pointer_arg(c, 0U);
    ALCboolean closed = alcCloseDevice(device);

    c->eax = (uint32_t)closed;
    if (closed)
        vita_audio_owner_device_closed(device);
    vita_audio_cdecl_return(c);
}

static void vita_audio_listener_3f(CPU *__restrict c)
{
    alListener3f((ALenum)vita_audio_arg(c, 0U),
                 (ALfloat)vita_audio_float_arg(c, 1U),
                 (ALfloat)vita_audio_float_arg(c, 2U),
                 (ALfloat)vita_audio_float_arg(c, 3U));
    vita_audio_cdecl_return(c);
}

static void vita_audio_source_i(CPU *__restrict c)
{
    alSourcei((ALuint)vita_audio_arg(c, 0U),
              (ALenum)vita_audio_arg(c, 1U),
              (ALint)vita_audio_arg(c, 2U));
    vita_audio_cdecl_return(c);
}

static void vita_audio_process_context(CPU *__restrict c)
{
    alcProcessContext((ALCcontext *)vita_audio_pointer_arg(c, 0U));
    vita_audio_cdecl_return(c);
}

static void vita_audio_get_source_f(CPU *__restrict c)
{
    alGetSourcef((ALuint)vita_audio_arg(c, 0U),
                 (ALenum)vita_audio_arg(c, 1U),
                 (ALfloat *)vita_audio_pointer_arg(c, 2U));
    vita_audio_cdecl_return(c);
}

static void vita_audio_gen_sources(CPU *__restrict c)
{
    alGenSources((ALsizei)vita_audio_arg(c, 0U),
                 (ALuint *)vita_audio_pointer_arg(c, 1U));
    vita_audio_cdecl_return(c);
}

static void vita_audio_get_error(CPU *__restrict c)
{
    c->eax = (uint32_t)alGetError();
    vita_audio_cdecl_return(c);
}

static void vita_audio_delete_buffers(CPU *__restrict c)
{
    alDeleteBuffers((ALsizei)vita_audio_arg(c, 0U),
                    (const ALuint *)vita_audio_pointer_arg(c, 1U));
    vita_audio_cdecl_return(c);
}

static void vita_audio_listener_fv(CPU *__restrict c)
{
    alListenerfv((ALenum)vita_audio_arg(c, 0U),
                 (const ALfloat *)vita_audio_pointer_arg(c, 1U));
    vita_audio_cdecl_return(c);
}

static void vita_audio_delete_sources(CPU *__restrict c)
{
    alDeleteSources((ALsizei)vita_audio_arg(c, 0U),
                    (const ALuint *)vita_audio_pointer_arg(c, 1U));
    vita_audio_cdecl_return(c);
}

static void vita_audio_source_3f(CPU *__restrict c)
{
    alSource3f((ALuint)vita_audio_arg(c, 0U),
               (ALenum)vita_audio_arg(c, 1U),
               (ALfloat)vita_audio_float_arg(c, 2U),
               (ALfloat)vita_audio_float_arg(c, 3U),
               (ALfloat)vita_audio_float_arg(c, 4U));
    vita_audio_cdecl_return(c);
}

static void vita_audio_source_f(CPU *__restrict c)
{
    alSourcef((ALuint)vita_audio_arg(c, 0U),
              (ALenum)vita_audio_arg(c, 1U),
              (ALfloat)vita_audio_float_arg(c, 2U));
    vita_audio_cdecl_return(c);
}

static void vita_audio_buffer_data(CPU *__restrict c)
{
    ALenum format = (ALenum)vita_audio_arg(c, 1U);
    ALsizei size = (ALsizei)vita_audio_arg(c, 3U);
    ALsizei frequency = (ALsizei)vita_audio_arg(c, 4U);

    alBufferData((ALuint)vita_audio_arg(c, 0U), format,
                 (const ALvoid *)vita_audio_pointer_arg(c, 2U),
                 size, frequency);
#if defined(ISAAC_VITA_AUDIO_REFILL_HOOKS)
    isaac_vita_audio_refill_note_buffer_data(
        ld32(guest_stack_address(c, c->esp, 4U, 0U)),
        (uint32_t)format, (uint32_t)size, (uint32_t)frequency);
#endif
    vita_audio_cdecl_return(c);
}

#define ISAAC_VITA_AUDIO_TABLE_ENTRY(id, name, iat_rva) \
    { name, vita_audio_##id },
static const vita_audio_import_entry s_vita_audio_imports[] = {
    ISAAC_VITA_AUDIO_IMPORTS(ISAAC_VITA_AUDIO_TABLE_ENTRY)
};
#undef ISAAC_VITA_AUDIO_TABLE_ENTRY

_Static_assert(sizeof s_vita_audio_imports / sizeof s_vita_audio_imports[0] ==
                   ISAAC_VITA_AUDIO_IMPORT_COUNT,
               "Vita OpenAL import inventory drifted");
_Static_assert((ISAAC_VITA_AUDIO_LAST_IAT_RVA -
                ISAAC_VITA_AUDIO_FIRST_IAT_RVA) / 4U + 1U ==
                   ISAAC_VITA_AUDIO_IMPORT_COUNT,
               "frozen PE OpenAL IAT block is no longer contiguous");

const char *isaac_vita_audio_import_name(uint32_t index)
{
    return index < ISAAC_VITA_AUDIO_IMPORT_COUNT
        ? s_vita_audio_imports[index].name : NULL;
}

int isaac_vita_audio_import_indexed(CPU *__restrict c, uint32_t index,
                                    unsigned *call_count)
{
    uint32_t observation_sequence;
    uint32_t return_address = 0U;

    if (index >= ISAAC_VITA_AUDIO_IMPORT_COUNT)
        return 0;
    vita_audio_openal_pool_ensure();
    if (call_count)
        ++*call_count;

    observation_sequence =
        vita_audio_counter_increment(&s_vita_audio_a005.sequence);
    if (index == VITA_AUDIO_INDEX_get_error) {
        return_address = ld32(guest_stack_address(
            c, c->esp, 4U, 0U));
    } else {
        vita_audio_record_operation(c, index, observation_sequence);
    }
    s_vita_audio_imports[index].handler(c);
    if (index == VITA_AUDIO_INDEX_get_error)
        vita_audio_observe_error(c, return_address, observation_sequence);
    return 1;
}

static int vita_audio_dispatch(CPU *__restrict c, const char *name,
                               unsigned *call_count)
{
    size_t index;

    if (!name)
        return 0;
    for (index = 0U; index < ISAAC_VITA_AUDIO_IMPORT_COUNT; ++index) {
        if (strcmp(name, s_vita_audio_imports[index].name) == 0)
            return isaac_vita_audio_import_indexed(
                c, (uint32_t)index, call_count);
    }
    return 0;
}

int isaac_vita_audio_import(CPU *__restrict c, const char *name)
{
    return vita_audio_dispatch(c, name, NULL);
}

int isaac_vita_audio_import_counted(CPU *__restrict c, const char *name,
                                    unsigned *call_count)
{
    return vita_audio_dispatch(c, name, call_count);
}
