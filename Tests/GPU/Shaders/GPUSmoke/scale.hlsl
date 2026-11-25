StructuredBuffer<float> input : register(t0);
RWStructuredBuffer<float> output : register(u1);
cbuffer Params : register(b0) { float scale; uint count; };

[numthreads(64, 1, 1)]
void computeMain(uint3 gid : SV_DispatchThreadID)
{
    if (gid.x >= count) return;
    output[gid.x] = input[gid.x] * scale;
}
