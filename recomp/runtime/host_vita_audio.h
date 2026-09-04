#ifndef ISAAC_HOST_VITA_AUDIO_H
#define ISAAC_HOST_VITA_AUDIO_H

#include <stdint.h>

#include "guest.h"

/* Exact OpenAL32 import block in the frozen Repentance PE.  All entries use
 * the Win32 OpenAL cdecl ABI, so an adapter removes only the return address;
 * the translated caller removes its own arguments. */
#define ISAAC_VITA_AUDIO_FIRST_IAT_RVA 0x00606308U
#define ISAAC_VITA_AUDIO_LAST_IAT_RVA  0x00606364U
#define ISAAC_VITA_AUDIO_IMPORT_COUNT  24U

/* Frozen Sound::Manager layout used by Initialize/Shutdown/Suspend/Resume. */
#define ISAAC_VITA_AUDIO_MANAGER_RVA             0x007e7d40U
#define ISAAC_VITA_AUDIO_MANAGER_UPDATE_RVA      0x0056f040U
#define ISAAC_VITA_AUDIO_MANAGER_RUNNING_OFFSET  0x00000004U
#define ISAAC_VITA_AUDIO_MANAGER_SUSPENDED_OFFSET 0x00000005U
#define ISAAC_VITA_AUDIO_MANAGER_CONTEXT_OFFSET  0x00000030U
#define ISAAC_VITA_AUDIO_MANAGER_DEVICE_OFFSET   0x00000034U
#define ISAAC_VITA_AUDIO_MANAGER_ACTIVE_BEGIN_OFFSET 0x0000000cU
#define ISAAC_VITA_AUDIO_MANAGER_ACTIVE_END_OFFSET   0x00000010U
#define ISAAC_VITA_AUDIO_MANAGER_SOURCES_OFFSET  0x00000138U
#define ISAAC_VITA_AUDIO_MANAGER_BUFFERS_OFFSET  0x00000238U
#define ISAAC_VITA_AUDIO_MANAGER_SOURCE_COUNT    64U
#define ISAAC_VITA_AUDIO_MANAGER_BUFFER_COUNT    64U

/* Authenticated KAGE::Sound::StreamSourceOgg layout in the frozen PE.  The
 * diagnostic cooperative-pump receipt reads only these fields, immediately
 * before and after the same Manager::Update which dereferences the objects.
 * It never changes them and is absent unless its CMake A/B option is enabled. */
#define ISAAC_VITA_AUDIO_OGG_VTABLE_RVA          0x0076654cU
#define ISAAC_VITA_AUDIO_OGG_STOPPED_OFFSET      0x00000008U
#define ISAAC_VITA_AUDIO_OGG_LOOP_OFFSET         0x00000009U
#define ISAAC_VITA_AUDIO_OGG_SOURCE_OFFSET       0x00000030U
#define ISAAC_VITA_AUDIO_OGG_QUEUE_OFFSET        0x00000058U
#define ISAAC_VITA_AUDIO_OGG_QUEUE_STRIDE        0x0000000cU
#define ISAAC_VITA_AUDIO_OGG_QUEUE_COUNT         4U
#define ISAAC_VITA_AUDIO_OGG_EOF_OFFSET          0x0000008cU

/* The removed guest worker called Manager::Update and then slept for 5 ms.
 * Room-construction polls retain that exact minimum cadence.  Save parsing
 * has more than one second of measured OGG refill headroom, so its owner-thread
 * safe point uses a lower 20 Hz cadence instead of spending the parser's CPU
 * budget scanning the audio manager at up to 200 Hz. */
#define ISAAC_VITA_AUDIO_COOPERATIVE_INTERVAL_US UINT64_C(5000)
#define ISAAC_VITA_AUDIO_SAVE_COOPERATIVE_INTERVAL_US UINT64_C(50000)
#define ISAAC_VITA_AUDIO_COOPERATIVE_RETURN      0xfff56f40U

/* Sound::Manager permanently owns its frozen 64-name table.  Music streams,
 * the independent jingle actor, and Theora audio allocate names outside that
 * table, so the native device must expose a separate 16-source headroom. */
