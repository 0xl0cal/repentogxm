/* Native executable oracle for every frozen x86 OpenAL cdecl adapter. */
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

#include "host_vita_audio.h"
#include "guest_stack_legacy_oracle_stub.h"
#include "vita_audio_test_openal.h"
#ifdef ISAAC_VITA_OPENAL_POOL_ROUTER_ORACLE
#include "guest_pe.h"
#include "host_vita_openal_pool.h"
#endif
#ifdef ISAAC_VITA_AUDIO_MANUAL_ORACLE
#include "host_vita_heap.h"
#include "host_vita_sync.h"
#include "manual_kage.h"
#endif
#ifdef ISAAC_VITA_AUDIO_REFILL_HOOKS
#include "host_vita_audio_refill.h"
#endif

#ifdef ISAAC_VITA_AUDIO_MANUAL_ORACLE
#define TEST_BYTES 0x00900000U
#else
#define TEST_BYTES 0x00010000U
#endif
#define STACK_OFFSET 0x1000U
#define DATA_OFFSET 0x2000U
#define AUX_OFFSET 0x3000U
#define MANAGER_OFFSET 0x4000U
#define FAILURE_MANAGER_OFFSET 0x5000U
#define MANAGER_BYTES 0x344U
#define MANUAL_MANAGER_OFFSET 0x007e7d40U
#define MANUAL_SOUND_POOL_OFFSET 0x00000008U
#define MANUAL_SOUND_MUTEX_A_OFFSET 0x00000018U
#define MANUAL_SOUND_MUTEX_B_OFFSET 0x00000024U
#define MANUAL_SOUND_THREAD_STATE_OFFSET 0x00000340U

enum audio_operation {
    OP_NONE = 0,
    OP_SOURCE_PLAY,
    OP_GET_SOURCE_I,
    OP_SOURCE_STOP,
    OP_GEN_BUFFERS,
    OP_SOURCE_QUEUE_BUFFERS,
    OP_SOURCE_PAUSE,
    OP_SOURCE_UNQUEUE_BUFFERS,
    OP_MAKE_CONTEXT_CURRENT,
    OP_DESTROY_CONTEXT,
    OP_OPEN_DEVICE,
    OP_CREATE_CONTEXT,
    OP_CLOSE_DEVICE,
    OP_LISTENER_3F,
    OP_SOURCE_I,
    OP_PROCESS_CONTEXT,
    OP_GET_SOURCE_F,
    OP_GEN_SOURCES,
    OP_GET_ERROR,
    OP_DELETE_BUFFERS,
    OP_LISTENER_FV,
    OP_DELETE_SOURCES,
    OP_SOURCE_3F,
    OP_SOURCE_F,
    OP_BUFFER_DATA
};

typedef struct audio_capture {
    enum audio_operation operation;
    uint32_t u[5];
    float f[3];
    uintptr_t p[3];
} audio_capture;

typedef struct import_evidence {
    const char *name;
    uint32_t iat_rva;
} import_evidence;

typedef struct manager_capture {
    unsigned open_device_calls;
    unsigned get_integer_calls;
    unsigned create_context_calls;
    unsigned make_current_calls;
    unsigned process_context_calls;
    unsigned destroy_context_calls;
    unsigned close_device_calls;
    uint32_t generated_sources;
    uint32_t generated_buffers;
    uint32_t deleted_sources;
    uint32_t deleted_buffers;
    uint32_t source_gen_calls;
    uint32_t source_delete_calls;
    uint32_t source_allocation_failures;
    uint32_t last_generated_source_count;
    uint32_t last_deleted_source_count;
    uint32_t live_sources;
    uint32_t peak_live_sources;
    float listener_position[3];
    float listener_orientation[6];
} manager_capture;

#define AUDIO_EVIDENCE(id, name, iat_rva) { name, iat_rva },
static const import_evidence s_imports[] = {
    ISAAC_VITA_AUDIO_IMPORTS(AUDIO_EVIDENCE)
};
#undef AUDIO_EVIDENCE

static audio_capture s_capture;
static manager_capture s_manager_capture;
static ALenum s_mock_al_error = (ALenum)UINT32_C(0x0000a004);
static ALCint s_mock_mono_sources = 79;
static ALCint s_mock_stereo_sources = 1;
static ALCboolean s_mock_close_device_result = 1;
static unsigned s_get_error_calls;
static unsigned s_audio_log_calls;
static char s_last_audio_log[384];
static unsigned s_failures;
static uint32_t s_base;
#ifdef ISAAC_VITA_OPENAL_POOL_ROUTER_ORACLE
static unsigned s_pool_initialize_calls;
static unsigned s_pool_snapshot_calls;
static unsigned s_pool_openal_before_initialize;
static unsigned s_pool_log_calls;
static uintptr_t s_pool_fixed_image_begin;
static uintptr_t s_pool_fixed_image_end;
static int s_pool_ready;
static char s_last_pool_log[384];
#endif
#ifdef ISAAC_VITA_AUDIO_MANUAL_ORACLE
static unsigned s_manual_malloc_calls;
static unsigned s_manual_calloc_calls;
static unsigned s_manual_free_calls;
static unsigned s_manual_sync_init_calls;
static unsigned s_manual_sync_delete_calls;
static unsigned s_manual_log_calls;
static int s_manual_heap_terminal;

unsigned char *g_guest_coverage_functions;
unsigned char *g_guest_coverage_imports;
unsigned char *g_guest_coverage_cases;
int g_guest_gl_inventory_mode;
#endif

_Static_assert(ISAAC_VITA_AUDIO_MANAGER_SOURCE_COUNT == 64U,
               "frozen guest manager source count drifted");
_Static_assert(ISAAC_VITA_AUDIO_DEVICE_SOURCE_COUNT == 80U,
               "native OpenAL device source count drifted");
_Static_assert(ISAAC_VITA_AUDIO_STREAM_SOURCE_HEADROOM == 16U,
               "stream source headroom drifted");

#define CHECK(condition, message)                                      \
    do {                                                               \
        if (!(condition)) {                                            \
            fprintf(stderr, "FAIL: %s (%s:%d)\n",                    \
                    message, __FILE__, __LINE__);                      \
            ++s_failures;                                              \
        }                                                              \
    } while (0)

