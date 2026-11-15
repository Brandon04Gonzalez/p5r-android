#version 450
// Single-pass Mobile FSR 1.0 (combined snapped EASU + overshoot-safe RCAS), adapted from
// FsrMobile (opt_fsr.fxh) by atyuwen (MIT License, https://gist.github.com/atyuwen/78d6e810e6d0f7fd4aa6207d416f2eeb).
// Rotates host uv -> upright uv via p.rot/p.off and fits p.crop into p.box like present.frag;
// p.off.z carries the RCAS sharpness constant exp2(-stops). Runs directly in the main present pass
// across all swapchain preTransform orientations without an intermediate RGBA16F render pass.
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform P {
    vec4 rot;   // mat2 columns (x, y) (z, w)
    vec4 off;   // xy: rot offset; z: sharpness exp2(-stops); w: target width / height (upright)
    vec4 box;   // x0 y0 w h
    vec4 crop;  // u0 v0 du dv
} p;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

void tap(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len2, float lob, float clp, vec3 c) {
    vec2 v = vec2(off.x * dir.x + off.y * dir.y, off.x * (-dir.y) + off.y * dir.x);
    v *= len2;
    float d2 = min(v.x * v.x + v.y * v.y, clp);
    float wB = (2.0 / 5.0) * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = (25.0 / 16.0) * wB - (25.0 / 16.0 - 1.0);
    float w = wB * wA;
    aC += c * w;
    aW += w;
}

float grad_dir(float lenv, float dirv) {
    return lenv > 1e-5 ? clamp(abs(dirv) / lenv, 0.0, 1.0) : 0.0;
}

void main() {
    vec2 up = mat2(p.rot.xy, p.rot.zw) * uv + p.off.xy;
    vec2 bx = (up - p.box.xy) / p.box.zw;
    if (any(lessThan(bx, vec2(0.0))) || any(greaterThan(bx, vec2(1.0)))) {
        color = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    ivec2 sz_i = textureSize(src, 0);
    vec2 sz = vec2(sz_i);
    ivec2 lo_i = clamp(ivec2(floor(p.crop.xy * sz + 0.5)), ivec2(0), sz_i - 1);
    ivec2 hi_i = clamp(ivec2(floor((p.crop.xy + p.crop.zw) * sz + 0.5)) - 1, lo_i, sz_i - 1);

    // Snap to the nearest texel center so samples have zero bilinear pre-blur and fp stays symmetric in [-0.5, 0.5].
    vec2 raw_pos = (p.crop.xy + bx * p.crop.zw) * sz;
    ivec2 ip = clamp(ivec2(floor(raw_pos)), lo_i, hi_i);
    vec2 fp = raw_pos - (vec2(ip) + 0.5);

    vec3 sC = texelFetch(src, ip, 0).rgb;
    vec3 sA = texelFetch(src, clamp(ip + ivec2( 0, -1), lo_i, hi_i), 0).rgb;
    vec3 sB = texelFetch(src, clamp(ip + ivec2(-1,  0), lo_i, hi_i), 0).rgb;
    vec3 sD = texelFetch(src, clamp(ip + ivec2( 1,  0), lo_i, hi_i), 0).rgb;
    vec3 sE = texelFetch(src, clamp(ip + ivec2( 0,  1), lo_i, hi_i), 0).rgb;

    // RGB RCAS contrast delta centered on sC (keeps zero-DC shift and preserves [0, 1] bounds).
    vec3 mn4 = min(min(sA, sB), min(sD, sE));
    vec3 mx4 = max(max(sA, sB), max(sD, sE));
    vec3 hitMin = min(mn4, sC) / (4.0 * max(mx4, vec3(1e-5)));
    vec3 hitMax = (1.0 - max(mx4, sC)) / min(4.0 * mn4 - 4.0, vec3(-1e-5));
    vec3 lobeRGB = max(-hitMin, hitMax);
    float lobe = max(-0.1875, min(max(lobeRGB.r, max(lobeRGB.g, lobeRGB.b)), 0.0)) * p.off.z;
    float rcpL = 1.0 / (4.0 * lobe + 1.0);
    vec3 delta = ((sA + sB + sD + sE) * lobe + sC) * rcpL - sC;

    float lA = sA.r * 0.5 + sA.g;
    float lB = sB.r * 0.5 + sB.g;
    float lC = sC.r * 0.5 + sC.g;
    float lD = sD.r * 0.5 + sD.g;
    float lE = sE.r * 0.5 + sE.g;

    float lenX = max(abs(lD - lC), abs(lC - lB));
    float dirX = lD - lB;
    float dirY = lE - lA;
    vec2 dir = vec2(dirX, dirY);
    float dirR = dot(dir, dir);

    ivec2 q_off = ivec2(fp.x >= 0.0 ? 1 : -1, fp.y >= 0.0 ? 1 : -1);
    vec3 sQ = texelFetch(src, clamp(ip + q_off, lo_i, hi_i), 0).rgb;
    vec3 sHx = (q_off.x > 0) ? sD : sB;
    vec3 sVy = (q_off.y > 0) ? sE : sA;
    vec3 minQ = min(min(sC, sHx), min(sVy, sQ));
    vec3 maxQ = max(max(sC, sHx), max(sVy, sQ));

    // Flat/smooth pixel early exit: exact C0 bilinear in the 2x2 cell + gentle RCAS without Lanczos cost.
    if (dirR < (1.0 / 32.0)) {
        vec2 afp = abs(fp);
        vec3 base = mix(mix(sC, sHx, afp.x), mix(sVy, sQ, afp.x), afp.y);
        color = vec4(clamp(base + delta * 0.5, minQ, maxQ), 1.0);
        return;
    }

    lenX = grad_dir(lenX, dirX);
    lenX *= lenX;
    float lenY = max(abs(lE - lC), abs(lC - lA));
    lenY = grad_dir(lenY, dirY);
    float len = (lenY * lenY + lenX) * 0.5;

    dir *= inversesqrt(dirR);
    len *= len;
    float stretch = dot(dir, dir) / max(abs(dir.x), abs(dir.y));
    vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 - 0.29 * len;
    float clp = 1.0 / lob;

    vec3 aC = vec3(0.0);
    float aW = 0.0;
    tap(aC, aW, vec2( 0.0, -1.0) - fp, dir, len2, lob, clp, sA);
    tap(aC, aW, vec2(-1.0,  0.0) - fp, dir, len2, lob, clp, sB);
    tap(aC, aW, vec2( 0.0,  0.0) - fp, dir, len2, lob, clp, sC);
    tap(aC, aW, vec2( 1.0,  0.0) - fp, dir, len2, lob, clp, sD);
    tap(aC, aW, vec2( 0.0,  1.0) - fp, dir, len2, lob, clp, sE);
    tap(aC, aW, vec2(q_off)      - fp, dir, len2, lob, clp, sQ);

    vec3 upscaled = clamp(aC / max(aW, 1e-4), minQ, maxQ);
    color = vec4(clamp(upscaled + delta * 0.5, max(vec3(0.0), minQ - 0.06), min(vec3(1.0), maxQ + 0.06)), 1.0);
}
