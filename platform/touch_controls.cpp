#include "touch_controls.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdio>
#include <cstring>

#include "inputs.h"

namespace {

// 5x7 letters for the buttons' labels (rows top to bottom, bit 4 = left)
struct Glyph {
    char c;
    uint8_t rows[7];
};
constexpr Glyph GLYPHS[] = {
    {'A', {0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}}, {'B', {0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E}},
    {'C', {0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E}}, {'D', {0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E}},
    {'E', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F}}, {'G', {0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F}},
    {'H', {0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11}}, {'I', {0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E}},
    {'K', {0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11}}, {'N', {0x11, 0x19, 0x15, 0x13, 0x11, 0x11, 0x11}},
    {'P', {0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10}}, {'R', {0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11}},
    {'S', {0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E}}, {'U', {0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}},
    {'V', {0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04}}, {'W', {0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A}},
    {'L', {0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F}}, {'X', {0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11}},
    {'M', {0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11}}, {'F', {0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10}},
    {'Y', {0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04}}, {'0', {0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E}},
    {'1', {0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E}}, {'2', {0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F}},
    {'3', {0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E}}, {'4', {0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02}},
    {'5', {0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E}}, {'6', {0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E}},
    {'7', {0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08}}, {'8', {0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E}},
    {'9', {0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C}},
    {'O', {0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E}}, {'T', {0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04}},
};

const uint8_t* glyph(char c) {
    for (const Glyph& g : GLYPHS)
        if (g.c == c) return g.rows;
    return nullptr;
}

// Premultiplied BGRA "over" onto the picture
struct Canvas {
    std::vector<uint32_t>& px;
    int w, h;
    void blend(int x, int y, uint32_t rgb, float a) {
        if (x < 0 || y < 0 || x >= w || y >= h || a <= 0) return;
        a = std::min(a, 1.0f);
        uint32_t& d = px[size_t(y) * size_t(w) + size_t(x)];
        float inv = 1 - a;
        auto ch = [&](int shift, float src) {
            float v = float((d >> shift) & 0xFF) * inv + src * a;
            return uint32_t(std::clamp(v, 0.0f, 255.0f)) << shift;
        };
        d = ch(0, float(rgb & 0xFF)) | ch(8, float((rgb >> 8) & 0xFF)) | ch(16, float((rgb >> 16) & 0xFF)) |
            ch(24, 255.0f);
    }
    // A disc, or a ring of that thickness, antialiased
    void circle(float cx, float cy, float r, uint32_t rgb, float a, float ring = 0) {
        int x0 = int(cx - r - 1), x1 = int(cx + r + 1), y0 = int(cy - r - 1), y1 = int(cy + r + 1);
        for (int y = y0; y <= y1; y++)
            for (int x = x0; x <= x1; x++) {
                float d = std::hypot(float(x) + 0.5f - cx, float(y) + 0.5f - cy);
                float cov = std::clamp(r - d + 0.5f, 0.0f, 1.0f);
                if (ring > 0) cov *= std::clamp(d - (r - ring) + 0.5f, 0.0f, 1.0f);
                blend(x, y, rgb, a * cov);
            }
    }
    void rect(float x0, float y0, float x1, float y1, uint32_t rgb, float a) {
        for (int y = int(y0); y < int(std::ceil(y1)); y++)
            for (int x = int(x0); x < int(std::ceil(x1)); x++) blend(x, y, rgb, a);
    }
    void triangle(float cx, float cy, float size, bool up, uint32_t rgb, float a) {
        int half = int(size);
        for (int dy = -half; dy <= half; dy++) {
            float t = float(dy + half) / float(2 * half);   // 0 at the top
            float width = (up ? t : 1 - t) * size;
            for (int dx = -int(width); dx <= int(width); dx++) blend(int(cx) + dx, int(cy) + dy, rgb, a);
        }
    }
    void text(const char* s, float cx, float cy, float scale, uint32_t rgb, float a) {
        int n = int(strlen(s));
        float cw = 6 * scale, x0 = cx - (cw * float(n) - scale) / 2, y0 = cy - 3.5f * scale;
        for (int i = 0; i < n; i++) {
            const uint8_t* g = glyph(s[i]);
            if (!g) continue;
            for (int row = 0; row < 7; row++)
                for (int col = 0; col < 5; col++)
                    if (g[row] & (0x10 >> col))
                        rect(x0 + float(i) * cw + float(col) * scale, y0 + float(row) * scale,
                             x0 + float(i) * cw + float(col + 1) * scale, y0 + float(row + 1) * scale, rgb, a);
        }
    }
};

}  // namespace

