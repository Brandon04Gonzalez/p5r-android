#version 450
layout(set = 0, binding = 0) uniform sampler2D src;
layout(location = 0) in vec2 v_src;
layout(location = 0) out vec4 color;

void main() { color = texture(src, v_src); }
