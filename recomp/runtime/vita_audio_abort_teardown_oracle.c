/* Native executable oracle for ISAAC_VITA_ABORT_SKIP_AUDIO_TEARDOWN: the
 * abort-stop replacement for isaac_vita_audio_shutdown_active must touch no
 * OpenAL entry point and no guest byte, must leave the owner record exactly
 * as it was, and the ordinary teardown chosen afterwards (the Exit game path)
 * must still retire the same handles with the same native call census.
 *
 * Built by recomp/vita/test_audio_abort_teardown.sh with
 * -DISAAC_VITA_AUDIO_ORACLE=1 -DISAAC_VITA_ABORT_SKIP_AUDIO_TEARDOWN=1
 * against the production host_vita_audio.c and the same stub OpenAL header
 * the ABI oracle uses; the OpenAL stubs below count every entry point. */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
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

#ifndef ISAAC_VITA_ABORT_SKIP_AUDIO_TEARDOWN
#error this oracle exercises ISAAC_VITA_ABORT_SKIP_AUDIO_TEARDOWN and needs the define
#endif

#define TEST_BYTES 0x00010000U
#define STACK_OFFSET 0x1000U
#define MANAGER_OFFSET 0x4000U
#define MANAGER_BYTES 0x344U
#define MOCK_DEVICE UINT32_C(0x30101010)
#define MOCK_CONTEXT UINT32_C(0x30202020)

typedef struct al_census {
    unsigned any;              /* every OpenAL/ALC entry point */
    unsigned open_device;
    unsigned create_context;
    unsigned make_current;
    unsigned process_context;
    unsigned destroy_context;
    unsigned close_device;
    unsigned gen_sources;
    unsigned gen_buffers;
    unsigned delete_sources;
    unsigned delete_buffers;
    unsigned source_stop;
    unsigned get_integer;
    unsigned other;
} al_census;

static al_census s_al;
static ALCboolean s_mock_close_device_result = 1;
static unsigned s_log_calls;
static char s_last_log[384];
static unsigned s_failures;
static uint32_t s_base;

#define CHECK(condition, message)                                      \
    do {                                                               \
        if (!(condition)) {                                            \
            fprintf(stderr, "FAIL: %s (%s:%d)\n",                    \
                    message, __FILE__, __LINE__);                      \
            ++s_failures;                                              \
        }                                                              \
    } while (0)

