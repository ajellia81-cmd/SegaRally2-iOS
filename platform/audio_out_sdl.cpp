// iOS port of audio_out.cpp: the game's sound through SDL3 audio instead of
// WASAPI. SDL asks for samples from its own thread; AudioStream resamples the
// 44.1 kHz frames of the emulation to the device's rate as before. Phones
// have two speakers (or headphones): surround is mixed down like Stereo.
#include "audio_out.h"

#include <SDL3/SDL.h>

#include <cstdio>
#include <vector>

namespace {
SDL_AudioStream* g_stream = nullptr;
std::vector<float> g_buf;
}  // namespace

bool AudioOut::start() {
    if (!(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        fprintf(stderr, "[audio] SDL audio: %s\n", SDL_GetError());
        return false;
    }
    SDL_AudioSpec dev{};
    int frames = 0;
    int rate = 48000;
    if (SDL_GetAudioDeviceFormat(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &dev, &frames) && dev.freq > 0) rate = dev.freq;
    stream_.set_output_rate(rate);
    SDL_AudioSpec spec{SDL_AUDIO_F32, 2, rate};
    auto feed = [](void* user, SDL_AudioStream* s, int additional, int) {
        auto* self = static_cast<AudioOut*>(user);
        int n = additional / int(2 * sizeof(float));
        if (n <= 0) return;
        if (g_buf.size() < size_t(n) * 2) g_buf.resize(size_t(n) * 2);
        auto mode = AudioStream::Speakers(self->speakers_.load());
        if (mode == AudioStream::Speakers::Surround) mode = AudioStream::Speakers::Stereo;
        self->stream_.pull(g_buf.data(), size_t(n), mode);
        SDL_PutAudioStreamData(s, g_buf.data(), n * int(2 * sizeof(float)));
    };
    g_stream = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, feed, this);
    if (!g_stream) {
        fprintf(stderr, "[audio] no output: %s\n", SDL_GetError());
        return false;
    }
    SDL_ResumeAudioStreamDevice(g_stream);
    running_ = true;
    printf("[audio] output at %d Hz, 2 channels (SDL)\n", rate);
    return true;
}

void AudioOut::stop() {
    running_ = false;
    if (g_stream) SDL_DestroyAudioStream(g_stream);
    g_stream = nullptr;
}

// (WASAPI only)
void AudioOut::run() {}

// iOS port: away from the app no sound at all (LiveContainer lets the app run
// on in the background: the game's sound and the menu music went on playing)
void menu_music_suspend(bool away);
void platform_audio_suspend(bool away) {
    if (g_stream) away ? SDL_PauseAudioStreamDevice(g_stream) : SDL_ResumeAudioStreamDevice(g_stream);
    menu_music_suspend(away);
}
bool AudioOut::play_default_device() { return false; }
