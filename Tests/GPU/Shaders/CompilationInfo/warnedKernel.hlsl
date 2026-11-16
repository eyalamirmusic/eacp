// eacp
RWStructuredBuffer<float> output : register(u0);

[numthreads(64, 1, 1)]
void computeMain(uint3 gid : SV_DispatchThreadID)
{
    float2 v = float4(1, 2, 3, 4);
    output[gid.x] = v.x;
}
