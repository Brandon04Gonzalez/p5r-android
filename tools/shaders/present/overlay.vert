#version 450
#extension GL_GOOGLE_include_directive : require
// One quad per circle (instance = circle), the ring included: upright uv in, host clip space out.
#include "overlay.glsl"
layout(location = 0) out vec2 up;
layout(location = 1) flat out int index;
void main() {
    vec4 s = ov.shape[gl_InstanceIndex];
    vec2 corner = vec2(float(gl_VertexIndex & 1), float(gl_VertexIndex >> 1)) * 2.0 - 1.0;
    float r = s.z + 0.006;
    up = s.xy + corner * vec2(r / p.off.w, r);
    // up = R * uv + off with R a quarter turn or a flip: uv = transpose(R) * (up - off).
    vec2 uv = transpose(mat2(p.rot.xy, p.rot.zw)) * (up - p.off.xy);
    index = gl_InstanceIndex;
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
}
