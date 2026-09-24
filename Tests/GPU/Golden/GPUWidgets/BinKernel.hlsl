struct Uniforms
{
    uint u0;
    uint u1;
    int u2;
    uint count;
};

cbuffer UniformsCB : register(b0)
{
    Uniforms uniforms;
};

StructuredBuffer<float> buffer0 : register(t0);
StructuredBuffer<float> buffer1 : register(t1);
StructuredBuffer<float> buffer2 : register(t2);
RWStructuredBuffer<uint> buffer3 : register(u3);
RWStructuredBuffer<uint> buffer4 : register(u4);
RWStructuredBuffer<uint> buffer5 : register(u5);
RWStructuredBuffer<float> buffer6 : register(u6);

[numthreads(64, 1, 1)]
void computeMain(uint3 threadId : SV_DispatchThreadID)
{
    uint gid = threadId.x;
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
    float4 t2 = float4(buffer1[t1], buffer1[t1 + 1u], buffer1[t1 + 2u], buffer1[t1 + 3u]);
    uint v2 = uint((t2).x);
    uint v3 = uint((t2).z);
    uint v4 = ((uint((t2).y) + 15u) / 16u);
    uint v5 = ((v3 + 15u) / 16u);
    uint v6 = uint((t2).w);
    uint t3 = (gid * 4u);
    float4 v7 = float4(buffer0[t3], buffer0[t3 + 1u], buffer0[t3 + 2u], buffer0[t3 + 3u]);
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
                            uint v25;
                            InterlockedAdd(buffer3[(v19 + uint(v22))], uint(int(floor(((v24 * v13) + 0.5)))), v25);
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
                    uint v28;
                    InterlockedAdd(buffer4[((v6 + (uint(v14) * v4)) + uint(v26))], 1u, v28);
                }
                else
                {
                    uint t7 = ((v6 + (uint(v14) * v4)) + uint(v26));
                    uint v29;
                    InterlockedAdd(buffer4[t7], 1u, v29);
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
