#version 450
// FSR 1 RCAS (robust contrast adaptive sharpening), after AMD FidelityFX FSR 1.0 (MIT), then the picture fitted into
// the target: `box` is where the EASU output sits in the upright target, `crop.xy` its size in texels, `crop.z` the
// sharpness constant exp2(-stops). Outside the box: black. The touch controls are drawn over it afterwards
// (../present/overlay.vert, overlay.frag).
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform P {
    vec4 rot;
    vec4 off;
    vec4 box;
    vec4 crop;
} p;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

void main() {
    vec2 up = mat2(p.rot.xy, p.rot.zw) * uv + p.off.xy;
    vec2 bx = (up - p.box.xy) / p.box.zw;
    vec3 c = vec3(0.0);
    if (all(greaterThanEqual(bx, vec2(0.0))) && all(lessThanEqual(bx, vec2(1.0)))) {
        ivec2 sz = ivec2(p.crop.xy);
        ivec2 ip = clamp(ivec2(floor(bx * p.crop.xy)), ivec2(0), sz - 1);
        vec3 b = texelFetch(src, clamp(ip + ivec2(0, -1), ivec2(0), sz - 1), 0).rgb;
        vec3 d = texelFetch(src, clamp(ip + ivec2(-1, 0), ivec2(0), sz - 1), 0).rgb;
        vec3 e = texelFetch(src, ip, 0).rgb;
        vec3 f = texelFetch(src, clamp(ip + ivec2(1, 0), ivec2(0), sz - 1), 0).rgb;
        vec3 h = texelFetch(src, clamp(ip + ivec2(0, 1), ivec2(0), sz - 1), 0).rgb;
        vec3 mn4 = min(min(b, d), min(f, h));
        vec3 mx4 = max(max(b, d), max(f, h));
        vec3 hitMin = min(mn4, e) / (4.0 * max(mx4, vec3(1e-5)));
        vec3 hitMax = (1.0 - max(mx4, e)) / (4.0 * mn4 - 4.0);
        vec3 lobeRGB = max(-hitMin, hitMax);
        float lobe = max(-0.1875, min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * p.crop.z;
        c = ((b + d + f + h) * lobe + e) / (4.0 * lobe + 1.0);
    }
    color = vec4(c, 1.0);
}
