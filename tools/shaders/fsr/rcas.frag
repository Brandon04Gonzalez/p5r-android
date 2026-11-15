#version 450
// Single-pass Mobile FSR 1.0 (combined EASU + RCAS with flat-pixel early exit), adapted from
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

    vec2 sz = vec2(textureSize(src, 0));
    vec2 invTex = 1.0 / sz;
    vec2 lo_uv = p.crop.xy + 0.5 * invTex;
    vec2 hi_uv = p.crop.xy + p.crop.zw - 0.5 * invTex;
    vec2 raw_tc = p.crop.xy + bx * p.crop.zw;
    vec2 tc = clamp(raw_tc, lo_uv, hi_uv);

    // 5-tap cross in input texels for combined RCAS sharpening and EASU edge detection.
    vec3 sC = textureLod(src, tc, 0.0).rgb;
    vec3 sA = textureLod(src, clamp(raw_tc - vec2(0.0, invTex.y), lo_uv, hi_uv), 0.0).rgb;
    vec3 sB = textureLod(src, clamp(raw_tc - vec2(invTex.x, 0.0), lo_uv, hi_uv), 0.0).rgb;
    vec3 sD = textureLod(src, clamp(raw_tc + vec2(invTex.x, 0.0), lo_uv, hi_uv), 0.0).rgb;
    vec3 sE = textureLod(src, clamp(raw_tc + vec2(0.0, invTex.y), lo_uv, hi_uv), 0.0).rgb;

    // Green-channel luma RCAS weight (opt_fsr.fxh)
    float sA_g = sA.g, sB_g = sB.g, sC_g = sC.g, sD_g = sD.g, sE_g = sE.g;
    float mn4G = min(min(sA_g, sB_g), min(sD_g, sE_g));
    float mx4G = max(max(sA_g, sB_g), max(sD_g, sE_g));
    float hitMinG = min(mn4G, sC_g) / (4.0 * max(mx4G, 1e-4));
    float hitMaxG = (1.0 - max(mx4G, sC_g)) / min(4.0 * mn4G - 4.0, -1e-4);
    float lobeG = max(-hitMinG, hitMaxG);
    float lobe = max(-0.1875, min(lobeG, 0.0)) * p.off.z;
    float rcpL = 1.0 / (4.0 * lobe + 1.0);
    vec3 contrast = (sA + sB + sD + sE) * (lobe * rcpL);

    float lA = sA.r * 0.5 + sA.g;
    float lB = sB.r * 0.5 + sB.g;
    float lC = sC.r * 0.5 + sC.g;
    float lD = sD.r * 0.5 + sD.g;
    float lE = sE.r * 0.5 + sE.g;

    float lenX = max(abs(lD - lC), abs(lC - lB));
    float dirX = lD - lB;
    lenX = grad_dir(lenX, dirX);
    lenX *= lenX;

    float lenY = max(abs(lE - lC), abs(lC - lA));
    float dirY = lE - lA;
    lenY = grad_dir(lenY, dirY);
    float len = (lenY * lenY + lenX) * 0.5;

    vec2 dir = vec2(dirX, dirY);
    float dirR = dot(dir, dir);
    // Flat/smooth pixel early exit: return RCAS-sharpened bilinear sample with only 5 taps.
    if (dirR < (1.0 / 64.0)) {
        color = vec4(clamp(contrast + sC * rcpL, 0.0, 1.0), 1.0);
        return;
    }

    dirR = inversesqrt(dirR);
    dir *= dirR;
    len *= len;
    float stretch = dot(dir, dir) / max(abs(dir.x), abs(dir.y));
    vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 - 0.29 * len;
    float clp = 1.0 / lob;

    vec2 fp = fract(raw_tc * sz - 0.5);
    vec3 min4 = min(min(sA, sB), min(sD, sE));
    vec3 max4 = max(max(sA, sB), max(sD, sE));

    vec3 aC = vec3(0.0);
    float aW = 0.0;
    tap(aC, aW, vec2( 0.0, -1.0) - fp, dir, len2, lob, clp, sA);
    tap(aC, aW, vec2(-1.0,  0.0) - fp, dir, len2, lob, clp, sB);
    tap(aC, aW, vec2( 0.0,  0.0) - fp, dir, len2, lob, clp, sC);
    tap(aC, aW, vec2( 1.0,  0.0) - fp, dir, len2, lob, clp, sD);
    tap(aC, aW, vec2( 0.0,  1.0) - fp, dir, len2, lob, clp, sE);

    vec2 sF_off = vec2(dir.y, -dir.x) * invTex;
    vec3 sF = textureLod(src, clamp(raw_tc + sF_off, lo_uv, hi_uv), 0.0).rgb;
    vec3 sG = textureLod(src, clamp(raw_tc - sF_off, lo_uv, hi_uv), 0.0).rgb;
    tap(aC, aW,  vec2(dir.y, -dir.x) - fp, dir, len2, lob, clp, sF);
    tap(aC, aW, -vec2(dir.y, -dir.x) - fp, dir, len2, lob, clp, sG);

    vec3 upscaled = min(max4, max(min4, aC / max(aW, 1e-4)));
    color = vec4(clamp(contrast + upscaled * rcpL, 0.0, 1.0), 1.0);
}
