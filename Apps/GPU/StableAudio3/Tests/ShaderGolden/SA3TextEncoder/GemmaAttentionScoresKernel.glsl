#version 450

layout(std140, set = 0, binding = 16) uniform Uniforms
{
    uint u0;
    uint u1;
    uint u2;
    float u3;
    float u4;
    uint count;
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

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

void main()
{
    uint gid = gl_GlobalInvocationID.x;
    if (gid >= uniforms.count)
        return;
    float v0 = 0.0;
    uint v1 = 0u;
    while ((v1 < uniforms.u1))
    {
        uint t0 = (gid / uniforms.u2);
        v0 = (v0 + (buffer0[((t0 * uniforms.u1) + v1)] * buffer1[(((((gid % uniforms.u2) * uniforms.u0) + (t0 % uniforms.u0)) * uniforms.u1) + v1)]));
        v1 = (v1 + 1u);
    }
    uint t1 = (gid / uniforms.u2);
    uint t2 = (gid % uniforms.u2);
    buffer3[gid] = ((uniforms.u4 * tanh(((v0 * uniforms.u3) / uniforms.u4))) + buffer2[(((t1 / uniforms.u0) * uniforms.u2) + t2)]);
}
