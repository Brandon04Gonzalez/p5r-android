#version 450
// Host pixel -> the picture: `rot` turns host uv into upright uv (the swapchain's preTransform undone), `box` is where
// the picture sits in the upright target (uv, fit with bars), `crop` the part of the texture shown. Alpha is 1. The
// touch controls are drawn over it afterwards (overlay.vert, overlay.frag).
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform P {
    vec4 rot;   // mat2 columns (x, y) (z, w)
    vec4 off;   // xy: rot offset; w: target width / height (upright)
    vec4 box;   // x0 y0 w h
    vec4 crop;  // u0 v0 du dv
} p;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

void main() {
    vec2 up = mat2(p.rot.xy, p.rot.zw) * uv + p.off.xy;
    vec2 b = (up - p.box.xy) / p.box.zw;
    vec3 c = vec3(0.0);
    if (all(greaterThanEqual(b, vec2(0.0))) && all(lessThanEqual(b, vec2(1.0))))
        c = texture(src, p.crop.xy + b * p.crop.zw).rgb;
    color = vec4(c, 1.0);
}
