// The pads' rumble (vibration and Xbox trigger motors), worked out once a
// game frame from the player's car in the game's RAM (pad_rumble.cpp)
#pragma once
#include <atomic>

// What a pad's two motors feel since the pads last took it, 0..1: [0] the low
// (heavy) motor, [1] the high (buzz) one. app_input.cpp sends it
extern std::atomic<float> g_pad_motor[2];

// `arcade`: the shake of the arcade wheel's force feedback this frame (0..1,
// runtime.cpp ffb_frame); `attract`: the attract demo is on (no rumble)
void pad_rumble_frame(float arcade, bool attract);