void TouchControls::set_window(int w, int h, Insets safe) {
    if (w == w_ && h == h_ && safe.left == safe_.left && safe.right == safe_.right && safe.top == safe_.top &&
        safe.bottom == safe_.bottom)
        return;
    w_ = w;
    h_ = h;
    safe_ = safe;
    layout();
    dirty_ = true;
}

void TouchControls::set_state(bool menu, bool menu_back, bool pad) {
    if (menu == menu_ && menu_back == menu_back_ && pad == pad_) return;
    if (menu != menu_ || pad != pad_) release_all();   // nothing stays held into a menu or onto a pad
    menu_ = menu;
    menu_back_ = menu_back;
    pad_ = pad;
    dirty_ = true;
}

void TouchControls::layout() {
    float W = float(w_), H = float(h_);
    float u = H / 10.0f;   // ~132 pixels on a 6.9" phone
    unit_ = u;
    float right = W - float(safe_.right), left = float(safe_.left), bottom = H - float(safe_.bottom);
    c_[GAS] = {right - 1.7f * u, bottom - 1.9f * u, 1.15f * u};
    c_[BRAKE] = {right - 4.3f * u, bottom - 1.45f * u, 0.9f * u};
    c_[UP] = {right - 0.9f * u, bottom - 4.55f * u, 0.55f * u};
    c_[DOWN] = {right - 2.35f * u, bottom - 4.55f * u, 0.55f * u};
    c_[HAND] = {right - 4.3f * u, bottom - 3.55f * u, 0.6f * u};
    c_[VIEW] = {right - 0.9f * u, bottom - 6.0f * u, 0.45f * u};
    c_[PAUSE] = {left + 0.75f * u, H * 0.42f, 0.45f * u};
    c_[BACK] = {left + 0.9f * u, float(safe_.top) + 0.8f * u, 0.55f * u};
    steer_range_ = 1.5f * u;   // a slide this far is full lock
    steer_split_ = W * 0.45f;  // the steering side
}

int TouchControls::hit(float x, float y) const {
    int best = -1;
    float best_d = 1e9f;
    for (int b = 0; b < COUNT; b++) {
        if (menu_ ? b != BACK || !menu_back_ : b == BACK) continue;
        float d = std::hypot(x - c_[b].x, y - c_[b].y);
        float reach = c_[b].r * (b == GAS || b == BRAKE ? 1.3f : 1.35f);
        if (d < reach && d / reach < best_d) best = b, best_d = d / reach;
    }
    return best;
}

TouchControls::Finger* TouchControls::finger(int64_t id, bool create) {
    for (Finger& f : fingers_)
        if (f.id == id) return &f;
    if (!create) return nullptr;
    for (Finger& f : fingers_)
        if (f.id < 0) {
            f = Finger{};
            f.id = id;
            return &f;
        }
    return nullptr;
}

void TouchControls::release_all() {
    for (Finger& f : fingers_) f = Finger{};
    for (bool& h : held_) h = false;
    steer_ = 0;
    dirty_ = true;
}

