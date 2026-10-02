struct Uniforms
{
    uint u0;
    float u1;
    uint width;
    uint height;
};

cbuffer UniformsCB : register(b0)
{
    Uniforms uniforms;
};

StructuredBuffer<float> buffer0 : register(t0);
StructuredBuffer<float> buffer1 : register(t1);
StructuredBuffer<float> buffer2 : register(t2);
RWStructuredBuffer<float> buffer3 : register(u3);

groupshared float groupScratch[256];

[numthreads(256, 1, 1)]
void computeMain(uint3 threadId : SV_DispatchThreadID, uint groupLane : SV_GroupIndex)
{
    uint2 gid = threadId.xy;
    float v0 = 0.0;
    uint v1 = gid.x;
    while ((v1 < uniforms.u0))
    {
        v0 = (v0 + buffer0[((gid.y * uniforms.u0) + v1)]);
        v1 = (v1 + 256u);
    }
    groupScratch[groupLane] = v0;
    GroupMemoryBarrierWithGroupSync();
    for (uint gr2 = 128u; gr2 > 0u; gr2 >>= 1u)
    {
        if (groupLane < gr2 && groupLane + gr2 < 256u)
            groupScratch[groupLane] = groupScratch[groupLane] + groupScratch[groupLane + gr2];
        GroupMemoryBarrierWithGroupSync();
    }
    float v2 = groupScratch[0];
    GroupMemoryBarrierWithGroupSync();
    float v3 = 0.0;
    v1 = gid.x;
    while ((v1 < uniforms.u0))
    {
        float t0 = (buffer0[((gid.y * uniforms.u0) + v1)] - (v2 / float(uniforms.u0)));
        v3 = (v3 + (t0 * t0));
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
    v1 = gid.x;
    while ((v1 < uniforms.u0))
    {
        uint t1 = (gid.y * uniforms.u0);
        buffer3[(t1 + v1)] = ((((buffer0[(t1 + v1)] - (v2 / float(uniforms.u0))) * rsqrt(((v4 / float(uniforms.u0)) + uniforms.u1))) * buffer1[v1]) + buffer2[v1]);
        v1 = (v1 + 256u);
    }
}
