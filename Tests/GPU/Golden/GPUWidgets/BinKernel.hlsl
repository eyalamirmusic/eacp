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
                            uint v23;
                            InterlockedAdd(buffer3[(v17 + uint(v20))], uint(int(floor(((v22 * v11) + 0.5)))), v23);
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
                    uint v26;
                    InterlockedAdd(buffer4[((v6 + (uint(v12) * v4)) + uint(v24))], 1u, v26);
                }
                else
                {
                    uint t7 = ((v6 + (uint(v12) * v4)) + uint(v24));
                    uint v27;
                    InterlockedAdd(buffer4[t7], 1u, v27);
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
