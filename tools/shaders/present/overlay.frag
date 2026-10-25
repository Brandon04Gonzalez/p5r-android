#version 450
#extension GL_GOOGLE_include_directive : require
// One circle of the touch controls over what is already in the target. Each step mixes the colour under it towards
// white or black, so the whole circle is an affine map of that colour: under * color.a + color.rgb, which the
// pipeline's blend applies (source ONE, destination SRC_ALPHA; the target's alpha is kept).
#include "overlay.glsl"
layout(location = 0) in vec2 up;
layout(location = 1) flat in int index;
layout(location = 0) out vec4 color;

// Glyph `g` at q (cells: x 0..5 left to right, y 0..7 top to bottom): 1 inside a lit cell.
float glyph(uint g, vec2 q) {
    if (g == 0u || g >= 25u || q.x < 0.0 || q.y < 0.0 || q.x >= 5.0 || q.y >= 7.0) return 0.0;
    uint f = g * 7u + uint(q.y);
    uint row = ov.font[f >> 2u][f & 3u];
    return float((row >> (4u - uint(q.x))) & 1u);
}

float label(uint g1, uint g2, vec2 q) {
    return max(glyph(g1, q), glyph(g2, q - vec2(6.0, 0.0)));
}

float mul = 1.0, add = 0.0;
void mix_to(float v, float t) { mul *= 1.0 - t, add = add * (1.0 - t) + v * t; }

// Alpha 0 with a label: the label alone, fully opaque (the FPS counter).
void main() {
    vec4 s = ov.shape[index];
    float id = floor(s.w * 0.25);
    float a = s.w - id * 4.0;
    vec2 v = vec2((up.x - s.x) * p.off.w, up.y - s.y);
    float d = length(v);
    if (d < s.z) {
        mix_to(1.0, a);
        uint g1 = uint(id) & 31u, g2 = uint(id) >> 5u;
        if (g1 != 0u) {
            vec2 q = v / (s.z * 0.1) + vec2(g2 != 0u ? 5.5 : 2.5, 3.5);
            float k = a > 0.0 ? min(1.0, a * 3.5) : 1.0;
            mix_to(0.0, 0.6 * k * label(g1, g2, q - vec2(0.35)));
            mix_to(1.0, k * label(g1, g2, q));
        }
    } else if (d < s.z + 0.005) {
        mix_to(1.0, min(1.0, a * 3.0));
    } else {
        discard;
    }
    color = vec4(vec3(add), mul);
}
