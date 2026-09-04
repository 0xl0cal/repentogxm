#ifndef ISAAC_VITA_AUDIO_TEST_OPENAL_H
#define ISAAC_VITA_AUDIO_TEST_OPENAL_H

#include <stdint.h>

typedef int32_t ALenum;
typedef int32_t ALint;
typedef uint32_t ALuint;
typedef int32_t ALsizei;
typedef float ALfloat;
typedef void ALvoid;
typedef char ALCboolean;
typedef char ALCchar;
typedef int32_t ALCenum;
typedef int32_t ALCint;
typedef int32_t ALCsizei;
typedef struct ALCdevice_struct ALCdevice;
typedef struct ALCcontext_struct ALCcontext;

#define AL_NO_ERROR    0
#define AL_POSITION    0x1004
#define AL_ORIENTATION 0x100f
#define ALC_MONO_SOURCES   0x1010
#define ALC_STEREO_SOURCES 0x1011

void alSourcePlay(ALuint source);
void alGetSourcei(ALuint source, ALenum parameter, ALint *value);
void alSourceStop(ALuint source);
void alGenBuffers(ALsizei count, ALuint *buffers);
void alSourceQueueBuffers(ALuint source, ALsizei count,
                          const ALuint *buffers);
void alSourcePause(ALuint source);
void alSourceUnqueueBuffers(ALuint source, ALsizei count, ALuint *buffers);
ALCboolean alcMakeContextCurrent(ALCcontext *context);
void alcDestroyContext(ALCcontext *context);
ALCdevice *alcOpenDevice(const ALCchar *name);
ALCcontext *alcCreateContext(ALCdevice *device, const ALCint *attributes);
ALCboolean alcCloseDevice(ALCdevice *device);
void alcGetIntegerv(ALCdevice *device, ALCenum parameter, ALCsizei size,
                    ALCint *values);
void alListener3f(ALenum parameter, ALfloat x, ALfloat y, ALfloat z);
void alSourcei(ALuint source, ALenum parameter, ALint value);
void alcProcessContext(ALCcontext *context);
void alGetSourcef(ALuint source, ALenum parameter, ALfloat *value);
void alGenSources(ALsizei count, ALuint *sources);
ALenum alGetError(void);
void alDeleteBuffers(ALsizei count, const ALuint *buffers);
void alListenerfv(ALenum parameter, const ALfloat *values);
void alDeleteSources(ALsizei count, const ALuint *sources);
void alSource3f(ALuint source, ALenum parameter,
                ALfloat x, ALfloat y, ALfloat z);
void alSourcef(ALuint source, ALenum parameter, ALfloat value);
void alBufferData(ALuint buffer, ALenum format, const ALvoid *data,
                  ALsizei size, ALsizei frequency);

#endif
