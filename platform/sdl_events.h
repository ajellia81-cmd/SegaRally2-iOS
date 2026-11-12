// iOS port: the window (vk_presenter_sdl.cpp) reads every SDL event; the game
// controllers' arrivals and departures are kept here for Gamepads::poll
#pragma once
union SDL_Event;
bool sr2_take_device_event(SDL_Event* e);
