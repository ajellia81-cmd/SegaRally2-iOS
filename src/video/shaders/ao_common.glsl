// Shared by ao.frag and ao_apply.frag: the depth of the game world (taken
// before the HUD viewports clear it) and the view-space position of a pixel.

layout(set = 0, binding = 0) uniform sampler2D depth_tex;   // reverse Z: near / -z_view, 0 = nothing
layout(set = 0, binding = 1) uniform sampler2D ao_tex;      // raw occlusion in a, bounced light in rgb (ao.frag)

layout(push_constant) uniform AmbientOcclusion {
    vec4 proj;     // world viewport projection: x scale, y scale, x offset, y offset (as drawn)
    vec4 rect;     // world viewport in target pixels: x, y, width, height
    vec4 params;   // radius (view units), strength, fog density, fog start
    vec4 misc;     // x: noise seed, y: largest radius in pixels, z: rays per pixel when traced (0 = from the depth),
                   // w: 1 = traced on the left half only (LW_RTSPLIT)
    vec4 gi;       // global illumination (traced): x strength (0 = off; negative: shown alone, for looking at it),
                   // y rays per pixel, z their reach (view units), w a turn of the rays (0: the same in every image)
    vec4 light;    // x: the world's ambient light
} ao;

const float AO_NEAR = 1e-3;   // r3d_scene.cpp projection

// View-space position of the surface at a target pixel; z >= 0: no surface
vec3 ao_view_pos(vec2 frag) {
    float d = texelFetch(depth_tex, ivec2(frag), 0).r;
    if (d <= 0.0) return vec3(0.0, 0.0, 1.0);
    float z = -AO_NEAR / d;
    vec2 ndc = (floor(frag) + 0.5 - ao.rect.xy) / ao.rect.zw * 2.0 - 1.0;
    return vec3(-z * (ndc.x + ao.proj.z) / ao.proj.x, -z * (ndc.y + ao.proj.w) / ao.proj.y, z);
}
