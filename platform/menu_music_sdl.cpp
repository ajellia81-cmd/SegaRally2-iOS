// iOS port of menu_music.cpp: the menus' music through SDL3 audio instead of
// Windows MCI. A WAV file (the game's tune decoded at launch, or your own
// third_party/menu/music.wav), looped, with the same fade and volume curve.
#include "menu_music.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

std::string g_menu_music_path = "data/menu_title_00L.wav";

namespace {
SDL_AudioStream* g_music = nullptr;
std::vector<uint8_t> g_pcm;   // the whole file, in its own format
SDL_AudioSpec g_spec{};
}  // namespace

bool MenuMusic::open() {
    if (!(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) && !SDL_InitSubSystem(SDL_INIT_AUDIO)) return false;
    for (const std::string& path : {std::string("third_party/menu/music.wav"), g_menu_music_path}) {
        Uint8* buf = nullptr;
        Uint32 len = 0;
        if (!SDL_LoadWAV(path.c_str(), &g_spec, &buf, &len)) continue;
        g_pcm.assign(buf, buf + len);
        SDL_free(buf);
        // Topped up as it plays: a loop
        auto loop = [](void*, SDL_AudioStream* s, int additional, int) {
            for (int guard = 0; guard < 4 && !g_pcm.empty() && SDL_GetAudioStreamQueued(s) < additional; guard++)
                SDL_PutAudioStreamData(s, g_pcm.data(), int(g_pcm.size()));
        };
        g_music = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &g_spec, loop, nullptr);
        if (!g_music) {
            fprintf(stderr, "[menu] could not play the music: %s\n", SDL_GetError());
            return false;
        }
        SDL_SetAudioStreamGain(g_music, 0);
        printf("[menu] music %s\n", path.c_str());
        open_ = true;
        return true;
    }
    return false;
}

void MenuMusic::close() {
    if (g_music) SDL_DestroyAudioStream(g_music);
    g_music = nullptr;
    open_ = playing_ = false;
}

void MenuMusic::update(bool want, int volume, double dt) {
    if (!open_) return;
    level_ = want ? std::min(1.0f, level_ + float(dt) / 1.0f) : std::max(0.0f, level_ - float(dt) / 0.5f);
    if (want && !playing_) {
        sent_ = -1;
        playing_ = SDL_ResumeAudioStreamDevice(g_music);
    }
    if (!playing_) return;
    if (level_ <= 0) {   // faded out: it waits where it was
        SDL_PauseAudioStreamDevice(g_music);
        playing_ = false;
        return;
    }
    float g = std::sqrt(std::clamp(float(volume) / 10.0f, 0.0f, 1.0f));
    int v = int(std::clamp(level_ * g, 0.0f, 1.0f) * 1000.0f + 0.5f);
    if (v != sent_) {
        SDL_SetAudioStreamGain(g_music, float(v) / 1000.0f);
        sent_ = v;
    }
}
