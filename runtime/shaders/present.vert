#version 450
// Fullscreen triangle over the host swapchain image; v_src is where to sample the game's image:
// src = mat2(p.m.xy, p.m.zw) * uv + p.o, uv being this pixel's position in the host image (0..1, y down).
layout(push_constant) uniform P {
    vec4 m;
    vec2 o;
} p;
layout(location = 0) out vec2 v_src;

void main() {
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
    v_src = mat2(p.m.xy, p.m.zw) * uv + p.o;
}