#define ISAAC_VITA_AUDIO_DEVICE_SOURCE_COUNT     80U
#define ISAAC_VITA_AUDIO_STREAM_SOURCE_HEADROOM \
    (ISAAC_VITA_AUDIO_DEVICE_SOURCE_COUNT - \
     ISAAC_VITA_AUDIO_MANAGER_SOURCE_COUNT)

enum isaac_vita_audio_manager_status {
    ISAAC_VITA_AUDIO_MANAGER_OK = 0,
    ISAAC_VITA_AUDIO_MANAGER_INVALID_ADDRESS = 1,
    ISAAC_VITA_AUDIO_MANAGER_OPEN_DEVICE_FAILED = 2,
    ISAAC_VITA_AUDIO_MANAGER_POINTER_OUT_OF_RANGE = 3,
    ISAAC_VITA_AUDIO_MANAGER_CREATE_CONTEXT_FAILED = 4,
    ISAAC_VITA_AUDIO_MANAGER_MAKE_CURRENT_FAILED = 5,
    ISAAC_VITA_AUDIO_MANAGER_AL_ERROR = 6,
    ISAAC_VITA_AUDIO_MANAGER_SOURCE_CAPACITY_MISMATCH = 7,
    /* A live or partially closed host owner already exists.  This rejection
     * is byte-exact: no manager field and no native OpenAL object is touched. */
    ISAAC_VITA_AUDIO_MANAGER_ALREADY_ACTIVE = 8
};

#define ISAAC_VITA_AUDIO_SOURCE_PLAY_NAME \
    "OpenAL32.dll!alSourcePlay"
#define ISAAC_VITA_AUDIO_SOURCE_PLAY_IAT_RVA 0x00606308U
#define ISAAC_VITA_AUDIO_GET_SOURCE_I_NAME \
    "OpenAL32.dll!alGetSourcei"
#define ISAAC_VITA_AUDIO_GET_SOURCE_I_IAT_RVA 0x0060630cU
#define ISAAC_VITA_AUDIO_SOURCE_STOP_NAME \
    "OpenAL32.dll!alSourceStop"
#define ISAAC_VITA_AUDIO_SOURCE_STOP_IAT_RVA 0x00606310U
#define ISAAC_VITA_AUDIO_GEN_BUFFERS_NAME \
    "OpenAL32.dll!alGenBuffers"
#define ISAAC_VITA_AUDIO_GEN_BUFFERS_IAT_RVA 0x00606314U
#define ISAAC_VITA_AUDIO_SOURCE_QUEUE_BUFFERS_NAME \
    "OpenAL32.dll!alSourceQueueBuffers"
#define ISAAC_VITA_AUDIO_SOURCE_QUEUE_BUFFERS_IAT_RVA 0x00606318U
#define ISAAC_VITA_AUDIO_SOURCE_PAUSE_NAME \
    "OpenAL32.dll!alSourcePause"
#define ISAAC_VITA_AUDIO_SOURCE_PAUSE_IAT_RVA 0x0060631cU
#define ISAAC_VITA_AUDIO_SOURCE_UNQUEUE_BUFFERS_NAME \
    "OpenAL32.dll!alSourceUnqueueBuffers"
#define ISAAC_VITA_AUDIO_SOURCE_UNQUEUE_BUFFERS_IAT_RVA 0x00606320U
#define ISAAC_VITA_AUDIO_MAKE_CONTEXT_CURRENT_NAME \
    "OpenAL32.dll!alcMakeContextCurrent"
#define ISAAC_VITA_AUDIO_MAKE_CONTEXT_CURRENT_IAT_RVA 0x00606324U
#define ISAAC_VITA_AUDIO_DESTROY_CONTEXT_NAME \
    "OpenAL32.dll!alcDestroyContext"
#define ISAAC_VITA_AUDIO_DESTROY_CONTEXT_IAT_RVA 0x00606328U
#define ISAAC_VITA_AUDIO_OPEN_DEVICE_NAME \
    "OpenAL32.dll!alcOpenDevice"