static uint32_t float_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static void *map_guest_test_memory(void)
{
    static const uintptr_t candidates[] = {
#ifdef ISAAC_VITA_AUDIO_MANUAL_ORACLE
        UINT32_C(0x27000000)
#else
        UINT32_C(0x27000000), UINT32_C(0x29000000),
        UINT32_C(0x2b000000), UINT32_C(0x2d000000)
#endif
    };
    size_t index;

    for (index = 0U; index < sizeof candidates / sizeof candidates[0];
         ++index) {
        void *wanted = (void *)candidates[index];
#ifdef _WIN32
        void *mapped = VirtualAlloc(wanted, TEST_BYTES,
                                    MEM_RESERVE | MEM_COMMIT,
                                    PAGE_READWRITE);
#else
        void *mapped = mmap(wanted, TEST_BYTES, PROT_READ | PROT_WRITE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (mapped == MAP_FAILED)
            mapped = NULL;
#endif
        if (mapped == wanted) {
            s_base = (uint32_t)candidates[index];
            return mapped;
        }
#ifdef _WIN32
        if (mapped)
            VirtualFree(mapped, 0U, MEM_RELEASE);
#else
        if (mapped)
            munmap(mapped, TEST_BYTES);
#endif
    }
    return NULL;
}

static void unmap_guest_test_memory(void *memory)
{
#ifdef _WIN32
    VirtualFree(memory, 0U, MEM_RELEASE);
#else
    munmap(memory, TEST_BYTES);
#endif
}

void guest_fault(CPU *__restrict c, uint32_t address, const char *message)
{
    c->fault_addr = address;
    c->fault = message;
}

#ifdef ISAAC_VITA_AUDIO_MANUAL_ORACLE
void *isaac_vita_guest_malloc(size_t size)
{
    (void)size;
    ++s_manual_malloc_calls;
    return NULL;
}

void *isaac_vita_guest_calloc(size_t count, size_t size)
{
    (void)count;
    (void)size;
    ++s_manual_calloc_calls;
    return NULL;
}

int isaac_vita_guest_free(void *pointer)
{
    (void)pointer;
    ++s_manual_free_calls;
    return 1;
}

int isaac_vita_guest_heap_terminal(void)
{
    return s_manual_heap_terminal;
}

int isaac_vita_sync_cs_initialize_plain(CPU *__restrict c, uint32_t address)
{
    (void)c;
    (void)address;
    ++s_manual_sync_init_calls;
    return 0;
}

int isaac_vita_sync_cs_delete(CPU *__restrict c, uint32_t address)
{
    (void)c;
    (void)address;
    ++s_manual_sync_delete_calls;
    return 0;
}
#endif

void isaac_vita_log(const char *format, ...)
{
    va_list arguments;
#ifdef ISAAC_VITA_OPENAL_POOL_ROUTER_ORACLE
    static const char pool_prefix[] = "KAGE VITA OPENAL POOL:";
    char *destination = s_last_audio_log;
    size_t capacity = sizeof s_last_audio_log;
    int pool_record = strncmp(format, pool_prefix,
                              sizeof pool_prefix - 1U) == 0;

    if (pool_record) {
        destination = s_last_pool_log;
        capacity = sizeof s_last_pool_log;
    }
#endif

    va_start(arguments, format);
#ifdef ISAAC_VITA_OPENAL_POOL_ROUTER_ORACLE
    (void)vsnprintf(destination, capacity, format, arguments);
#else
    (void)vsnprintf(s_last_audio_log, sizeof s_last_audio_log,
                    format, arguments);
#endif
    va_end(arguments);
#ifdef ISAAC_VITA_OPENAL_POOL_ROUTER_ORACLE
    if (pool_record)
        ++s_pool_log_calls;
    else
        ++s_audio_log_calls;
#else
    ++s_audio_log_calls;
#endif
#ifdef ISAAC_VITA_AUDIO_MANUAL_ORACLE
    ++s_manual_log_calls;
#endif
}

#ifdef ISAAC_VITA_OPENAL_POOL_ROUTER_ORACLE
int isaac_vita_openal_pool_initialize(uintptr_t fixed_image_begin,
                                      uintptr_t fixed_image_end)
{
    ++s_pool_initialize_calls;
    s_pool_fixed_image_begin = fixed_image_begin;
    s_pool_fixed_image_end = fixed_image_end;
    s_pool_ready = 1;
    return 1;
}

int isaac_vita_openal_pool_snapshot_get(
    isaac_vita_openal_pool_snapshot *snapshot_out)
{
    ++s_pool_snapshot_calls;
    if (!snapshot_out)
        return 0;
    memset(snapshot_out, 0, sizeof *snapshot_out);
    snapshot_out->state = s_pool_ready ? ISAAC_VITA_OPENAL_POOL_READY :
        ISAAC_VITA_OPENAL_POOL_UNINITIALIZED;
    snapshot_out->uid = ISAAC_VITA_OPENAL_POOL_INVALID_UID;
    snapshot_out->mutex_uid = ISAAC_VITA_OPENAL_POOL_INVALID_UID;
    snapshot_out->backing_bytes = s_pool_ready ?
        ISAAC_VITA_OPENAL_POOL_BYTES : 0U;
    snapshot_out->has_mspace = s_pool_ready;
    return 1;
}

static void note_openal_call(void)
{
    if (!s_pool_ready)
        ++s_pool_openal_before_initialize;
}
#else
static void note_openal_call(void) { }
#endif

static CPU invoke_at(const char *name, const uint32_t *arguments,
                     uint32_t argument_count, unsigned *calls,
                     uint32_t return_address)
{
    CPU cpu;
    uint32_t stack = s_base + STACK_OFFSET;
    uint32_t index;

    memset(&cpu, 0, sizeof cpu);
    memset(&s_capture, 0, sizeof s_capture);
    st32(stack, return_address);
    for (index = 0U; index < argument_count; ++index)
        st32(stack + 4U + index * 4U, arguments[index]);
    cpu.esp = stack;

    CHECK(isaac_vita_audio_import_counted(&cpu, name, calls) == 1,
          "known OpenAL import was not handled");
    CHECK(cpu.esp == stack + 4U,
          "OpenAL cdecl adapter did not pop exactly the return address");
    CHECK(cpu.fault == NULL, "OpenAL adapter raised an unexpected fault");
    return cpu;
}

static CPU invoke(const char *name, const uint32_t *arguments,
                  uint32_t argument_count, unsigned *calls)
{
    return invoke_at(name, arguments, argument_count, calls,
                     UINT32_C(0x12345678));
}

void alSourcePlay(ALuint source)
{
    s_capture.operation = OP_SOURCE_PLAY;
    s_capture.u[0] = source;
}

#ifdef ISAAC_VITA_AUDIO_REFILL_HOOKS
/* Refill-hook oracle: a deterministic owner-thread clock and OpenAL queue
 * state so the adapters' return-site filter can be driven end to end. */
static uint64_t s_refill_now_us;
static int32_t s_mock_processed;
static int32_t s_mock_queued;
#ifdef ISAAC_VITA_AUDIO_REFILL_REPLAYS
static int32_t s_mock_state;
#endif

uint64_t isaac_vita_get_process_time(void)
{
    return s_refill_now_us;
}
#endif

void alGetSourcei(ALuint source, ALenum parameter, ALint *value)
{
    s_capture.operation = OP_GET_SOURCE_I;
    s_capture.u[0] = source;
    s_capture.u[1] = (uint32_t)parameter;
    s_capture.p[0] = (uintptr_t)value;
    *value = -73;
#ifdef ISAAC_VITA_AUDIO_REFILL_HOOKS
    if (parameter == ISAAC_VITA_AUDIO_AL_BUFFERS_PROCESSED)
        *value = s_mock_processed;
    else if (parameter == ISAAC_VITA_AUDIO_AL_BUFFERS_QUEUED)
        *value = s_mock_queued;
#ifdef ISAAC_VITA_AUDIO_REFILL_REPLAYS
    /* the replay test sets a real AL state; 0 keeps the generic -73 probe */
    else if (parameter == ISAAC_VITA_AUDIO_AL_SOURCE_STATE && s_mock_state)
        *value = s_mock_state;
#endif
#endif
}

void alSourceStop(ALuint source)
{
    s_capture.operation = OP_SOURCE_STOP;
    s_capture.u[0] = source;
}

void alGenBuffers(ALsizei count, ALuint *buffers)
{
    ALsizei index;

    s_capture.operation = OP_GEN_BUFFERS;
    s_capture.u[0] = (uint32_t)count;
    s_capture.p[0] = (uintptr_t)buffers;
    s_manager_capture.generated_buffers += (uint32_t)count;
    for (index = 0; index < count; ++index)
        buffers[index] = UINT32_C(0x7101) + (uint32_t)index;
}

void alSourceQueueBuffers(ALuint source, ALsizei count,
                          const ALuint *buffers)
{
    s_capture.operation = OP_SOURCE_QUEUE_BUFFERS;
    s_capture.u[0] = source;
    s_capture.u[1] = (uint32_t)count;
    s_capture.p[0] = (uintptr_t)buffers;
}

void alSourcePause(ALuint source)
{
    s_capture.operation = OP_SOURCE_PAUSE;
    s_capture.u[0] = source;
}

void alSourceUnqueueBuffers(ALuint source, ALsizei count, ALuint *buffers)
{
    s_capture.operation = OP_SOURCE_UNQUEUE_BUFFERS;
    s_capture.u[0] = source;
    s_capture.u[1] = (uint32_t)count;
    s_capture.p[0] = (uintptr_t)buffers;
    buffers[0] = UINT32_C(0x7201);
}

ALCboolean alcMakeContextCurrent(ALCcontext *context)
{
    s_capture.operation = OP_MAKE_CONTEXT_CURRENT;
    s_capture.p[0] = (uintptr_t)context;
    ++s_manager_capture.make_current_calls;
    return 1;
}

void alcDestroyContext(ALCcontext *context)
{
    s_capture.operation = OP_DESTROY_CONTEXT;
    s_capture.p[0] = (uintptr_t)context;
    ++s_manager_capture.destroy_context_calls;
    if ((uintptr_t)context == UINT32_C(0x30202020))
        s_manager_capture.live_sources = 0U;
}

ALCdevice *alcOpenDevice(const ALCchar *name)
{
    note_openal_call();
    s_capture.operation = OP_OPEN_DEVICE;
    s_capture.p[0] = (uintptr_t)name;
    ++s_manager_capture.open_device_calls;
    return (ALCdevice *)(uintptr_t)UINT32_C(0x30101010);
}

void alcGetIntegerv(ALCdevice *device, ALCenum parameter, ALCsizei size,
                    ALCint *values)
{
    (void)device;
    ++s_manager_capture.get_integer_calls;
    if (!values || size < 1)
        return;
    if (parameter == ALC_MONO_SOURCES)
        values[0] = s_mock_mono_sources;
    else if (parameter == ALC_STEREO_SOURCES)
        values[0] = s_mock_stereo_sources;
}

ALCcontext *alcCreateContext(ALCdevice *device, const ALCint *attributes)
{
    s_capture.operation = OP_CREATE_CONTEXT;
    s_capture.p[0] = (uintptr_t)device;
    s_capture.p[1] = (uintptr_t)attributes;
    ++s_manager_capture.create_context_calls;
    return (ALCcontext *)(uintptr_t)UINT32_C(0x30202020);
}

ALCboolean alcCloseDevice(ALCdevice *device)
{
    s_capture.operation = OP_CLOSE_DEVICE;
    s_capture.p[0] = (uintptr_t)device;
    ++s_manager_capture.close_device_calls;
    return s_mock_close_device_result;
}

void alListener3f(ALenum parameter, ALfloat x, ALfloat y, ALfloat z)
{
    s_capture.operation = OP_LISTENER_3F;
    s_capture.u[0] = (uint32_t)parameter;
    s_capture.f[0] = x;
    s_capture.f[1] = y;
    s_capture.f[2] = z;
    s_manager_capture.listener_position[0] = x;
    s_manager_capture.listener_position[1] = y;
    s_manager_capture.listener_position[2] = z;
}

void alSourcei(ALuint source, ALenum parameter, ALint value)
{
    s_capture.operation = OP_SOURCE_I;
    s_capture.u[0] = source;
    s_capture.u[1] = (uint32_t)parameter;
    s_capture.u[2] = (uint32_t)value;
}

void alcProcessContext(ALCcontext *context)
{
    s_capture.operation = OP_PROCESS_CONTEXT;
    s_capture.p[0] = (uintptr_t)context;
    ++s_manager_capture.process_context_calls;
}

void alGetSourcef(ALuint source, ALenum parameter, ALfloat *value)
{
    s_capture.operation = OP_GET_SOURCE_F;
    s_capture.u[0] = source;
    s_capture.u[1] = (uint32_t)parameter;
    s_capture.p[0] = (uintptr_t)value;
    *value = 0.75f;
}

void alGenSources(ALsizei count, ALuint *sources)
{
    ALsizei index;
    uint32_t requested;
    uint32_t first_name;

    s_capture.operation = OP_GEN_SOURCES;
    s_capture.u[0] = (uint32_t)count;
    s_capture.p[0] = (uintptr_t)sources;
    ++s_manager_capture.source_gen_calls;
    s_manager_capture.last_generated_source_count = (uint32_t)count;
    if (count < 0 || (uint32_t)count >
                         ISAAC_VITA_AUDIO_DEVICE_SOURCE_COUNT -
                             s_manager_capture.live_sources) {
        s_mock_al_error = (ALenum)UINT32_C(0x0000a005);
        ++s_manager_capture.source_allocation_failures;
        if (count > 0 && sources)
            sources[0] = 0U;
        return;
    }

    requested = (uint32_t)count;
    first_name = s_manager_capture.generated_sources;
    for (index = 0; index < count; ++index)
        sources[index] = UINT32_C(0x7301) + first_name + (uint32_t)index;
    s_manager_capture.generated_sources += requested;
    s_manager_capture.live_sources += requested;
    if (s_manager_capture.peak_live_sources <
        s_manager_capture.live_sources)
        s_manager_capture.peak_live_sources =
            s_manager_capture.live_sources;
}

ALenum alGetError(void)
{
    ALenum error = s_mock_al_error;

    note_openal_call();
    s_capture.operation = OP_GET_ERROR;
    ++s_get_error_calls;
    s_mock_al_error = AL_NO_ERROR;
    return error;
}

void alDeleteBuffers(ALsizei count, const ALuint *buffers)
{
    s_capture.operation = OP_DELETE_BUFFERS;
    s_capture.u[0] = (uint32_t)count;
    s_capture.p[0] = (uintptr_t)buffers;
    s_manager_capture.deleted_buffers += (uint32_t)count;
}

void alListenerfv(ALenum parameter, const ALfloat *values)
{
    unsigned index;

    s_capture.operation = OP_LISTENER_FV;
    s_capture.u[0] = (uint32_t)parameter;
    s_capture.p[0] = (uintptr_t)values;
    for (index = 0U; index < 6U; ++index)
        s_manager_capture.listener_orientation[index] = values[index];
}

void alDeleteSources(ALsizei count, const ALuint *sources)
{
    s_capture.operation = OP_DELETE_SOURCES;
    s_capture.u[0] = (uint32_t)count;
    s_capture.p[0] = (uintptr_t)sources;
    ++s_manager_capture.source_delete_calls;
    s_manager_capture.last_deleted_source_count = (uint32_t)count;
    s_manager_capture.deleted_sources += (uint32_t)count;
    CHECK(count >= 0 && (uint32_t)count <= s_manager_capture.live_sources,
          "mock source deletion exceeded the live source count");
    if (count >= 0 && (uint32_t)count <= s_manager_capture.live_sources)
        s_manager_capture.live_sources -= (uint32_t)count;
}

void alSource3f(ALuint source, ALenum parameter,
                ALfloat x, ALfloat y, ALfloat z)
{
    s_capture.operation = OP_SOURCE_3F;
    s_capture.u[0] = source;
    s_capture.u[1] = (uint32_t)parameter;
    s_capture.f[0] = x;
    s_capture.f[1] = y;
    s_capture.f[2] = z;
}

void alSourcef(ALuint source, ALenum parameter, ALfloat value)
{
    s_capture.operation = OP_SOURCE_F;
    s_capture.u[0] = source;
    s_capture.u[1] = (uint32_t)parameter;
    s_capture.f[0] = value;
}

void alBufferData(ALuint buffer, ALenum format, const ALvoid *data,
                  ALsizei size, ALsizei frequency)
{
    s_capture.operation = OP_BUFFER_DATA;
    s_capture.u[0] = buffer;
    s_capture.u[1] = (uint32_t)format;
    s_capture.u[2] = (uint32_t)size;
    s_capture.u[3] = (uint32_t)frequency;
    s_capture.p[0] = (uintptr_t)data;
}

static void test_inventory(void)
{
    size_t index;

    CHECK(sizeof s_imports / sizeof s_imports[0] ==
              ISAAC_VITA_AUDIO_IMPORT_COUNT,
          "OpenAL evidence table count drifted");
    for (index = 0U; index < ISAAC_VITA_AUDIO_IMPORT_COUNT; ++index) {
        CHECK(s_imports[index].iat_rva ==
                  ISAAC_VITA_AUDIO_FIRST_IAT_RVA + (uint32_t)index * 4U,
              "OpenAL evidence table is not in exact IAT order");
        CHECK(strncmp(s_imports[index].name, "OpenAL32.dll!", 13U) == 0,
              "OpenAL import lost its exact DLL prefix");
    }
}

static void test_dispatch(void)
{
    CPU cpu;
    unsigned calls = 0U;
    uint32_t stack = s_base + STACK_OFFSET;

    memset(&cpu, 0, sizeof cpu);
    cpu.esp = stack;
    st32(stack, UINT32_C(0xabcdef01));
    CHECK(!isaac_vita_audio_import_counted(
              &cpu, "OpenAL32.dll!unknown", &calls),
          "unknown OpenAL import was claimed");
    CHECK(!isaac_vita_audio_import_counted(&cpu, NULL, &calls),
          "null OpenAL import was claimed");
    CHECK(cpu.esp == stack && calls == 0U,
          "rejected OpenAL import mutated caller state");
}

static void test_all_adapters(void)
{
    unsigned calls = 0U;
    uint32_t args[5];
    uint32_t data = s_base + DATA_OFFSET;
    uint32_t aux = s_base + AUX_OFFSET;
    CPU cpu;

#define INVOKE(name, count) invoke((name), args, (count), &calls)
#define EXPECT(op) CHECK(s_capture.operation == (op), \
                         "wrong native OpenAL function was called")

    args[0] = UINT32_C(0x1101);
    (void)INVOKE(ISAAC_VITA_AUDIO_SOURCE_PLAY_NAME, 1U);
    EXPECT(OP_SOURCE_PLAY);
    CHECK(s_capture.u[0] == args[0], "alSourcePlay source drifted");

    args[0] = UINT32_C(0x1102); args[1] = UINT32_C(0x1010); args[2] = data;
    (void)INVOKE(ISAAC_VITA_AUDIO_GET_SOURCE_I_NAME, 3U);
    EXPECT(OP_GET_SOURCE_I);
    CHECK(s_capture.u[0] == args[0] && s_capture.u[1] == args[1] &&
              s_capture.p[0] == data && (int32_t)ld32(data) == -73,
          "alGetSourcei ABI drifted");

    args[0] = UINT32_C(0x1103);
    (void)INVOKE(ISAAC_VITA_AUDIO_SOURCE_STOP_NAME, 1U);
    EXPECT(OP_SOURCE_STOP);

    args[0] = 2U; args[1] = data;
    (void)INVOKE(ISAAC_VITA_AUDIO_GEN_BUFFERS_NAME, 2U);
    EXPECT(OP_GEN_BUFFERS);
    CHECK(ld32(data) == UINT32_C(0x7101) &&
              ld32(data + 4U) == UINT32_C(0x7102),
          "alGenBuffers output pointer drifted");

    args[0] = UINT32_C(0x1104); args[1] = 2U; args[2] = data;
    (void)INVOKE(ISAAC_VITA_AUDIO_SOURCE_QUEUE_BUFFERS_NAME, 3U);
    EXPECT(OP_SOURCE_QUEUE_BUFFERS);
    CHECK(s_capture.u[0] == args[0] && s_capture.u[1] == 2U &&
              s_capture.p[0] == data,
          "alSourceQueueBuffers ABI drifted");

    args[0] = UINT32_C(0x1105);
    (void)INVOKE(ISAAC_VITA_AUDIO_SOURCE_PAUSE_NAME, 1U);
    EXPECT(OP_SOURCE_PAUSE);

    args[0] = UINT32_C(0x1106); args[1] = 1U; args[2] = data;
    (void)INVOKE(ISAAC_VITA_AUDIO_SOURCE_UNQUEUE_BUFFERS_NAME, 3U);
    EXPECT(OP_SOURCE_UNQUEUE_BUFFERS);
    CHECK(ld32(data) == UINT32_C(0x7201),
          "alSourceUnqueueBuffers output pointer drifted");

    args[0] = UINT32_C(0x30202020);
    cpu = INVOKE(ISAAC_VITA_AUDIO_MAKE_CONTEXT_CURRENT_NAME, 1U);
    EXPECT(OP_MAKE_CONTEXT_CURRENT);
    CHECK(cpu.eax == 1U && s_capture.p[0] == args[0],
          "alcMakeContextCurrent return or pointer drifted");

    (void)INVOKE(ISAAC_VITA_AUDIO_DESTROY_CONTEXT_NAME, 1U);
    EXPECT(OP_DESTROY_CONTEXT);

    memcpy((void *)(uintptr_t)data, "device", 7U);
    args[0] = data;
    cpu = INVOKE(ISAAC_VITA_AUDIO_OPEN_DEVICE_NAME, 1U);
    EXPECT(OP_OPEN_DEVICE);
    CHECK(cpu.eax == UINT32_C(0x30101010) && s_capture.p[0] == data,
          "alcOpenDevice return or pointer drifted");

    args[0] = UINT32_C(0x30101010); args[1] = aux;
    cpu = INVOKE(ISAAC_VITA_AUDIO_CREATE_CONTEXT_NAME, 2U);
    EXPECT(OP_CREATE_CONTEXT);
    CHECK(cpu.eax == UINT32_C(0x30202020) &&
              s_capture.p[0] == args[0] && s_capture.p[1] == aux,
          "alcCreateContext return or pointers drifted");

    args[0] = UINT32_C(0x30101010);
    cpu = INVOKE(ISAAC_VITA_AUDIO_CLOSE_DEVICE_NAME, 1U);
    EXPECT(OP_CLOSE_DEVICE);
    CHECK(cpu.eax == 1U, "alcCloseDevice return drifted");

    args[0] = UINT32_C(0x1004); args[1] = float_bits(1.25f);
    args[2] = float_bits(-2.5f); args[3] = float_bits(3.75f);
    (void)INVOKE(ISAAC_VITA_AUDIO_LISTENER_3F_NAME, 4U);
    EXPECT(OP_LISTENER_3F);
    CHECK(s_capture.u[0] == args[0] && s_capture.f[0] == 1.25f &&
              s_capture.f[1] == -2.5f && s_capture.f[2] == 3.75f,
          "alListener3f float ABI drifted");

    args[0] = UINT32_C(0x1107); args[1] = UINT32_C(0x1009);
    args[2] = UINT32_C(0xfffffff9);
    (void)INVOKE(ISAAC_VITA_AUDIO_SOURCE_I_NAME, 3U);
    EXPECT(OP_SOURCE_I);
    CHECK(s_capture.u[2] == args[2], "alSourcei signed value drifted");

    args[0] = UINT32_C(0x30202020);
    (void)INVOKE(ISAAC_VITA_AUDIO_PROCESS_CONTEXT_NAME, 1U);
    EXPECT(OP_PROCESS_CONTEXT);

    args[0] = UINT32_C(0x1108); args[1] = UINT32_C(0x100a);
    args[2] = data;
    (void)INVOKE(ISAAC_VITA_AUDIO_GET_SOURCE_F_NAME, 3U);
    EXPECT(OP_GET_SOURCE_F);
    CHECK(ldf(data) == 0.75f, "alGetSourcef output pointer drifted");

    args[0] = 2U; args[1] = data;
    (void)INVOKE(ISAAC_VITA_AUDIO_GEN_SOURCES_NAME, 2U);
    EXPECT(OP_GEN_SOURCES);
    CHECK(ld32(data) == UINT32_C(0x7301) &&
              ld32(data + 4U) == UINT32_C(0x7302),
          "alGenSources output pointer drifted");

    cpu = INVOKE(ISAAC_VITA_AUDIO_GET_ERROR_NAME, 0U);
    EXPECT(OP_GET_ERROR);
    CHECK(cpu.eax == UINT32_C(0x0000a004), "alGetError return drifted");

    args[0] = 2U; args[1] = data;
    (void)INVOKE(ISAAC_VITA_AUDIO_DELETE_BUFFERS_NAME, 2U);
    EXPECT(OP_DELETE_BUFFERS);
    CHECK(s_capture.p[0] == data, "alDeleteBuffers pointer drifted");

    args[0] = UINT32_C(0x100f); args[1] = aux;
    (void)INVOKE(ISAAC_VITA_AUDIO_LISTENER_FV_NAME, 2U);
    EXPECT(OP_LISTENER_FV);

    args[0] = 2U; args[1] = data;
    (void)INVOKE(ISAAC_VITA_AUDIO_DELETE_SOURCES_NAME, 2U);
    EXPECT(OP_DELETE_SOURCES);

    args[0] = UINT32_C(0x1109); args[1] = UINT32_C(0x1004);
    args[2] = float_bits(-1.0f); args[3] = float_bits(0.5f);
    args[4] = float_bits(4.0f);
    (void)INVOKE(ISAAC_VITA_AUDIO_SOURCE_3F_NAME, 5U);
    EXPECT(OP_SOURCE_3F);
    CHECK(s_capture.f[0] == -1.0f && s_capture.f[1] == 0.5f &&
              s_capture.f[2] == 4.0f,
          "alSource3f float ABI drifted");

    args[0] = UINT32_C(0x1110); args[1] = UINT32_C(0x100a);
    args[2] = float_bits(0.625f);
    (void)INVOKE(ISAAC_VITA_AUDIO_SOURCE_F_NAME, 3U);
    EXPECT(OP_SOURCE_F);
    CHECK(s_capture.f[0] == 0.625f, "alSourcef float ABI drifted");

    args[0] = UINT32_C(0x7401); args[1] = UINT32_C(0x1101);
    args[2] = data; args[3] = UINT32_C(0x2000); args[4] = 22050U;
    (void)INVOKE(ISAAC_VITA_AUDIO_BUFFER_DATA_NAME, 5U);
    EXPECT(OP_BUFFER_DATA);
    CHECK(s_capture.u[0] == args[0] && s_capture.u[1] == args[1] &&
              s_capture.p[0] == data && s_capture.u[2] == args[3] &&
              s_capture.u[3] == args[4],
          "alBufferData PCM ABI drifted");

    CHECK(calls == ISAAC_VITA_AUDIO_IMPORT_COUNT,
          "not every OpenAL adapter incremented the shared count once");
#undef EXPECT
#undef INVOKE
}

