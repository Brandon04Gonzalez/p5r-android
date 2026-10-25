#version 450
// FSR 1 EASU (edge adaptive spatial upsampling), after AMD FidelityFX FSR 1.0 (MIT): 12 taps around the output pixel,
// a direction and length from their luma, then a Lanczos-like kernel stretched along the edge. Taps are fetched as
// texels (no gather), clamped to the shown part of the texture.
//   push a: xy source texels per output pixel, zw offset (0.5 * scale - 0.5); b: xy origin and zw size in texels
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform P {
    vec4 a;
    vec4 b;
    vec4 c;
    vec4 d;
} p;
layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 color;

ivec2 lo, hi;
vec3 F(ivec2 q) { return texelFetch(src, clamp(q, lo, hi), 0).rgb; }
float L(vec3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }

void tap(inout vec3 aC, inout float aW, vec2 off, vec2 dir, vec2 len, float lob, float clp, vec3 c) {
    vec2 v = vec2(off.x * dir.x + off.y * dir.y, off.x * (-dir.y) + off.y * dir.x);
    v *= len;
    float d2 = min(v.x * v.x + v.y * v.y, clp);
    float wB = 2.0 / 5.0 * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = 25.0 / 16.0 * wB - (25.0 / 16.0 - 1.0);
    float w = wB * wA;
    aC += c * w;
    aW += w;
}

float edge(float lenv, float dirv) { return lenv > 0.0 ? clamp(abs(dirv) / lenv, 0.0, 1.0) : 0.0; }

void setd(inout vec2 dir, inout float len, float w, float lA, float lB, float lC, float lD, float lE) {
    float lenX = max(abs(lD - lC), abs(lC - lB));
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = edge(lenX, dirX);
    lenX *= lenX;
    len += lenX * w;
    float lenY = max(abs(lE - lC), abs(lC - lA));
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = edge(lenY, dirY);
    lenY *= lenY;
    len += lenY * w;
}

void main() {
    lo = ivec2(p.b.xy);
    hi = lo + ivec2(p.b.zw) - 1;
    ivec2 ip = ivec2(gl_FragCoord.xy);
    vec2 pp = vec2(ip) * p.a.xy + p.a.zw;
    vec2 fpv = floor(pp);
    pp -= fpv;
    ivec2 fp = ivec2(fpv) + lo;
    //    b c
    //  e f g h
    //  i j k l
    //    n o
    vec3 b = F(fp + ivec2(0, -1)), c = F(fp + ivec2(1, -1));
    vec3 e = F(fp + ivec2(-1, 0)), f = F(fp), g = F(fp + ivec2(1, 0)), h = F(fp + ivec2(2, 0));
    vec3 i = F(fp + ivec2(-1, 1)), j = F(fp + ivec2(0, 1)), k = F(fp + ivec2(1, 1)), l = F(fp + ivec2(2, 1));
    vec3 n = F(fp + ivec2(0, 2)), o = F(fp + ivec2(1, 2));
    float bL = L(b), cL = L(c), eL = L(e), fL = L(f), gL = L(g), hL = L(h);
    float iL = L(i), jL = L(j), kL = L(k), lL = L(l), nL = L(n), oL = L(o);
    vec2 dir = vec2(0.0);
    float len = 0.0;
    setd(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
    setd(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);
    setd(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);
    setd(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);
    vec2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    bool zro = dirR < (1.0 / 32768.0);
    dirR = inversesqrt(max(dirR, 1e-20));
    dirR = zro ? 1.0 : dirR;
    dir.x = zro ? 1.0 : dir.x;
    dir *= vec2(dirR);
    len = len * 0.5;
    len *= len;
    float stretch = (dir.x * dir.x + dir.y * dir.y) / max(abs(dir.x), abs(dir.y));
    vec2 len2 = vec2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = 1.0 / lob;
    vec3 min4 = min(min(f, g), min(j, k));
    vec3 max4 = max(max(f, g), max(j, k));
    vec3 aC = vec3(0.0);
    float aW = 0.0;
    tap(aC, aW, vec2(0.0, -1.0) - pp, dir, len2, lob, clp, b);
    tap(aC, aW, vec2(1.0, -1.0) - pp, dir, len2, lob, clp, c);
    tap(aC, aW, vec2(-1.0, 1.0) - pp, dir, len2, lob, clp, i);
    tap(aC, aW, vec2(0.0, 1.0) - pp, dir, len2, lob, clp, j);
    tap(aC, aW, vec2(0.0, 0.0) - pp, dir, len2, lob, clp, f);
    tap(aC, aW, vec2(-1.0, 0.0) - pp, dir, len2, lob, clp, e);
    tap(aC, aW, vec2(1.0, 1.0) - pp, dir, len2, lob, clp, k);
    tap(aC, aW, vec2(2.0, 1.0) - pp, dir, len2, lob, clp, l);
    tap(aC, aW, vec2(2.0, 0.0) - pp, dir, len2, lob, clp, h);
    tap(aC, aW, vec2(1.0, 0.0) - pp, dir, len2, lob, clp, g);
    tap(aC, aW, vec2(1.0, 2.0) - pp, dir, len2, lob, clp, o);
    tap(aC, aW, vec2(0.0, 2.0) - pp, dir, len2, lob, clp, n);
    color = vec4(min(max4, max(min4, aC / aW)), 1.0);
}
