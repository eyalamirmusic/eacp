// eacp
RWStructuredBuffer<float> output : register(u0);

[numthreads(64, 1, 1)]
void computeMain(uint3 gid : SV_DispatchThreadID)
{
    output[gid.x] = undeclaredThing * 2.0f;
}
