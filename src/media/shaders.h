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

float4 PSMain(VSOut i) : SV_TARGET
{
    float3 col;

    if (gMode < 0.5)
    {
        col = gSrc.SampleLevel(gSmp, i.uv, 0).rgb;
    }
    else
    {
        col = CatmullRom(i.uv);

        if (gMode > 1.5)
        {
            float2 o = 1.0 / gSrcSize;
            float3 blur =
                (gSrc.SampleLevel(gSmp, i.uv + float2(o.x, 0), 0).rgb +
                 gSrc.SampleLevel(gSmp, i.uv - float2(o.x, 0), 0).rgb +
                 gSrc.SampleLevel(gSmp, i.uv + float2(0, o.y), 0).rgb +
                 gSrc.SampleLevel(gSmp, i.uv - float2(0, o.y), 0).rgb) * 0.25;
            col += (col - blur) * gSharp;
        }
    }

    return float4(saturate(col), 1.0);
}
)hlsl";

}  // namespace brp::shaders
