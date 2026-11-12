#include "r3d_dust.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>

namespace {

std::atomic<bool> g_enhanced{true};

constexpr float FRAME_S = 1.0f / 57.524f;   // one game frame
constexpr size_t MAX_PUFFS = 1800, MAX_CLODS = 1500;
constexpr size_t MAX_EMITTERS = 32;
constexpr float GRAVITY = 9.8f * 1.2f;   // units (metres) per second squared; a little heavy reads better at this scale
constexpr float RIBBON_FRAMES = 11.8f;   // the game's fan is this many frames of the wheel's way long

struct V3 {
    float x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float length(V3 a) { return std::sqrt(dot(a, a)); }
V3 normalize(V3 a, V3 or_else = {0, 0, 1}) {
    float l = length(a);
    return l > 1e-9f ? a * (1.0f / l) : or_else;
}
V3 load(const float* p) { return {p[0], p[1], p[2]}; }
void store(float* p, V3 a) { p[0] = a.x, p[1] = a.y, p[2] = a.z; }

uint32_t hash32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7FEB352Du;
    x ^= x >> 15;
    x *= 0x846CA68Bu;
    x ^= x >> 16;
    return x;
}

struct Rng {
    uint32_t s;
    float f() { return float((s = hash32(s + 0x9E3779B9u)) >> 8) * (1.0f / 16777216.0f); }
    float r(float a, float b) { return a + (b - a) * f(); }
};

// The world in view space: where its origin is and how its axes lie (y is
// up). The game builds the fans of dust in world coordinates, so the model
// that holds one gives it exactly
struct Frame {
    V3 o, x{1, 0, 0}, y{0, 1, 0}, z{0, 0, 1};
    bool ok = false;
    V3 dir(V3 w) const { return x * w.x + y * w.y + z * w.z; }
    V3 view(V3 w) const { return o + dir(w); }
    V3 local_dir(V3 v) const { return {dot(v, x), dot(v, y), dot(v, z)}; }
    V3 world(V3 v) const { return local_dir(v - o); }
};

enum Kind : uint32_t { PUFF = 0, CLOD = 1 };

struct Particle {
    Kind kind = PUFF;
    V3 p, v;          // world; units per second
    V3 before;        // where it was when last drawn (world)
    float age = 0, life = 1;
    float size = 0.3f, grow = 1.0f;   // a puff's radius at first and how much it gains; a clod's half size
    float drag = 1.5f, lift = 0;
    float alpha = 0.4f;
    float ground = 0;   // world height of the road where it was thrown up
    uint32_t color = 0xFFFFFFFFu, seed = 0;
    int bounces = 0;
    bool drop = false;   // a drop of water: gone when it lands
};

// A wheel raising dust: the tip of one of the game's fans, followed from frame to frame
struct Emitter {
    uint32_t id = 0;
    V3 tip, vel;        // world; units per game frame
    float carry = 0;    // way gone since its last puff
    float clods = 0;    // clods owed (fractions carried over)
    float drops = 0;    // ... and drops of water
    bool matched = false;
    int unseen = 0;
};

// One of the game's fans as drawn this frame
struct Ribbon {
    uint32_t viewport = 0;
    std::vector<uint32_t> tris;   // first vertex of each of its triangles
    std::vector<V3> path;         // along the ground from the wheel back (world)
    V3 rgb;                       // its tint, 0..1
    float strength = 1;           // its alpha at the wheel
    float length = 0;
    bool full = false;            // drawn out to its whole length (it grows from the wheel when the dirt begins)
    uint32_t debris = 0;          // the texture of the flying bits drawn with it (0: none)
    V3 tip_view;                  // the wheel's end in view space
    bool wet = false;             // the wheel is in a puddle: the game draws its splash sprites at it
};

std::vector<Particle> g_parts;
std::vector<Emitter> g_emitters;
Frame g_before;            // the world in the previous frame's view
V3 g_delta;                // the fans' world minus the tracker's (to go on while no fan is drawn)
bool g_have_delta = false;
uint64_t g_last_frame = 0;
uint32_t g_next_id = 1;

// The colours of a debris sprite's solid texels (ARGB1555), to colour the clods with
struct Bits {
    uint64_t generation = 0;
    std::vector<uint32_t> colors;
};
std::unordered_map<uint32_t, Bits> g_bits;

uint32_t rgba(V3 c, float a = 1) {
    auto q = [](float v) { return uint32_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    return q(c.x) | q(c.y) << 8 | q(c.z) << 16 | q(a) << 24;
}

const std::vector<uint32_t>& debris_colors(uint32_t tex, R3DTextureRam* ram) {
    static const std::vector<uint32_t> none;
    if (!ram || (tex >> 28) != 0) return none;
    int x = int(tex & 2047), y = int((tex >> 11) & 2047);
    int w = 32 << int((tex >> 22) & 7), h = 32 << int((tex >> 25) & 7);
    if (g_bits.size() > 64 && !g_bits.count(tex)) g_bits.clear();
    Bits& b = g_bits[tex];
    uint64_t gen = ram->texture_generation(x, y, w, h);
    if (b.generation != gen || b.colors.empty()) {
        std::vector<uint16_t> texels;
        b.generation = ram->read_texture(x, y, w, h, texels);
        b.colors.clear();
        for (uint16_t t : texels)
            if (!(t & 0x8000))
                b.colors.push_back(rgba({float((t >> 10) & 31) / 31.0f, float((t >> 5) & 31) / 31.0f, float(t & 31) / 31.0f}));
    }
    return b.colors;
}

bool is_fan(const R3DVertex& x) {
    const uint32_t need = R3D_TEXTURED | R3D_TEX_ALPHA | R3D_FIXED_SHADE;
    return (x.flags & (need | R3D_SPECULAR | R3D_LUMINOUS)) == need;
}

// A puddle's splash: a self-lit, alpha tested sprite of drops, many along the water's flight
bool is_splash(const R3DVertex& x) {
    const uint32_t need = R3D_TEXTURED | R3D_ALPHA_TEST | R3D_LUMINOUS;
    return (x.flags & (need | R3D_SPECULAR | R3D_TEX_ALPHA | R3D_FIXED_SHADE)) == need;
}

bool is_bit(const R3DVertex& x) {
    const uint32_t need = R3D_TEXTURED | R3D_ALPHA_TEST;
    return (x.flags & (need | R3D_SPECULAR | R3D_LUMINOUS | R3D_TEX_ALPHA | R3D_FIXED_SHADE)) == need;
}

// The dust's own colour from the game's tint of its cloud texture: the
// ground's colour, paler and less saturated, as dust in the air is
V3 dust_color(V3 tint) {
    float luma = dot(tint, V3{0.2126f, 0.7152f, 0.0722f});
    V3 c = tint * 0.42f + V3{luma, luma, luma} * 0.58f;
    float peak = std::max(c.x, std::max(c.y, c.z));
    float gain = peak > 1e-3f ? std::clamp(0.72f / peak, 1.0f, 1.7f) : 1.0f;
    c = c * gain;
    return c * 0.7f + V3{0.80f, 0.74f, 0.64f} * 0.3f;
}

void emit_puff(const Ribbon& r, V3 at, V3 wheel_vel, float speed, float back, float lod, Rng& rng) {
    if (g_parts.size() >= MAX_PUFFS + MAX_CLODS) return;
    size_t puffs = 0;
    for (const Particle& p : g_parts) puffs += p.kind == PUFF;
    if (puffs >= MAX_PUFFS) return;
    float fast = std::clamp(speed / 30.0f, 0.0f, 1.0f);   // 0 crawling .. 1 at 108 km/h and over
    V3 fwd = normalize(V3{wheel_vel.x, 0, wheel_vel.z}, {0, 0, 0});
    V3 side = cross(V3{0, 1, 0}, fwd);
    Particle p;
    p.kind = PUFF;
    p.ground = at.y;
    p.size = rng.r(0.22f, 0.36f) * (0.7f + 0.3f * fast) * lod;
    p.grow = rng.r(1.2f, 2.2f) * (0.45f + 0.55f * fast) * lod;
    p.p = at + V3{rng.r(-0.12f, 0.12f), p.size * 0.6f, rng.r(-0.12f, 0.12f)};
    // Drawn along by the car's wake at first (it boils up right behind the
    // car, not a car's length back), rolling out sideways and up
    p.v = wheel_vel * rng.r(0.25f, 0.75f) + side * rng.r(-3.0f, 3.0f) * (0.4f + 0.6f * fast) +
          V3{0, rng.r(1.0f, 6.0f) * (0.4f + 0.6f * fast), 0};
    p.drag = rng.r(3.5f, 5.5f);
    p.lift = rng.r(0.15f, 0.6f);
    p.life = rng.r(1.8f, 3.2f) * (0.45f + 0.55f * fast);
    // A slow wheel raises little (the game's fan is short and faint then): thin puffs,
    // or they pile up where the car crawls into a wall of dust
    p.alpha = r.strength * rng.r(0.8f, 1.0f) * std::clamp(speed / 28.0f, 0.12f, 1.0f);
    V3 c = dust_color(r.rgb) * rng.r(0.9f, 1.06f);
    p.color = rgba(c);
    if (r.wet) {
        // Spray: a pale mist thrown wide and high that falls back and is gone in a
        // moment, with a little of the puddle's mud in it. A slow wheel splashes too
        p.size = rng.r(0.25f, 0.45f) * lod;
        p.grow = rng.r(0.8f, 1.5f) * (0.5f + 0.5f * fast) * lod;
        p.v = wheel_vel * rng.r(0.3f, 0.8f) + side * rng.r(-4.5f, 4.5f) + V3{0, rng.r(2.5f, 8.5f) * (0.45f + 0.55f * fast), 0};
        p.drag = rng.r(3.0f, 4.5f);
        p.lift = -rng.r(0.5f, 1.2f);
        p.life = rng.r(0.5f, 1.0f);
        p.alpha = r.strength * rng.r(0.5f, 0.7f) * std::clamp(speed / 18.0f, 0.3f, 1.0f);
        V3 w = V3{0.84f, 0.86f, 0.88f} * 0.58f + dust_color(r.rgb) * 0.42f;
        p.color = rgba(w * rng.r(0.92f, 1.05f));
    }
    p.seed = hash32(rng.s);
    p.age = 0;
    // Its share of the frame already gone by
    p.p = p.p + p.v * back;
    p.age = back;
    p.before = p.p - p.v * FRAME_S;
    g_parts.push_back(p);
}

void emit_clod(const Ribbon& r, V3 at, V3 wheel_vel, float speed, float back, const std::vector<uint32_t>& colors, Rng& rng) {
    size_t clods = 0;
    for (const Particle& p : g_parts) clods += p.kind == CLOD;
    if (clods >= MAX_CLODS) return;
    float fast = std::clamp(speed / 30.0f, 0.0f, 1.0f);
    V3 fwd = normalize(V3{wheel_vel.x, 0, wheel_vel.z}, {0, 0, 0});
    V3 side = cross(V3{0, 1, 0}, fwd);
    Particle p;
    p.kind = CLOD;
    p.ground = at.y;
    p.size = rng.r(0.016f, 0.05f);
    if (rng.f() < 0.08f) p.size *= 1.8f;   // now and then a bigger one
    p.p = at + V3{rng.r(-0.1f, 0.1f), 0.04f, rng.r(-0.1f, 0.1f)};
    // Thrown back and up by the tread; some carried along with the car a moment
    p.v = wheel_vel * rng.r(0.1f, 0.55f) - fwd * rng.r(1.0f, 7.0f) * (0.4f + 0.6f * fast) + side * rng.r(-2.2f, 2.2f) +
          V3{0, rng.r(1.2f, 5.5f) * (0.5f + 0.5f * fast), 0};
    p.drag = 0.25f;
    p.life = rng.r(0.55f, 1.25f);
    p.alpha = 1;
    if (!colors.empty()) {
        uint32_t c = colors[size_t(rng.f() * float(colors.size())) % colors.size()];
        // (grass and earth are dark in the texture; a little of the ground's dust on them)
        V3 k{float(c & 255) / 255.0f, float((c >> 8) & 255) / 255.0f, float((c >> 16) & 255) / 255.0f};
        p.color = rgba((k * 0.8f + r.rgb * 0.2f) * 0.75f);
    } else {
        p.color = rgba(r.rgb * rng.r(0.35f, 0.7f));
    }
    p.seed = hash32(rng.s);
    p.p = p.p + p.v * back;
    p.age = back;
    p.before = p.p - p.v * FRAME_S;
    g_parts.push_back(p);
}

// A drop of a puddle's water: thrown up and out to both sides by the wheel,
// carried along with the car, falling in an arc
void emit_drop(const Ribbon& r, V3 at, V3 wheel_vel, float speed, float back, Rng& rng) {
    size_t clods = 0;
    for (const Particle& p : g_parts) clods += p.kind == CLOD;
    if (clods >= MAX_CLODS) return;
    float fast = std::clamp(speed / 30.0f, 0.0f, 1.0f);
    V3 fwd = normalize(V3{wheel_vel.x, 0, wheel_vel.z}, {0, 0, 0});
    V3 side = cross(V3{0, 1, 0}, fwd);
    Particle p;
    p.kind = CLOD;
    p.drop = true;
    p.ground = at.y;
    p.size = rng.r(0.006f, 0.017f);
    p.p = at + V3{rng.r(-0.12f, 0.12f), 0.05f, rng.r(-0.12f, 0.12f)};
    float out = rng.r(-1.0f, 1.0f);
    p.v = wheel_vel * rng.r(0.2f, 0.85f) - fwd * rng.r(0.0f, 3.0f) + side * (out * rng.r(2.0f, 7.5f)) +
          V3{0, rng.r(1.5f, 7.5f) * (0.6f + 0.4f * fast), 0};
    p.drag = 0.7f;
    p.life = rng.r(0.5f, 1.1f);
    // Muddy water catching the light: pale, some drops almost white
    V3 w = V3{0.74f, 0.76f, 0.77f} * 0.55f + dust_color(r.rgb) * 0.45f;
    p.color = rgba(w * rng.r(0.6f, 1.1f));
    p.seed = hash32(rng.s);
    p.p = p.p + p.v * back;
    p.age = back;
    p.before = p.p - p.v * FRAME_S;
    g_parts.push_back(p);
}

void step(Particle& p, float dt) {
    p.age += dt;
    if (p.kind == PUFF) {
        p.v = p.v * std::exp(-p.drag * dt);
        p.p = p.p + (p.v + V3{0, p.lift, 0}) * dt;
        return;
    }
    p.v = p.v * std::exp(-p.drag * dt);
    p.v.y -= GRAVITY * dt;
    p.p = p.p + p.v * dt;
    float floor = p.ground + p.size;
    if (p.p.y < floor && p.v.y < 0) {   // the road: a bounce or two, then it rolls to a stop
        p.p.y = floor;
        if (p.drop) p.life = std::min(p.life, p.age);
        p.bounces++;
        p.v = V3{p.v.x * 0.55f, -p.v.y * (p.bounces > 2 ? 0.0f : 0.32f), p.v.z * 0.55f};
        if (p.bounces > 2) p.life = std::min(p.life, p.age + 0.25f);
    }
}

// The wheel's way over one frame, and what it throws up along it. a: where
// the wheel was `frames` game frames ago, b: where it is (world)
void emit_along(Emitter& e, const Ribbon& r, V3 a, V3 b, float frames, float dist_eye, R3DTextureRam* ram, Rng& rng) {
    float dt = frames * FRAME_S;
    if (dt <= 0) return;
    V3 move = b - a;
    float dist = length(move);
    V3 vel = move * (1.0f / dt);   // units per second
    float speed = dist / dt;
    if (speed < 1.0f || speed > 150.0f) return;   // standing, or not one wheel's way (a cut)
    if (r.wet && dist_eye < 90.0f) {
        e.drops += r.strength * std::clamp(speed * 9.0f, 110.0f, 380.0f) * dt;
        while (e.drops >= 1.0f) {
            e.drops -= 1.0f;
            float f = rng.f();
            emit_drop(r, a + move * f, vel, speed, (1.0f - f) * dt, rng);
        }
    }
    // Far away, fewer and bigger puffs
    float lod = std::clamp(dist_eye / 45.0f, 1.0f, 3.0f);
    float spacing = (r.wet ? 0.42f : 0.55f) * lod;   // (water comes up thick)
    e.carry += dist;
    while (e.carry >= spacing) {
        e.carry -= spacing;
        float f = dist > 1e-6f ? 1.0f - e.carry / dist : 1.0f;   // how far along a -> b
        f = std::clamp(f, 0.0f, 1.0f);
        emit_puff(r, a + move * f, vel, speed, (1.0f - f) * dt, std::sqrt(lod), rng);
    }
    if (r.debris && dist_eye < 70.0f) {
        const std::vector<uint32_t>& colors = debris_colors(r.debris, ram);
        e.clods += r.strength * std::clamp(speed * 2.2f, 0.0f, 85.0f) * dt;
        while (e.clods >= 1.0f) {
            e.clods -= 1.0f;
            float f = rng.f();
            emit_clod(r, a + move * f, vel, speed, (1.0f - f) * dt, colors, rng);
        }
    }
}

R3DVertex vertex(V3 pos, V3 prev, V3 nrm, float u, float v, const Particle& p, float t, float extra, uint32_t color) {
    R3DVertex o{};
    store(o.pos, pos);
    store(o.prev, prev);
    store(o.nrm, nrm);
    o.uv[0] = u, o.uv[1] = v;
    o.color = color;
    o.tex = uint32_t(p.kind) | (p.seed & 0xFFFFu) << 8;
    o.flags = R3D_DUST | R3D_WORLD | R3D_NO_SHADOW;
    o.shade = t;
    o.fog = 1.0f;
    o.spec = extra;
    o.replace = 0;
    return o;
}

void draw(const Frame& f, const Frame& before, std::vector<R3DVertex>& out) {
    // The camera's own speed through the world (units per second)
    V3 eye_vel = before.ok ? (f.world(V3{}) - before.world(V3{})) * (1.0f / FRAME_S) : V3{};
    if (length(eye_vel) > 150.0f) eye_vel = {};   // a cut
    struct Order {
        float z;
        uint32_t i;
    };
    std::vector<Order> order;
    order.reserve(g_parts.size());
    for (uint32_t i = 0; i < g_parts.size(); i++) {
        V3 c = f.view(g_parts[i].p);
        if (c.z > -0.05f || c.z < -500.0f) continue;
        order.push_back({c.z, i});
    }
    // Farthest first: they are blended over each other
    std::sort(order.begin(), order.end(), [](const Order& a, const Order& b) { return a.z < b.z; });
    const int corners[6] = {0, 1, 2, 0, 2, 3};
    for (const Order& o : order) {
        const Particle& p = g_parts[o.i];
        V3 c = f.view(p.p);
        V3 was = before.ok ? before.view(p.before) : c;
        V3 delta = c - was;
        if (length(delta) > 0.25f * std::max(1.0f, -c.z)) delta = {};   // a cut: not blended from where it was
        float t = std::clamp(p.age / p.life, 0.0f, 1.0f);
        V3 d = normalize(c * -1.0f);
        R3DVertex q[4];
        if (p.kind == PUFF) {
            V3 r = normalize(cross(f.y, d), {1, 0, 0}), u = cross(d, r);   // upright in the world
            float s = p.size + p.grow * (1.0f - std::exp(-3.0f * p.age));
            V3 corner[4] = {c - r * s - u * s, c + r * s - u * s, c + r * s + u * s, c - r * s + u * s};
            const float uv[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
            // Thick where the wheel raises it, thinner as it spreads (and it does not pop up under the wheel)
            float a = p.alpha * std::pow(p.size / s, 0.4f) * std::clamp(p.age / 0.035f, 0.0f, 1.0f);
            uint32_t color = (p.color & 0x00FFFFFFu) | uint32_t(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f) << 24;
            for (int k = 0; k < 4; k++) q[k] = vertex(corner[k], corner[k] - delta, d, uv[k][0], uv[k][1], p, t, s, color);
        } else {
            // A short streak along its way (what the eye makes of a stone in flight), at least its own size
            // (as the eye moving with the camera sees it)
            float shutter = p.drop ? 0.008f : 0.004f, longest = p.drop ? 0.3f : 0.25f;   // (water draws out into threads)
            V3 along = f.dir(p.v - eye_vel) * shutter;
            float len = length(along);
            if (len > longest) along = along * (longest / len), len = longest;
            V3 tail = c - along;
            V3 a = len > 1e-5f ? along * (1.0f / len) : V3{0, 1, 0};
            V3 side = normalize(cross(a, d), {1, 0, 0});
            float w = std::max(p.size, -c.z * 0.0011f);   // about a pixel at least: smaller ones flicker
            V3 head = c + a * w, foot = tail - a * w;
            V3 corner[4] = {foot - side * w, head - side * w, head + side * w, foot + side * w};
            const float uv[4][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, 1}};
            float aspect = (len + 2 * w) / (2 * w);
            for (int k = 0; k < 4; k++) q[k] = vertex(corner[k], corner[k] - delta, d, uv[k][0], uv[k][1], p, t, aspect, p.color);
        }
        for (int k : corners) out.push_back(q[k]);
    }
}

}  // namespace

void r3d_set_sr2_dust(bool enhanced) { g_enhanced = enhanced; }

void r3d_sr2_dust(R3DScene& out, const std::vector<std::vector<R3DInstance>>& instances, R3DTextureRam* textures) {
    static const bool off = getenv("SR2_DUST") && atoi(getenv("SR2_DUST")) == 0;
    static FILE* log = getenv("SR2_DUSTLOG") ? fopen(getenv("SR2_DUSTLOG"), "w") : nullptr;
    if (!g_enhanced || off) {
        g_parts.clear();
        g_emitters.clear();
        g_before.ok = false;
        g_have_delta = false;
        g_last_frame = 0;
        return;
    }
    auto began = std::chrono::steady_clock::now();
    int steps = 1;
    if (g_last_frame) steps = int(std::clamp<int64_t>(int64_t(out.frame) - int64_t(g_last_frame), 0, 8));
    g_last_frame = out.frame;

    // The viewport of the world: the particles are drawn at the end of its see-through list
    int world = -1;
    for (uint32_t i = 0; i < out.viewports.size(); i++)
        if (out.viewports[i].world) world = int(i);

    // The game's fans of dust: models it builds in polygon RAM, in world
    // coordinates (so their node's matrix is the camera's), drawn in the cars'
    // viewports beside the world's. A ridge (texture v = 1) between two edges
    // on the ground (v = 0), from the wheel (the highest u) back
    Frame f;
    std::vector<Ribbon> ribbons;
    std::vector<bool> has_fan(out.viewports.size(), false);
    for (uint32_t vi = 0; vi < out.viewports.size(); vi++) {
        const R3DViewport& v = out.viewports[vi];
        if (v.world || v.build_index >= instances.size() || v.x > 0.5f || v.w < 495.5f) continue;
        for (const R3DInstance& in : instances[v.build_index]) {
            if ((in.start & 0xFFFFFF) >= 0x100000 || in.trans[1] < in.trans[0] + 8 * 3) continue;
            Ribbon r;
            r.viewport = vi;
            Frame own;
            own.o = load(in.origin), own.x = load(in.axes), own.y = load(in.axes + 3), own.z = load(in.axes + 6);
            own.ok = true;
            struct Rung {
                float u;
                V3 sum;
                int n;
                V3 seen[4];
            };
            std::vector<Rung> rungs;
            float alpha = 0, newest = -1e30f;
            for (uint32_t i = in.trans[0]; i + 2 < in.trans[1]; i += 3) {
                uint32_t k = v.trans_first + i;
                if (k + 2 >= out.vertices.size()) break;
                const R3DVertex* x = &out.vertices[k];
                if (!is_fan(x[0])) continue;
                r.tris.push_back(k);
                alpha = std::max(alpha, float(x[0].color >> 24) / 255.0f);
                // (each stretch keeps the colour of the ground it was raised from: the wheel's is the newest)
                float u = std::max(x[0].uv[0], std::max(x[1].uv[0], x[2].uv[0]));
                if (u >= newest) {
                    newest = u;
                    r.rgb = {float(x[0].color & 255) / 255.0f, float((x[0].color >> 8) & 255) / 255.0f, float((x[0].color >> 16) & 255) / 255.0f};
                }
                for (int c = 0; c < 3; c++) {
                    if (x[c].uv[1] > 0.5f) continue;   // the ridge
                    V3 w = own.world(load(x[c].pos));
                    Rung* rung = nullptr;
                    for (Rung& g : rungs)
                        if (std::fabs(g.u - x[c].uv[0]) < 0.02f) rung = &g;
                    if (!rung) {
                        rungs.push_back({x[c].uv[0], {}, 0, {}});
                        rung = &rungs.back();
                    }
                    bool dup = false;
                    for (int s = 0; s < rung->n; s++) dup |= length(rung->seen[s] - w) < 1e-3f;
                    if (dup || rung->n >= 4) continue;
                    rung->seen[rung->n++] = w;
                    rung->sum = rung->sum + w;
                }
            }
            if (r.tris.size() < 8 || rungs.size() < 3) continue;
            std::sort(rungs.begin(), rungs.end(), [](const Rung& a, const Rung& b) { return a.u > b.u; });
            for (const Rung& g : rungs) r.path.push_back(g.sum * (1.0f / float(g.n)));
            for (size_t i = 1; i < r.path.size(); i++) r.length += length(r.path[i] - r.path[i - 1]);
            r.strength = alpha;
            r.tip_view = own.view(r.path.front());
            r.full = rungs.size() >= 11;
            if (!f.ok) f = own;
            has_fan[vi] = true;
            ribbons.push_back(std::move(r));
        }
    }
    // The flying bits drawn with them: one small alpha tested sprite of the
    // VROM facing the eye, many times over along the wheels' way
    std::vector<uint32_t> bits;                              // first vertex of each sprite triangle
    std::vector<uint32_t> bits_tex(out.viewports.size(), 0);
    for (uint32_t vi = 0; vi < out.viewports.size(); vi++) {
        if (!has_fan[vi]) continue;
        const R3DViewport& v = out.viewports[vi];
        for (const R3DInstance& in : instances[v.build_index]) {
            if ((in.start & 0xFFFFFF) < 0x100000 || in.polygons != 1 || in.opaque[1] != in.opaque[0] + 6 || in.axes[8] < 0.999f) continue;
            uint32_t k = v.opaque_first + in.opaque[0];
            if (k + 5 >= out.vertices.size() || !is_bit(out.vertices[k])) continue;
            if (length(load(out.vertices[k].pos) - load(out.vertices[k + 1].pos)) > 1.0f) continue;
            bits.push_back(k), bits.push_back(k + 3);
            bits_tex[vi] = out.vertices[k].tex;
        }
    }
    for (Ribbon& r : ribbons) r.debris = bits_tex[r.viewport];
    // A puddle's splash sprites (drawn in the see-through list): the fan of the
    // wheel they start at is water, not dust. They go when a wheel takes their place
    for (uint32_t vi = 0; vi < out.viewports.size(); vi++) {
        if (!has_fan[vi]) continue;
        const R3DViewport& v = out.viewports[vi];
        std::vector<uint32_t> splash;
        bool wet = false;
        V3 last;
        float pace = 0;
        int rows = 0;
        for (const R3DInstance& in : instances[v.build_index]) {
            if ((in.start & 0xFFFFFF) < 0x100000 || in.polygons != 1 || in.trans[1] != in.trans[0] + 6 || in.axes[8] < 0.999f) continue;
            uint32_t k = v.trans_first + in.trans[0];
            if (k + 5 >= out.vertices.size() || !is_splash(out.vertices[k])) continue;
            if (length(load(out.vertices[k].pos) - load(out.vertices[k + 1].pos)) > 1.5f) continue;
            splash.push_back(k), splash.push_back(k + 3);
            // Each row of them is the flight of the water from one wheel, newest
            // first: a row begins where the place jumps. Its first is at the wheel
            // (a little ahead of it at speed); the rest trail away, past the other wheel too
            V3 at = load(in.origin);
            float jump = length(at - last);
            bool head = !rows || (jump > 0.6f && jump > 2.5f * pace);
            pace = head ? 0.0f : (pace > 0 ? 0.5f * (pace + jump) : jump);
            last = at;
            rows++;
            if (!head) continue;
            Ribbon* near = nullptr;
            float best = 1.4f;
            for (Ribbon& r : ribbons) {
                float d = length(r.tip_view - load(in.origin));
                if (r.viewport == vi && d < best) best = d, near = &r;
            }
            if (near) near->wet = wet = true;
        }
        if (wet) bits.insert(bits.end(), splash.begin(), splash.end());
    }
    // SR2_DUSTDUMP (with SR2_DUSTLOG): the other effect models drawn beside the fans, to tell what they are
    static const bool dump = getenv("SR2_DUSTDUMP") != nullptr;
    if (log && dump)
        for (uint32_t vi = 0; vi < out.viewports.size(); vi++) {
            if (!has_fan[vi]) continue;
            const R3DViewport& v = out.viewports[vi];
            for (const R3DInstance& in : instances[v.build_index])
                for (int pass = 0; pass < 2; pass++) {
                    const uint32_t* r = pass ? in.trans : in.opaque;
                    uint32_t k = (pass ? v.trans_first : v.opaque_first) + r[0];
                    if (r[1] <= r[0] || k + 2 >= out.vertices.size()) continue;
                    const R3DVertex* x = &out.vertices[k];
                    if (!(x->flags & R3D_TEXTURED) || (x->flags & R3D_SPECULAR)) continue;
                    fprintf(log, "  model %06x polys %u %c tris %u flags %x tex %08x color %08x origin %.2f %.2f %.2f z axis %.2f %.2f %.2f: (%.2f %.2f %.2f) (%.2f %.2f %.2f) (%.2f %.2f %.2f)\n",
                            in.start, in.polygons, pass ? 'T' : 'O', (r[1] - r[0]) / 3, x->flags, x->tex, x->color, in.origin[0], in.origin[1],
                            in.origin[2], in.axes[6], in.axes[7], in.axes[8], x[0].pos[0], x[0].pos[1], x[0].pos[2], x[1].pos[0], x[1].pos[1],
                            x[1].pos[2], x[2].pos[0], x[2].pos[1], x[2].pos[2]);
                }
        }

    // Without a fan this frame the world goes on from the tracker's camera
    const R3DViewport* w = world >= 0 ? &out.viewports[size_t(world)] : nullptr;
    if (w) {
        Frame t;
        t.x = load(w->world_axes), t.y = load(w->world_axes + 3), t.z = load(w->world_axes + 6);
        V3 cam = load(w->camera);
        if (f.ok) {
            g_delta = f.world(V3{}) - cam;
            g_have_delta = true;
        } else if (g_have_delta) {
            t.o = t.dir(cam + g_delta) * -1.0f;
            t.ok = true;
            f = t;
        }
    }
    if (!f.ok) {   // nowhere to put them: they wait (and age)
        for (Particle& p : g_parts) p.age += FRAME_S * float(steps);
        g_parts.erase(std::remove_if(g_parts.begin(), g_parts.end(), [](const Particle& p) { return p.age >= p.life; }), g_parts.end());
        g_emitters.clear();
        g_before.ok = false;
        return;
    }

    for (Particle& p : g_parts) p.before = p.p;
    for (Particle& p : g_parts)
        for (int s = 0; s < steps; s++) step(p, FRAME_S);

    // Each fan to the wheel it was last frame, or a new one
    Rng rng{hash32(uint32_t(out.frame) * 2654435761u + 17u)};
    for (Emitter& e : g_emitters) e.matched = false;
    for (const Ribbon& r : ribbons) {
        V3 tip = r.path.front();
        float dist_eye = length(f.view(tip));
        int best = -1;
        float best_d = 1e30f;
        for (int i = 0; i < int(g_emitters.size()); i++) {
            const Emitter& e = g_emitters[size_t(i)];
            if (e.matched) continue;
            float d = length(e.tip + e.vel * float(std::max(steps, 1)) - tip);
            if (d < 1.5f + 4.0f * float(std::max(steps, 1)) && d < best_d) best = i, best_d = d;
        }
        if (best >= 0) {
            Emitter& e = g_emitters[size_t(best)];
            e.matched = true;
            e.unseen = 0;
            if (steps > 0) {
                emit_along(e, r, e.tip, tip, float(steps), dist_eye, textures, rng);
                e.vel = (tip - e.tip) * (1.0f / float(steps));
                e.tip = tip;
            }
            continue;
        }
        if (g_emitters.size() >= MAX_EMITTERS) continue;
        Emitter e;
        e.id = g_next_id++;
        e.tip = tip;
        e.matched = true;
        // A fan already drawn out when first seen (a car coming into view, a
        // change of camera): the dust it has left on its way so far is made up,
        // back along the fan and straight on behind it, so the cloud does not
        // grow out of nothing. The fan is about a dozen frames of the wheel's way
        float per_frame = r.length / RIBBON_FRAMES;
        V3 heading = normalize(r.path.front() - r.path[1], {0, 0, 0});
        e.vel = heading * per_frame;
        if (r.full && per_frame > 0.05f) {
            auto along = [&](float s) {   // the point a way s back from the wheel
                for (size_t i = 1; i < r.path.size(); i++) {
                    float seg = length(r.path[i] - r.path[i - 1]);
                    if (s <= seg || i + 1 == r.path.size()) return r.path[i - 1] + (r.path[i] - r.path[i - 1]) * (seg > 1e-6f ? s / seg : 0.0f);
                    s -= seg;
                }
                return r.path.back();
            };
            const int frames = 75;   // (not all of it: it would go straight on through a bend's inside)
            size_t from = g_parts.size();
            for (int k = frames; k >= 1; k--) {
                for (size_t i = from; i < g_parts.size(); i++) step(g_parts[i], FRAME_S);
                emit_along(e, r, along(per_frame * float(k)), along(per_frame * float(k - 1)), 1.0f, dist_eye, textures, rng);
            }
            for (size_t i = from; i < g_parts.size(); i++) g_parts[i].before = g_parts[i].p - g_parts[i].v * FRAME_S;
        }
        g_emitters.push_back(e);
    }
    for (Emitter& e : g_emitters)
        if (!e.matched) e.unseen += std::max(steps, 1);
    g_emitters.erase(std::remove_if(g_emitters.begin(), g_emitters.end(), [](const Emitter& e) { return e.unseen > 2; }), g_emitters.end());
    g_parts.erase(std::remove_if(g_parts.begin(), g_parts.end(), [](const Particle& p) { return p.age >= p.life; }), g_parts.end());

    // The game's own fans and bits are taken out: folded to a point (the ray
    // tracer takes the cars' polygons as they are) and fully see-through
    auto fold = [&](uint32_t k) {
        V3 at = load(out.vertices[k].pos), was = load(out.vertices[k].prev);
        for (int j = 0; j < 3; j++) {
            R3DVertex& x = out.vertices[k + j];
            store(x.pos, at), store(x.prev, was);
            x.color &= 0x00FFFFFFu;
            x.flags &= ~(R3D_TEXTURED | R3D_TEX_ALPHA | R3D_ALPHA_TEST);
        }
    };
    for (const Ribbon& r : ribbons)
        for (uint32_t k : r.tris) fold(k);
    for (uint32_t k : bits) fold(k);

    if (log) {
        size_t puffs = 0;
        for (const Particle& p : g_parts) puffs += p.kind == PUFF;
        fprintf(log, "frame %llu steps %d fans %zu bits %zu emitters %zu puffs %zu clods %zu world %d ms %.3f\n", (unsigned long long)out.frame, steps,
                ribbons.size(), bits.size() / 2, g_emitters.size(), puffs, g_parts.size() - puffs, world,
                std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - began).count());
        for (const Ribbon& r : ribbons)
            fprintf(log, "  fan vp %u tris %zu tip %.2f %.2f %.2f tail %.2f %.2f %.2f length %.2f strength %.2f rgb %.2f %.2f %.2f debris %08x wet %d\n",
                    r.viewport, r.tris.size(), r.path.front().x, r.path.front().y, r.path.front().z, r.path.back().x, r.path.back().y,
                    r.path.back().z, r.length, r.strength, r.rgb.x, r.rgb.y, r.rgb.z, r.debris, int(r.wet));
        fflush(log);
    }

    if (world < 0 || g_parts.empty()) {
        g_before = f;
        return;
    }
    std::vector<R3DVertex> add;
    add.reserve(g_parts.size() * 6);
    draw(f, g_before, add);
    g_before = f;
    if (add.empty()) return;
    // A viewport of their own: the world's (its camera, light and fog), drawn
    // after the last one of its priority, so they come over the cars' own
    // see-through polygons too (the shadow the game lays under the car is one,
    // in the car's viewport: drawn after the dust, it cut its shape out of it).
    // It is not the world again (nothing of it is solid, lit or tracked)
    R3DViewport own = out.viewports[size_t(world)];
    own.world = false;
    own.opaque_first = own.trans_first = uint32_t(out.vertices.size());
    own.opaque_count = 0;
    own.trans_count = uint32_t(add.size());
    own.back_first = own.back_count = 0;
    own.scroll_fog = 0;
    out.vertices.insert(out.vertices.end(), add.begin(), add.end());
    size_t at = size_t(world) + 1;
    while (at < out.viewports.size() && out.viewports[at].priority == own.priority) at++;
    out.viewports.insert(out.viewports.begin() + ptrdiff_t(at), own);
}
