#version 450
// Ambient occlusion, second step: the raw occlusion (half resolution, each
// texel taken at the top left pixel of its 2x2 block) is smoothed over the
// 5x5 texels around, without mixing across depth edges, and multiplied into
// the image of the game world (the colour target blends with dst * src), and
// with it the bounced light of the global illumination, smoothed the same way.
#extension GL_GOOGLE_include_directive : require
#include "ao_common.glsl"

layout(location = 0) out vec4 out_factor;

// One texel of the 5x5 around: its occlusion into sum/weight and, with the
// global illumination on, its bounced light into the part it belongs to
void tap(ivec2 centre, ivec2 o, ivec2 size, ivec2 full, float z, bool gi,
         inout float sum, inout float weight, inout vec4 part) {
    ivec2 q = clamp(centre + o, ivec2(0), size - 1);
    float dq = texelFetch(depth_tex, min(q * 2, full - 1), 0).r;   // where that texel was taken
    if (dq <= 0.0) return;
    // a surface seen at a slant (a board along the road) changes depth
    // fast from texel to texel: a tight tolerance left its rays' pattern
    // unsmoothed, a grey hatch over it
    float w = exp(-abs(AO_NEAR / dq - z) / (0.08 * z));
    vec4 t = texelFetch(ao_tex, q, 0);
    sum += t.a * w;
    weight += w;
    if (gi) part += vec4(t.rgb * w, w);
}

void main() {
    vec2 frag = gl_FragCoord.xy;
    float d = texelFetch(depth_tex, ivec2(frag), 0).r;
    if (d <= 0.0) {
        out_factor = vec4(1.0);
        return;
    }
    float z = AO_NEAR / d;
    ivec2 size = textureSize(ao_tex, 0), full = textureSize(depth_tex, 0);
    ivec2 centre = ivec2(frag) / 2;
    float sum = 0.0, weight = 0.0;
    bool gi = ao.gi.x != 0.0;
    // The bounced light (ao.frag traced_bounce): every texel of a block of
    // 4x4 holds its own part of the rays (its place in the block). Each of
    // the 16 parts is averaged on its own and the parts then count the same:
    // summed with the depth weights alone, the parts did not, wherever the
    // depth changes from texel to texel (the road going away from the
    // camera), and their pattern was left over the picture, fixed to the
    // screen: a faint film of dots that went along with the car over the
    // tarmac. A part only on another surface (all its texels far off in
    // depth) is left out.
    // The 5x5 texels are walked part by part, so that no array of the 16
    // is kept (indexed by the texel's place, it went to slow memory: the
    // pass took 2.7 times as long, issue #142). Offsets -2 and 2 are 4
    // apart, the same part: part r takes offset r (3: -1), and 2 also -2
    vec3 bounce = vec3(0.0);
    float bounce_weight = 0.0;
    for (int ry = 0; ry < 4; ry++)
        for (int rx = 0; rx < 4; rx++) {
            int x = rx == 3 ? -1 : rx, y = ry == 3 ? -1 : ry;
            vec4 part = vec4(0.0);
            tap(centre, ivec2(x, y), size, full, z, gi, sum, weight, part);
            if (rx == 2) tap(centre, ivec2(-2, y), size, full, z, gi, sum, weight, part);
            if (ry == 2) tap(centre, ivec2(x, -2), size, full, z, gi, sum, weight, part);
            if (rx == 2 && ry == 2) tap(centre, ivec2(-2, -2), size, full, z, gi, sum, weight, part);
            if (gi) {
                float c = smoothstep(0.05, 0.3, part.a);
                if (c > 0.0) bounce += part.rgb / part.a * c, bounce_weight += c;
            }
        }
    float open = weight > 0.0 ? sum / weight : 1.0;
    vec3 factor = vec3(mix(1.0, open, ao.params.y));
    // The colour so far is the surface's own times its light: the bounced
    // light, as a share more of it
    if (gi && bounce_weight > 0.0) {
        bounce /= bounce_weight;
        factor = ao.gi.x < 0.0 ? bounce * -ao.gi.x : factor * (1.0 + ao.gi.x * bounce);
    }
    // (never NaN, infinite or below nothing: a NaN multiplied into the colour
    // showed black on Apple's GPUs, the cars and the countdown; iOS port)
    if (any(isnan(factor)) || any(isinf(factor))) factor = vec3(1.0);
    out_factor = vec4(max(factor, vec3(0.0)), 1.0);
}