#define ISAAC_VITA_AUDIO_OPEN_DEVICE_IAT_RVA 0x0060632cU
#define ISAAC_VITA_AUDIO_CREATE_CONTEXT_NAME \
    "OpenAL32.dll!alcCreateContext"
#define ISAAC_VITA_AUDIO_CREATE_CONTEXT_IAT_RVA 0x00606330U
#define ISAAC_VITA_AUDIO_CLOSE_DEVICE_NAME \
    "OpenAL32.dll!alcCloseDevice"
#define ISAAC_VITA_AUDIO_CLOSE_DEVICE_IAT_RVA 0x00606334U
#define ISAAC_VITA_AUDIO_LISTENER_3F_NAME \
    "OpenAL32.dll!alListener3f"
#define ISAAC_VITA_AUDIO_LISTENER_3F_IAT_RVA 0x00606338U
#define ISAAC_VITA_AUDIO_SOURCE_I_NAME \
    "OpenAL32.dll!alSourcei"
#define ISAAC_VITA_AUDIO_SOURCE_I_IAT_RVA 0x0060633cU
#define ISAAC_VITA_AUDIO_PROCESS_CONTEXT_NAME \
    "OpenAL32.dll!alcProcessContext"
#define ISAAC_VITA_AUDIO_PROCESS_CONTEXT_IAT_RVA 0x00606340U
#define ISAAC_VITA_AUDIO_GET_SOURCE_F_NAME \
    "OpenAL32.dll!alGetSourcef"
#define ISAAC_VITA_AUDIO_GET_SOURCE_F_IAT_RVA 0x00606344U
#define ISAAC_VITA_AUDIO_GEN_SOURCES_NAME \
    "OpenAL32.dll!alGenSources"
#define ISAAC_VITA_AUDIO_GEN_SOURCES_IAT_RVA 0x00606348U
#define ISAAC_VITA_AUDIO_GET_ERROR_NAME \
    "OpenAL32.dll!alGetError"
#define ISAAC_VITA_AUDIO_GET_ERROR_IAT_RVA 0x0060634cU
#define ISAAC_VITA_AUDIO_DELETE_BUFFERS_NAME \
    "OpenAL32.dll!alDeleteBuffers"
#define ISAAC_VITA_AUDIO_DELETE_BUFFERS_IAT_RVA 0x00606350U
#define ISAAC_VITA_AUDIO_LISTENER_FV_NAME \
    "OpenAL32.dll!alListenerfv"
#define ISAAC_VITA_AUDIO_LISTENER_FV_IAT_RVA 0x00606354U
#define ISAAC_VITA_AUDIO_DELETE_SOURCES_NAME \
    "OpenAL32.dll!alDeleteSources"
#define ISAAC_VITA_AUDIO_DELETE_SOURCES_IAT_RVA 0x00606358U
#define ISAAC_VITA_AUDIO_SOURCE_3F_NAME \
    "OpenAL32.dll!alSource3f"
#define ISAAC_VITA_AUDIO_SOURCE_3F_IAT_RVA 0x0060635cU
#define ISAAC_VITA_AUDIO_SOURCE_F_NAME \
    "OpenAL32.dll!alSourcef"
#define ISAAC_VITA_AUDIO_SOURCE_F_IAT_RVA 0x00606360U
#define ISAAC_VITA_AUDIO_BUFFER_DATA_NAME \
    "OpenAL32.dll!alBufferData"
#define ISAAC_VITA_AUDIO_BUFFER_DATA_IAT_RVA 0x00606364U

/* These are the two frozen guest return sites whose only use of A005 is to
 * synchronously format and flush an error line before continuing normally.
 * The host adapter keeps first/power-of-two observations visible and folds the
 * duplicate storm to AL_NO_ERROR only at these exact sites. */
#define ISAAC_VITA_AUDIO_A005_IS_PLAYING_RETURN 0x005be811U
#define ISAAC_VITA_AUDIO_A005_SET_VOLUME_RETURN 0x005be8ffU

