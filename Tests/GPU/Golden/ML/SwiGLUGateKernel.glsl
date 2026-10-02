#version 450

layout(std140, set = 0, binding = 16) uniform Uniforms
{
    uint u0;
    uint count;
} uniforms;

layout(std430, set = 0, binding = 0) readonly buffer Buffer0
{
    float buffer0[];
};
layout(std430, set = 0, binding = 1) buffer Buffer1
{
    float buffer1[];
};

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

void main()
{
    uint gid = gl_GlobalInvocationID.x;
    if (gid >= uniforms.count)
        return;
    uint t0 = (((gid / uniforms.u0) * uniforms.u0) * 2u);
    uint t1 = (gid % uniforms.u0);
    float t2 = buffer0[((t0 + uniforms.u0) + t1)];
    buffer1[gid] = (buffer0[(t0 + t1)] * (t2 / (1.0 + exp((-(t2))))));
}