bool TouchControls::on_event(const SDL_Event& e, float x, float y) {
    if (!enabled_ || w_ <= 0) return false;
    // A mouse button: only the BACK button (the pointer works the menus as it is)
    if (e.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
        int b = hit(x, y);
        if (b == BACK || b == PAUSE) {
            escape_ = true;
            return true;
        }
        return false;
    }
    if (e.type != SDL_EVENT_FINGER_DOWN && e.type != SDL_EVENT_FINGER_MOTION && e.type != SDL_EVENT_FINGER_UP &&
        e.type != SDL_EVENT_FINGER_CANCELED)
        return false;
    int64_t id = int64_t(e.tfinger.fingerID);
    bool down = e.type == SDL_EVENT_FINGER_DOWN, up = e.type == SDL_EVENT_FINGER_UP || e.type == SDL_EVENT_FINGER_CANCELED;

    if (menu_) {   // the menus take every touch but BACK
        if (down && hit(x, y) == BACK) {
            escape_ = true;
            return true;
        }
        return false;
    }
    if (pad_) return true;   // a controller drives: the screen does nothing

    Finger* f = finger(id, down);
    if (!f) return true;
    if (down) {
        int b = hit(x, y);
        if (b == PAUSE) {
            escape_ = true;
            *f = Finger{};
            return true;
        }
        if (b >= 0) {
            f->button = b;
            if (b == UP) up_edge_ = true;
            if (b == DOWN) down_edge_ = true;
        } else if (x < steer_split_) {
            f->steering = true;
            f->origin = f->x = x;
        }
    } else if (up) {
        *f = Finger{};
    } else {   // moved
        if (f->steering) {
            f->x = x;
            // The centre follows a thumb that slides past full lock, so coming
            // back always turns the other way at once
            if (x - f->origin > steer_range_) f->origin = x - steer_range_;
            if (f->origin - x > steer_range_) f->origin = x + steer_range_;
        } else if (f->button == GAS || f->button == BRAKE || f->button < 0) {
            int b = hit(x, y);   // slide between the pedals
            if (b == GAS || b == BRAKE) f->button = b;
        }
    }
    // What is held now
    bool held[COUNT] = {};
    float steer = 0;
    bool steering = false;
    for (const Finger& g : fingers_) {
        if (g.id < 0) continue;
        if (g.button >= 0) held[g.button] = true;
        if (g.steering) {
            steer = std::clamp((g.x - g.origin) / steer_range_, -1.0f, 1.0f);
            steering = true;
        }
    }
    steer_ = steering ? steer : 0;
    int steer_px = int(steer_ * 40);
    if (memcmp(held, held_, sizeof held) != 0 || steer_px != last_steer_px_) dirty_ = true;
    memcpy(held_, held, sizeof held);
    last_steer_px_ = steer_px;
    return true;
}

void TouchControls::apply(Inputs& in) {
    if (!driving()) return;
    // A little curve: finer near the centre, full lock at the end
    float s = steer_ * (0.55f + 0.45f * std::fabs(steer_));
    if (std::fabs(s) > std::fabs((float(in.steer) - 128.0f) / 127.0f))
        in.steer = std::clamp(int(std::lround(128.0f + 127.0f * s)), 0, 255);
    if (held_[GAS]) in.accel = 0xFF;
    if (held_[BRAKE]) in.brake = 0xFF;
    if (held_[HAND]) in.handbrake = true;
    if (held_[VIEW]) in.view = true;
    if (up_edge_) in.gear = std::min(int(in.gear) + 1, 4);
    if (down_edge_) in.gear = std::max(int(in.gear) - 1, 1);
    up_edge_ = down_edge_ = false;
}

bool TouchControls::take_escape() {
    bool e = escape_;
    escape_ = false;
    return e;
}