#define ISAAC_VITA_AUDIO_IMPORTS(X) \
    X(source_play, ISAAC_VITA_AUDIO_SOURCE_PLAY_NAME, \
      ISAAC_VITA_AUDIO_SOURCE_PLAY_IAT_RVA) \
    X(get_source_i, ISAAC_VITA_AUDIO_GET_SOURCE_I_NAME, \
      ISAAC_VITA_AUDIO_GET_SOURCE_I_IAT_RVA) \
    X(source_stop, ISAAC_VITA_AUDIO_SOURCE_STOP_NAME, \
      ISAAC_VITA_AUDIO_SOURCE_STOP_IAT_RVA) \
    X(gen_buffers, ISAAC_VITA_AUDIO_GEN_BUFFERS_NAME, \
      ISAAC_VITA_AUDIO_GEN_BUFFERS_IAT_RVA) \
    X(source_queue_buffers, ISAAC_VITA_AUDIO_SOURCE_QUEUE_BUFFERS_NAME, \
      ISAAC_VITA_AUDIO_SOURCE_QUEUE_BUFFERS_IAT_RVA) \
    X(source_pause, ISAAC_VITA_AUDIO_SOURCE_PAUSE_NAME, \
      ISAAC_VITA_AUDIO_SOURCE_PAUSE_IAT_RVA) \
    X(source_unqueue_buffers, \
      ISAAC_VITA_AUDIO_SOURCE_UNQUEUE_BUFFERS_NAME, \
      ISAAC_VITA_AUDIO_SOURCE_UNQUEUE_BUFFERS_IAT_RVA) \
    X(make_context_current, ISAAC_VITA_AUDIO_MAKE_CONTEXT_CURRENT_NAME, \
      ISAAC_VITA_AUDIO_MAKE_CONTEXT_CURRENT_IAT_RVA) \
    X(destroy_context, ISAAC_VITA_AUDIO_DESTROY_CONTEXT_NAME, \
      ISAAC_VITA_AUDIO_DESTROY_CONTEXT_IAT_RVA) \
    X(open_device, ISAAC_VITA_AUDIO_OPEN_DEVICE_NAME, \
      ISAAC_VITA_AUDIO_OPEN_DEVICE_IAT_RVA) \
    X(create_context, ISAAC_VITA_AUDIO_CREATE_CONTEXT_NAME, \
      ISAAC_VITA_AUDIO_CREATE_CONTEXT_IAT_RVA) \
    X(close_device, ISAAC_VITA_AUDIO_CLOSE_DEVICE_NAME, \
      ISAAC_VITA_AUDIO_CLOSE_DEVICE_IAT_RVA) \
    X(listener_3f, ISAAC_VITA_AUDIO_LISTENER_3F_NAME, \
      ISAAC_VITA_AUDIO_LISTENER_3F_IAT_RVA) \
    X(source_i, ISAAC_VITA_AUDIO_SOURCE_I_NAME, \
      ISAAC_VITA_AUDIO_SOURCE_I_IAT_RVA) \
    X(process_context, ISAAC_VITA_AUDIO_PROCESS_CONTEXT_NAME, \
      ISAAC_VITA_AUDIO_PROCESS_CONTEXT_IAT_RVA) \
    X(get_source_f, ISAAC_VITA_AUDIO_GET_SOURCE_F_NAME, \
      ISAAC_VITA_AUDIO_GET_SOURCE_F_IAT_RVA) \
    X(gen_sources, ISAAC_VITA_AUDIO_GEN_SOURCES_NAME, \
      ISAAC_VITA_AUDIO_GEN_SOURCES_IAT_RVA) \
    X(get_error, ISAAC_VITA_AUDIO_GET_ERROR_NAME, \
      ISAAC_VITA_AUDIO_GET_ERROR_IAT_RVA) \
    X(delete_buffers, ISAAC_VITA_AUDIO_DELETE_BUFFERS_NAME, \
      ISAAC_VITA_AUDIO_DELETE_BUFFERS_IAT_RVA) \
    X(listener_fv, ISAAC_VITA_AUDIO_LISTENER_FV_NAME, \
      ISAAC_VITA_AUDIO_LISTENER_FV_IAT_RVA) \
    X(delete_sources, ISAAC_VITA_AUDIO_DELETE_SOURCES_NAME, \
      ISAAC_VITA_AUDIO_DELETE_SOURCES_IAT_RVA) \
    X(source_3f, ISAAC_VITA_AUDIO_SOURCE_3F_NAME, \
      ISAAC_VITA_AUDIO_SOURCE_3F_IAT_RVA) \
    X(source_f, ISAAC_VITA_AUDIO_SOURCE_F_NAME, \
      ISAAC_VITA_AUDIO_SOURCE_F_IAT_RVA) \
    X(buffer_data, ISAAC_VITA_AUDIO_BUFFER_DATA_NAME, \
      ISAAC_VITA_AUDIO_BUFFER_DATA_IAT_RVA)