static void test_a005_cold_start(void)
{
    uint32_t arguments[1] = { 0U };
    CPU cpu;
    unsigned calls = 0U;

    /* This deliberately runs before any test reset or imported operation.
     * Production zero-initialization must not turn "no predecessor" into the
     * valid source_play index zero. */
    s_audio_log_calls = 0U;
    s_get_error_calls = 0U;
    s_last_audio_log[0] = '\0';
    s_mock_al_error = (ALenum)UINT32_C(0x0000a005);
    cpu = invoke_at(ISAAC_VITA_AUDIO_GET_ERROR_NAME, arguments, 0U, &calls,
                    ISAAC_VITA_AUDIO_A005_IS_PLAYING_RETURN);
    CHECK(cpu.eax == UINT32_C(0x0000a005) && calls == 1U &&
              s_get_error_calls == 1U && s_audio_log_calls == 1U,
          "cold-start A005 was not preserved and logged once");
    CHECK(strstr(s_last_audio_log, "prev=4294967295/0") != NULL &&
              strstr(s_last_audio_log, "alloc=4294967295/0") != NULL,
          "cold-start A005 invented a valid predecessor");
#ifdef ISAAC_VITA_OPENAL_POOL_ROUTER_ORACLE
    CHECK(s_pool_initialize_calls == 1U &&
              s_pool_openal_before_initialize == 0U,
          "indexed OpenAL dispatch did not initialize the pool first");
    CHECK(s_pool_fixed_image_begin == GUEST_PE_VITA_TARGET_BASE &&
              s_pool_fixed_image_end == GUEST_PE_VITA_TARGET_BASE +
                  GUEST_PE_EXPECTED_IMAGE_SIZE,
          "indexed pool initialization lost the frozen image range");
    CHECK(s_pool_snapshot_calls == 2U && s_pool_log_calls == 2U &&
              strstr(s_last_pool_log, "phase=a005 state=2") != NULL &&
              strlen(s_last_pool_log) < sizeof s_last_pool_log - 1U,
          "indexed pool init/A005 telemetry drifted or was truncated");
#endif
    isaac_vita_audio_a005_test_reset();
    s_mock_al_error = (ALenum)UINT32_C(0x0000a004);
}

