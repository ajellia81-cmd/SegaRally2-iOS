// iOS port: on-screen controls for driving without a game controller.
//
//   left part of the screen   steering: put a thumb down anywhere and slide it
//                             left or right (analog, centred where it landed)
//   right part                GAS (large), BRAKE, gear UP / DOWN, HAND brake,
//                             VIEW change; a finger can slide between them
//   left edge                 pause (II): the pause menu
//   menus                     a BACK button (top left) does what Esc does
//
// They are hidden, and leave the screen to the game, while a game controller
// is connected; they come back when it goes away. Drawn on the CPU at half the
// window's size into a premultiplied BGRA picture for a UiOverlay.
#pragma once
#include <cstdint>
#include <functional>
#include <vector>

union SDL_Event;
struct Inputs;

class TouchControls {
public:
    struct Insets {
        int left = 0, top = 0, right = 0, bottom = 0;
    };

    // The window's size in pixels and its safe area
    void set_window(int w, int h, Insets safe);
    // menu: a menu has the screen (only the BACK button then, on screens that
    // have somewhere to go back to); pad: a game controller is connected
    void set_state(bool menu, bool menu_back, bool pad);

    // A touch (window pixels); true when it was the controls' (not a menu click)
    bool on_event(const SDL_Event& e, float x, float y);

    // The car's controls into the inputs, over what the keyboard set
    void apply(Inputs& in);
    // Pause / BACK pressed since the last call
    bool take_escape();

    bool driving() const { return enabled_ && !menu_ && !pad_; }
    bool shown() const { return enabled_ && (driving() || (menu_ && menu_back_)); }
    void set_enabled(bool on) { enabled_ = on; dirty_ = true; }

    // The picture, when it changed: w x h premultiplied BGRA (half the window)
    bool draw(std::vector<uint32_t>& px, int& w, int& h);

private:
    enum Button { GAS, BRAKE, UP, DOWN, HAND, VIEW, PAUSE, BACK, COUNT };
    struct Circle {
        float x = 0, y = 0, r = 0;
    };
    struct Finger {
        int64_t id = -1;
        bool steering = false;
        float origin = 0, x = 0;
        int button = -1;
    };
    void layout();
    int hit(float x, float y) const;
    Finger* finger(int64_t id, bool create);
    void release_all();

    int w_ = 0, h_ = 0;
    Insets safe_{};
    Circle c_[COUNT];
    float unit_ = 1, steer_range_ = 1, steer_split_ = 0;
    bool enabled_ = true, menu_ = false, menu_back_ = false, pad_ = false;
    Finger fingers_[10];
    float steer_ = 0;                 // -1..1
    bool held_[COUNT] = {};
    bool up_edge_ = false, down_edge_ = false, escape_ = false;
    bool dirty_ = true;
    int last_steer_px_ = 0;
};
