StructuredBuffer<int> Flow : register(t0);
RWTexture2D<float2> Motion : register(u0);

cbuffer Params : register(b0)
{
    uint GridW;
    uint GridH;
    uint FullW;
    uint FullH;
};

[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= FullW || id.y >= FullH)
    {
        return;
    }
    uint gx = min(GridW - 1, id.x * GridW / FullW);
    uint gy = min(GridH - 1, id.y * GridH / FullH);
    int raw = Flow[gy * GridW + gx];
    int2 f = int2((raw << 16) >> 16, raw >> 16);
    Motion[id.xy] = float2(f.x, f.y) / 32.0;
}
