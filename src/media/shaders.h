#pragma once

// Shader sources are embedded so the bench binary has no runtime asset
// dependency. When the real decoder lands these move to compiled .cso files,
// but the pipeline shape stays identical.

namespace brp::shaders {

// Generates a synthetic "game frame" at the source resolution. This stands in
// for the decoder output: high-frequency detail (stresses any scaler), hard
// edges (ringing test), fine strokes (sharpness test), a luminance sweep
// (banding test), and a moving disc (motion / interpolation test).
inline constexpr const char* kGenCS = R"hlsl(
RWTexture2D<float4> gOut : register(u0);

cbuffer GenParams : register(b0)
{
    float gTime;
    float3 gPad;
};

[numthreads(16, 16, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint w, h;
    gOut.GetDimensions(w, h);
    if (id.x >= w || id.y >= h)
        return;

    float2 uv = (float2(id.xy) + 0.5) / float2(w, h);
    float t = gTime;

    float checker = frac(floor(uv.x * 220.0 + t * 24.0) * 0.5 + floor(uv.y * 124.0) * 0.5);
    float3 col = lerp(float3(0.035, 0.055, 0.085), float3(0.72, 0.30, 0.55), checker);

    float2 c = float2(0.5 + 0.34 * sin(t * 0.90), 0.5 + 0.28 * cos(t * 1.37));
    float d = length((uv - c) * float2(1.7778, 1.0));
    col = lerp(col, float3(0.16, 0.92, 0.62), 1.0 - smoothstep(0.055, 0.058, d));

    float strokes = step(0.5, frac(uv.x * 420.0)) * step(0.86, uv.y);
    col = lerp(col, float3(0.95, 0.95, 0.95), strokes * 0.9);

    col *= 0.75 + 0.25 * sin(uv.y * 12.0 + t * 0.6);

    gOut[id.xy] = float4(col, 1.0);
}
)hlsl";

// Fullscreen triangle. No vertex buffer, no input layout.
inline constexpr const char* kFullscreenVS = R"hlsl(
struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

VSOut VSMain(uint vid : SV_VertexID)
{
    float2 uv = float2((vid << 1) & 2, vid & 2);
    VSOut o;
    o.pos = float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 0.0, 1.0);
    o.uv = uv;
    return o;
}
)hlsl";

// mode 0: bilinear (baseline, deliberately soft)
// mode 1: Catmull-Rom (16-tap, sharp, no ringing control)
// mode 2: Catmull-Rom + unsharp (inverse-halo sharpening, the cheap CAS stand-in)
inline constexpr const char* kUpscalePS = R"hlsl(
Texture2D gSrc : register(t0);
SamplerState gSmp : register(s0);

cbuffer PsParams : register(b0)
{
    float gSharp;
    float gMode;
    float2 gSrcSize;
    float gHdr;
    float2 gOutSize;
    float gPad0;
    float4 gCon0;
    float4 gCon1;
    float4 gCon2;
    float4 gCon3;
};

struct VSOut
{
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
};

float3 CatmullRom(float2 uv)
{
    float2 sp = uv * gSrcSize - 0.5;
    float2 base = floor(sp);
    float2 f = sp - base;

    float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    float2 w3 = f * f * (-0.5 + 0.5 * f);

    float wx[4] = { w0.x, w1.x, w2.x, w3.x };
    float wy[4] = { w0.y, w1.y, w2.y, w3.y };

    float3 acc = 0.0;
    [unroll]
    for (int j = 0; j < 4; ++j)
    {
        [unroll]
        for (int i = 0; i < 4; ++i)
        {
            float2 tuv = (base + float2(i, j) + 0.5) / gSrcSize;
            acc += gSrc.SampleLevel(gSmp, tuv, 0).rgb * wx[i] * wy[j];
        }
    }
    return acc;
}

float LanczosWeight(float x)
{
    x = abs(x);
    if (x < 1e-4)
        return 1.0;
    if (x >= 3.0)
        return 0.0;
    const float pi = 3.14159265;
    float px = pi * x;
    return (3.0 * sin(px) * sin(px / 3.0)) / (px * px);
}

float3 Lanczos3(float2 uv)
{
    float2 sp = uv * gSrcSize - 0.5;
    float2 base = floor(sp);
    float3 acc = 0.0;
    float wsum = 0.0;

    [unroll]
    for (int j = -2; j <= 3; ++j)
    {
        [unroll]
        for (int i = -2; i <= 3; ++i)
        {
            float2 texel = base + float2(i, j) + 0.5;
            float2 d = abs(texel - (sp + 0.5));
            float w = LanczosWeight(d.x) * LanczosWeight(d.y);
            acc += gSrc.SampleLevel(gSmp, texel / gSrcSize, 0).rgb * w;
            wsum += w;
        }
    }
    return acc / max(wsum, 1e-4);
}

