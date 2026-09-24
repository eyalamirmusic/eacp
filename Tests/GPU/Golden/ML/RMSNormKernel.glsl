#version 450

layout(std140, set = 0, binding = 16) uniform Uniforms
{
    uint u0;
    float u1;
    uint u2;
    uint u3;
    uint u4;
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
layout(std430, set = 0, binding = 2) buffer Buffer2
{
    float buffer2[];
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
        float t0 = buffer0[(((((gid.y / uniforms.u2) * uniforms.u3) + uniforms.u4) + ((gid.y % uniforms.u2) * uniforms.u0)) + v1)];
        v0 = (v0 + (t0 * t0));
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
    v1 = gid.x;
    while ((v1 < uniforms.u0))
    {
        buffer2[((gid.y * uniforms.u0) + v1)] = ((buffer0[(((((gid.y / uniforms.u2) * uniforms.u3) + uniforms.u4) + ((gid.y % uniforms.u2) * uniforms.u0)) + v1)] * inversesqrt(((v2 / float(uniforms.u0)) + uniforms.u1))) * buffer1[v1]);
        v1 = (v1 + 256u);
    }
}
