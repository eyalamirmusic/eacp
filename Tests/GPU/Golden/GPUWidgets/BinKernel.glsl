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
    float v8 = (v7).x;
    float v9 = (v7).y;
    float v10 = min((v7).y, (v7).w);
    float v11 = max((v7).y, (v7).w);
    float v12 = (((v7).z - (v7).x) / ((v7).w - (v7).y));
    float v13 = (((v7).w > (v7).y) ? 1.04858e+06 : -1.04858e+06);
    int v14 = max(int(floor((v10 * 0.0625))), 0);
    int v15 = min((int(ceil((v11 * 0.0625))) - 1), (int(v5) - 1));
    while ((v14 <= v15))
    {
        float v16 = max(v10, (float(v14) * 16.0));
        float v17 = min(v11, (float((v14 + 1)) * 16.0));
        if ((v17 > v16))
        {
            float t4 = (v8 + ((v16 - v9) * v12));
            float t5 = (v8 + ((v17 - v9) * v12));
            int v18 = max(int(ceil((max(t4, t5) * 0.0625))), 0);
            if ((uniforms.u0 == 0u))
            {
                uint t6 = uint(v18);
                if ((t6 < v4))
                {
                    uint v19 = (v2 + (t6 * v3));
                    float v20 = v16;
                    float v21 = v17;
                    int v22 = max(int(floor(v20)), 0);
                    int v23 = min(int(ceil(v21)), int(v3));
                    while ((v22 < v23))
                    {
                        float v24 = (min(v21, float((v22 + 1))) - max(v20, float(v22)));
                        if ((v24 > 0.0))
                        {
                            uint v25 = atomicAdd(buffer3[(v19 + uint(v22))], uint(int(floor(((v24 * v13) + 0.5)))));
                        }
                        v22 = (v22 + 1);
                    }
                }
            }
            int v26 = max(int(floor((min(t4, t5) * 0.0625))), 0);
            int v27 = min((v18 - 1), (int(v4) - 1));
            while ((v26 <= v27))
            {
                if ((uniforms.u0 == 0u))
                {
                    uint v28 = atomicAdd(buffer4[((v6 + (uint(v14) * v4)) + uint(v26))], 1u);
                }
                else
                {
                    uint t7 = ((v6 + (uint(v14) * v4)) + uint(v26));
                    uint v29 = atomicAdd(buffer4[t7], 1u);
                    uint v30 = (buffer5[t7] + v29);
                    if ((v30 < uniforms.u1))
                    {
                        uint t8 = (v30 * 4u);
                        buffer6[t8] = (v7).x;
                        buffer6[(t8 + 1u)] = (v7).y;
                        buffer6[(t8 + 2u)] = (v7).z;
                        buffer6[(t8 + 3u)] = (v7).w;
                    }
                }
                v26 = (v26 + 1);
            }
        }
        v14 = (v14 + 1);
    }
}