#ifdef ISAAC_VITA_OPENAL_POOL_ROUTER_ORACLE
static void test_pool_manager_first_openal_order(void)
{
    uint32_t manager = s_base + FAILURE_MANAGER_OFFSET;
    uint32_t al_error = UINT32_MAX;
    int status;

    isaac_vita_audio_a005_test_reset();
    memset((void *)(uintptr_t)manager, 0, MANAGER_BYTES);
    memset(&s_manager_capture, 0, sizeof s_manager_capture);
    s_pool_initialize_calls = 0U;
    s_pool_snapshot_calls = 0U;
    s_pool_openal_before_initialize = 0U;
    s_pool_log_calls = 0U;
    s_pool_fixed_image_begin = 0U;
    s_pool_fixed_image_end = 0U;
    s_pool_ready = 0;
    s_last_pool_log[0] = '\0';
    s_mock_mono_sources = 79;
    s_mock_stereo_sources = 1;
    s_mock_close_device_result = 1;
    s_mock_al_error = AL_NO_ERROR;

    status = isaac_vita_audio_manager_initialize(manager, &al_error);
    CHECK(status == ISAAC_VITA_AUDIO_MANAGER_OK && al_error == AL_NO_ERROR,
          "manager pool-order fixture did not initialize audio");
    CHECK(s_pool_initialize_calls == 1U &&
              s_pool_openal_before_initialize == 0U &&
              s_manager_capture.open_device_calls == 1U,
          "manager called alcOpenDevice before pool initialization");
    CHECK(s_pool_fixed_image_begin == GUEST_PE_VITA_TARGET_BASE &&
              s_pool_fixed_image_end == GUEST_PE_VITA_TARGET_BASE +
                  GUEST_PE_EXPECTED_IMAGE_SIZE,
          "manager pool initialization lost the frozen image range");
    CHECK(s_pool_snapshot_calls == 1U && s_pool_log_calls == 1U &&
              strstr(s_last_pool_log, "phase=init state=2") != NULL &&
              strlen(s_last_pool_log) < sizeof s_last_pool_log - 1U,
          "manager pool init telemetry drifted or was truncated");

    isaac_vita_audio_manager_close(manager);
    CHECK(isaac_vita_audio_manager_is_active() == 0,
          "manager pool-order fixture leaked its owner");
    CHECK(isaac_vita_audio_shutdown_active() ==
              ISAAC_VITA_AUDIO_SHUTDOWN_NONE && s_pool_log_calls == 1U,
          "no-owner shutdown consumed process-final pool telemetry");
    isaac_vita_audio_log_final();
    CHECK(s_pool_log_calls == 2U &&
              strstr(s_last_pool_log, "phase=final state=2") != NULL,
          "explicit process-final pool telemetry was not emitted");
    isaac_vita_audio_log_final();
    CHECK(s_pool_log_calls == 2U,
          "process-final pool telemetry was not one-shot");
    memset(&s_manager_capture, 0, sizeof s_manager_capture);
    s_mock_al_error = (ALenum)UINT32_C(0x0000a004);
}
#endif

static void test_a005_attribution_and_folding(void)
{
    uint32_t arguments[5] = {
        UINT32_C(0x7401), UINT32_C(0x1101), s_base + DATA_OFFSET,
        UINT32_C(0x2000), UINT32_C(22050)
    };
    CPU cpu;
    unsigned calls = 0U;
    unsigned index;

    isaac_vita_audio_a005_test_reset();
    s_audio_log_calls = 0U;
    s_get_error_calls = 0U;
    s_last_audio_log[0] = '\0';

    /* A005 is observed after alSourcef, but the interval must retain the
     * allocation-affecting alBufferData which can actually create it. */
    (void)invoke(ISAAC_VITA_AUDIO_BUFFER_DATA_NAME, arguments, 5U, &calls);
    arguments[0] = UINT32_C(0x1110);
    arguments[1] = UINT32_C(0x100a);
    arguments[2] = float_bits(0.625f);
    (void)invoke(ISAAC_VITA_AUDIO_SOURCE_F_NAME, arguments, 3U, &calls);
    s_mock_al_error = (ALenum)UINT32_C(0x0000a005);
    cpu = invoke_at(ISAAC_VITA_AUDIO_GET_ERROR_NAME, arguments, 0U, &calls,
                    ISAAC_VITA_AUDIO_A005_SET_VOLUME_RETURN);
    CHECK(cpu.eax == UINT32_C(0x0000a005) && s_audio_log_calls == 1U,
          "first SetVolume A005 was not preserved and logged");
    CHECK(strstr(s_last_audio_log,
                 "n=1 site=005be8ff seq=3 prev=22/2") != NULL &&
              strstr(s_last_audio_log,
                     "alloc=23/1 aargs=00007401,00001101") != NULL &&
              strlen(s_last_audio_log) < sizeof s_last_audio_log - 1U,
          "A005 attribution lost the observer or PCM allocation context");

    /* Counts 1,2,4 remain guest-visible.  Other duplicates at this exact
     * logging-only site are folded without another native alGetError call. */
    for (index = 2U; index <= 4U; ++index) {
        (void)invoke(ISAAC_VITA_AUDIO_SOURCE_F_NAME, arguments, 3U, &calls);
        s_mock_al_error = (ALenum)UINT32_C(0x0000a005);
        cpu = invoke_at(ISAAC_VITA_AUDIO_GET_ERROR_NAME, arguments, 0U,
                        &calls, ISAAC_VITA_AUDIO_A005_SET_VOLUME_RETURN);
        CHECK(cpu.eax == (index == 3U ? AL_NO_ERROR :
                          UINT32_C(0x0000a005)),
              "SetVolume A005 power-of-two folding drifted");
    }
    CHECK(s_audio_log_calls == 3U,
          "SetVolume A005 logger was not first/power-of-two bounded");

    arguments[0] = UINT32_C(0x1102);
    arguments[1] = UINT32_C(0x1010);
    arguments[2] = s_base + DATA_OFFSET;
    for (index = 1U; index <= 3U; ++index) {
        (void)invoke(ISAAC_VITA_AUDIO_GET_SOURCE_I_NAME, arguments, 3U,
                     &calls);
        s_mock_al_error = (ALenum)UINT32_C(0x0000a005);
        cpu = invoke_at(ISAAC_VITA_AUDIO_GET_ERROR_NAME, arguments, 0U,
                        &calls, ISAAC_VITA_AUDIO_A005_IS_PLAYING_RETURN);
        CHECK(cpu.eax == (index == 3U ? AL_NO_ERROR :
                          UINT32_C(0x0000a005)),
              "IsPlaying A005 power-of-two folding drifted");
    }
    CHECK(s_audio_log_calls == 5U,
          "IsPlaying A005 logger was not independently bounded");

    /* Unknown observers are diagnostics only: never rewrite guest AL state. */
    for (index = 1U; index <= 3U; ++index) {
        s_mock_al_error = (ALenum)UINT32_C(0x0000a005);
        cpu = invoke_at(ISAAC_VITA_AUDIO_GET_ERROR_NAME, arguments, 0U,
                        &calls, UINT32_C(0x00500001));
        CHECK(cpu.eax == UINT32_C(0x0000a005),
              "unproved A005 observer was incorrectly folded");
    }
    CHECK(s_audio_log_calls == 7U,
          "unproved A005 observer logging was not first/power-of-two bounded");

    s_mock_al_error = (ALenum)UINT32_C(0x0000a004);
    cpu = invoke_at(ISAAC_VITA_AUDIO_GET_ERROR_NAME, arguments, 0U, &calls,
                    ISAAC_VITA_AUDIO_A005_SET_VOLUME_RETURN);
    CHECK(cpu.eax == UINT32_C(0x0000a004) && s_audio_log_calls == 7U,
          "non-A005 OpenAL error was changed or logged as A005");
    CHECK(s_get_error_calls == 11U,
          "A005 attribution introduced or skipped a native alGetError call");
    CHECK(calls == 19U,
          "A005 attribution adapter call census drifted");

    isaac_vita_audio_a005_test_reset();
}

static void test_a005_source_play_attribution(void)
{
    uint32_t arguments[3] = {
        UINT32_C(0x7401), UINT32_C(0x1010),
        s_base + DATA_OFFSET
    };
    CPU cpu;
    unsigned calls = 0U;

    isaac_vita_audio_a005_test_reset();
    s_audio_log_calls = 0U;
    s_get_error_calls = 0U;
    s_last_audio_log[0] = '\0';

    /* OpenAL Soft 1.19.1 alSourcePlayv can set AL_OUT_OF_MEMORY while
     * allocating a voice.  The later IsPlaying query is only the observer,
     * so retain source_play as the allocation-affecting predecessor. */
    (void)invoke(ISAAC_VITA_AUDIO_SOURCE_PLAY_NAME, arguments, 1U, &calls);
    (void)invoke(ISAAC_VITA_AUDIO_GET_SOURCE_I_NAME, arguments, 3U, &calls);
    s_mock_al_error = (ALenum)UINT32_C(0x0000a005);
    cpu = invoke_at(ISAAC_VITA_AUDIO_GET_ERROR_NAME, arguments, 0U, &calls,
                    ISAAC_VITA_AUDIO_A005_IS_PLAYING_RETURN);
    CHECK(cpu.eax == UINT32_C(0x0000a005) && calls == 3U &&
              s_get_error_calls == 1U && s_audio_log_calls == 1U,
          "source-play A005 interval was not preserved");
    CHECK(strstr(s_last_audio_log, "seq=3 prev=1/2") != NULL &&
              strstr(s_last_audio_log,
                     "alloc=0/1 aargs=00007401") != NULL,
          "source-play A005 lost its allocation origin");
    isaac_vita_audio_a005_test_reset();
}

static void expect_source_capacity_mismatch(uint32_t manager,
                                            ALCint mono_sources,
                                            ALCint stereo_sources)
{
    uint32_t al_error = UINT32_MAX;
    int status;
#ifdef ISAAC_VITA_OPENAL_POOL_ROUTER_ORACLE
    unsigned pool_logs_before = s_pool_log_calls;
    unsigned pool_snapshots_before = s_pool_snapshot_calls;
#endif

    memset((void *)(uintptr_t)manager, 0xcc, 0x344U);
    memset(&s_manager_capture, 0, sizeof s_manager_capture);
    s_mock_mono_sources = mono_sources;
    s_mock_stereo_sources = stereo_sources;
    s_mock_close_device_result = 1;
    status = isaac_vita_audio_manager_initialize(manager, &al_error);
    CHECK(status == ISAAC_VITA_AUDIO_MANAGER_SOURCE_CAPACITY_MISMATCH &&
              strcmp(isaac_vita_audio_manager_status_name(status),
                     "source-capacity-mismatch") == 0,
          "manager accepted a non-80 OpenAL source capacity");
    CHECK(s_manager_capture.open_device_calls == 1U &&
              s_manager_capture.get_integer_calls == 2U &&
              s_manager_capture.create_context_calls == 0U &&
              s_manager_capture.close_device_calls == 1U,
          "source-capacity guard ran after context allocation or leaked device");
    CHECK(ld32(manager + ISAAC_VITA_AUDIO_MANAGER_DEVICE_OFFSET) == 0U &&
              ld32(manager + ISAAC_VITA_AUDIO_MANAGER_CONTEXT_OFFSET) == 0U,
          "source-capacity mismatch retained native handles");
    CHECK(isaac_vita_audio_manager_is_active() == 0,
          "source-capacity mismatch published materialisation-ready state");
    CHECK(isaac_vita_audio_shutdown_active() == 0,
          "source-capacity mismatch published an active owner");
#ifdef ISAAC_VITA_OPENAL_POOL_ROUTER_ORACLE
    CHECK(s_pool_log_calls == pool_logs_before + 1U &&
              s_pool_snapshot_calls == pool_snapshots_before + 1U &&
              strstr(s_last_pool_log, "phase=manager-fail state=2") != NULL,
          "manager failure lost its immediate pool-pressure snapshot");
#endif
}

