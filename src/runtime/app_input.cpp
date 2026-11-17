// Keyboard, gamepads and steering wheels: the car's controls (moved from app.cpp)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>

#include "app_internal.h"
#include "inputs.h"
#include "runtime.h"
#include "wheel.h"
#ifndef _WIN32
#include "touch_controls.h"
#ifdef __APPLE__
#include <SDL3/SDL_gamepad.h>   // (the vibration test)
#endif
#endif

bool g_menu_open = false;   // a menu has the controls
Gamepads g_pads;
int g_pad_rumble = 2;
#ifdef __APPLE__
void platform_crash_haptic(float strength, int where = 0);   // platform/ios_lifecycle.mm (where: 0 default, 1 handles, 2 left handle)
#endif

static int adc(float v) { return std::clamp(int(std::lround(v)), 0, 255); }

// The car on the keyboard: arrows steer (the wheel turns over ~0.3 s and
// comes back by itself), Up / Down are the accelerator and the brake, A / Z
// shift up / down, V is VIEW CHANGE and Space the hand brake. The pads and
// wheels add theirs after this (poll_gamepads)
template <class Down>
static void poll_car_keys(Inputs& in, Down down) {
    static double last = qpc_seconds();
    static float steer = 0;
    static bool up_was = false, down_was = false;
    double now = qpc_seconds();
    float dt = std::clamp(float(now - last), 0.0f, 0.05f);
    last = now;
    int dir = down(VK_RIGHT) - down(VK_LEFT);
    float target = float(dir);
    float rate = dir == 0 || (target > 0) != (steer > 0) ? 6.0f : 3.5f;   // back to the centre faster
    steer += std::clamp(target - steer, -rate * dt, rate * dt);
    in.steer = adc(128.0f + 127.0f * steer);
    in.accel = down(VK_UP) ? 0xFF : 0;
    in.brake = down(VK_DOWN) ? 0xFF : 0;
    bool up = down('A'), dn = down('Z');
    if (up && !up_was) in.gear = std::min(int(in.gear) + 1, 4);
    if (dn && !down_was) in.gear = std::max(int(in.gear) - 1, 1);
    up_was = up, down_was = dn;
    in.view = down('V');
    in.handbrake = down(VK_SPACE);
}

// Keyboard (only while the window is active): 5 = coin, 1 = start (6 / 2 the
// second pair), F2 = test, F3 = service, and the car's keys
void poll_keyboard(HWND hwnd) {
    Inputs& in = g_inputs;
    // Scripted input (--auto) and the unseen run to SELECT GAME take over the controls
    bool scripted = !g_auto.events.empty() || g_arcade_entry;
    if (GetForegroundWindow() != hwnd || scripted) {
        if (!scripted) in.release_car();   // in the background: nothing stays held
        return;
    }
    auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    if (g_menu_open) {
        in.test = in.service = false;
        in.coin[0] = in.coin[1] = in.start[0] = in.start[1] = false;
        in.release_car();
        return;
    }
    in.coin[0] = down('5');
    in.start[0] = down('1');
    in.coin[1] = down('6');
    in.start[1] = down('2');
    in.test = down(VK_F2);
    in.service = down(VK_F3);
    poll_car_keys(in, down);
}