// --- FSR1 EASU (12-tap edge-adaptive upsampling) ---------------------------
float EasuLuma(float3 c) { return c.b * 0.5 + (c.r * 0.5 + c.g); }

void EasuSet(inout float2 dir, inout float len, float w,
             float lA, float lB, float lC, float lD, float lE)
{
    if (w <= 0.0)
        return;

    float dc = lD - lC;
    float cb = lC - lB;
    float lenX = max(abs(dc), abs(cb));
    lenX = 1.0 / max(lenX, 1e-6);
    float dirX = lD - lB;
    dir.x += dirX * w;
    lenX = saturate(abs(dirX) * lenX);
    lenX *= lenX;
    len += lenX * w;

    float ec = lE - lC;
    float ca = lC - lA;
    float lenY = max(abs(ec), abs(ca));
    lenY = 1.0 / max(lenY, 1e-6);
    float dirY = lE - lA;
    dir.y += dirY * w;
    lenY = saturate(abs(dirY) * lenY);
    lenY *= lenY;
    len += lenY * w;
}

void EasuTap(inout float3 aC, inout float aW, float2 off, float2 dir, float2 len,
             float lob, float clp, float3 c)
{
    float2 v;
    v.x = off.x * dir.x + off.y * dir.y;
    v.y = off.x * (-dir.y) + off.y * dir.x;
    v *= len;
    float d2 = v.x * v.x + v.y * v.y;
    d2 = min(d2, clp);
    float wB = (2.0 / 5.0) * d2 - 1.0;
    float wA = lob * d2 - 1.0;
    wB *= wB;
    wA *= wA;
    wB = (25.0 / 16.0) * wB - (25.0 / 16.0 - 1.0);
    float w = wB * wA;
    aC += c * w;
    aW += w;
}

float3 Easu(float2 uv)
{
    float2 inv = 1.0 / gSrcSize;
    float2 ip = uv * gOutSize;
    float2 pp = ip * gCon0.xy + gCon0.zw;
    float2 fp = floor(pp);
    pp -= fp;

    float3 b = gSrc.SampleLevel(gSmp, (fp + float2(0, -1) + 0.5) * inv, 0).rgb;
    float3 c = gSrc.SampleLevel(gSmp, (fp + float2(1, -1) + 0.5) * inv, 0).rgb;
    float3 e = gSrc.SampleLevel(gSmp, (fp + float2(-1, 0) + 0.5) * inv, 0).rgb;
    float3 f = gSrc.SampleLevel(gSmp, (fp + float2(0, 0) + 0.5) * inv, 0).rgb;
    float3 g = gSrc.SampleLevel(gSmp, (fp + float2(1, 0) + 0.5) * inv, 0).rgb;
    float3 h = gSrc.SampleLevel(gSmp, (fp + float2(2, 0) + 0.5) * inv, 0).rgb;
    float3 i = gSrc.SampleLevel(gSmp, (fp + float2(-1, 1) + 0.5) * inv, 0).rgb;
    float3 j = gSrc.SampleLevel(gSmp, (fp + float2(0, 1) + 0.5) * inv, 0).rgb;
    float3 k = gSrc.SampleLevel(gSmp, (fp + float2(1, 1) + 0.5) * inv, 0).rgb;
    float3 l = gSrc.SampleLevel(gSmp, (fp + float2(2, 1) + 0.5) * inv, 0).rgb;
    float3 n = gSrc.SampleLevel(gSmp, (fp + float2(0, 2) + 0.5) * inv, 0).rgb;
    float3 o = gSrc.SampleLevel(gSmp, (fp + float2(1, 2) + 0.5) * inv, 0).rgb;

    float bL = EasuLuma(b), cL = EasuLuma(c), eL = EasuLuma(e), fL = EasuLuma(f);
    float gL = EasuLuma(g), hL = EasuLuma(h), iL = EasuLuma(i), jL = EasuLuma(j);
    float kL = EasuLuma(k), lL = EasuLuma(l), nL = EasuLuma(n), oL = EasuLuma(o);

    float2 dir = 0.0;
    float len = 0.0;
    EasuSet(dir, len, (1.0 - pp.x) * (1.0 - pp.y), bL, eL, fL, gL, jL);
    EasuSet(dir, len, pp.x * (1.0 - pp.y), cL, fL, gL, hL, kL);
    EasuSet(dir, len, (1.0 - pp.x) * pp.y, fL, iL, jL, kL, nL);
    EasuSet(dir, len, pp.x * pp.y, gL, jL, kL, lL, oL);

    float2 dir2 = dir * dir;
    float dirR = dir2.x + dir2.y;
    bool zro = dirR < (1.0 / 32768.0);
    dirR = rsqrt(max(dirR, 1e-12));
    dirR = zro ? 1.0 : dirR;
    dir.x = zro ? 1.0 : dir.x;
    dir *= dirR;

    len *= 0.5;
    len *= len;

    float stretch = (dir.x * dir.x + dir.y * dir.y) / max(abs(dir.x), abs(dir.y));
    float2 len2 = float2(1.0 + (stretch - 1.0) * len, 1.0 - 0.5 * len);
    float lob = 0.5 + ((1.0 / 4.0 - 0.04) - 0.5) * len;
    float clp = 1.0 / lob;

    float3 mn4 = min(min(min(f, g), j), k);
    float3 mx4 = max(max(max(f, g), j), k);

    float3 aC = 0.0;
    float aW = 0.0;
    EasuTap(aC, aW, float2(0, -1) - pp, dir, len2, lob, clp, b);
    EasuTap(aC, aW, float2(1, -1) - pp, dir, len2, lob, clp, c);
    EasuTap(aC, aW, float2(-1, 1) - pp, dir, len2, lob, clp, i);
    EasuTap(aC, aW, float2(0, 1) - pp, dir, len2, lob, clp, j);
    EasuTap(aC, aW, float2(0, 0) - pp, dir, len2, lob, clp, f);
    EasuTap(aC, aW, float2(-1, 0) - pp, dir, len2, lob, clp, e);
    EasuTap(aC, aW, float2(1, 1) - pp, dir, len2, lob, clp, k);
    EasuTap(aC, aW, float2(2, 1) - pp, dir, len2, lob, clp, l);
    EasuTap(aC, aW, float2(2, 0) - pp, dir, len2, lob, clp, h);
    EasuTap(aC, aW, float2(1, 0) - pp, dir, len2, lob, clp, g);
    EasuTap(aC, aW, float2(1, 2) - pp, dir, len2, lob, clp, o);
    EasuTap(aC, aW, float2(0, 2) - pp, dir, len2, lob, clp, n);

    return clamp(aC / max(aW, 1e-6), mn4, mx4);
}