static int initialize_ready_manager(uint32_t manager)
{
    uint32_t al_error = UINT32_MAX;
    int status;

    memset((void *)(uintptr_t)manager, 0xcc, MANAGER_BYTES);
    s_mock_al_error = AL_NO_ERROR;
    s_mock_mono_sources = 79;
    s_mock_stereo_sources = 1;
    s_mock_close_device_result = 1;
    status = isaac_vita_audio_manager_initialize(manager, &al_error);
    CHECK(status == ISAAC_VITA_AUDIO_MANAGER_OK && al_error == AL_NO_ERROR,
          "owner edge-case setup did not initialize OpenAL");
    return status;
}

static void test_owner_edge_cases(void)
{
    uint32_t manager = s_base + MANAGER_OFFSET;
    uint32_t other_manager = s_base + FAILURE_MANAGER_OFFSET;
    uint32_t arguments[1];
    uint32_t al_error;
    uint8_t manager_before[MANAGER_BYTES];
    uint8_t other_before[MANAGER_BYTES];
    manager_capture native_before;
    unsigned calls;
    CPU cpu;
    int status;

    /* Repeated init, including a different candidate manager, is an exact
     * rejection.  The same proof also pins non-owner manager_close as a no-op.
     */
    memset(&s_manager_capture, 0, sizeof s_manager_capture);
    memset((void *)(uintptr_t)other_manager, 0x5a, MANAGER_BYTES);
    (void)initialize_ready_manager(manager);
    memcpy(manager_before, (const void *)(uintptr_t)manager, MANAGER_BYTES);
    memcpy(other_before, (const void *)(uintptr_t)other_manager, MANAGER_BYTES);
    native_before = s_manager_capture;
    al_error = UINT32_MAX;
    status = isaac_vita_audio_manager_initialize(manager, &al_error);
    CHECK(status == ISAAC_VITA_AUDIO_MANAGER_ALREADY_ACTIVE &&
              al_error == AL_NO_ERROR &&
              strcmp(isaac_vita_audio_manager_status_name(status),
                     "already-active") == 0,
          "same-manager duplicate init was not rejected explicitly");
    CHECK(memcmp((const void *)(uintptr_t)manager, manager_before,
                 MANAGER_BYTES) == 0 &&
              memcmp(&s_manager_capture, &native_before,
                     sizeof native_before) == 0,
          "same-manager duplicate init mutated guest or native state");
    al_error = UINT32_MAX;
    status = isaac_vita_audio_manager_initialize(other_manager, &al_error);
    CHECK(status == ISAAC_VITA_AUDIO_MANAGER_ALREADY_ACTIVE &&
              al_error == AL_NO_ERROR &&
              memcmp((const void *)(uintptr_t)manager, manager_before,
                     MANAGER_BYTES) == 0 &&
              memcmp((const void *)(uintptr_t)other_manager, other_before,
                     MANAGER_BYTES) == 0 &&
              memcmp(&s_manager_capture, &native_before,
                     sizeof native_before) == 0,
          "different-manager duplicate init was not a byte-exact rejection");
    isaac_vita_audio_manager_close(other_manager);
    CHECK(memcmp((const void *)(uintptr_t)manager, manager_before,
                 MANAGER_BYTES) == 0 &&
              memcmp((const void *)(uintptr_t)other_manager, other_before,
                     MANAGER_BYTES) == 0 &&
              memcmp(&s_manager_capture, &native_before,
                     sizeof native_before) == 0,
          "non-owner manager close touched the live owner or caller bytes");
    isaac_vita_audio_manager_close(manager);
    CHECK(isaac_vita_audio_manager_is_active() == 0 &&
              isaac_vita_audio_shutdown_active() == 0,
          "duplicate-init proof did not retire its owner");

    /* Destroying the owned context directly makes the manager unusable but
     * retains device ownership.  Final shutdown must close only that device.
     */
    memset(&s_manager_capture, 0, sizeof s_manager_capture);
    (void)initialize_ready_manager(manager);
    calls = 0U;
    arguments[0] = ld32(manager + ISAAC_VITA_AUDIO_MANAGER_CONTEXT_OFFSET);
    (void)invoke(ISAAC_VITA_AUDIO_DESTROY_CONTEXT_NAME, arguments, 1U, &calls);
    CHECK(calls == 1U &&
              ld32(manager + ISAAC_VITA_AUDIO_MANAGER_CONTEXT_OFFSET) == 0U &&
              ld32(manager + ISAAC_VITA_AUDIO_MANAGER_DEVICE_OFFSET) ==
                  UINT32_C(0x30101010) &&
              ld32(manager + ISAAC_VITA_AUDIO_MANAGER_SOURCES_OFFSET) == 0U &&
              ld32(manager + ISAAC_VITA_AUDIO_MANAGER_BUFFERS_OFFSET) == 0U &&
              isaac_vita_audio_manager_is_active() == 0,
          "direct owned context destroy did not leave one partial device owner");
    CHECK(isaac_vita_audio_shutdown_active() == 1 &&
              isaac_vita_audio_shutdown_active() == 0 &&
              s_manager_capture.destroy_context_calls == 1U &&
              s_manager_capture.close_device_calls == 1U &&
              s_manager_capture.deleted_sources == 0U &&
              s_manager_capture.deleted_buffers == 0U,
          "shutdown double-destroyed state after direct context teardown");

    /* Symmetric direct device close: the remaining context is still owned and
     * shutdown destroys it without issuing a second device close. */
    memset(&s_manager_capture, 0, sizeof s_manager_capture);
    (void)initialize_ready_manager(manager);
    calls = 0U;
    arguments[0] = ld32(manager + ISAAC_VITA_AUDIO_MANAGER_DEVICE_OFFSET);
    cpu = invoke(ISAAC_VITA_AUDIO_CLOSE_DEVICE_NAME, arguments, 1U, &calls);
    CHECK(cpu.eax == 1U && calls == 1U &&
              ld32(manager + ISAAC_VITA_AUDIO_MANAGER_DEVICE_OFFSET) == 0U &&
              ld32(manager + ISAAC_VITA_AUDIO_MANAGER_CONTEXT_OFFSET) ==
                  UINT32_C(0x30202020) &&
              isaac_vita_audio_manager_is_active() == 0,
          "direct owned device close did not leave one partial context owner");
    CHECK(isaac_vita_audio_shutdown_active() == 1 &&
              isaac_vita_audio_shutdown_active() == 0 &&
              s_manager_capture.close_device_calls == 1U &&
              s_manager_capture.destroy_context_calls == 1U &&
              s_manager_capture.deleted_sources == 64U &&
              s_manager_capture.deleted_buffers == 64U,
          "shutdown double-closed state after direct device teardown");

    /* A successful direct context+device teardown retires the owner and makes
     * a fresh manager eligible for initialization again. */
    memset(&s_manager_capture, 0, sizeof s_manager_capture);
    (void)initialize_ready_manager(manager);
    calls = 0U;
    arguments[0] = ld32(manager + ISAAC_VITA_AUDIO_MANAGER_CONTEXT_OFFSET);
    (void)invoke(ISAAC_VITA_AUDIO_DESTROY_CONTEXT_NAME, arguments, 1U, &calls);
    arguments[0] = ld32(manager + ISAAC_VITA_AUDIO_MANAGER_DEVICE_OFFSET);
    cpu = invoke(ISAAC_VITA_AUDIO_CLOSE_DEVICE_NAME, arguments, 1U, &calls);
    CHECK(cpu.eax == 1U && calls == 2U &&
              isaac_vita_audio_shutdown_active() == 0 &&
              isaac_vita_audio_manager_is_active() == 0,
          "full direct teardown retained a stale manager owner");
    status = initialize_ready_manager(other_manager);
    CHECK(status == ISAAC_VITA_AUDIO_MANAGER_OK &&
              isaac_vita_audio_manager_is_active() == 1,
          "full direct teardown did not permit reinitialization");
    isaac_vita_audio_manager_close(other_manager);
    CHECK(isaac_vita_audio_shutdown_active() == 0,
          "reinitialized manager did not close normally");

    /* Failed direct close retains the exact device for both a duplicate-init
     * rejection and later shutdown retries. */
    memset(&s_manager_capture, 0, sizeof s_manager_capture);
    memset((void *)(uintptr_t)other_manager, 0xa5, MANAGER_BYTES);
    (void)initialize_ready_manager(manager);
    arguments[0] = ld32(manager + ISAAC_VITA_AUDIO_MANAGER_CONTEXT_OFFSET);
    (void)invoke(ISAAC_VITA_AUDIO_DESTROY_CONTEXT_NAME, arguments, 1U, NULL);
    s_mock_close_device_result = 0;
    arguments[0] = ld32(manager + ISAAC_VITA_AUDIO_MANAGER_DEVICE_OFFSET);
    cpu = invoke(ISAAC_VITA_AUDIO_CLOSE_DEVICE_NAME, arguments, 1U, NULL);
    CHECK(cpu.eax == 0U &&
              ld32(manager + ISAAC_VITA_AUDIO_MANAGER_DEVICE_OFFSET) ==
                  UINT32_C(0x30101010),
          "failed direct device close discarded the owned retry handle");
    memcpy(other_before, (const void *)(uintptr_t)other_manager, MANAGER_BYTES);
    al_error = UINT32_MAX;
    status = isaac_vita_audio_manager_initialize(other_manager, &al_error);
    CHECK(status == ISAAC_VITA_AUDIO_MANAGER_ALREADY_ACTIVE &&
              al_error == AL_NO_ERROR &&
              memcmp((const void *)(uintptr_t)other_manager, other_before,
                     MANAGER_BYTES) == 0 &&
              s_manager_capture.open_device_calls == 1U,
          "partial close did not block a second native manager");
    CHECK(isaac_vita_audio_shutdown_active() ==
              ISAAC_VITA_AUDIO_SHUTDOWN_INCOMPLETE &&
              s_manager_capture.close_device_calls == 2U &&
              ld32(manager + ISAAC_VITA_AUDIO_MANAGER_DEVICE_OFFSET) ==
                  UINT32_C(0x30101010),
          "failed shutdown retry did not report its retained device owner");
    s_mock_close_device_result = 1;
    CHECK(isaac_vita_audio_shutdown_active() ==
              ISAAC_VITA_AUDIO_SHUTDOWN_COMPLETE &&
              isaac_vita_audio_shutdown_active() ==
                  ISAAC_VITA_AUDIO_SHUTDOWN_NONE &&
              s_manager_capture.close_device_calls == 3U &&
              ld32(manager + ISAAC_VITA_AUDIO_MANAGER_DEVICE_OFFSET) == 0U,
          "successful shutdown retry did not retire the partial owner");

    /* Foreign direct calls retain the owner byte-for-byte.  Their native calls
     * still pass through, while final owner close touches each owned handle
     * exactly once. */
    memset(&s_manager_capture, 0, sizeof s_manager_capture);
    (void)initialize_ready_manager(manager);
    memcpy(manager_before, (const void *)(uintptr_t)manager, MANAGER_BYTES);
    arguments[0] = UINT32_C(0x30f0f0f0);
    (void)invoke(ISAAC_VITA_AUDIO_DESTROY_CONTEXT_NAME, arguments, 1U, NULL);
    arguments[0] = UINT32_C(0x30e0e0e0);
    cpu = invoke(ISAAC_VITA_AUDIO_CLOSE_DEVICE_NAME, arguments, 1U, NULL);
    CHECK(cpu.eax == 1U && isaac_vita_audio_manager_is_active() == 1 &&
              memcmp((const void *)(uintptr_t)manager, manager_before,
                     MANAGER_BYTES) == 0,
          "foreign direct close altered the live manager owner");
    isaac_vita_audio_manager_close(manager);
    CHECK(s_manager_capture.destroy_context_calls == 2U &&
              s_manager_capture.close_device_calls == 2U &&
              isaac_vita_audio_shutdown_active() == 0,
          "foreign close prevented exact final owner teardown");
}

