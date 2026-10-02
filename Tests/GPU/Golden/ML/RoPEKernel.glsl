#version 450

layout(std140, set = 0, binding = 16) uniform Uniforms
{
    uint u0;
    uint u1;
    uint u2;
    uint u3;
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
layout(std430, set = 0, binding = 2) buffer Buffer2
{
    float buffer2[];
};

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

void main()
{
    uint gid = gl_GlobalInvocationID.x;
    if (gid >= uniforms.count)
        return;
    uint t0 = (gid % uniforms.u1);
    if ((t0 < (uniforms.u2 * 2u)))
    {
        uint t1 = (gid / uniforms.u1);
        uint t2 = (t1 * uniforms.u1);
        bool t3 = (t0 < uniforms.u2);
        uint t4 = (t3 ? t0 : (t0 - uniforms.u2));
        float t5 = buffer0[(t2 + t4)];
        float t6 = (float(((t1 / uniforms.u0) % uniforms.u3)) * buffer1[t4]);
        float t7 = cos(t6);
        float t8 = buffer0[((t2 + uniforms.u2) + t4)];
        float t9 = sin(t6);
        buffer2[(t2 + t0)] = (t3 ? ((t5 * t7) - (t8 * t9)) : ((t8 * t7) + (t5 * t9)));
    }
    else
    {
        uint t10 = (((gid / uniforms.u1) * uniforms.u1) + t0);
        buffer2[t10] = buffer0[t10];
    }
}
