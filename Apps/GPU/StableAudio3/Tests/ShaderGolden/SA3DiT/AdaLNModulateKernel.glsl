#version 450

layout(std140, set = 0, binding = 16) uniform Uniforms
{
    uint u0;
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

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

void main()
{
    uvec2 gid = gl_GlobalInvocationID.xy;
    if (gid.x >= uniforms.width || gid.y >= uniforms.height)
        return;
    uint t0 = ((gid.y * uniforms.u0) + gid.x);
    buffer3[t0] = ((buffer0[t0] * (1.0 + buffer1[gid.x])) + buffer2[gid.x]);
}
