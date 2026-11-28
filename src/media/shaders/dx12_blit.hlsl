Texture2D<float4> Source : register(t0);
SamplerState LinearClamp : register(s0);

struct VsOut
{
    float4 position : SV_Position;
    float2 uv : TEXCOORD0;
};

// Fullscreen triangle: no vertex buffer, no constant buffer, no root constants.
VsOut VSMain(uint id : SV_VertexID)
{
    float2 p = float2((id == 2) ? 3.0 : -1.0, (id == 1) ? 3.0 : -1.0);
    VsOut o;
    o.position = float4(p, 0.0, 1.0);
    o.uv = float2((p.x + 1.0) * 0.5, 1.0 - (p.y + 1.0) * 0.5);
    return o;
}

float4 PSMain(VsOut input) : SV_Target
{
    return Source.Sample(LinearClamp, input.uv);
}