#ifdef ISAAC_VITA_AUDIO_MANUAL_ORACLE
static void test_manual_duplicate_policy(void)
{
    uint32_t manager = s_base + MANUAL_MANAGER_OFFSET;
    uint32_t stack = s_base + STACK_OFFSET;
    uint8_t manager_before[MANAGER_BYTES];
    manager_capture native_before;
    CPU cpu;

    CHECK(s_base == (uint32_t)GUEST_IMAGE_BASE,
          "manual audio oracle did not map its compiled guest base");
    memset(&s_manager_capture, 0, sizeof s_manager_capture);
    (void)initialize_ready_manager(manager);

    /* Model the first manual call's guest-owned state.  The second call must
     * not borrow any of it into its failure cleanup. */
    st16(manager + ISAAC_VITA_AUDIO_MANAGER_RUNNING_OFFSET, 1U);
    st32(manager + MANUAL_SOUND_POOL_OFFSET, UINT32_C(0x27aa0004));
    st8(manager + MANUAL_SOUND_MUTEX_A_OFFSET + 4U, 1U);
    st32(manager + MANUAL_SOUND_MUTEX_A_OFFSET + 8U,
         UINT32_C(0x27aa0100));
    st8(manager + MANUAL_SOUND_MUTEX_B_OFFSET + 4U, 1U);
    st32(manager + MANUAL_SOUND_MUTEX_B_OFFSET + 8U,
         UINT32_C(0x27aa0200));
    st32(manager + MANUAL_SOUND_THREAD_STATE_OFFSET, 0U);
    memcpy(manager_before, (const void *)(uintptr_t)manager, MANAGER_BYTES);
    native_before = s_manager_capture;

    s_manual_malloc_calls = 0U;
    s_manual_calloc_calls = 0U;
    s_manual_free_calls = 0U;
    s_manual_sync_init_calls = 0U;
    s_manual_sync_delete_calls = 0U;
    s_manual_log_calls = 0U;
    memset(&cpu, 0, sizeof cpu);
    st32(stack, UINT32_C(0x12345678));
    cpu.ecx = manager;
    cpu.esp = stack;
    cpu.eax = UINT32_C(0x765432ff);
    sub_0056dd70(&cpu);

    CHECK(cpu.fault == NULL && cpu.esp == stack + 4U &&
              cpu.eax == UINT32_C(0x76543200),
          "manual duplicate SoundInitialize return ABI drifted");
    CHECK(memcmp((const void *)(uintptr_t)manager, manager_before,
                 MANAGER_BYTES) == 0 &&
              memcmp(&s_manager_capture, &native_before,
                     sizeof native_before) == 0 &&
              isaac_vita_audio_manager_is_active() == 1,
          "manual duplicate SoundInitialize mutated the first owner");
    CHECK(s_manual_malloc_calls == 0U && s_manual_calloc_calls == 0U &&
              s_manual_free_calls == 0U && s_manual_sync_init_calls == 0U &&
              s_manual_sync_delete_calls == 0U && s_manual_log_calls == 1U,
          "manual duplicate SoundInitialize acquired or released guest state");
    isaac_vita_audio_manager_close(manager);
    CHECK(isaac_vita_audio_shutdown_active() == 0,
          "manual duplicate oracle did not retire its original owner");
}

static void test_manual_terminal_heap_policy(void)
{
    uint32_t manager = s_base + MANUAL_MANAGER_OFFSET;
    uint32_t stack = s_base + STACK_OFFSET;
    CPU cpu;

    memset((void *)(uintptr_t)manager, 0, MANAGER_BYTES);
    memset(&s_manager_capture, 0, sizeof s_manager_capture);
    s_mock_al_error = AL_NO_ERROR;
    s_mock_mono_sources = 79;
    s_mock_stereo_sources = 1;
    s_mock_close_device_result = 1;
    s_manual_malloc_calls = 0U;
    s_manual_calloc_calls = 0U;
    s_manual_free_calls = 0U;
    s_manual_sync_init_calls = 0U;
    s_manual_sync_delete_calls = 0U;
    s_manual_log_calls = 0U;
    s_manual_heap_terminal = 1;
    memset(&cpu, 0, sizeof cpu);
    st32(stack, UINT32_C(0x12345678));
    cpu.ecx = manager;
    cpu.esp = stack;
    cpu.eax = UINT32_C(0x765432ff);

    sub_0056dd70(&cpu);

    CHECK(cpu.fault != NULL && cpu.fault_addr == manager &&
              cpu.esp == stack && cpu.eax == UINT32_C(0x765432ff),
          "manual terminal heap failure performed a normal guest return");
    CHECK(s_manual_malloc_calls == 1U && s_manual_calloc_calls == 0U &&
              s_manual_free_calls == 0U &&
              s_manual_sync_init_calls == 0U &&
              s_manual_sync_delete_calls == 0U && s_manual_log_calls == 0U,
          "manual terminal heap failure crossed later guest-state stages");
    CHECK(isaac_vita_audio_manager_is_active() == 0 &&
              isaac_vita_audio_shutdown_active() == 0,
          "manual terminal heap failure did not unwind native audio owner");
    s_manual_heap_terminal = 0;
}
#endif

