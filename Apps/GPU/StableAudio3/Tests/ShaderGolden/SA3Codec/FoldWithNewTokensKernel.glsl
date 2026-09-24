#version 450

layout(std140, set = 0, binding = 16) uniform Uniforms
{
    uint u0;
    uint u1;
    uint u2;
    uint u3;
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

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

void main()
{
    uvec2 gid = gl_GlobalInvocationID.xy;
    if (gid.x >= uniforms.width || gid.y >= uniforms.height)
        return;
    uint t0 = (gid.y % uniforms.u2);
    buffer2[((gid.y * uniforms.u0) + gid.x)] = ((t0 < uniforms.u1) ? buffer0[((min((((gid.y / uniforms.u2) * uniforms.u1) + t0), (uniforms.u3 - 1u)) * uniforms.u0) + gid.x)] : buffer1[gid.x]);
}
