/* Link-only proof that the softfp VitaSDK OpenAL archive exports every frozen
 * PE entry with the signatures consumed by host_vita_audio.c. */
#ifndef AL_LIBTYPE_STATIC
#define AL_LIBTYPE_STATIC
#endif
#include <AL/al.h>
#include <AL/alc.h>
#include <stddef.h>

static volatile ALuint s_audio_link_sink;

int main(void)
{
    ALCdevice *device = alcOpenDevice(NULL);
    ALCcontext *context = alcCreateContext(device, NULL);
    ALuint sources[2] = { 0U, 0U };
    ALuint buffers[2] = { 0U, 0U };
    ALint state = 0;
    ALfloat gain = 0.0f;
    const ALfloat orientation[6] = {
        0.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f
    };
    const short pcm[8] = { 0 };

    (void)alcMakeContextCurrent(context);
    alcProcessContext(context);
    alGenSources(2, sources);
    alGenBuffers(2, buffers);
    alListener3f(AL_POSITION, 0.0f, 0.0f, 0.0f);
    alListenerfv(AL_ORIENTATION, orientation);
    alBufferData(buffers[0], AL_FORMAT_MONO16, pcm,
                 (ALsizei)sizeof pcm, 22050);
    alSourcei(sources[0], AL_BUFFER, (ALint)buffers[0]);
    alSourcef(sources[0], AL_GAIN, 1.0f);
    alSource3f(sources[0], AL_POSITION, 0.0f, 0.0f, 0.0f);
    alSourceQueueBuffers(sources[1], 1, &buffers[1]);
    alSourcePlay(sources[0]);
    alSourcePause(sources[0]);
    alGetSourcei(sources[0], AL_SOURCE_STATE, &state);
    alGetSourcef(sources[0], AL_GAIN, &gain);
    alSourceStop(sources[0]);
    alSourceUnqueueBuffers(sources[1], 1, &buffers[1]);
    s_audio_link_sink = (ALuint)alGetError() + (ALuint)state +
                        (ALuint)(gain != 0.0f);
    alDeleteSources(2, sources);
    alDeleteBuffers(2, buffers);
    alcDestroyContext(context);
    (void)alcCloseDevice(device);
    return (int)(s_audio_link_sink & 0U);
}
