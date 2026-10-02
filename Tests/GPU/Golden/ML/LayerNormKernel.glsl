#version 450

layout(std140, set = 0, binding = 16) uniform Uniforms
{
    uint u0;
    float u1;
    uint width;
    uint height;
} uniforms;

layout(std430, set = 0, binding = 0) readonly buffer Buffer0
{
    float buffer0[];
};
layout(std430, set = 0, binding = 1) readonly buffer Buffer1
{
    float buffer1[];
};
layout(std430, set = 0, binding = 2) readonly buffer Buffer2
{
    float buffer2[];
};
layout(std430, set = 0, binding = 3) buffer Buffer3
{
    float buffer3[];
};

shared float groupScratch[256];

layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

void main()
{
    uvec2 gid = gl_GlobalInvocationID.xy;
    float v0 = 0.0;
    uint v1 = gid.x;
    while ((v1 < uniforms.u0))
    {
        v0 = (v0 + buffer0[((gid.y * uniforms.u0) + v1)]);
        v1 = (v1 + 256u);
    }
    groupScratch[gl_LocalInvocationIndex] = v0;
    memoryBarrierShared();
    barrier();
    for (uint gr2 = 128u; gr2 > 0u; gr2 >>= 1u)
    {
        if (gl_LocalInvocationIndex < gr2 && gl_LocalInvocationIndex + gr2 < 256u)
            groupScratch[gl_LocalInvocationIndex] = groupScratch[gl_LocalInvocationIndex] + groupScratch[gl_LocalInvocationIndex + gr2];
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
        float t0 = (buffer0[((gid.y * uniforms.u0) + v1)] - (v2 / float(uniforms.u0)));
        v3 = (v3 + (t0 * t0));
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
    v1 = gid.x;
    while ((v1 < uniforms.u0))
    {
        uint t1 = (gid.y * uniforms.u0);
        buffer3[(t1 + v1)] = ((((buffer0[(t1 + v1)] - (v2 / float(uniforms.u0))) * inversesqrt(((v4 / float(uniforms.u0)) + uniforms.u1))) * buffer1[v1]) + buffer2[v1]);
        v1 = (v1 + 256u);
    }
}
