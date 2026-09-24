#version 450

layout(std140, set = 0, binding = 16) uniform Uniforms
{
    uint u0;
    uint width;
    uint height;
    uint depth;
} uniforms;

layout(std430, set = 0, binding = 0) buffer Buffer0
{
    float buffer0[];
};
layout(std430, set = 0, binding = 1) buffer Buffer1
{
    float buffer1[];
};

shared float groupScratch[256];

layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

void main()
{
    uvec3 gid = gl_GlobalInvocationID.xyz;
    float v0 = -3e+38;
    uint v1 = gid.x;
    while ((v1 < uniforms.u0))
    {
        v0 = max(v0, buffer0[((((gid.y * uniforms.depth) + gid.z) * uniforms.u0) + v1)]);
        v1 = (v1 + 256u);
    }
    groupScratch[gl_LocalInvocationIndex] = v0;
    memoryBarrierShared();
    barrier();
    for (uint gr2 = 128u; gr2 > 0u; gr2 >>= 1u)
    {
        if (gl_LocalInvocationIndex < gr2 && gl_LocalInvocationIndex + gr2 < 256u)
            groupScratch[gl_LocalInvocationIndex] = max(groupScratch[gl_LocalInvocationIndex], groupScratch[gl_LocalInvocationIndex + gr2]);
        memoryBarrierShared();
        barrier();
    }
    float v2 = groupScratch[0];
    memoryBarrierShared();
    barrier();
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
    groupScratch[gl_LocalInvocationIndex] = v3;
    memoryBarrierShared();
    barrier();
    for (uint gr4 = 128u; gr4 > 0u; gr4 >>= 1u)
    {
        if (gl_LocalInvocationIndex < gr4 && gl_LocalInvocationIndex + gr4 < 256u)
            groupScratch[gl_LocalInvocationIndex] = groupScratch[gl_LocalInvocationIndex] + groupScratch[gl_LocalInvocationIndex + gr4];
        memoryBarrierShared();
        barrier();
    }
    float v4 = groupScratch[0];
    memoryBarrierShared();
    barrier();
    if ((gid.x == 0u))
    {
        buffer1[((gid.y * uniforms.depth) + gid.z)] = v4;
    }
}
