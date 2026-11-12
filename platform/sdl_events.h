// iOS port: the window (vk_presenter_sdl.cpp) reads every SDL event; the game
// controllers' arrivals and departures are kept here for Gamepads::poll
#pragma once
union SDL_Event;
bool sr2_take_device_event(SDL_Event* e);

#include <atomic>
// The app went to the background since the last look (ios_lifecycle.mm; SDL's
// focus events elsewhere): app.cpp opens the pause menu
extern std::atomic<bool> g_sr2_backgrounded;
#ifdef __APPLE__
void platform_watch_lifecycle();
#endif
