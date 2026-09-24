#version 450

layout(std140, set = 0, binding = 16) uniform Uniforms
{
    int u0;
    uint count;
} uniforms;

layout(std430, set = 0, binding = 0) readonly buffer Buffer0
{
    float buffer0[];
};
layout(std430, set = 0, binding = 1) buffer Buffer1
{
    uint buffer1[];
};
layout(std430, set = 0, binding = 2) readonly buffer Buffer2
{
    float buffer2[];
};

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

void main()
{
    uint gid = gl_GlobalInvocationID.x;
    if (gid >= uniforms.count)
        return;
    int v0 = 0;
    int v1 = uniforms.u0;
    while ((v0 < v1))
    {
        int t0 = ((v0 + v1) >> 1);
        if ((buffer2[uint(t0)] <= float(gid)))
        {
            v0 = (t0 + 1);
        }
        else
        {
            v1 = t0;
        }
    }
    uint t1 = uint((max(v0, 1) - 1));
    uint t2 = ((t1 * 2u) * 4u);
    vec4 t3 = vec4(buffer0[t2], buffer0[t2 + 1u], buffer0[t2 + 2u], buffer0[t2 + 3u]);
    uint v2 = uint((t3).x);
    uint v3 = uint((t3).z);
    uint v4 = ((uint((t3).y) + 15u) / 16u);
    uint v5 = (gid - uint(buffer2[t1]));
    uint v6 = 0u;
    uint v7 = 0u;
    while ((v7 < v4))
    {
        uint t4 = ((v2 + (v7 * v3)) + v5);
        v6 = (v6 + buffer1[t4]);
        buffer1[t4] = v6;
        v7 = (v7 + 1u);
    }
}