static void test_manager_lifecycle(void)
{
    uint32_t manager = s_base + MANAGER_OFFSET;
    uint32_t failed_manager = s_base + FAILURE_MANAGER_OFFSET;
    uint32_t stream_names = s_base + AUX_OFFSET;
    uint32_t al_error = UINT32_MAX;
    uint32_t stream_args[2];
    unsigned stream_import_calls = 0U;
    uint32_t index;
    CPU cpu;
    manager_capture closed_capture;
    int status;

    memset((void *)(uintptr_t)manager, 0xcc, 0x344U);
    memset(&s_manager_capture, 0, sizeof s_manager_capture);
    s_mock_al_error = AL_NO_ERROR;
    status = isaac_vita_audio_manager_initialize(manager, &al_error);

    CHECK(status == ISAAC_VITA_AUDIO_MANAGER_OK && al_error == AL_NO_ERROR,
          "manager OpenAL initialization did not report success");
    CHECK(isaac_vita_audio_manager_is_active() == 1,
          "successful manager init did not publish materialisation-ready state");
    CHECK(strcmp(isaac_vita_audio_manager_status_name(status), "ready") == 0,
          "manager success status text drifted");
    CHECK(ld8(manager + ISAAC_VITA_AUDIO_MANAGER_RUNNING_OFFSET) == 0U &&
              ld8(manager + ISAAC_VITA_AUDIO_MANAGER_SUSPENDED_OFFSET) == 0U,
          "native manager half wrote guest running/suspended policy");
    CHECK(ld32(manager + ISAAC_VITA_AUDIO_MANAGER_DEVICE_OFFSET) ==
              UINT32_C(0x30101010) &&
              ld32(manager + ISAAC_VITA_AUDIO_MANAGER_CONTEXT_OFFSET) ==
              UINT32_C(0x30202020),
          "manager device/context handles drifted");
    CHECK(ld32(manager + ISAAC_VITA_AUDIO_MANAGER_SOURCES_OFFSET) ==
              UINT32_C(0x7301) &&
              ld32(manager + ISAAC_VITA_AUDIO_MANAGER_SOURCES_OFFSET +
                   (ISAAC_VITA_AUDIO_MANAGER_SOURCE_COUNT - 1U) * 4U) ==
              UINT32_C(0x7340),
          "manager 64-source table drifted");
    CHECK(ld32(manager + ISAAC_VITA_AUDIO_MANAGER_BUFFERS_OFFSET) ==
              UINT32_C(0x7101) &&
              ld32(manager + ISAAC_VITA_AUDIO_MANAGER_BUFFERS_OFFSET +
                   (ISAAC_VITA_AUDIO_MANAGER_BUFFER_COUNT - 1U) * 4U) ==
              UINT32_C(0x7140),
          "manager 64-buffer table drifted");
    CHECK(s_manager_capture.open_device_calls == 1U &&
              s_manager_capture.get_integer_calls == 2U &&
              s_manager_capture.create_context_calls == 1U &&
              s_manager_capture.make_current_calls == 1U &&
              s_manager_capture.process_context_calls == 1U &&
              s_manager_capture.generated_sources == 64U &&
              s_manager_capture.generated_buffers == 64U &&
              s_manager_capture.source_gen_calls == 1U &&
              s_manager_capture.last_generated_source_count == 64U &&
              s_manager_capture.live_sources == 64U &&
              s_manager_capture.peak_live_sources == 64U,
          "manager native OpenAL call census drifted");
    CHECK(s_manager_capture.listener_position[0] == 0.0f &&
              s_manager_capture.listener_position[1] == 0.0f &&
              s_manager_capture.listener_position[2] == 0.0f &&
              s_manager_capture.listener_orientation[0] == 0.0f &&
              s_manager_capture.listener_orientation[1] == 0.0f &&
              s_manager_capture.listener_orientation[2] == -1.0f &&
              s_manager_capture.listener_orientation[3] == 0.0f &&
              s_manager_capture.listener_orientation[4] == 1.0f &&
              s_manager_capture.listener_orientation[5] == 0.0f,
          "manager listener state drifted from the frozen initializer");

    /* The frozen manager permanently consumes 64 source names.  Exercise the
     * exact one-at-a-time allocation shape used by StreamSourceBase::Open:
     * all 16 independent names must fit, while source 81 must fail A005. */
    memset((void *)(uintptr_t)stream_names, 0,
           (ISAAC_VITA_AUDIO_STREAM_SOURCE_HEADROOM + 1U) * 4U);
    for (index = 0U; index < ISAAC_VITA_AUDIO_STREAM_SOURCE_HEADROOM;
         ++index) {
        stream_args[0] = 1U;
        stream_args[1] = stream_names + index * 4U;
        (void)invoke(ISAAC_VITA_AUDIO_GEN_SOURCES_NAME, stream_args, 2U,
                     &stream_import_calls);
        CHECK(ld32(stream_names + index * 4U) ==
                  UINT32_C(0x7341) + index,
              "one-source stream allocation failed inside the headroom");
        cpu = invoke(ISAAC_VITA_AUDIO_GET_ERROR_NAME, stream_args, 0U,
                     &stream_import_calls);
        CHECK(cpu.eax == AL_NO_ERROR,
              "OpenAL reported an error before stream headroom was full");
    }
    stream_args[0] = 1U;
    stream_args[1] = stream_names +
                     ISAAC_VITA_AUDIO_STREAM_SOURCE_HEADROOM * 4U;
    (void)invoke(ISAAC_VITA_AUDIO_GEN_SOURCES_NAME, stream_args, 2U,
                 &stream_import_calls);
    cpu = invoke(ISAAC_VITA_AUDIO_GET_ERROR_NAME, stream_args, 0U,
                 &stream_import_calls);
    CHECK(cpu.eax == UINT32_C(0x0000a005) &&
              ld32(stream_names +
                   ISAAC_VITA_AUDIO_STREAM_SOURCE_HEADROOM * 4U) == 0U,
          "source 81 did not fail atomically with AL_OUT_OF_MEMORY");
    CHECK(s_manager_capture.generated_sources == 80U &&
              s_manager_capture.live_sources == 80U &&
              s_manager_capture.peak_live_sources == 80U &&
              s_manager_capture.source_gen_calls == 18U &&
              s_manager_capture.source_allocation_failures == 1U,
          "64-manager/16-stream source accounting drifted");
    stream_args[0] = ISAAC_VITA_AUDIO_STREAM_SOURCE_HEADROOM;
    stream_args[1] = stream_names;
    (void)invoke(ISAAC_VITA_AUDIO_DELETE_SOURCES_NAME, stream_args, 2U,
                 &stream_import_calls);
    CHECK(stream_import_calls == 35U &&
              s_manager_capture.deleted_sources == 16U &&
              s_manager_capture.last_deleted_source_count == 16U &&
              s_manager_capture.live_sources == 64U,
          "stream headroom cleanup did not preserve the manager's 64 names");

    CHECK(isaac_vita_audio_shutdown_active() == 1,
          "active manager teardown did not claim initialized OpenAL state");
    CHECK(isaac_vita_audio_manager_is_active() == 0,
          "active manager teardown retained materialisation-ready state");
    CHECK(ld32(manager + ISAAC_VITA_AUDIO_MANAGER_DEVICE_OFFSET) == 0U &&
              ld32(manager + ISAAC_VITA_AUDIO_MANAGER_CONTEXT_OFFSET) == 0U &&
              ld32(manager + ISAAC_VITA_AUDIO_MANAGER_SOURCES_OFFSET) == 0U &&
              ld32(manager + ISAAC_VITA_AUDIO_MANAGER_BUFFERS_OFFSET) == 0U,
          "manager close did not clear guest OpenAL state");
    CHECK(s_manager_capture.deleted_sources == 80U &&
              s_manager_capture.deleted_buffers == 64U &&
              s_manager_capture.source_delete_calls == 2U &&
              s_manager_capture.last_deleted_source_count == 64U &&
              s_manager_capture.live_sources == 0U &&
              s_manager_capture.destroy_context_calls == 1U &&
              s_manager_capture.close_device_calls == 1U &&
              s_manager_capture.make_current_calls == 2U,
          "manager close lifecycle drifted");
    closed_capture = s_manager_capture;
    CHECK(isaac_vita_audio_shutdown_active() == 0,
          "repeated active manager teardown was not idempotent");
    CHECK(memcmp(&s_manager_capture, &closed_capture,
                 sizeof s_manager_capture) == 0,
          "inactive manager teardown repeated native OpenAL calls");

    /* The explicit SoundInitialize failure/unwind path still calls the
     * address-taking close API.  It must clear the same active owner so the
     * host boundary cannot close stale handles a second time. */
    memset((void *)(uintptr_t)manager, 0xcc, 0x344U);
    memset(&s_manager_capture, 0, sizeof s_manager_capture);
    s_mock_al_error = AL_NO_ERROR;
    al_error = UINT32_MAX;
    status = isaac_vita_audio_manager_initialize(manager, &al_error);
    CHECK(status == ISAAC_VITA_AUDIO_MANAGER_OK && al_error == AL_NO_ERROR,
          "manager reinitialization for direct-close proof failed");
    CHECK(isaac_vita_audio_manager_is_active() == 1,
          "manager reinitialization did not republish active state");
    isaac_vita_audio_manager_close(manager);
    CHECK(isaac_vita_audio_manager_is_active() == 0,
          "direct manager close retained materialisation-ready state");
    CHECK(isaac_vita_audio_shutdown_active() == 0,
          "direct manager close retained a stale active owner");
    CHECK(s_manager_capture.deleted_sources == 64U &&
              s_manager_capture.deleted_buffers == 64U &&
              s_manager_capture.destroy_context_calls == 1U &&
              s_manager_capture.close_device_calls == 1U,
          "direct manager close lifecycle drifted");

    memset((void *)(uintptr_t)failed_manager, 0xcc, 0x344U);
    memset(&s_manager_capture, 0, sizeof s_manager_capture);
    s_mock_al_error = (ALenum)UINT32_C(0x0000a004);
    al_error = 0U;
    status = isaac_vita_audio_manager_initialize(failed_manager, &al_error);
    CHECK(status == ISAAC_VITA_AUDIO_MANAGER_AL_ERROR &&
              al_error == UINT32_C(0x0000a004),
          "manager AL failure attribution drifted");
    CHECK(ld32(failed_manager + ISAAC_VITA_AUDIO_MANAGER_DEVICE_OFFSET) ==
              0U &&
              ld32(failed_manager + ISAAC_VITA_AUDIO_MANAGER_CONTEXT_OFFSET) ==
              0U &&
              ld32(failed_manager + ISAAC_VITA_AUDIO_MANAGER_SOURCES_OFFSET) ==
              0U &&
              ld32(failed_manager + ISAAC_VITA_AUDIO_MANAGER_BUFFERS_OFFSET) ==
              0U,
          "manager AL failure leaked guest handles");
    CHECK(s_manager_capture.deleted_sources == 64U &&
              s_manager_capture.deleted_buffers == 64U &&
              s_manager_capture.destroy_context_calls == 1U &&
              s_manager_capture.close_device_calls == 1U,
          "manager AL failure did not unwind native resources");
    CHECK(isaac_vita_audio_manager_is_active() == 0,
          "failed manager initialization published materialisation-ready state");
    CHECK(isaac_vita_audio_shutdown_active() == 0,
          "failed manager initialization published an active owner");

    /* app0:/alsoft.conf is a release artefact, so reject both the old starving
     * 64-source file and the 256-source fallback before context allocation. */
    expect_source_capacity_mismatch(failed_manager, 63, 1);
    expect_source_capacity_mismatch(failed_manager, 255, 1);
    s_mock_mono_sources = 79;
    s_mock_stereo_sources = 1;
}

#ifdef ISAAC_VITA_AUDIO_REFILL_HOOKS
static unsigned long refill_receipt_field(const char *key)
{
    const char *at = strstr(s_last_audio_log, key);

    return at ? strtoul(at + strlen(key), NULL, 10) : ULONG_MAX;
}

#ifdef ISAAC_VITA_AUDIO_REFILL_REPLAYS
/* Stream refills from the frozen site until the bounded receipt logs a line
 * (powers of two, then every 256), so its fields can be read. */
static void drive_refill_line(uint32_t data, unsigned *calls)
{
    uint32_t a[5];
    unsigned before = s_audio_log_calls;
    unsigned tries;

    a[0] = UINT32_C(0x7201); a[1] = UINT32_C(0x1103); a[2] = data;
    a[3] = ISAAC_VITA_AUDIO_STREAM_SLOT_BYTES; a[4] = 48000U;
    for (tries = 0U; tries < 300U && s_audio_log_calls == before; ++tries)
        (void)invoke_at(ISAAC_VITA_AUDIO_BUFFER_DATA_NAME, a, 5U, calls,
                        ISAAC_VITA_AUDIO_STREAM_BUFFER_DATA_RETURN);
    CHECK(s_audio_log_calls == before + 1U &&
              strncmp(s_last_audio_log, "KAGE VITA AUDIO REFILL: n=", 26U) == 0,
          "no stream refill receipt line was produced");
}
#endif

/* Drive the refill hooks through the real cdecl adapters with the return
 * marker pushed exactly as the generated call sites push it: the BARE RVA
 * (guest_0177.c gpush_generated(c, 0x5bf136U)).  The pure pacer oracle cannot
 * see the site match; this is the test that catches a base-offset mismatch. */
