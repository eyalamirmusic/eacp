struct Uniforms
{
    uint u0;
    uint width;
    uint height;
    uint depth;
};

cbuffer UniformsCB : register(b0)
{
    Uniforms uniforms;
};

RWStructuredBuffer<float> buffer0 : register(u0);
RWStructuredBuffer<float> buffer1 : register(u1);

groupshared float groupScratch[256];

[numthreads(256, 1, 1)]
void computeMain(uint3 threadId : SV_DispatchThreadID, uint groupLane : SV_GroupIndex)
{
    uint3 gid = threadId.xyz;
    float v0 = -3e+38;
    uint v1 = gid.x;
    while ((v1 < uniforms.u0))
    {
        v0 = max(v0, buffer0[((((gid.y * uniforms.depth) + gid.z) * uniforms.u0) + v1)]);
        v1 = (v1 + 256u);
    }
    groupScratch[groupLane] = v0;
    GroupMemoryBarrierWithGroupSync();
    for (uint gr2 = 128u; gr2 > 0u; gr2 >>= 1u)
    {
        if (groupLane < gr2 && groupLane + gr2 < 256u)
            groupScratch[groupLane] = max(groupScratch[groupLane], groupScratch[groupLane + gr2]);
        GroupMemoryBarrierWithGroupSync();
    }
    float v2 = groupScratch[0];
    GroupMemoryBarrierWithGroupSync();
    float v3 = 0.0;
    v1 = gid.x;
    while ((v1 < uniforms.u0))
    {
        uint t0 = ((((gid.y * uniforms.depth) + gid.z) * uniforms.u0) + v1);
        float t1 = exp((buffer0[t0] - v2));
        buffer0[t0] = t1;
        v3 = (v3 + t1);
        v1 = (v1 + 256u);
    }
    groupScratch[groupLane] = v3;
    GroupMemoryBarrierWithGroupSync();
    for (uint gr4 = 128u; gr4 > 0u; gr4 >>= 1u)
    {
        if (groupLane < gr4 && groupLane + gr4 < 256u)
            groupScratch[groupLane] = groupScratch[groupLane] + groupScratch[groupLane + gr4];
        GroupMemoryBarrierWithGroupSync();
    }
    float v4 = groupScratch[0];
    GroupMemoryBarrierWithGroupSync();
    if ((gid.x == 0u))
    {
        buffer1[((gid.y * uniforms.depth) + gid.z)] = v4;
    }
}