static void *map_guest_test_memory(void)
{
    static const uintptr_t candidates[] = {
        UINT32_C(0x27000000), UINT32_C(0x29000000),
        UINT32_C(0x2b000000), UINT32_C(0x2d000000)
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

void isaac_vita_log(const char *format, ...)
{
    va_list arguments;

    va_start(arguments, format);
    (void)vsnprintf(s_last_log, sizeof s_last_log, format, arguments);
    va_end(arguments);
    ++s_log_calls;
}

/* ---- OpenAL stubs: every entry point counts itself. ------------------- */

void alSourcePlay(ALuint source)
{
    (void)source;
    ++s_al.any;
    ++s_al.other;
}

void alGetSourcei(ALuint source, ALenum parameter, ALint *value)
{
    (void)source;
    (void)parameter;
    ++s_al.any;
    ++s_al.other;
    if (value)
        *value = 0;
}

void alSourceStop(ALuint source)
{
    (void)source;
    ++s_al.any;
    ++s_al.source_stop;
}

void alGenBuffers(ALsizei count, ALuint *buffers)
{
    ALsizei index;

    ++s_al.any;
    ++s_al.gen_buffers;
    for (index = 0; index < count; ++index)
        buffers[index] = UINT32_C(0x7101) + (uint32_t)index;
}

void alSourceQueueBuffers(ALuint source, ALsizei count,
                          const ALuint *buffers)
{
    (void)source;
    (void)count;
    (void)buffers;
    ++s_al.any;
    ++s_al.other;
}

void alSourcePause(ALuint source)
{
    (void)source;
    ++s_al.any;
    ++s_al.other;
}

void alSourceUnqueueBuffers(ALuint source, ALsizei count, ALuint *buffers)
{
    (void)source;
    (void)count;
    ++s_al.any;
    ++s_al.other;
    if (buffers)
        buffers[0] = UINT32_C(0x7201);
}

ALCboolean alcMakeContextCurrent(ALCcontext *context)
{
    (void)context;
    ++s_al.any;
    ++s_al.make_current;
    return 1;
}

void alcDestroyContext(ALCcontext *context)
{
    (void)context;
    ++s_al.any;
    ++s_al.destroy_context;
}

ALCdevice *alcOpenDevice(const ALCchar *name)
{
    (void)name;
    ++s_al.any;
    ++s_al.open_device;
    return (ALCdevice *)(uintptr_t)MOCK_DEVICE;
}

void alcGetIntegerv(ALCdevice *device, ALCenum parameter, ALCsizei size,
                    ALCint *values)
{
    (void)device;
    ++s_al.any;
    ++s_al.get_integer;
    if (!values || size < 1)
        return;
    if (parameter == ALC_MONO_SOURCES)
        values[0] = 79;
    else if (parameter == ALC_STEREO_SOURCES)
        values[0] = 1;
}

ALCcontext *alcCreateContext(ALCdevice *device, const ALCint *attributes)
{
    (void)device;
    (void)attributes;
    ++s_al.any;
    ++s_al.create_context;
    return (ALCcontext *)(uintptr_t)MOCK_CONTEXT;
}

ALCboolean alcCloseDevice(ALCdevice *device)
{
    (void)device;
    ++s_al.any;
    ++s_al.close_device;
    return s_mock_close_device_result;
}

void alListener3f(ALenum parameter, ALfloat x, ALfloat y, ALfloat z)
{
    (void)parameter;
    (void)x;
    (void)y;
    (void)z;
    ++s_al.any;
    ++s_al.other;
}

void alSourcei(ALuint source, ALenum parameter, ALint value)
{
    (void)source;
    (void)parameter;
    (void)value;
    ++s_al.any;
    ++s_al.other;
}

void alcProcessContext(ALCcontext *context)
{
    (void)context;
    ++s_al.any;
    ++s_al.process_context;
}

void alGetSourcef(ALuint source, ALenum parameter, ALfloat *value)
{
    (void)source;
    (void)parameter;
    ++s_al.any;
    ++s_al.other;
    if (value)
        *value = 0.0f;
}

void alGenSources(ALsizei count, ALuint *sources)
{
    ALsizei index;

    ++s_al.any;
    ++s_al.gen_sources;
    for (index = 0; index < count; ++index)
        sources[index] = UINT32_C(0x7301) + (uint32_t)index;
}

ALenum alGetError(void)
{
    ++s_al.any;
    ++s_al.other;
    return AL_NO_ERROR;
}

void alDeleteBuffers(ALsizei count, const ALuint *buffers)
{
    (void)count;
    (void)buffers;
    ++s_al.any;
    ++s_al.delete_buffers;
}

void alListenerfv(ALenum parameter, const ALfloat *values)
{
    (void)parameter;
    (void)values;
    ++s_al.any;
    ++s_al.other;
}

void alDeleteSources(ALsizei count, const ALuint *sources)
{
    (void)count;
    (void)sources;
    ++s_al.any;
    ++s_al.delete_sources;
}

void alSource3f(ALuint source, ALenum parameter,
                ALfloat x, ALfloat y, ALfloat z)
{
    (void)source;
    (void)parameter;
    (void)x;
    (void)y;
    (void)z;
    ++s_al.any;
    ++s_al.other;
}

void alSourcef(ALuint source, ALenum parameter, ALfloat value)
{
    (void)source;
    (void)parameter;
    (void)value;
    ++s_al.any;
    ++s_al.other;
}

void alBufferData(ALuint buffer, ALenum format, const ALvoid *data,
                  ALsizei size, ALsizei frequency)
{
    (void)buffer;
    (void)format;
    (void)data;
    (void)size;
    (void)frequency;
    ++s_al.any;
    ++s_al.other;
}

/* ---- Fixture helpers. ------------------------------------------------- */

static void reset_census(void)
{
    memset(&s_al, 0, sizeof s_al);
    s_log_calls = 0U;
    s_last_log[0] = '\0';
}

static uint32_t manager_address(void)
{
    return s_base + MANAGER_OFFSET;
}

static void initialize_owner(void)
{
    uint32_t manager = manager_address();
    uint32_t al_error = UINT32_MAX;
    int status;

    memset((void *)(uintptr_t)manager, 0xcc, MANAGER_BYTES);
    reset_census();
    status = isaac_vita_audio_manager_initialize(manager, &al_error);
    CHECK(status == ISAAC_VITA_AUDIO_MANAGER_OK && al_error == AL_NO_ERROR,
          "fixture manager initialization did not succeed");
    CHECK(isaac_vita_audio_manager_is_active() == 1,
          "fixture manager did not publish an active owner");
    CHECK(ld32(manager + ISAAC_VITA_AUDIO_MANAGER_DEVICE_OFFSET) ==
              MOCK_DEVICE &&
              ld32(manager + ISAAC_VITA_AUDIO_MANAGER_CONTEXT_OFFSET) ==
              MOCK_CONTEXT,
          "fixture manager device/context words drifted");
}

/* Drive one imported cdecl adapter exactly as generated code does: return
 * word at ESP, the single argument above it. */
static void invoke_import(const char *name, uint32_t argument)
{
    CPU cpu;
    uint32_t stack = s_base + STACK_OFFSET;
    unsigned calls = 0U;

    memset(&cpu, 0, sizeof cpu);
    st32(stack, UINT32_C(0x12345678));
    st32(stack + 4U, argument);
    cpu.esp = stack;
    CHECK(isaac_vita_audio_import_counted(&cpu, name, &calls) == 1,
          "known OpenAL import was not handled");
    CHECK(cpu.fault == NULL, "OpenAL adapter raised an unexpected fault");
    CHECK(cpu.esp == stack + 4U,
          "OpenAL cdecl adapter did not pop exactly the return address");
}

static void expect_abort_log(const char *expected)
{
    CHECK(s_log_calls == 1U, "abort skip did not log exactly one line");
    if (strcmp(s_last_log, expected) != 0) {
        fprintf(stderr, "  expected: %s\n  actual:   %s\n", expected,
                s_last_log);
        CHECK(0, "abort skip receipt text drifted");
    }
}

/* ---- Tests. ----------------------------------------------------------- */

static void test_no_owner(void)
{
    reset_census();
    CHECK(isaac_vita_audio_abort_skip_teardown() ==
              ISAAC_VITA_AUDIO_SHUTDOWN_NONE,
          "abort skip without an owner did not report NONE");
    CHECK(s_al.any == 0U, "abort skip without an owner called OpenAL");
    CHECK(s_log_calls == 0U, "abort skip without an owner logged");
    CHECK(isaac_vita_audio_manager_is_active() == 0,
          "abort skip without an owner published an owner");
}

static void test_full_owner_then_exit_path(void)
{
    uint32_t manager = manager_address();
    unsigned char before[MANAGER_BYTES];
    char expected[256];
    al_census after_init;

    initialize_owner();
    after_init = s_al;
    memcpy(before, (const void *)(uintptr_t)manager, MANAGER_BYTES);
    reset_census();

    /* Abort stop: nothing native, nothing guest, one receipt. */
    CHECK(isaac_vita_audio_abort_skip_teardown() ==
              ISAAC_VITA_AUDIO_SHUTDOWN_SKIPPED,
          "abort skip with a live owner did not report SKIPPED");
    CHECK(s_al.any == 0U,
          "abort skip with a live owner called an OpenAL entry point");
    CHECK(isaac_vita_audio_manager_is_active() == 1,
          "abort skip changed the owner record");
    CHECK(memcmp(before, (const void *)(uintptr_t)manager,
                 MANAGER_BYTES) == 0,
          "abort skip wrote guest Sound::Manager bytes");
    (void)snprintf(expected, sizeof expected,
                   "KAGE VITA AUDIO ABORT: owner=0x%08x context=1 device=1 "
                   "names=1/1 (OpenAL teardown skipped; process exit "
                   "retires the device)",
                   (unsigned)manager);
    expect_abort_log(expected);

    /* A second call is the same no-op (the tail calls it once; this pins
     * that the skip is stateless). */
    reset_census();
    CHECK(isaac_vita_audio_abort_skip_teardown() ==
              ISAAC_VITA_AUDIO_SHUTDOWN_SKIPPED &&
              s_al.any == 0U && s_log_calls == 1U &&
              memcmp(before, (const void *)(uintptr_t)manager,
                     MANAGER_BYTES) == 0,
          "repeated abort skip was not a stateless no-op");

    /* The Exit game path after a skip: the unchanged full teardown with
     * exactly the frozen census (sources, buffers, current NULL, destroy,
     * close). */
    reset_census();
    CHECK(isaac_vita_audio_shutdown_active() ==
              ISAAC_VITA_AUDIO_SHUTDOWN_COMPLETE,
          "full teardown after an abort skip did not complete");
    CHECK(s_al.delete_sources == 1U && s_al.delete_buffers == 1U &&
              s_al.make_current == 1U && s_al.destroy_context == 1U &&
              s_al.close_device == 1U && s_al.any == 5U,
          "full teardown census after an abort skip drifted");
    CHECK(isaac_vita_audio_manager_is_active() == 0,
          "full teardown after an abort skip retained the owner");
    CHECK(ld32(manager + ISAAC_VITA_AUDIO_MANAGER_DEVICE_OFFSET) == 0U &&
              ld32(manager + ISAAC_VITA_AUDIO_MANAGER_CONTEXT_OFFSET) == 0U &&
              ld32(manager + ISAAC_VITA_AUDIO_MANAGER_SOURCES_OFFSET) == 0U &&
              ld32(manager + ISAAC_VITA_AUDIO_MANAGER_BUFFERS_OFFSET) == 0U,
          "full teardown after an abort skip left guest OpenAL words");
    CHECK(after_init.open_device == 1U && after_init.create_context == 1U &&
              after_init.make_current == 1U && after_init.gen_sources == 1U &&
              after_init.gen_buffers == 1U,
          "fixture initialization census drifted");

    /* No owner any more: back to NONE, silently. */
    reset_census();
    CHECK(isaac_vita_audio_abort_skip_teardown() ==
              ISAAC_VITA_AUDIO_SHUTDOWN_NONE &&
              s_al.any == 0U && s_log_calls == 0U,
          "abort skip after a complete teardown did not report NONE");
}

static void test_partial_owner(void)
{
    uint32_t manager = manager_address();
    unsigned char before[MANAGER_BYTES];
    char expected[256];

    /* The guest destroyed its context through the import (Sound::Manager
     * shutdown half done), then faulted: the device is still owned. */
    initialize_owner();
    reset_census();
    invoke_import(ISAAC_VITA_AUDIO_DESTROY_CONTEXT_NAME, MOCK_CONTEXT);
    CHECK(s_al.destroy_context == 1U && s_al.any == 1U,
          "imported alcDestroyContext census drifted");
    CHECK(isaac_vita_audio_manager_is_active() == 0,
          "partial owner is still materialisation-ready");
    CHECK(ld32(manager + ISAAC_VITA_AUDIO_MANAGER_CONTEXT_OFFSET) == 0U &&
              ld32(manager + ISAAC_VITA_AUDIO_MANAGER_DEVICE_OFFSET) ==
              MOCK_DEVICE,
          "partial owner guest words drifted");
    memcpy(before, (const void *)(uintptr_t)manager, MANAGER_BYTES);

    reset_census();
    CHECK(isaac_vita_audio_abort_skip_teardown() ==
              ISAAC_VITA_AUDIO_SHUTDOWN_SKIPPED,
          "abort skip with a partial owner did not report SKIPPED");
    CHECK(s_al.any == 0U, "abort skip with a partial owner called OpenAL");
    CHECK(memcmp(before, (const void *)(uintptr_t)manager,
                 MANAGER_BYTES) == 0,
          "abort skip with a partial owner wrote guest bytes");
    (void)snprintf(expected, sizeof expected,
                   "KAGE VITA AUDIO ABORT: owner=0x%08x context=0 device=1 "
                   "names=0/0 (OpenAL teardown skipped; process exit "
                   "retires the device)",
                   (unsigned)manager);
    expect_abort_log(expected);

    /* The ordinary teardown still closes only what is left. */
    reset_census();
    CHECK(isaac_vita_audio_shutdown_active() ==
              ISAAC_VITA_AUDIO_SHUTDOWN_COMPLETE,
          "partial owner teardown after an abort skip did not complete");
    CHECK(s_al.destroy_context == 0U && s_al.delete_sources == 0U &&
              s_al.delete_buffers == 0U && s_al.make_current == 0U &&
              s_al.close_device == 1U && s_al.any == 1U,
          "partial owner teardown census after an abort skip drifted");
}

static void test_retained_owner_after_failed_close(void)
{
    uint32_t manager = manager_address();
    char expected[256];

    /* A failed device close keeps the owner (INCOMPLETE); an abort skip on
     * that state is still silent towards OpenAL. */
    initialize_owner();
    s_mock_close_device_result = 0;
    reset_census();
    CHECK(isaac_vita_audio_shutdown_active() ==
              ISAAC_VITA_AUDIO_SHUTDOWN_INCOMPLETE,
          "failed device close did not report INCOMPLETE");
    CHECK(s_al.destroy_context == 1U && s_al.close_device == 1U,
          "failed device close census drifted");

    reset_census();
    CHECK(isaac_vita_audio_abort_skip_teardown() ==
              ISAAC_VITA_AUDIO_SHUTDOWN_SKIPPED,
          "abort skip with a retained device did not report SKIPPED");
    CHECK(s_al.any == 0U, "abort skip with a retained device called OpenAL");
    (void)snprintf(expected, sizeof expected,
                   "KAGE VITA AUDIO ABORT: owner=0x%08x context=0 device=1 "
                   "names=0/0 (OpenAL teardown skipped; process exit "
                   "retires the device)",
                   (unsigned)manager);
    expect_abort_log(expected);

    s_mock_close_device_result = 1;
    reset_census();
    CHECK(isaac_vita_audio_shutdown_active() ==
              ISAAC_VITA_AUDIO_SHUTDOWN_COMPLETE &&
              s_al.close_device == 1U && s_al.any == 1U,
          "retry after an abort skip did not close the retained device");
    CHECK(isaac_vita_audio_manager_is_active() == 0 &&
              ld32(manager + ISAAC_VITA_AUDIO_MANAGER_DEVICE_OFFSET) == 0U,
          "retry after an abort skip left the owner");
}

int main(void)
{
    void *memory = map_guest_test_memory();

    if (!memory) {
        fprintf(stderr, "FAIL: could not map 32-bit guest oracle memory\n");
        return 2;
    }
    CHECK(isaac_vita_audio_manager_is_active() == 0,
          "fresh audio runtime started materialisation-ready");
    test_no_owner();
    test_full_owner_then_exit_path();
    test_partial_owner();
    test_retained_owner_after_failed_close();
    unmap_guest_test_memory(memory);
    reset_census();
    CHECK(isaac_vita_audio_abort_skip_teardown() ==
              ISAAC_VITA_AUDIO_SHUTDOWN_NONE && s_al.any == 0U,
          "inactive abort skip touched an unmapped guest image");
    CHECK(guest_stack_legacy_oracle_violation_calls() == 0U,
          "standalone oracle entered a guest-stack violation helper");

    if (s_failures != 0U) {
        fprintf(stderr, "Vita OpenAL abort teardown oracle: %u failure(s)\n",
                s_failures);
        return 1;
    }
    puts("Vita OpenAL abort teardown oracle: PASS (no owner -> NONE silent; "
         "full/partial/retained owner -> SKIPPED with zero OpenAL calls, "
         "guest bytes and owner record untouched, one receipt; the Exit "
         "game teardown afterwards keeps its frozen census)");
    return 0;
}
