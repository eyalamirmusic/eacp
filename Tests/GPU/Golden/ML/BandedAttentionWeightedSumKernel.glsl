#version 450

layout(std140, set = 0, binding = 16) uniform Uniforms
{
    uint u0;
    uint u1;
    uint u2;
    uint u3;
    uint u4;
    uint u5;
    uint u6;
    uint u7;
    uint u8;
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
    uint t0 = (gid / uniforms.u1);
    uint t1 = (t0 / uniforms.u0);
    uint t2 = (max(t1, (((t1 / uniforms.u4) * uniforms.u4) + uniforms.u5)) - uniforms.u5);
    uint v1 = t2;
    while ((v1 < min(min((((t1 / uniforms.u4) * uniforms.u4) + uniforms.u4), uniforms.u3), ((t1 + uniforms.u6) + 1u))))
    {
        v0 = (v0 + (buffer1[(((t0 * uniforms.u2) - t2) + v1)] * buffer0[((((v1 * uniforms.u7) + uniforms.u8) + ((t0 % uniforms.u0) * uniforms.u1)) + (gid % uniforms.u1))]));
        v1 = (v1 + 1u);
    }
    uint t3 = (gid % uniforms.u1);
    buffer3[((t0 * uniforms.u1) + t3)] = (v0 / buffer2[t0]);
}