// Gamepads (gamepad.h) and steering wheels (wheel.h), after the keyboard:
// what they press is added to it, the stronger steering of the two. The first
// pad connected drives; on any pad BACK is the coin, and START starts the
// game after a coin (within 30 s, once per coin) and otherwise asks for the
// pause menu. Returns true when a pad asks for it
bool poll_gamepads(HWND hwnd) {
    bool focused = GetForegroundWindow() == hwnd;
    g_pads.poll(g_pad_rumble, focused, g_menu_open);
#ifdef __APPLE__
    {   // (diagnosis, temporary) The vibration test: the left stick pressed in
        // starts nine steps, 1.5 s apart, each logged: 1 left handle (SDL low),
        // 2 right handle (SDL high), 3 both, 4 both pulsing, 5 both at 0.4,
        // 6-8 a knock on our own engine (default, both handles, left handle),
        // 9 SDL both again; then three of 2 s, 3 s apart, as a scrape along a
        // wall: 10 SDL both and a knock every 0.13 s (as before), 11 the same
        // with a knock every 0.4 s, 12 SDL both alone
        static double test_at = -1;
        static int test_step = 0;
        static bool was = false;
        int p = g_pads.first();
        bool down = p >= 0 && g_pads.button_down(p, SDL_GAMEPAD_BUTTON_LEFT_STICK);
        double now = qpc_seconds();
        if (down && !was && test_at < 0) {
            test_at = now;
            test_step = 0;
            printf("[vibtest] start\n");
        }
        was = down;
        if (test_at >= 0 && p >= 0) {
            double t = now - test_at;
            int step = t < 13.5 ? int(t / 1.5) + 1 : 10 + int((t - 13.5) / 3.0);
            double in = step <= 9 ? t - (step - 1) * 1.5 : t - 13.5 - (step - 10) * 3.0;
            if (step > 12) {
                test_at = -1;
                printf("[vibtest] end\n");
            } else {
                if (step != test_step) {
                    test_step = step;
                    printf("[vibtest] step %d\n", step);
                    if (step >= 6 && step <= 8) platform_crash_haptic(1.0f, step - 6);
                }
                if (step >= 10 && in < 2.0) {
                    g_pads.rumble(p, 1.0f, 1.0f, 60);
                    static double knock_at = -1;
                    double every = step == 10 ? 0.13 : 0.4;
                    if (step != 12 && (in < 0.02 || now - knock_at >= every)) {
                        knock_at = now;
                        platform_crash_haptic(0.75f, 0);
                    }
                }
                if (in < 0.6 && (step <= 5 || step == 9)) {
                    float lo = step == 2 ? 0.0f : 1.0f, hi = step == 1 ? 0.0f : 1.0f;
                    if (step == 4 && int(in / 0.06) % 2) lo = hi = 0.2f;
                    if (step == 5) lo = hi = 0.4f;
                    g_pads.rumble(p, lo, hi, 60);
                }
            }
            if (test_at >= 0) return false;   // (nothing else rumbles meanwhile)
        }
    }
#endif
    if (!focused || g_menu_open || !g_auto.events.empty()) return false;
    Inputs& in = g_inputs;
    auto keyboard_steer = [&] { return (float(in.steer) - 128.0f) / 127.0f; };
    if (const WheelState& w = wheel_state(); w.connected) {
        if (std::fabs(w.steer) >= std::fabs(keyboard_steer())) in.steer = adc(128.0f + 127.0f * w.steer);
        in.accel = std::max(int(in.accel), adc(255.0f * w.gas));
        in.brake = std::max(int(in.brake), adc(255.0f * w.brake));
        if (w.gear >= 0) in.gear = w.gear;   // H shifter: where the stick is (0 = neutral)
        if (w.shift_up) in.gear = std::min(std::max(int(in.gear), 1) + 1, 4);
        if (w.shift_down) in.gear = std::max(int(in.gear) - 1, 1);
        if (w.view) in.view = true;
        if (w.handbrake) in.handbrake = true;
        if (w.coin) press_coin();
        if (w.start) press_start();
    }
    bool pause = false, driver = true;   // the first connected pad is the driver's
    for (int i = 0; i < Gamepads::MAX; i++) {
        const PadState& pad = g_pads.pad(i);
        if (!pad.connected) continue;
        if (driver) {
            if (std::fabs(pad.steer) > std::fabs(keyboard_steer())) in.steer = adc(128.0f + 127.0f * pad.steer);
            in.accel = std::max(int(in.accel), adc(255.0f * pad.gas));
            in.brake = std::max(int(in.brake), adc(255.0f * pad.brake));
            if (pad.shift_up) in.gear = std::min(int(in.gear) + 1, 4);
            if (pad.shift_down) in.gear = std::max(int(in.gear) - 1, 1);
            if (pad.view) in.view = true;
            if (pad.handbrake) in.handbrake = true;
            static const bool no_ffb = getenv("SR2_NO_FFB_RUMBLE") != nullptr;
            if (!no_ffb) {   // the wheel's force feedback (runtime.cpp ffb_frame) as rumble
                float sh = std::min(1.0f, g_ffb_shake.exchange(0.0f) * 2.5f);
                if (sh > 0.08f) g_pads.rumble(i, sh, 0.6f * sh, 60);
#ifdef __APPLE__
                {   // A crash sound: a knock of its own in the controller (platform/ios_lifecycle.mm),
                    // at most one every 0.35 s (a slow knock is several sounds at once, and a scrape
                    // along a wall one every few frames: a knock started over the last one
                    // before it was through, several a second, was not felt)
                    static uint32_t seen = g_crash_knocks.load();
                    static double last = -1;
                    uint32_t knocks = g_crash_knocks.load();
                    if (knocks != seen) {
                        seen = knocks;
                        float st = g_crash_knock_strength.exchange(0.0f);
                        double now = qpc_seconds();
                        static const float level[4] = {0.0f, 0.45f, 0.75f, 1.0f};
                        if (now - last > 0.35 && g_pad_rumble > 0) {
                            last = now;
                            platform_crash_haptic(st * level[std::clamp(g_pad_rumble, 0, 3)]);
                        }
                    }
                }
#endif
#ifdef __APPLE__
                {   // (diagnosis, temporary: the strong rumbles sent, at most 4 a second)
                    static double logged = 0;
                    double now = qpc_seconds();
                    if (sh > 0.3f && now - logged > 0.25) {
                        logged = now;
                        printf("[rumble] pad %d strength %.2f (vibration setting %d)\n", i, sh, g_pad_rumble);
                    }
                }
#endif
            }
            driver = false;
        }
        static double coin_at = -1;
        if (pad.back) {
            press_coin();
            coin_at = qpc_seconds();
        }
        if (pad.start) {
            if (coin_at >= 0 && qpc_seconds() - coin_at < 30) {
                press_start();
                coin_at = -1;
            } else {
                pause = true;
            }
        }
    }
#ifndef _WIN32
    g_touch.apply(in);   // iOS port: the on-screen controls (hidden, and idle, with a pad connected)
#endif
    return pause;
}
