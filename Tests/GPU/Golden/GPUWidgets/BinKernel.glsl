#version 450

layout(std140, set = 0, binding = 16) uniform Uniforms
{
    uint u0;
    uint u1;
    int u2;
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
    uint buffer3[];
};
layout(std430, set = 0, binding = 4) buffer Buffer4
{
    uint buffer4[];
};
layout(std430, set = 0, binding = 5) buffer Buffer5
{
    uint buffer5[];
};
layout(std430, set = 0, binding = 6) buffer Buffer6
{
    float buffer6[];
};

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

void main()
{
    uint gid = gl_GlobalInvocationID.x;
    if (gid >= uniforms.count)
        return;
    int v0 = 0;
    int v1 = uniforms.u2;
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
    uint t1 = ((uint((max(v0, 1) - 1)) * 2u) * 4u);
    vec4 t2 = vec4(buffer1[t1], buffer1[t1 + 1u], buffer1[t1 + 2u], buffer1[t1 + 3u]);
    uint v2 = uint((t2).x);
    uint v3 = uint((t2).z);
    uint v4 = ((uint((t2).y) + 15u) / 16u);
    uint v5 = ((v3 + 15u) / 16u);
    uint v6 = uint((t2).w);
    uint t3 = (gid * 4u);
    vec4 v7 = vec4(buffer0[t3], buffer0[t3 + 1u], buffer0[t3 + 2u], buffer0[t3 + 3u]);
    float v8 = min((v7).y, (v7).w);
    float v9 = max((v7).y, (v7).w);
    float v10 = (((v7).z - (v7).x) / ((v7).w - (v7).y));
    float v11 = (((v7).w > (v7).y) ? 1048576.0 : -1048576.0);
    int v12 = max(int(floor((v8 * 0.0625))), 0);
    int v13 = min((int(ceil((v9 * 0.0625))) - 1), (int(v5) - 1));
    while ((v12 <= v13))
    {
        float v14 = max(v8, (float(v12) * 16.0));
        float v15 = min(v9, (float((v12 + 1)) * 16.0));
        if ((v15 > v14))
        {
            float t4 = ((v14 == (v7).w) ? (v7).z : ((v7).x + ((v14 - (v7).y) * v10)));
            float t5 = ((v15 == (v7).w) ? (v7).z : ((v7).x + ((v15 - (v7).y) * v10)));
            int v16 = max(int(ceil((max(t4, t5) * 0.0625))), 0);
            if ((uniforms.u0 == 0u))
            {
                uint t6 = uint(v16);
                if ((t6 < v4))
                {
                    uint v17 = (v2 + (t6 * v3));
                    float v18 = v14;
                    float v19 = v15;
                    int v20 = max(int(floor(v18)), 0);
                    int v21 = min(int(ceil(v19)), int(v3));
                    while ((v20 < v21))
                    {
                        float v22 = (min(v19, float((v20 + 1))) - max(v18, float(v20)));
                        if ((v22 > 0.0))
                        {
                            uint v23 = atomicAdd(buffer3[(v17 + uint(v20))], uint(int(floor(((v22 * v11) + 0.5)))));
                        }
                        v20 = (v20 + 1);
                    }
                }
            }
            int v24 = max(int(floor((min(t4, t5) * 0.0625))), 0);
            int v25 = min((v16 - 1), (int(v4) - 1));
            while ((v24 <= v25))
            {
                if ((uniforms.u0 == 0u))
                {
                    uint v26 = atomicAdd(buffer4[((v6 + (uint(v12) * v4)) + uint(v24))], 1u);
                }
                else
                {
                    uint t7 = ((v6 + (uint(v12) * v4)) + uint(v24));
                    uint v27 = atomicAdd(buffer4[t7], 1u);
                    uint v28 = (buffer5[t7] + v27);
                    if ((v28 < uniforms.u1))
                    {
                        uint t8 = (v28 * 4u);
                        buffer6[t8] = (v7).x;
                        buffer6[(t8 + 1u)] = (v7).y;
                        buffer6[(t8 + 2u)] = (v7).z;
                        buffer6[(t8 + 3u)] = (v7).w;
                    }
                }
                v24 = (v24 + 1);
            }
        }
        v12 = (v12 + 1);
    }
}
