#include "r3d_screen.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

// The cutscene letterbox is two flat black quads drawn in an overlay viewport
// (higher priority than the scene) across the top and bottom of the screen.
// They are collapsed to nothing so the scene shows through, which also lets the
// widescreen view use the whole window height.
// Screen bounding box of a triangle in native pixels: x0, y0, x1, y1
static bool screen_bbox(const R3DViewport& v, const R3DVertex* tri, float* b) {
    b[0] = b[1] = 1e9f;
    b[2] = b[3] = -1e9f;
    const float* m = v.proj;
    for (int k = 0; k < 3; k++) {
        const float* p = tri[k].pos;
        float cw = m[3] * p[0] + m[7] * p[1] + m[11] * p[2] + m[15];
        if (cw <= 1e-6f) return false;
        float sx = v.x + ((m[0] * p[0] + m[4] * p[1] + m[8] * p[2] + m[12]) / cw + 1) / 2 * v.w;
        float sy = v.y + ((m[1] * p[0] + m[5] * p[1] + m[9] * p[2] + m[13]) / cw + 1) / 2 * v.h;
        b[0] = std::min(b[0], sx), b[2] = std::max(b[2], sx), b[1] = std::min(b[1], sy), b[3] = std::max(b[3], sy);
    }
    return true;
}

void r3d_strip_letterbox(R3DScene& out) {
    if (out.viewports.empty()) return;
    int scene_priority = out.viewports.front().priority;
    for (const R3DViewport& v : out.viewports) {
        if (v.priority == scene_priority) continue;
        for (uint32_t t = 0; t + 2 < v.opaque_count; t += 3) {
            R3DVertex* tri = &out.vertices[v.opaque_first + t];
            if ((tri[0].color & 0xFFFFFF) != 0 || (tri[0].flags & R3D_TEXTURED)) continue;
            float b[4];
            if (!screen_bbox(v, tri, b)) continue;
            float x0 = b[0], y0 = b[1], x1 = b[2], y1 = b[3];
            bool wide = x1 - x0 >= 0.9f * v.w;
            bool band = y1 - y0 < 0.5f * v.h;   // full-screen fades are kept
            bool edge = y0 <= v.y + 1 || y1 >= v.y + v.h - 1;
            if (!(wide && band && edge)) continue;
            for (int k = 1; k < 3; k++) memcpy(tri[k].pos, tri[0].pos, sizeof(tri[0].pos));
        }
    }
}

void r3d_widen_screen_fills(R3DScene& out, float wide) {
    if (wide <= 1.001f) return;
    for (const R3DViewport& v : out.viewports) {
        if (v.world || v.x > 0.5f || v.w < 495.5f) continue;
        for (int pass = 0; pass < 2; pass++) {
            uint32_t first = pass ? v.trans_first : v.opaque_first, count = pass ? v.trans_count : v.opaque_count;
            for (uint32_t t = 0; t + 2 < count; t += 3) {
                R3DVertex* tri = &out.vertices[first + t];
                float b[4];
                if (!screen_bbox(v, tri, b)) continue;
                if (b[0] > v.x + 2 || b[2] < v.x + v.w - 2 || b[1] > v.y + 2 || b[3] < v.y + v.h - 2) continue;
                for (int k = 0; k < 3; k++) tri[k].pos[0] *= wide;
            }
        }
    }
}
