#version 450

layout(std140, set = 0, binding = 16) uniform Uniforms
{
    uint u0;
    uint u1;
    uint u2;
    uint u3;
    uint u4;
    uint u5;
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
    uint t0 = ((gid.y / uniforms.u3) * uniforms.u3);
    uint t1 = (max(gid.y, (t0 + uniforms.u4)) - uniforms.u4);
    uint t2 = (t1 - t0);
    uint t3 = ((t0 + gid.x) + (((t2 > gid.x) ? (((t2 - gid.x) + 255u) / 256u) : 0u) * 256u));
    uint v1 = t3;
    while ((v1 < min(min((t0 + uniforms.u3), uniforms.u2), ((gid.y + uniforms.u5) + 1u))))
    {
        v0 = max(v0, buffer0[(((((gid.y * uniforms.u0) + gid.z) * uniforms.u1) - t1) + v1)]);
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
    v1 = t3;
    while ((v1 < min(min((t0 + uniforms.u3), uniforms.u2), ((gid.y + uniforms.u5) + 1u))))
    {
        uint t4 = (((((gid.y * uniforms.u0) + gid.z) * uniforms.u1) - t1) + v1);
        float t5 = exp((buffer0[t4] - v2));
        buffer0[t4] = t5;
        v3 = (v3 + t5);
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
        buffer1[((gid.y * uniforms.u0) + gid.z)] = v4;
    }
}