int isaac_vita_audio_import(CPU *__restrict c, const char *name);
int isaac_vita_audio_import_counted(CPU *__restrict c, const char *name,
                                    unsigned *call_count);
#ifdef ISAAC_VITA_AUDIO_ORACLE
void isaac_vita_audio_a005_test_reset(void);
#endif

/* Reserve the dedicated OpenAL USER_RW pool now, before vitaGL runs.  The
 * adapter keeps the single pre-OpenAL initialization/logging edge: this is
 * the same idempotent attempt that manager initialization makes, only
 * earlier.  vitaGL's stock profile hands every USER_RW byte above its 16 MiB
 * threshold to its RAM pool, and inside that threshold the pool's 0xbd6000
 * memblock competes with the texel scratch (also 0xbd6000) and loses: the
 * device measured 0x5aa000 free at the request, state=3 init=5, and the mixer
 * then shared newlib with the guest heap for the whole session.  Reserved
 * before vglInit*, the same bytes come out of the vitaGL RAM pool instead.
 * Returns 1 when the pool is READY, 0 for every fallback (pool compiled out,
 * disabled, or a native allocation failure); the manager's later attempt is
 * then a no-op.  Never touches guest memory. */
int isaac_vita_audio_openal_pool_reserve(void);

/* Native half of the frozen Sound::Manager initialization.  It owns only
 * OpenAL state: the manual boundary still constructs the guest mutexes and
 * 32-slot bookkeeping pool, and deliberately does not start the unsafe guest
 * update thread.  Zero is success; al_error receives the final AL error word. */
int isaac_vita_audio_manager_initialize(uint32_t manager,
                                        uint32_t *al_error);
void isaac_vita_audio_manager_close(uint32_t manager);
/* Host-owned truth for policies that are valid only while both the native
 * device and context are live.  A partially closed owner is deliberately not
 * materialisation-ready, but still rejects reinitialization until teardown
 * has retired its final native handle.  Never infer either state from mutable
 * guest manager bytes. */
int isaac_vita_audio_manager_is_active(void);
enum isaac_vita_audio_shutdown_status {
    ISAAC_VITA_AUDIO_SHUTDOWN_INCOMPLETE = -1,
    ISAAC_VITA_AUDIO_SHUTDOWN_NONE = 0,
    ISAAC_VITA_AUDIO_SHUTDOWN_COMPLETE = 1
};
/* Attempt to close the one host-owned manager while its fixed guest image is
 * still mapped.  This also claims a partial owner left by direct imported
 * alcDestroyContext/alcCloseDevice calls.  A failed device close stays owned
 * and is retried by the next call.  The no-owner path touches no guest memory.
 * Returns COMPLETE only when an owner was retired, INCOMPLETE when a native
 * handle remains owned, and NONE when there was no owner to close. */
int isaac_vita_audio_shutdown_active(void);
/* Emit the process-final OpenAL allocator snapshot after the last shutdown
 * attempt.  This is separate from shutdown_active so an incomplete close can
 * still be retried without consuming the one-shot final telemetry record. */
void isaac_vita_audio_log_final(void);
const char *isaac_vita_audio_manager_status_name(int status);

#endif
