// The touch controls over the picture (platform_android_input.cpp, touch_layout.cpp), drawn by overlay.vert and
// overlay.frag after the picture pass: up to 32 circles in upright uv, shape[i] = (x, y, radius in target heights,
// alpha + 4 * label). A label is up to two 5x7 glyphs of `font` (first + 32 * second), drawn in the middle of the
// circle with a shadow. `font` holds 25 glyphs of 7 rows (nvn_present.cpp kFont), four rows per uvec4.
// They used to be evaluated for every pixel of the screen inside the picture pass (a loop over all circles, the font a
// constant array): ~18 ms of the Adreno 610's ~64 ms frame in the P5R casino (PROGRESO 76). Now only the circles'
// own quads run this.
layout(push_constant) uniform P {
    vec4 rot;   // mat2 columns (x, y) (z, w): host uv -> upright uv
    vec4 off;   // xy: rot offset; w: target width / height (upright)
    vec4 box;
    vec4 crop;
} p;
layout(set = 0, binding = 1) uniform Overlay {
    vec4 shape[32];
    uvec4 font[44];
} ov;