static void test_refill_hooks_through_adapters(void)
{
    unsigned calls = 0U;
    uint32_t args[5];
    uint32_t data = s_base + DATA_OFFSET;
    uint32_t aux = s_base + AUX_OFFSET;
    const uint32_t bare = ISAAC_VITA_AUDIO_STREAM_PROCESSED_RETURN;
    const uint32_t based = (uint32_t)GUEST_IMAGE_BASE + bare;
    const uint32_t foreign = UINT32_C(0x12345678);
    unsigned long static_before;
    unsigned long unqueues_before;

    s_refill_now_us = UINT64_C(1000000);
    s_mock_processed = 2;
    s_mock_queued = 4;

    /* 1. StreamSource::Update site, bare marker: filtered to one grant. */
    args[0] = UINT32_C(0x1102);
    args[1] = (uint32_t)ISAAC_VITA_AUDIO_AL_BUFFERS_PROCESSED;
    args[2] = data;
    (void)invoke_at(ISAAC_VITA_AUDIO_GET_SOURCE_I_NAME, args, 3U, &calls,
                    bare);
    CHECK(s_capture.operation == OP_GET_SOURCE_I &&
              s_capture.u[1] == (uint32_t)ISAAC_VITA_AUDIO_AL_BUFFERS_QUEUED,
          "bare-RVA StreamSource site did not run the queued follow-up query");
    CHECK((int32_t)ld32(data) == 1,
          "bare-RVA StreamSource site was not capped to one processed");

    /* 2. Same window, another layer: deferred to zero. */
    s_refill_now_us += 10U;
    (void)invoke_at(ISAAC_VITA_AUDIO_GET_SOURCE_I_NAME, args, 3U, &calls,
                    bare);
    CHECK((int32_t)ld32(data) == 0,
          "second layer inside the spacing window was not deferred");

    /* 3. Based marker after the window: still recognised, granted. */
    s_refill_now_us += 20000U;
    (void)invoke_at(ISAAC_VITA_AUDIO_GET_SOURCE_I_NAME, args, 3U, &calls,
                    based);
    CHECK((int32_t)ld32(data) == 1,
          "GUEST_IMAGE_BASE + RVA marker was not recognised");

    /* 4. Foreign caller (Theora, SFX polls): untouched, no extra query. */
    (void)invoke_at(ISAAC_VITA_AUDIO_GET_SOURCE_I_NAME, args, 3U, &calls,
                    foreign);
    CHECK(s_capture.u[1] == (uint32_t)ISAAC_VITA_AUDIO_AL_BUFFERS_PROCESSED &&
              (int32_t)ld32(data) == 2,
          "foreign AL_BUFFERS_PROCESSED caller was filtered");

    /* 5. Headroom valve through the adapter: 3 of 4 processed at the site
     * inside the window is answered truthfully. */
    s_mock_processed = 3;
    (void)invoke_at(ISAAC_VITA_AUDIO_GET_SOURCE_I_NAME, args, 3U, &calls,
                    bare);
    CHECK((int32_t)ld32(data) == 3,
          "headroom valve did not pass the full count through the adapter");
    s_mock_processed = 2;

    /* 6. Receipt attribution: unqueue -> alBufferData from 0x5be117 is a
     * stream refill (logs at n=1); a foreign alBufferData is a static SFX
     * upload (never logs); the next stream refill logs at n=2. */
    s_audio_log_calls = 0U;
    s_last_audio_log[0] = '\0';
    args[0] = UINT32_C(0x1102); args[1] = 1U; args[2] = aux;
    (void)invoke_at(ISAAC_VITA_AUDIO_SOURCE_UNQUEUE_BUFFERS_NAME, args, 3U,
                    &calls, UINT32_C(0x005bf14d));
    s_refill_now_us += 18000U;
    args[0] = UINT32_C(0x7201); args[1] = UINT32_C(0x1103); args[2] = data;
    args[3] = ISAAC_VITA_AUDIO_STREAM_SLOT_BYTES; args[4] = 44100U;
    (void)invoke_at(ISAAC_VITA_AUDIO_BUFFER_DATA_NAME, args, 5U, &calls,
                    ISAAC_VITA_AUDIO_STREAM_BUFFER_DATA_RETURN);
    CHECK(s_audio_log_calls == 1U &&
              strncmp(s_last_audio_log, "KAGE VITA AUDIO REFILL: n=1 ",
                      28U) == 0,
          "bare-RVA alBufferData site was not counted as a stream refill");
    CHECK(strstr(s_last_audio_log, " decode_us=18000 ") != NULL,
          "unqueue -> alBufferData gap was not measured as the decode time");
    /* Earlier adapter sweeps already uploaded/unqueued through the same
     * counters, so compare the receipt fields relatively. */
    static_before = refill_receipt_field(" static=");
    unqueues_before = refill_receipt_field(" unq=");
    (void)invoke_at(ISAAC_VITA_AUDIO_BUFFER_DATA_NAME, args, 5U, &calls,
                    foreign);
    CHECK(s_audio_log_calls == 1U,
          "foreign alBufferData was counted as a stream refill");
    (void)invoke_at(ISAAC_VITA_AUDIO_BUFFER_DATA_NAME, args, 5U, &calls,
                    (uint32_t)GUEST_IMAGE_BASE +
                        ISAAC_VITA_AUDIO_STREAM_BUFFER_DATA_RETURN);
    CHECK(s_audio_log_calls == 2U &&
              strncmp(s_last_audio_log, "KAGE VITA AUDIO REFILL: n=2 ",
                      28U) == 0 &&
              refill_receipt_field(" static=") == static_before + 1UL &&
              refill_receipt_field(" unq=") == unqueues_before &&
              unqueues_before != ULONG_MAX,
          "foreign alBufferData was not counted as exactly one static upload");

    /* 7. alSourceQueueBuffers from 0x5be126 asks the source state; a
     * foreign caller does not. */
    args[0] = UINT32_C(0x1102); args[1] = 1U; args[2] = aux;
    (void)invoke_at(ISAAC_VITA_AUDIO_SOURCE_QUEUE_BUFFERS_NAME, args, 3U,
                    &calls, ISAAC_VITA_AUDIO_STREAM_QUEUE_RETURN);
    CHECK(s_capture.operation == OP_GET_SOURCE_I &&
              s_capture.u[1] == (uint32_t)ISAAC_VITA_AUDIO_AL_SOURCE_STATE,
          "bare-RVA alSourceQueueBuffers site did not query the state");
    (void)invoke_at(ISAAC_VITA_AUDIO_SOURCE_QUEUE_BUFFERS_NAME, args, 3U,
                    &calls, foreign);
    CHECK(s_capture.operation == OP_SOURCE_QUEUE_BUFFERS,
          "foreign alSourceQueueBuffers caller was treated as a refill");
    CHECK(strstr(s_last_audio_log, "REFILL WARN") == NULL,
          "refill WARN fired although refills were attributed");

#ifdef ISAAC_VITA_AUDIO_REFILL_REPLAYS
    /* 8. Replay receipt.  alSourcePlay returning to QueueData (0x5be144)
     * with the source AL_INITIAL is a start; AL_STOPPED with buffers queued
     * is a replay (the source ran dry and QueueData restarted it); AL_STOPPED
     * right after the guest's own alSourceStop is a restart, not a replay; a
     * foreign caller is only a play.  A stream alBufferData below the slot
     * size (ASYNC partial slot) is still a refill and counted as partial. */
    {
        const uint32_t play_site = ISAAC_VITA_AUDIO_STREAM_PLAY_RETURN;
        unsigned long replays_before, site_before, plays_before, partial_before;
        unsigned log_before;

        drive_refill_line(data, &calls);
        replays_before = refill_receipt_field(" replays=");
        site_before = refill_receipt_field(" site_plays=");
        plays_before = refill_receipt_field(" plays=");
        partial_before = refill_receipt_field(" partial=");
        CHECK(replays_before != ULONG_MAX && site_before != ULONG_MAX &&
                  plays_before != ULONG_MAX && partial_before != ULONG_MAX,
              "REFILL line does not carry the replay fields");

        /* a fresh stream (source 66, a manager name as on the device):
         * AL_INITIAL at the QueueData site */
        s_mock_state = ISAAC_VITA_AUDIO_AL_INITIAL;
        s_mock_queued = 1;
        args[0] = 66U;
        log_before = s_audio_log_calls;
        (void)invoke_at(ISAAC_VITA_AUDIO_SOURCE_PLAY_NAME, args, 1U, &calls,
                        play_site);
        CHECK(s_capture.operation == OP_SOURCE_PLAY && s_capture.u[0] == 66U,
              "play at the QueueData site did not reach alSourcePlay last");
        CHECK(s_audio_log_calls == log_before,
              "a start from AL_INITIAL was logged as a replay");
        /* the source ran dry: AL_STOPPED with the buffer just queued */
        s_mock_state = ISAAC_VITA_AUDIO_AL_STOPPED;
        (void)invoke_at(ISAAC_VITA_AUDIO_SOURCE_PLAY_NAME, args, 1U, &calls,
                        play_site);
        CHECK(s_audio_log_calls == log_before + 1U &&
                  strncmp(s_last_audio_log, "KAGE VITA AUDIO REPLAY: n=",
                          26U) == 0 &&
                  strstr(s_last_audio_log, " src=66 ") != NULL,
              "AL_STOPPED play from QueueData was not logged as a replay");
        /* the guest stopped the source itself: the restart is not a replay */
        (void)invoke_at(ISAAC_VITA_AUDIO_SOURCE_STOP_NAME, args, 1U, &calls,
                        foreign);
        log_before = s_audio_log_calls;
        (void)invoke_at(ISAAC_VITA_AUDIO_SOURCE_PLAY_NAME, args, 1U, &calls,
                        play_site);
        CHECK(s_audio_log_calls == log_before,
              "restart after the guest's alSourceStop was counted as a replay");
        /* a foreign caller (SFX) in AL_STOPPED: a play, nothing else */
        (void)invoke_at(ISAAC_VITA_AUDIO_SOURCE_PLAY_NAME, args, 1U, &calls,
                        foreign);
        CHECK(s_capture.operation == OP_SOURCE_PLAY &&
                  s_audio_log_calls == log_before,
              "foreign alSourcePlay was treated as a stream replay");
        /* dry again (eligibility was restored by the plays above) */
        (void)invoke_at(ISAAC_VITA_AUDIO_SOURCE_PLAY_NAME, args, 1U, &calls,
                        play_site);
        CHECK(s_audio_log_calls == log_before + 1U &&
                  strncmp(s_last_audio_log, "KAGE VITA AUDIO REPLAY: n=",
                          26U) == 0,
              "second dry restart was not logged as a replay");
        /* a source name beyond the per-source table cannot be tracked for
         * guest stops: a site play, never a replay */
        log_before = s_audio_log_calls;
        args[0] = UINT32_C(0x1102);
        (void)invoke_at(ISAAC_VITA_AUDIO_SOURCE_PLAY_NAME, args, 1U, &calls,
                        play_site);
        CHECK(s_capture.operation == OP_SOURCE_PLAY &&
                  s_capture.u[0] == UINT32_C(0x1102) &&
                  s_audio_log_calls == log_before,
              "out-of-table source name was claimed as a replay");
        /* a partial stream refill (ASYNC slot of 12000 bytes) */
        args[0] = UINT32_C(0x7201); args[1] = UINT32_C(0x1103); args[2] = data;
        args[3] = 12000U; args[4] = 48000U;
        (void)invoke_at(ISAAC_VITA_AUDIO_BUFFER_DATA_NAME, args, 5U, &calls,
                        ISAAC_VITA_AUDIO_STREAM_BUFFER_DATA_RETURN);
        drive_refill_line(data, &calls);
        CHECK(refill_receipt_field(" replays=") == replays_before + 2UL,
              "REFILL line does not count exactly the two dry restarts");
        CHECK(refill_receipt_field(" site_plays=") == site_before + 5UL,
              "REFILL line does not count the five QueueData plays");
        CHECK(refill_receipt_field(" plays=") == plays_before + 6UL,
              "REFILL line does not count every alSourcePlay");
        CHECK(refill_receipt_field(" partial=") == partial_before + 1UL,
              "partial stream refill was not counted");
        s_mock_state = 0;
    }
#endif
}
#endif

int main(void)
{
    void *memory = map_guest_test_memory();

    if (!memory) {
        fprintf(stderr, "FAIL: could not map 32-bit guest oracle memory\n");
        return 2;
    }
    test_a005_cold_start();
#ifdef ISAAC_VITA_OPENAL_POOL_ROUTER_ORACLE
    test_pool_manager_first_openal_order();
#endif
    test_inventory();
    test_dispatch();
    test_all_adapters();
    test_a005_attribution_and_folding();
    test_a005_source_play_attribution();
#ifdef ISAAC_VITA_AUDIO_REFILL_HOOKS
    test_refill_hooks_through_adapters();
#endif
    CHECK(isaac_vita_audio_manager_is_active() == 0,
          "fresh audio runtime started materialisation-ready");
    test_manager_lifecycle();
    test_owner_edge_cases();
#ifdef ISAAC_VITA_AUDIO_MANUAL_ORACLE
    test_manual_duplicate_policy();
    test_manual_terminal_heap_policy();
#endif
    unmap_guest_test_memory(memory);
    CHECK(isaac_vita_audio_shutdown_active() == 0,
          "inactive teardown touched an unmapped guest image");
    CHECK(isaac_vita_audio_manager_is_active() == 0,
          "inactive post-unmap state became materialisation-ready");
    CHECK(guest_stack_legacy_oracle_violation_calls() == 0U,
          "standalone oracle entered a guest-stack violation helper");

    if (s_failures != 0U) {
        fprintf(stderr, "Vita OpenAL ABI oracle: %u failure(s)\n",
                s_failures);
        return 1;
    }
#ifdef ISAAC_VITA_OPENAL_POOL_ROUTER_ORACLE
    puts("Vita OpenAL pool router oracle: PASS (indexed/manager init-before-first-API + exact fixed-image range + bounded init/A005/final telemetry + retry-safe explicit final)");
#elif defined(ISAAC_VITA_AUDIO_REFILL_REPLAYS)
    puts("Vita OpenAL refill-replays oracle: PASS (refill hooks + alSourcePlay from QueueData: AL_INITIAL start, AL_STOPPED replay, guest alSourceStop restart, foreign play, out-of-table source never a replay; partial stream refill counted)");
#elif defined(ISAAC_VITA_AUDIO_REFILL_HOOKS)
    puts("Vita OpenAL refill-hook oracle: PASS (bare-RVA + based return markers through the real alGetSourcei/alBufferData/alSourceQueueBuffers adapters: grant/defer/valve, foreign callers untouched, receipt attribution)");
#else
    puts("Vita OpenAL ABI oracle: PASS (24/24 cdecl adapters + 64/16 source contract + duplicate-init rejection + direct/foreign/failed-close ownership + honest tri-state shutdown + idempotent partial teardown + manual duplicate-state preservation)");
#endif
    return 0;
}