float4 PSMain(VSOut i) : SV_TARGET
{
    float3 col;

    if (gMode < 0.5)
    {
        col = gSrc.SampleLevel(gSmp, i.uv, 0).rgb;
    }
    else if (gMode < 1.5)
    {
        col = CatmullRom(i.uv);
    }
    else if (gMode < 2.5)
    {
        col = CatmullRom(i.uv);

        float2 o = 1.0 / gSrcSize;
        float3 blur =
            (gSrc.SampleLevel(gSmp, i.uv + float2(o.x, 0), 0).rgb +
             gSrc.SampleLevel(gSmp, i.uv - float2(o.x, 0), 0).rgb +
             gSrc.SampleLevel(gSmp, i.uv + float2(0, o.y), 0).rgb +
             gSrc.SampleLevel(gSmp, i.uv - float2(0, o.y), 0).rgb) * 0.25;
        col += (col - blur) * gSharp;
    }
    else if (gMode < 3.5)
    {
        // Lanczos3 + contrast-adaptive sharpening, clamped to the local
        // neighbourhood so dark detail is not crushed into black.
        col = Lanczos3(i.uv);

        float2 o = 1.0 / gSrcSize;
        float3 n = gSrc.SampleLevel(gSmp, i.uv + float2(o.x, 0), 0).rgb;
        float3 s = gSrc.SampleLevel(gSmp, i.uv - float2(o.x, 0), 0).rgb;
        float3 e = gSrc.SampleLevel(gSmp, i.uv + float2(0, o.y), 0).rgb;
        float3 w = gSrc.SampleLevel(gSmp, i.uv - float2(0, o.y), 0).rgb;

        float3 blur = (n + s + e + w) * 0.25;
        float3 mn = min(min(min(n, s), min(e, w)), col);
        float3 mx = max(max(max(n, s), max(e, w)), col);
        float3 sharpened = col + (col - blur) * gSharp;
        col = clamp(sharpened, mn, mx);
    }
    else
    {
        col = Easu(i.uv);
    }

    if (gHdr > 0.5)
    {
        // SDR source into an HDR10 (PQ) container at ~100 nits reference.
        float3 lin = pow(max(col, 0.0), 2.2) * (100.0 / 10000.0);
        const float m1 = 0.1593017578125;
        const float m2 = 78.84375;
        const float c1 = 0.8359375;
        const float c2 = 18.8515625;
        const float c3 = 18.6875;
        float3 p = pow(lin, m1);
        col = pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
    }

    return float4(saturate(col), 1.0);
}
)hlsl";

}  // namespace brp::shaders
