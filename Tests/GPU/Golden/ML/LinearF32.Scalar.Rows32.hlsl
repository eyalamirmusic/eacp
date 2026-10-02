struct Uniforms
{
    uint u0;
    uint u1;
    uint u2;
    uint width;
    uint height;
};

cbuffer UniformsCB : register(b0)
{
    Uniforms uniforms;
};

StructuredBuffer<float> buffer0 : register(t0);
StructuredBuffer<float> buffer1 : register(t1);
RWStructuredBuffer<float> buffer2 : register(u2);

groupshared float s0[2176];

[numthreads(128, 1, 1)]
void computeMain(uint3 threadId : SV_DispatchThreadID, uint3 localThread : SV_GroupThreadID, uint3 groupIndex : SV_GroupID, uint groupLane : SV_GroupIndex)
{
    uint2 gid = threadId.xy;
    uint2 lid = localThread.xy;
    uint2 tgid = groupIndex.xy;
    uint sgmLane = groupLane % 32u;
    uint sgmRow = sgmLane / 4u;
    uint sgmColumn = (sgmLane % 4u) * 2u;
    uint sgmBase = (groupLane / 32u) * 128u;
    float4 t0 = float4(0.0, 0.0, 0.0, 0.0);
    float4 v0 = t0;
    float4 v1 = t0;
    float4 v2 = t0;
    float4 v3 = t0;
    float4 v4 = t0;
    float4 v5 = t0;
    float4 v6 = t0;
    float4 v7 = t0;
    float2 sgm0 = float2(0.0, 0.0);
    float2 sgm1 = float2(0.0, 0.0);
    float2 sgm2 = float2(0.0, 0.0);
    float2 sgm3 = float2(0.0, 0.0);
    float2 sgm4 = float2(0.0, 0.0);
    float2 sgm5 = float2(0.0, 0.0);
    float2 sgm6 = float2(0.0, 0.0);
    float2 sgm7 = float2(0.0, 0.0);
    uint t1 = (lid.x + 0u);
    uint t2 = ((t1 % 4u) * 4u);
    uint t3 = (0u + t2);
    uint t4 = (t3 + 0u);
    uint t5 = (tgid.y * 32u);
    uint t6 = (t1 / 4u);
    uint t7 = (t5 + t6);
    uint t8 = (uniforms.u0 - 1u);
    uint t9 = (min(t7, t8) * uniforms.u2);
    uint t10 = (uniforms.u2 - 1u);
    uint t11 = (t3 + 1u);
    uint t12 = (t3 + 2u);
    uint t13 = (t3 + 3u);
    v0 = float4(((t4 < uniforms.u2) ? buffer0[(t9 + min(t4, t10))] : 0.0), ((t11 < uniforms.u2) ? buffer0[(t9 + min(t11, t10))] : 0.0), ((t12 < uniforms.u2) ? buffer0[(t9 + min(t12, t10))] : 0.0), ((t13 < uniforms.u2) ? buffer0[(t9 + min(t13, t10))] : 0.0));
    uint t14 = (tgid.x * 64u);
    uint t15 = (t14 + t6);
    uint t16 = (uniforms.u1 - 1u);
    uint t17 = (min(t15, t16) * uniforms.u2);
    v4 = float4(((t4 < uniforms.u2) ? buffer1[(t17 + min(t4, t10))] : 0.0), ((t11 < uniforms.u2) ? buffer1[(t17 + min(t11, t10))] : 0.0), ((t12 < uniforms.u2) ? buffer1[(t17 + min(t12, t10))] : 0.0), ((t13 < uniforms.u2) ? buffer1[(t17 + min(t13, t10))] : 0.0));
    uint t18 = (lid.x + 128u);
    uint t19 = ((t18 % 4u) * 4u);
    uint t20 = (0u + t19);
    uint t21 = (t20 + 0u);
    uint t22 = (t18 / 4u);
    uint t23 = (t14 + t22);
    uint t24 = (min(t23, t16) * uniforms.u2);
    uint t25 = (t20 + 1u);
    uint t26 = (t20 + 2u);
    uint t27 = (t20 + 3u);
    v5 = float4(((t21 < uniforms.u2) ? buffer1[(t24 + min(t21, t10))] : 0.0), ((t25 < uniforms.u2) ? buffer1[(t24 + min(t25, t10))] : 0.0), ((t26 < uniforms.u2) ? buffer1[(t24 + min(t26, t10))] : 0.0), ((t27 < uniforms.u2) ? buffer1[(t24 + min(t27, t10))] : 0.0));
    uint v8 = 0u;
    while ((v8 < uniforms.u2))
    {
        GroupMemoryBarrierWithGroupSync();
        uint t28 = ((t6 * 24u) + t2);
        s0[t28] = (v0).x;
        s0[(t28 + 1u)] = (v0).y;
        s0[(t28 + 2u)] = (v0).z;
        s0[(t28 + 3u)] = (v0).w;
        uint t29 = ((768u + (t2 * 72u)) + t6);
        s0[t29] = (v4).x;
        s0[(t29 + 72u)] = (v4).y;
        s0[(t29 + 144u)] = (v4).z;
        s0[(t29 + 216u)] = (v4).w;
        uint t30 = ((768u + (t19 * 72u)) + t22);
        s0[t30] = (v5).x;
        s0[(t30 + 72u)] = (v5).y;
        s0[(t30 + 144u)] = (v5).z;
        s0[(t30 + 216u)] = (v5).w;
        GroupMemoryBarrierWithGroupSync();
        uint t31 = (v8 + 16u);
        uint t32 = (t31 + t2);
        uint t33 = (t32 + 0u);
        uint t34 = (min(t7, t8) * uniforms.u2);
        uint t35 = (t32 + 1u);
        uint t36 = (t32 + 2u);
        uint t37 = (t32 + 3u);
        v0 = float4(((t33 < uniforms.u2) ? buffer0[(t34 + min(t33, t10))] : 0.0), ((t35 < uniforms.u2) ? buffer0[(t34 + min(t35, t10))] : 0.0), ((t36 < uniforms.u2) ? buffer0[(t34 + min(t36, t10))] : 0.0), ((t37 < uniforms.u2) ? buffer0[(t34 + min(t37, t10))] : 0.0));
        uint t38 = (t31 + t2);
        uint t39 = (t38 + 0u);
        uint t40 = (min(t15, t16) * uniforms.u2);
        uint t41 = (t38 + 1u);
        uint t42 = (t38 + 2u);
        uint t43 = (t38 + 3u);
        v4 = float4(((t39 < uniforms.u2) ? buffer1[(t40 + min(t39, t10))] : 0.0), ((t41 < uniforms.u2) ? buffer1[(t40 + min(t41, t10))] : 0.0), ((t42 < uniforms.u2) ? buffer1[(t40 + min(t42, t10))] : 0.0), ((t43 < uniforms.u2) ? buffer1[(t40 + min(t43, t10))] : 0.0));
        uint t44 = (t31 + t19);
        uint t45 = (t44 + 0u);
        uint t46 = (min(t23, t16) * uniforms.u2);
        uint t47 = (t44 + 1u);
        uint t48 = (t44 + 2u);
        uint t49 = (t44 + 3u);
        v5 = float4(((t45 < uniforms.u2) ? buffer1[(t46 + min(t45, t10))] : 0.0), ((t47 < uniforms.u2) ? buffer1[(t46 + min(t47, t10))] : 0.0), ((t48 < uniforms.u2) ? buffer1[(t46 + min(t48, t10))] : 0.0), ((t49 < uniforms.u2) ? buffer1[(t46 + min(t49, t10))] : 0.0));
        uint t50 = (((groupLane / 32u) % 1u) * 32u);
        uint t51 = ((t50 + 0u) * 24u);
        float2 sgm8 = float2(s0[((t51 + 0u)) + sgmRow * (24u) + sgmColumn], s0[((t51 + 0u)) + sgmRow * (24u) + sgmColumn + 1u]);
        uint t52 = ((t50 + 8u) * 24u);
        float2 sgm9 = float2(s0[((t52 + 0u)) + sgmRow * (24u) + sgmColumn], s0[((t52 + 0u)) + sgmRow * (24u) + sgmColumn + 1u]);
        uint t53 = ((t50 + 16u) * 24u);
        float2 sgm10 = float2(s0[((t53 + 0u)) + sgmRow * (24u) + sgmColumn], s0[((t53 + 0u)) + sgmRow * (24u) + sgmColumn + 1u]);
        uint t54 = ((t50 + 24u) * 24u);
        float2 sgm11 = float2(s0[((t54 + 0u)) + sgmRow * (24u) + sgmColumn], s0[((t54 + 0u)) + sgmRow * (24u) + sgmColumn + 1u]);
        uint t55 = (((groupLane / 32u) / 1u) * 16u);
        uint t56 = (768u + t55);
        float2 sgm12 = float2(s0[((t56 + 0u)) + sgmRow * (72u) + sgmColumn], s0[((t56 + 0u)) + sgmRow * (72u) + sgmColumn + 1u]);
        float2 sgm13 = float2(s0[((t56 + 8u)) + sgmRow * (72u) + sgmColumn], s0[((t56 + 8u)) + sgmRow * (72u) + sgmColumn + 1u]);
        for (uint sgm0k = 0u; sgm0k < 8u; ++sgm0k)
        {
            float sgm0l = s0[((t51 + 0u)) + sgmRow * (24u) + sgm0k];
            sgm0.x += sgm0l * s0[((t56 + 0u)) + sgm0k * (72u) + sgmColumn];
            sgm0.y += sgm0l * s0[((t56 + 0u)) + sgm0k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm1k = 0u; sgm1k < 8u; ++sgm1k)
        {
            float sgm1l = s0[((t51 + 0u)) + sgmRow * (24u) + sgm1k];
            sgm1.x += sgm1l * s0[((t56 + 8u)) + sgm1k * (72u) + sgmColumn];
            sgm1.y += sgm1l * s0[((t56 + 8u)) + sgm1k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm2k = 0u; sgm2k < 8u; ++sgm2k)
        {
            float sgm2l = s0[((t52 + 0u)) + sgmRow * (24u) + sgm2k];
            sgm2.x += sgm2l * s0[((t56 + 0u)) + sgm2k * (72u) + sgmColumn];
            sgm2.y += sgm2l * s0[((t56 + 0u)) + sgm2k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm3k = 0u; sgm3k < 8u; ++sgm3k)
        {
            float sgm3l = s0[((t52 + 0u)) + sgmRow * (24u) + sgm3k];
            sgm3.x += sgm3l * s0[((t56 + 8u)) + sgm3k * (72u) + sgmColumn];
            sgm3.y += sgm3l * s0[((t56 + 8u)) + sgm3k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm4k = 0u; sgm4k < 8u; ++sgm4k)
        {
            float sgm4l = s0[((t53 + 0u)) + sgmRow * (24u) + sgm4k];
            sgm4.x += sgm4l * s0[((t56 + 0u)) + sgm4k * (72u) + sgmColumn];
            sgm4.y += sgm4l * s0[((t56 + 0u)) + sgm4k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm5k = 0u; sgm5k < 8u; ++sgm5k)
        {
            float sgm5l = s0[((t53 + 0u)) + sgmRow * (24u) + sgm5k];
            sgm5.x += sgm5l * s0[((t56 + 8u)) + sgm5k * (72u) + sgmColumn];
            sgm5.y += sgm5l * s0[((t56 + 8u)) + sgm5k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm6k = 0u; sgm6k < 8u; ++sgm6k)
        {
            float sgm6l = s0[((t54 + 0u)) + sgmRow * (24u) + sgm6k];
            sgm6.x += sgm6l * s0[((t56 + 0u)) + sgm6k * (72u) + sgmColumn];
            sgm6.y += sgm6l * s0[((t56 + 0u)) + sgm6k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm7k = 0u; sgm7k < 8u; ++sgm7k)
        {
            float sgm7l = s0[((t54 + 0u)) + sgmRow * (24u) + sgm7k];
            sgm7.x += sgm7l * s0[((t56 + 8u)) + sgm7k * (72u) + sgmColumn];
            sgm7.y += sgm7l * s0[((t56 + 8u)) + sgm7k * (72u) + sgmColumn + 1u];
        }
        float2 sgm14 = float2(s0[((t51 + 8u)) + sgmRow * (24u) + sgmColumn], s0[((t51 + 8u)) + sgmRow * (24u) + sgmColumn + 1u]);
        float2 sgm15 = float2(s0[((t52 + 8u)) + sgmRow * (24u) + sgmColumn], s0[((t52 + 8u)) + sgmRow * (24u) + sgmColumn + 1u]);
        float2 sgm16 = float2(s0[((t53 + 8u)) + sgmRow * (24u) + sgmColumn], s0[((t53 + 8u)) + sgmRow * (24u) + sgmColumn + 1u]);
        float2 sgm17 = float2(s0[((t54 + 8u)) + sgmRow * (24u) + sgmColumn], s0[((t54 + 8u)) + sgmRow * (24u) + sgmColumn + 1u]);
        uint t57 = (1344u + t55);
        float2 sgm18 = float2(s0[((t57 + 0u)) + sgmRow * (72u) + sgmColumn], s0[((t57 + 0u)) + sgmRow * (72u) + sgmColumn + 1u]);
        float2 sgm19 = float2(s0[((t57 + 8u)) + sgmRow * (72u) + sgmColumn], s0[((t57 + 8u)) + sgmRow * (72u) + sgmColumn + 1u]);
        for (uint sgm0k = 0u; sgm0k < 8u; ++sgm0k)
        {
            float sgm0l = s0[((t51 + 8u)) + sgmRow * (24u) + sgm0k];
            sgm0.x += sgm0l * s0[((t57 + 0u)) + sgm0k * (72u) + sgmColumn];
            sgm0.y += sgm0l * s0[((t57 + 0u)) + sgm0k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm1k = 0u; sgm1k < 8u; ++sgm1k)
        {
            float sgm1l = s0[((t51 + 8u)) + sgmRow * (24u) + sgm1k];
            sgm1.x += sgm1l * s0[((t57 + 8u)) + sgm1k * (72u) + sgmColumn];
            sgm1.y += sgm1l * s0[((t57 + 8u)) + sgm1k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm2k = 0u; sgm2k < 8u; ++sgm2k)
        {
            float sgm2l = s0[((t52 + 8u)) + sgmRow * (24u) + sgm2k];
            sgm2.x += sgm2l * s0[((t57 + 0u)) + sgm2k * (72u) + sgmColumn];
            sgm2.y += sgm2l * s0[((t57 + 0u)) + sgm2k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm3k = 0u; sgm3k < 8u; ++sgm3k)
        {
            float sgm3l = s0[((t52 + 8u)) + sgmRow * (24u) + sgm3k];
            sgm3.x += sgm3l * s0[((t57 + 8u)) + sgm3k * (72u) + sgmColumn];
            sgm3.y += sgm3l * s0[((t57 + 8u)) + sgm3k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm4k = 0u; sgm4k < 8u; ++sgm4k)
        {
            float sgm4l = s0[((t53 + 8u)) + sgmRow * (24u) + sgm4k];
            sgm4.x += sgm4l * s0[((t57 + 0u)) + sgm4k * (72u) + sgmColumn];
            sgm4.y += sgm4l * s0[((t57 + 0u)) + sgm4k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm5k = 0u; sgm5k < 8u; ++sgm5k)
        {
            float sgm5l = s0[((t53 + 8u)) + sgmRow * (24u) + sgm5k];
            sgm5.x += sgm5l * s0[((t57 + 8u)) + sgm5k * (72u) + sgmColumn];
            sgm5.y += sgm5l * s0[((t57 + 8u)) + sgm5k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm6k = 0u; sgm6k < 8u; ++sgm6k)
        {
            float sgm6l = s0[((t54 + 8u)) + sgmRow * (24u) + sgm6k];
            sgm6.x += sgm6l * s0[((t57 + 0u)) + sgm6k * (72u) + sgmColumn];
            sgm6.y += sgm6l * s0[((t57 + 0u)) + sgm6k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm7k = 0u; sgm7k < 8u; ++sgm7k)
        {
            float sgm7l = s0[((t54 + 8u)) + sgmRow * (24u) + sgm7k];
            sgm7.x += sgm7l * s0[((t57 + 8u)) + sgm7k * (72u) + sgmColumn];
            sgm7.y += sgm7l * s0[((t57 + 8u)) + sgm7k * (72u) + sgmColumn + 1u];
        }
        v8 = (v8 + 16u);
    }
    uint t58 = (((groupLane / 32u) % 1u) * 32u);
    uint t59 = (t5 + t58);
    uint t60 = (t59 + 0u);
    uint t61 = (t60 + 8u);
    uint t62 = (((groupLane / 32u) / 1u) * 16u);
    uint t63 = (t14 + t62);
    uint t64 = (t63 + 0u);
    uint t65 = (t64 + 8u);
    bool t66 = ((t61 <= uniforms.u0) && (t65 <= uniforms.u1));
    if (t66)
    {
        buffer2[(((t60 * uniforms.u1) + t64)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm0.x;
        buffer2[(((t60 * uniforms.u1) + t64)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm0.y;
    }
    else
    {
        s0[((1920u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm0.x;
        s0[((1920u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm0.y;
    }
    GroupMemoryBarrierWithGroupSync();
    if ((!(t66)))
    {
        uint t67 = (lid.x % 32u);
        uint t68 = (t67 + 0u);
        uint t69 = (t60 + (t68 / 8u));
        uint t70 = (t64 + (t68 % 8u));
        if (((t69 < uniforms.u0) && (t70 < uniforms.u1)))
        {
            buffer2[((t69 * uniforms.u1) + t70)] = s0[((1920u + ((groupLane / 32u) * 64u)) + t68)];
        }
        uint t71 = (t67 + 32u);
        uint t72 = (t60 + (t71 / 8u));
        uint t73 = (t64 + (t71 % 8u));
        if (((t72 < uniforms.u0) && (t73 < uniforms.u1)))
        {
            buffer2[((t72 * uniforms.u1) + t73)] = s0[((1920u + ((groupLane / 32u) * 64u)) + t71)];
        }
    }
    GroupMemoryBarrierWithGroupSync();
    uint t74 = (t63 + 8u);
    uint t75 = (t74 + 8u);
    bool t76 = ((t61 <= uniforms.u0) && (t75 <= uniforms.u1));
    if (t76)
    {
        buffer2[(((t60 * uniforms.u1) + t74)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm1.x;
        buffer2[(((t60 * uniforms.u1) + t74)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm1.y;
    }
    else
    {
        s0[((1920u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm1.x;
        s0[((1920u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm1.y;
    }
    GroupMemoryBarrierWithGroupSync();
    if ((!(t76)))
    {
        uint t77 = (lid.x % 32u);
        uint t78 = (t77 + 0u);
        uint t79 = (t60 + (t78 / 8u));
        uint t80 = (t74 + (t78 % 8u));
        if (((t79 < uniforms.u0) && (t80 < uniforms.u1)))
        {
            buffer2[((t79 * uniforms.u1) + t80)] = s0[((1920u + ((groupLane / 32u) * 64u)) + t78)];
        }
        uint t81 = (t77 + 32u);
        uint t82 = (t60 + (t81 / 8u));
        uint t83 = (t74 + (t81 % 8u));
        if (((t82 < uniforms.u0) && (t83 < uniforms.u1)))
        {
            buffer2[((t82 * uniforms.u1) + t83)] = s0[((1920u + ((groupLane / 32u) * 64u)) + t81)];
        }
    }
    GroupMemoryBarrierWithGroupSync();
    uint t84 = (t59 + 8u);
    uint t85 = (t84 + 8u);
    bool t86 = ((t85 <= uniforms.u0) && (t65 <= uniforms.u1));
    if (t86)
    {
        buffer2[(((t84 * uniforms.u1) + t64)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm2.x;
        buffer2[(((t84 * uniforms.u1) + t64)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm2.y;
    }
    else
    {
        s0[((1920u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm2.x;
        s0[((1920u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm2.y;
    }
    GroupMemoryBarrierWithGroupSync();
    if ((!(t86)))
    {
        uint t87 = (lid.x % 32u);
        uint t88 = (t87 + 0u);
        uint t89 = (t84 + (t88 / 8u));
        uint t90 = (t64 + (t88 % 8u));
        if (((t89 < uniforms.u0) && (t90 < uniforms.u1)))
        {
            buffer2[((t89 * uniforms.u1) + t90)] = s0[((1920u + ((groupLane / 32u) * 64u)) + t88)];
        }
        uint t91 = (t87 + 32u);
        uint t92 = (t84 + (t91 / 8u));
        uint t93 = (t64 + (t91 % 8u));
        if (((t92 < uniforms.u0) && (t93 < uniforms.u1)))
        {
            buffer2[((t92 * uniforms.u1) + t93)] = s0[((1920u + ((groupLane / 32u) * 64u)) + t91)];
        }
    }
    GroupMemoryBarrierWithGroupSync();
    bool t94 = ((t85 <= uniforms.u0) && (t75 <= uniforms.u1));
    if (t94)
    {
        buffer2[(((t84 * uniforms.u1) + t74)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm3.x;
        buffer2[(((t84 * uniforms.u1) + t74)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm3.y;
    }
    else
    {
        s0[((1920u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm3.x;
        s0[((1920u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm3.y;
    }
    GroupMemoryBarrierWithGroupSync();
    if ((!(t94)))
    {
        uint t95 = (lid.x % 32u);
        uint t96 = (t95 + 0u);
        uint t97 = (t84 + (t96 / 8u));
        uint t98 = (t74 + (t96 % 8u));
        if (((t97 < uniforms.u0) && (t98 < uniforms.u1)))
        {
            buffer2[((t97 * uniforms.u1) + t98)] = s0[((1920u + ((groupLane / 32u) * 64u)) + t96)];
        }
        uint t99 = (t95 + 32u);
        uint t100 = (t84 + (t99 / 8u));
        uint t101 = (t74 + (t99 % 8u));
        if (((t100 < uniforms.u0) && (t101 < uniforms.u1)))
        {
            buffer2[((t100 * uniforms.u1) + t101)] = s0[((1920u + ((groupLane / 32u) * 64u)) + t99)];
        }
    }
    GroupMemoryBarrierWithGroupSync();
    uint t102 = (t59 + 16u);
    uint t103 = (t102 + 8u);
    bool t104 = ((t103 <= uniforms.u0) && (t65 <= uniforms.u1));
    if (t104)
    {
        buffer2[(((t102 * uniforms.u1) + t64)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm4.x;
        buffer2[(((t102 * uniforms.u1) + t64)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm4.y;
    }
    else
    {
        s0[((1920u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm4.x;
        s0[((1920u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm4.y;
    }
    GroupMemoryBarrierWithGroupSync();
    if ((!(t104)))
    {
        uint t105 = (lid.x % 32u);
        uint t106 = (t105 + 0u);
        uint t107 = (t102 + (t106 / 8u));
        uint t108 = (t64 + (t106 % 8u));
        if (((t107 < uniforms.u0) && (t108 < uniforms.u1)))
        {
            buffer2[((t107 * uniforms.u1) + t108)] = s0[((1920u + ((groupLane / 32u) * 64u)) + t106)];
        }
        uint t109 = (t105 + 32u);
        uint t110 = (t102 + (t109 / 8u));
        uint t111 = (t64 + (t109 % 8u));
        if (((t110 < uniforms.u0) && (t111 < uniforms.u1)))
        {
            buffer2[((t110 * uniforms.u1) + t111)] = s0[((1920u + ((groupLane / 32u) * 64u)) + t109)];
        }
    }
    GroupMemoryBarrierWithGroupSync();
    bool t112 = ((t103 <= uniforms.u0) && (t75 <= uniforms.u1));
    if (t112)
    {
        buffer2[(((t102 * uniforms.u1) + t74)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm5.x;
        buffer2[(((t102 * uniforms.u1) + t74)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm5.y;
    }
    else
    {
        s0[((1920u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm5.x;
        s0[((1920u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm5.y;
    }
    GroupMemoryBarrierWithGroupSync();
    if ((!(t112)))
    {
        uint t113 = (lid.x % 32u);
        uint t114 = (t113 + 0u);
        uint t115 = (t102 + (t114 / 8u));
        uint t116 = (t74 + (t114 % 8u));
        if (((t115 < uniforms.u0) && (t116 < uniforms.u1)))
        {
            buffer2[((t115 * uniforms.u1) + t116)] = s0[((1920u + ((groupLane / 32u) * 64u)) + t114)];
        }
        uint t117 = (t113 + 32u);
        uint t118 = (t102 + (t117 / 8u));
        uint t119 = (t74 + (t117 % 8u));
        if (((t118 < uniforms.u0) && (t119 < uniforms.u1)))
        {
            buffer2[((t118 * uniforms.u1) + t119)] = s0[((1920u + ((groupLane / 32u) * 64u)) + t117)];
        }
    }
    GroupMemoryBarrierWithGroupSync();
    uint t120 = (t59 + 24u);
    uint t121 = (t120 + 8u);
    bool t122 = ((t121 <= uniforms.u0) && (t65 <= uniforms.u1));
    if (t122)
    {
        buffer2[(((t120 * uniforms.u1) + t64)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm6.x;
        buffer2[(((t120 * uniforms.u1) + t64)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm6.y;
    }
    else
    {
        s0[((1920u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm6.x;
        s0[((1920u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm6.y;
    }
    GroupMemoryBarrierWithGroupSync();
    if ((!(t122)))
    {
        uint t123 = (lid.x % 32u);
        uint t124 = (t123 + 0u);
        uint t125 = (t120 + (t124 / 8u));
        uint t126 = (t64 + (t124 % 8u));
        if (((t125 < uniforms.u0) && (t126 < uniforms.u1)))
        {
            buffer2[((t125 * uniforms.u1) + t126)] = s0[((1920u + ((groupLane / 32u) * 64u)) + t124)];
        }
        uint t127 = (t123 + 32u);
        uint t128 = (t120 + (t127 / 8u));
        uint t129 = (t64 + (t127 % 8u));
        if (((t128 < uniforms.u0) && (t129 < uniforms.u1)))
        {
            buffer2[((t128 * uniforms.u1) + t129)] = s0[((1920u + ((groupLane / 32u) * 64u)) + t127)];
        }
    }
    GroupMemoryBarrierWithGroupSync();
    bool t130 = ((t121 <= uniforms.u0) && (t75 <= uniforms.u1));
    if (t130)
    {
        buffer2[(((t120 * uniforms.u1) + t74)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm7.x;
        buffer2[(((t120 * uniforms.u1) + t74)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm7.y;
    }
    else
    {
        s0[((1920u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm7.x;
        s0[((1920u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm7.y;
    }
    GroupMemoryBarrierWithGroupSync();
    if ((!(t130)))
    {
        uint t131 = (lid.x % 32u);
        uint t132 = (t131 + 0u);
        uint t133 = (t120 + (t132 / 8u));
        uint t134 = (t74 + (t132 % 8u));
        if (((t133 < uniforms.u0) && (t134 < uniforms.u1)))
        {
            buffer2[((t133 * uniforms.u1) + t134)] = s0[((1920u + ((groupLane / 32u) * 64u)) + t132)];
        }
        uint t135 = (t131 + 32u);
        uint t136 = (t120 + (t135 / 8u));
        uint t137 = (t74 + (t135 % 8u));
        if (((t136 < uniforms.u0) && (t137 < uniforms.u1)))
        {
            buffer2[((t136 * uniforms.u1) + t137)] = s0[((1920u + ((groupLane / 32u) * 64u)) + t135)];
        }
    }
    GroupMemoryBarrierWithGroupSync();
}
