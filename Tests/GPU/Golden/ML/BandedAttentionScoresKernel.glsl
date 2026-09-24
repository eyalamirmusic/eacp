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
    float u7;
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
    uint t0 = (gid / uniforms.u2);
    uint t1 = (t0 / uniforms.u0);
    uint t2 = ((t1 / uniforms.u4) * uniforms.u4);
    uint t3 = ((max(t1, (t2 + uniforms.u5)) - uniforms.u5) + (gid % uniforms.u2));
    if ((t3 < min(min((t2 + uniforms.u4), uniforms.u3), ((t1 + uniforms.u6) + 1u))))
    {
        float v0 = 0.0;
        uint v1 = 0u;
        while ((v1 < uniforms.u1))
        {
            v0 = (v0 + (buffer0[((t0 * uniforms.u1) + v1)] * buffer1[((((t3 * uniforms.u0) + (t0 % uniforms.u0)) * uniforms.u1) + v1)]));
            v1 = (v1 + 1u);
        }
        buffer2[gid] = (v0 * uniforms.u7);
    }
}
