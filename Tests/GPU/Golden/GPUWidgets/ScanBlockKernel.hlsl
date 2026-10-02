struct Uniforms
{
    uint u0;
    uint count;
};

cbuffer UniformsCB : register(b0)
{
    Uniforms uniforms;
};

RWStructuredBuffer<uint> buffer0 : register(u0);
RWStructuredBuffer<uint> buffer1 : register(u1);
RWStructuredBuffer<uint> buffer2 : register(u2);

groupshared uint s0[64];

[numthreads(64, 1, 1)]
void computeMain(uint3 threadId : SV_DispatchThreadID, uint3 localThread : SV_GroupThreadID, uint3 groupIndex : SV_GroupID)
{
    uint gid = threadId.x;
    uint lid = localThread.x;
    uint tgid = groupIndex.x;
    uint v0 = ((tgid * 1024u) + (lid * 16u));
    uint v1 = 0u;
    uint t0 = (v0 + 0u);
    if ((t0 < uniforms.u0))
    {
        v1 = (v1 + buffer0[t0]);
    }
    uint t1 = (v0 + 1u);
    if ((t1 < uniforms.u0))
    {
        v1 = (v1 + buffer0[t1]);
    }
    uint t2 = (v0 + 2u);
    if ((t2 < uniforms.u0))
    {
        v1 = (v1 + buffer0[t2]);
    }
    uint t3 = (v0 + 3u);
    if ((t3 < uniforms.u0))
    {
        v1 = (v1 + buffer0[t3]);
    }
    uint t4 = (v0 + 4u);
    if ((t4 < uniforms.u0))
    {
        v1 = (v1 + buffer0[t4]);
    }
    uint t5 = (v0 + 5u);
    if ((t5 < uniforms.u0))
    {
        v1 = (v1 + buffer0[t5]);
    }
    uint t6 = (v0 + 6u);
    if ((t6 < uniforms.u0))
    {
        v1 = (v1 + buffer0[t6]);
    }
    uint t7 = (v0 + 7u);
    if ((t7 < uniforms.u0))
    {
        v1 = (v1 + buffer0[t7]);
    }
    uint t8 = (v0 + 8u);
    if ((t8 < uniforms.u0))
    {
        v1 = (v1 + buffer0[t8]);
    }
    uint t9 = (v0 + 9u);
    if ((t9 < uniforms.u0))
    {
        v1 = (v1 + buffer0[t9]);
    }
    uint t10 = (v0 + 10u);
    if ((t10 < uniforms.u0))
    {
        v1 = (v1 + buffer0[t10]);
    }
    uint t11 = (v0 + 11u);
    if ((t11 < uniforms.u0))
    {
        v1 = (v1 + buffer0[t11]);
    }
    uint t12 = (v0 + 12u);
    if ((t12 < uniforms.u0))
    {
        v1 = (v1 + buffer0[t12]);
    }
    uint t13 = (v0 + 13u);
    if ((t13 < uniforms.u0))
    {
        v1 = (v1 + buffer0[t13]);
    }
    uint t14 = (v0 + 14u);
    if ((t14 < uniforms.u0))
    {
        v1 = (v1 + buffer0[t14]);
    }
    uint t15 = (v0 + 15u);
    if ((t15 < uniforms.u0))
    {
        v1 = (v1 + buffer0[t15]);
    }
    s0[lid] = v1;
    GroupMemoryBarrierWithGroupSync();
    uint v2 = 0u;
    if ((lid >= 1u))
    {
        v2 = s0[(max(lid, 1u) - 1u)];
    }
    GroupMemoryBarrierWithGroupSync();
    s0[lid] = (s0[lid] + v2);
    GroupMemoryBarrierWithGroupSync();
    uint v3 = 0u;
    if ((lid >= 2u))
    {
        v3 = s0[(max(lid, 2u) - 2u)];
    }
    GroupMemoryBarrierWithGroupSync();
    s0[lid] = (s0[lid] + v3);
    GroupMemoryBarrierWithGroupSync();
    uint v4 = 0u;
    if ((lid >= 4u))
    {
        v4 = s0[(max(lid, 4u) - 4u)];
    }
    GroupMemoryBarrierWithGroupSync();
    s0[lid] = (s0[lid] + v4);
    GroupMemoryBarrierWithGroupSync();
    uint v5 = 0u;
    if ((lid >= 8u))
    {
        v5 = s0[(max(lid, 8u) - 8u)];
    }
    GroupMemoryBarrierWithGroupSync();
    s0[lid] = (s0[lid] + v5);
    GroupMemoryBarrierWithGroupSync();
    uint v6 = 0u;
    if ((lid >= 16u))
    {
        v6 = s0[(max(lid, 16u) - 16u)];
    }
    GroupMemoryBarrierWithGroupSync();
    s0[lid] = (s0[lid] + v6);
    GroupMemoryBarrierWithGroupSync();
    uint v7 = 0u;
    if ((lid >= 32u))
    {
        v7 = s0[(max(lid, 32u) - 32u)];
    }
    GroupMemoryBarrierWithGroupSync();
    s0[lid] = (s0[lid] + v7);
    GroupMemoryBarrierWithGroupSync();
    uint v8 = (s0[lid] - v1);
    uint t16 = (v0 + 0u);
    if ((t16 < uniforms.u0))
    {
        buffer1[t16] = v8;
        v8 = (v8 + buffer0[t16]);
        buffer0[t16] = 0u;
    }
    uint t17 = (v0 + 1u);
    if ((t17 < uniforms.u0))
    {
        buffer1[t17] = v8;
        v8 = (v8 + buffer0[t17]);
        buffer0[t17] = 0u;
    }
    uint t18 = (v0 + 2u);
    if ((t18 < uniforms.u0))
    {
        buffer1[t18] = v8;
        v8 = (v8 + buffer0[t18]);
        buffer0[t18] = 0u;
    }
    uint t19 = (v0 + 3u);
    if ((t19 < uniforms.u0))
    {
        buffer1[t19] = v8;
        v8 = (v8 + buffer0[t19]);
        buffer0[t19] = 0u;
    }
    uint t20 = (v0 + 4u);
    if ((t20 < uniforms.u0))
    {
        buffer1[t20] = v8;
        v8 = (v8 + buffer0[t20]);
        buffer0[t20] = 0u;
    }
    uint t21 = (v0 + 5u);
    if ((t21 < uniforms.u0))
    {
        buffer1[t21] = v8;
        v8 = (v8 + buffer0[t21]);
        buffer0[t21] = 0u;
    }
    uint t22 = (v0 + 6u);
    if ((t22 < uniforms.u0))
    {
        buffer1[t22] = v8;
        v8 = (v8 + buffer0[t22]);
        buffer0[t22] = 0u;
    }
    uint t23 = (v0 + 7u);
    if ((t23 < uniforms.u0))
    {
        buffer1[t23] = v8;
        v8 = (v8 + buffer0[t23]);
        buffer0[t23] = 0u;
    }
    uint t24 = (v0 + 8u);
    if ((t24 < uniforms.u0))
    {
        buffer1[t24] = v8;
        v8 = (v8 + buffer0[t24]);
        buffer0[t24] = 0u;
    }
    uint t25 = (v0 + 9u);
    if ((t25 < uniforms.u0))
    {
        buffer1[t25] = v8;
        v8 = (v8 + buffer0[t25]);
        buffer0[t25] = 0u;
    }
    uint t26 = (v0 + 10u);
    if ((t26 < uniforms.u0))
    {
        buffer1[t26] = v8;
        v8 = (v8 + buffer0[t26]);
        buffer0[t26] = 0u;
    }
    uint t27 = (v0 + 11u);
    if ((t27 < uniforms.u0))
    {
        buffer1[t27] = v8;
        v8 = (v8 + buffer0[t27]);
        buffer0[t27] = 0u;
    }
    uint t28 = (v0 + 12u);
    if ((t28 < uniforms.u0))
    {
        buffer1[t28] = v8;
        v8 = (v8 + buffer0[t28]);
        buffer0[t28] = 0u;
    }
    uint t29 = (v0 + 13u);
    if ((t29 < uniforms.u0))
    {
        buffer1[t29] = v8;
        v8 = (v8 + buffer0[t29]);
        buffer0[t29] = 0u;
    }
    uint t30 = (v0 + 14u);
    if ((t30 < uniforms.u0))
    {
        buffer1[t30] = v8;
        v8 = (v8 + buffer0[t30]);
        buffer0[t30] = 0u;
    }
    uint t31 = (v0 + 15u);
    if ((t31 < uniforms.u0))
    {
        buffer1[t31] = v8;
        v8 = (v8 + buffer0[t31]);
        buffer0[t31] = 0u;
    }
    if ((lid == 63u))
    {
        buffer2[tgid] = s0[63u];
    }
}
