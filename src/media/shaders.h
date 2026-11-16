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
    float3 gPad;
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
    else
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
