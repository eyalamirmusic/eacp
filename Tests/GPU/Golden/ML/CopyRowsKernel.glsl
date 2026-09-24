#version 450

layout(std140, set = 0, binding = 16) uniform Uniforms
{
    uint u0;
    uint u1;
    uint u2;
    uint width;
    uint height;
} uniforms;

layout(std430, set = 0, binding = 0) readonly buffer Buffer0
{
    float buffer0[];
};
layout(std430, set = 0, binding = 1) buffer Buffer1
{
    float buffer1[];
};

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

void main()
{
    uvec2 gid = gl_GlobalInvocationID.xy;
    if (gid.x >= uniforms.width || gid.y >= uniforms.height)
        return;
    buffer1[(((uniforms.u2 + gid.y) * uniforms.u0) + gid.x)] = buffer0[(((uniforms.u1 + gid.y) * uniforms.u0) + gid.x)];
}