bool TouchControls::draw(std::vector<uint32_t>& px, int& w, int& h) {
    extern std::atomic<int> g_sr2_debug_view;
    int dbg = g_sr2_debug_view.load();
    if (dbg != shown_debug_) dirty_ = true, shown_debug_ = dbg;
    if (!dirty_ || w_ <= 0) return false;
    dirty_ = false;
    w = std::max(1, w_ / 2);
    h = std::max(1, h_ / 2);
    px.assign(size_t(w) * size_t(h), 0);
    Canvas cv{px, w, h};
    if (shown_debug_ > 0) {
        static const char* NAMES[27] = {"", "VERTEX COLOR", "TEXTURE", "LIGHT", "LIT", "COAT", "FINAL", "NORMAL", "PARAMS",
                                        "VP PUSH", "VP PASSED", "TEX SLOTS", "PACK ONLY", "DIRT COLOR", "DIRT ALPHA",
                                        "RAW TEXEL", "BILINEAR", "NO SHINE", "PACK RAW", "TEX ID", "NO DEPTH PASS", "NO AO", "SOLID ID", "SOLID ONLY", "ALL RED", "ID NO SPLIT", "NO SHADOWS"};
        if (shown_debug_ >= 27) shown_debug_ = 0;
        char label[48];
        snprintf(label, sizeof label, "DEBUG %d %s", shown_debug_, NAMES[shown_debug_]);
        cv.rect(float(w) * 0.5f - 200, 4, float(w) * 0.5f + 200, 30, 0x000000, 0.7f);
        cv.text(label, float(w) * 0.5f, 17, 2.0f, 0xFFFF00, 1.0f);
    }
    if (!shown()) return true;   // nothing else
    const float k = 0.5f;   // window pixels -> picture pixels
    auto button = [&](int b, const char* label, uint32_t tint) {
        const Circle& c = c_[b];
        bool on = held_[b];
        cv.circle(c.x * k, c.y * k, c.r * k, on ? tint : 0x101010, on ? 0.55f : 0.28f);
        cv.circle(c.x * k, c.y * k, c.r * k, 0xFFFFFF, on ? 0.9f : 0.55f, std::max(1.5f, c.r * k * 0.06f));
        if (label) cv.text(label, c.x * k, c.y * k, std::max(1.0f, c.r * k / 11.0f), 0xFFFFFF, on ? 1.0f : 0.8f);
    };
    if (menu_) {
        const Circle& c = c_[BACK];
        cv.circle(c.x * k, c.y * k, c.r * k, 0x101010, 0.45f);
        cv.circle(c.x * k, c.y * k, c.r * k, 0xFFFFFF, 0.7f, std::max(1.5f, c.r * k * 0.06f));
        cv.text("BACK", c.x * k, c.y * k, std::max(1.0f, c.r * k / 13.0f), 0xFFFFFF, 0.9f);
        return true;
    }
    button(GAS, "GAS", 0x2EB82E);
    button(BRAKE, "BRAKE", 0xC8241E);
    button(HAND, "HAND", 0xE08A1E);
    button(VIEW, "VIEW", 0x3C78D8);
    button(UP, nullptr, 0x3C78D8);
    button(DOWN, nullptr, 0x3C78D8);
    cv.triangle(c_[UP].x * k, c_[UP].y * k, c_[UP].r * k * 0.45f, true, 0xFFFFFF, 0.9f);
    cv.triangle(c_[DOWN].x * k, c_[DOWN].y * k, c_[DOWN].r * k * 0.45f, false, 0xFFFFFF, 0.9f);
    {   // pause: two bars
        const Circle& c = c_[PAUSE];
        cv.circle(c.x * k, c.y * k, c.r * k, 0x101010, 0.28f);
        cv.circle(c.x * k, c.y * k, c.r * k, 0xFFFFFF, 0.5f, std::max(1.5f, c.r * k * 0.06f));
        float bw = c.r * k * 0.18f, bh = c.r * k * 0.5f;
        cv.rect(c.x * k - 2.2f * bw, c.y * k - bh, c.x * k - 0.8f * bw, c.y * k + bh, 0xFFFFFF, 0.85f);
        cv.rect(c.x * k + 0.8f * bw, c.y * k - bh, c.x * k + 2.2f * bw, c.y * k + bh, 0xFFFFFF, 0.85f);
    }
    {   // steering: where the thumb is, or a hint of where it goes
        const Finger* g = nullptr;
        for (const Finger& f : fingers_)
            if (f.id >= 0 && f.steering) g = &f;
        float cy = (float(h_) - float(safe_.bottom) - 1.9f * unit_) * k;
        float cx = g ? g->origin * k : (float(safe_.left) + 3.0f * unit_) * k;
        float half = steer_range_ * k;
        cv.rect(cx - half, cy - 3, cx + half, cy + 3, 0xFFFFFF, g ? 0.45f : 0.25f);
        cv.circle(cx - half, cy, 5, 0xFFFFFF, 0.4f);
        cv.circle(cx + half, cy, 5, 0xFFFFFF, 0.4f);
        float kx = cx + steer_ * half;
        cv.circle(kx, cy, 0.55f * unit_ * k, g ? 0xF5C400 : 0x101010, g ? 0.6f : 0.28f);
        cv.circle(kx, cy, 0.55f * unit_ * k, 0xFFFFFF, g ? 0.9f : 0.5f, 2.0f);
        if (!g) cv.text("STEER", cx, cy - 0.95f * unit_ * k, 2.0f, 0xFFFFFF, 0.55f);
    }
    return true;
}
