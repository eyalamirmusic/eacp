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

groupshared float s0[2944];

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
    float2 sgm8 = float2(0.0, 0.0);
    float2 sgm9 = float2(0.0, 0.0);
    float2 sgm10 = float2(0.0, 0.0);
    float2 sgm11 = float2(0.0, 0.0);
    float2 sgm12 = float2(0.0, 0.0);
    float2 sgm13 = float2(0.0, 0.0);
    float2 sgm14 = float2(0.0, 0.0);
    float2 sgm15 = float2(0.0, 0.0);
    uint t1 = (lid.x + 0u);
    uint t2 = ((t1 % 4u) * 4u);
    uint t3 = (0u + t2);
    uint t4 = (t3 + 0u);
    uint t5 = (tgid.y * 64u);
    uint t6 = (t1 / 4u);
    uint t7 = (t5 + t6);
    uint t8 = (uniforms.u0 - 1u);
    uint t9 = (min(t7, t8) * uniforms.u2);
    uint t10 = (uniforms.u2 - 1u);
    uint t11 = (t3 + 1u);
    uint t12 = (t3 + 2u);
    uint t13 = (t3 + 3u);
    v0 = float4(((t4 < uniforms.u2) ? buffer0[(t9 + min(t4, t10))] : 0.0), ((t11 < uniforms.u2) ? buffer0[(t9 + min(t11, t10))] : 0.0), ((t12 < uniforms.u2) ? buffer0[(t9 + min(t12, t10))] : 0.0), ((t13 < uniforms.u2) ? buffer0[(t9 + min(t13, t10))] : 0.0));
    uint t14 = (lid.x + 128u);
    uint t15 = ((t14 % 4u) * 4u);
    uint t16 = (0u + t15);
    uint t17 = (t16 + 0u);
    uint t18 = (t14 / 4u);
    uint t19 = (t5 + t18);
    uint t20 = (min(t19, t8) * uniforms.u2);
    uint t21 = (t16 + 1u);
    uint t22 = (t16 + 2u);
    uint t23 = (t16 + 3u);
    v1 = float4(((t17 < uniforms.u2) ? buffer0[(t20 + min(t17, t10))] : 0.0), ((t21 < uniforms.u2) ? buffer0[(t20 + min(t21, t10))] : 0.0), ((t22 < uniforms.u2) ? buffer0[(t20 + min(t22, t10))] : 0.0), ((t23 < uniforms.u2) ? buffer0[(t20 + min(t23, t10))] : 0.0));
    uint t24 = (tgid.x * 64u);
    uint t25 = (t24 + t6);
    uint t26 = (uniforms.u1 - 1u);
    uint t27 = (min(t25, t26) * uniforms.u2);
    v4 = float4(((t4 < uniforms.u2) ? buffer1[(t27 + min(t4, t10))] : 0.0), ((t11 < uniforms.u2) ? buffer1[(t27 + min(t11, t10))] : 0.0), ((t12 < uniforms.u2) ? buffer1[(t27 + min(t12, t10))] : 0.0), ((t13 < uniforms.u2) ? buffer1[(t27 + min(t13, t10))] : 0.0));
    uint t28 = (t24 + t18);
    uint t29 = (min(t28, t26) * uniforms.u2);
    v5 = float4(((t17 < uniforms.u2) ? buffer1[(t29 + min(t17, t10))] : 0.0), ((t21 < uniforms.u2) ? buffer1[(t29 + min(t21, t10))] : 0.0), ((t22 < uniforms.u2) ? buffer1[(t29 + min(t22, t10))] : 0.0), ((t23 < uniforms.u2) ? buffer1[(t29 + min(t23, t10))] : 0.0));
    uint v8 = 0u;
    while ((v8 < uniforms.u2))
    {
        GroupMemoryBarrierWithGroupSync();
        uint t30 = ((t6 * 24u) + t2);
        s0[t30] = (v0).x;
        s0[(t30 + 1u)] = (v0).y;
        s0[(t30 + 2u)] = (v0).z;
        s0[(t30 + 3u)] = (v0).w;
        uint t31 = ((t18 * 24u) + t15);
        s0[t31] = (v1).x;
        s0[(t31 + 1u)] = (v1).y;
        s0[(t31 + 2u)] = (v1).z;
        s0[(t31 + 3u)] = (v1).w;
        uint t32 = ((1536u + (t2 * 72u)) + t6);
        s0[t32] = (v4).x;
        s0[(t32 + 72u)] = (v4).y;
        s0[(t32 + 144u)] = (v4).z;
        s0[(t32 + 216u)] = (v4).w;
        uint t33 = ((1536u + (t15 * 72u)) + t18);
        s0[t33] = (v5).x;
        s0[(t33 + 72u)] = (v5).y;
        s0[(t33 + 144u)] = (v5).z;
        s0[(t33 + 216u)] = (v5).w;
        GroupMemoryBarrierWithGroupSync();
        uint t34 = (v8 + 16u);
        uint t35 = (t34 + t2);
        uint t36 = (t35 + 0u);
        uint t37 = (min(t7, t8) * uniforms.u2);
        uint t38 = (t35 + 1u);
        uint t39 = (t35 + 2u);
        uint t40 = (t35 + 3u);
        v0 = float4(((t36 < uniforms.u2) ? buffer0[(t37 + min(t36, t10))] : 0.0), ((t38 < uniforms.u2) ? buffer0[(t37 + min(t38, t10))] : 0.0), ((t39 < uniforms.u2) ? buffer0[(t37 + min(t39, t10))] : 0.0), ((t40 < uniforms.u2) ? buffer0[(t37 + min(t40, t10))] : 0.0));
        uint t41 = (t34 + t15);
        uint t42 = (t41 + 0u);
        uint t43 = (min(t19, t8) * uniforms.u2);
        uint t44 = (t41 + 1u);
        uint t45 = (t41 + 2u);
        uint t46 = (t41 + 3u);
        v1 = float4(((t42 < uniforms.u2) ? buffer0[(t43 + min(t42, t10))] : 0.0), ((t44 < uniforms.u2) ? buffer0[(t43 + min(t44, t10))] : 0.0), ((t45 < uniforms.u2) ? buffer0[(t43 + min(t45, t10))] : 0.0), ((t46 < uniforms.u2) ? buffer0[(t43 + min(t46, t10))] : 0.0));
        uint t47 = (t34 + t2);
        uint t48 = (t47 + 0u);
        uint t49 = (min(t25, t26) * uniforms.u2);
        uint t50 = (t47 + 1u);
        uint t51 = (t47 + 2u);
        uint t52 = (t47 + 3u);
        v4 = float4(((t48 < uniforms.u2) ? buffer1[(t49 + min(t48, t10))] : 0.0), ((t50 < uniforms.u2) ? buffer1[(t49 + min(t50, t10))] : 0.0), ((t51 < uniforms.u2) ? buffer1[(t49 + min(t51, t10))] : 0.0), ((t52 < uniforms.u2) ? buffer1[(t49 + min(t52, t10))] : 0.0));
        uint t53 = (t34 + t15);
        uint t54 = (t53 + 0u);
        uint t55 = (min(t28, t26) * uniforms.u2);
        uint t56 = (t53 + 1u);
        uint t57 = (t53 + 2u);
        uint t58 = (t53 + 3u);
        v5 = float4(((t54 < uniforms.u2) ? buffer1[(t55 + min(t54, t10))] : 0.0), ((t56 < uniforms.u2) ? buffer1[(t55 + min(t56, t10))] : 0.0), ((t57 < uniforms.u2) ? buffer1[(t55 + min(t57, t10))] : 0.0), ((t58 < uniforms.u2) ? buffer1[(t55 + min(t58, t10))] : 0.0));
        uint t59 = (((groupLane / 32u) % 2u) * 32u);
        uint t60 = ((t59 + 0u) * 24u);
        float2 sgm16 = float2(s0[((t60 + 0u)) + sgmRow * (24u) + sgmColumn], s0[((t60 + 0u)) + sgmRow * (24u) + sgmColumn + 1u]);
        uint t61 = ((t59 + 8u) * 24u);
        float2 sgm17 = float2(s0[((t61 + 0u)) + sgmRow * (24u) + sgmColumn], s0[((t61 + 0u)) + sgmRow * (24u) + sgmColumn + 1u]);
        uint t62 = ((t59 + 16u) * 24u);
        float2 sgm18 = float2(s0[((t62 + 0u)) + sgmRow * (24u) + sgmColumn], s0[((t62 + 0u)) + sgmRow * (24u) + sgmColumn + 1u]);
        uint t63 = ((t59 + 24u) * 24u);
        float2 sgm19 = float2(s0[((t63 + 0u)) + sgmRow * (24u) + sgmColumn], s0[((t63 + 0u)) + sgmRow * (24u) + sgmColumn + 1u]);
        uint t64 = (((groupLane / 32u) / 2u) * 32u);
        uint t65 = (1536u + t64);
        float2 sgm20 = float2(s0[((t65 + 0u)) + sgmRow * (72u) + sgmColumn], s0[((t65 + 0u)) + sgmRow * (72u) + sgmColumn + 1u]);
        float2 sgm21 = float2(s0[((t65 + 8u)) + sgmRow * (72u) + sgmColumn], s0[((t65 + 8u)) + sgmRow * (72u) + sgmColumn + 1u]);
        float2 sgm22 = float2(s0[((t65 + 16u)) + sgmRow * (72u) + sgmColumn], s0[((t65 + 16u)) + sgmRow * (72u) + sgmColumn + 1u]);
        float2 sgm23 = float2(s0[((t65 + 24u)) + sgmRow * (72u) + sgmColumn], s0[((t65 + 24u)) + sgmRow * (72u) + sgmColumn + 1u]);
        for (uint sgm0k = 0u; sgm0k < 8u; ++sgm0k)
        {
            float sgm0l = s0[((t60 + 0u)) + sgmRow * (24u) + sgm0k];
            sgm0.x += sgm0l * s0[((t65 + 0u)) + sgm0k * (72u) + sgmColumn];
            sgm0.y += sgm0l * s0[((t65 + 0u)) + sgm0k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm1k = 0u; sgm1k < 8u; ++sgm1k)
        {
            float sgm1l = s0[((t60 + 0u)) + sgmRow * (24u) + sgm1k];
            sgm1.x += sgm1l * s0[((t65 + 8u)) + sgm1k * (72u) + sgmColumn];
            sgm1.y += sgm1l * s0[((t65 + 8u)) + sgm1k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm2k = 0u; sgm2k < 8u; ++sgm2k)
        {
            float sgm2l = s0[((t60 + 0u)) + sgmRow * (24u) + sgm2k];
            sgm2.x += sgm2l * s0[((t65 + 16u)) + sgm2k * (72u) + sgmColumn];
            sgm2.y += sgm2l * s0[((t65 + 16u)) + sgm2k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm3k = 0u; sgm3k < 8u; ++sgm3k)
        {
            float sgm3l = s0[((t60 + 0u)) + sgmRow * (24u) + sgm3k];
            sgm3.x += sgm3l * s0[((t65 + 24u)) + sgm3k * (72u) + sgmColumn];
            sgm3.y += sgm3l * s0[((t65 + 24u)) + sgm3k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm4k = 0u; sgm4k < 8u; ++sgm4k)
        {
            float sgm4l = s0[((t61 + 0u)) + sgmRow * (24u) + sgm4k];
            sgm4.x += sgm4l * s0[((t65 + 0u)) + sgm4k * (72u) + sgmColumn];
            sgm4.y += sgm4l * s0[((t65 + 0u)) + sgm4k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm5k = 0u; sgm5k < 8u; ++sgm5k)
        {
            float sgm5l = s0[((t61 + 0u)) + sgmRow * (24u) + sgm5k];
            sgm5.x += sgm5l * s0[((t65 + 8u)) + sgm5k * (72u) + sgmColumn];
            sgm5.y += sgm5l * s0[((t65 + 8u)) + sgm5k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm6k = 0u; sgm6k < 8u; ++sgm6k)
        {
            float sgm6l = s0[((t61 + 0u)) + sgmRow * (24u) + sgm6k];
            sgm6.x += sgm6l * s0[((t65 + 16u)) + sgm6k * (72u) + sgmColumn];
            sgm6.y += sgm6l * s0[((t65 + 16u)) + sgm6k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm7k = 0u; sgm7k < 8u; ++sgm7k)
        {
            float sgm7l = s0[((t61 + 0u)) + sgmRow * (24u) + sgm7k];
            sgm7.x += sgm7l * s0[((t65 + 24u)) + sgm7k * (72u) + sgmColumn];
            sgm7.y += sgm7l * s0[((t65 + 24u)) + sgm7k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm8k = 0u; sgm8k < 8u; ++sgm8k)
        {
            float sgm8l = s0[((t62 + 0u)) + sgmRow * (24u) + sgm8k];
            sgm8.x += sgm8l * s0[((t65 + 0u)) + sgm8k * (72u) + sgmColumn];
            sgm8.y += sgm8l * s0[((t65 + 0u)) + sgm8k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm9k = 0u; sgm9k < 8u; ++sgm9k)
        {
            float sgm9l = s0[((t62 + 0u)) + sgmRow * (24u) + sgm9k];
            sgm9.x += sgm9l * s0[((t65 + 8u)) + sgm9k * (72u) + sgmColumn];
            sgm9.y += sgm9l * s0[((t65 + 8u)) + sgm9k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm10k = 0u; sgm10k < 8u; ++sgm10k)
        {
            float sgm10l = s0[((t62 + 0u)) + sgmRow * (24u) + sgm10k];
            sgm10.x += sgm10l * s0[((t65 + 16u)) + sgm10k * (72u) + sgmColumn];
            sgm10.y += sgm10l * s0[((t65 + 16u)) + sgm10k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm11k = 0u; sgm11k < 8u; ++sgm11k)
        {
            float sgm11l = s0[((t62 + 0u)) + sgmRow * (24u) + sgm11k];
            sgm11.x += sgm11l * s0[((t65 + 24u)) + sgm11k * (72u) + sgmColumn];
            sgm11.y += sgm11l * s0[((t65 + 24u)) + sgm11k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm12k = 0u; sgm12k < 8u; ++sgm12k)
        {
            float sgm12l = s0[((t63 + 0u)) + sgmRow * (24u) + sgm12k];
            sgm12.x += sgm12l * s0[((t65 + 0u)) + sgm12k * (72u) + sgmColumn];
            sgm12.y += sgm12l * s0[((t65 + 0u)) + sgm12k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm13k = 0u; sgm13k < 8u; ++sgm13k)
        {
            float sgm13l = s0[((t63 + 0u)) + sgmRow * (24u) + sgm13k];
            sgm13.x += sgm13l * s0[((t65 + 8u)) + sgm13k * (72u) + sgmColumn];
            sgm13.y += sgm13l * s0[((t65 + 8u)) + sgm13k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm14k = 0u; sgm14k < 8u; ++sgm14k)
        {
            float sgm14l = s0[((t63 + 0u)) + sgmRow * (24u) + sgm14k];
            sgm14.x += sgm14l * s0[((t65 + 16u)) + sgm14k * (72u) + sgmColumn];
            sgm14.y += sgm14l * s0[((t65 + 16u)) + sgm14k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm15k = 0u; sgm15k < 8u; ++sgm15k)
        {
            float sgm15l = s0[((t63 + 0u)) + sgmRow * (24u) + sgm15k];
            sgm15.x += sgm15l * s0[((t65 + 24u)) + sgm15k * (72u) + sgmColumn];
            sgm15.y += sgm15l * s0[((t65 + 24u)) + sgm15k * (72u) + sgmColumn + 1u];
        }
        float2 sgm24 = float2(s0[((t60 + 8u)) + sgmRow * (24u) + sgmColumn], s0[((t60 + 8u)) + sgmRow * (24u) + sgmColumn + 1u]);
        float2 sgm25 = float2(s0[((t61 + 8u)) + sgmRow * (24u) + sgmColumn], s0[((t61 + 8u)) + sgmRow * (24u) + sgmColumn + 1u]);
        float2 sgm26 = float2(s0[((t62 + 8u)) + sgmRow * (24u) + sgmColumn], s0[((t62 + 8u)) + sgmRow * (24u) + sgmColumn + 1u]);
        float2 sgm27 = float2(s0[((t63 + 8u)) + sgmRow * (24u) + sgmColumn], s0[((t63 + 8u)) + sgmRow * (24u) + sgmColumn + 1u]);
        uint t66 = (2112u + t64);
        float2 sgm28 = float2(s0[((t66 + 0u)) + sgmRow * (72u) + sgmColumn], s0[((t66 + 0u)) + sgmRow * (72u) + sgmColumn + 1u]);
        float2 sgm29 = float2(s0[((t66 + 8u)) + sgmRow * (72u) + sgmColumn], s0[((t66 + 8u)) + sgmRow * (72u) + sgmColumn + 1u]);
        float2 sgm30 = float2(s0[((t66 + 16u)) + sgmRow * (72u) + sgmColumn], s0[((t66 + 16u)) + sgmRow * (72u) + sgmColumn + 1u]);
        float2 sgm31 = float2(s0[((t66 + 24u)) + sgmRow * (72u) + sgmColumn], s0[((t66 + 24u)) + sgmRow * (72u) + sgmColumn + 1u]);
        for (uint sgm0k = 0u; sgm0k < 8u; ++sgm0k)
        {
            float sgm0l = s0[((t60 + 8u)) + sgmRow * (24u) + sgm0k];
            sgm0.x += sgm0l * s0[((t66 + 0u)) + sgm0k * (72u) + sgmColumn];
            sgm0.y += sgm0l * s0[((t66 + 0u)) + sgm0k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm1k = 0u; sgm1k < 8u; ++sgm1k)
        {
            float sgm1l = s0[((t60 + 8u)) + sgmRow * (24u) + sgm1k];
            sgm1.x += sgm1l * s0[((t66 + 8u)) + sgm1k * (72u) + sgmColumn];
            sgm1.y += sgm1l * s0[((t66 + 8u)) + sgm1k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm2k = 0u; sgm2k < 8u; ++sgm2k)
        {
            float sgm2l = s0[((t60 + 8u)) + sgmRow * (24u) + sgm2k];
            sgm2.x += sgm2l * s0[((t66 + 16u)) + sgm2k * (72u) + sgmColumn];
            sgm2.y += sgm2l * s0[((t66 + 16u)) + sgm2k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm3k = 0u; sgm3k < 8u; ++sgm3k)
        {
            float sgm3l = s0[((t60 + 8u)) + sgmRow * (24u) + sgm3k];
            sgm3.x += sgm3l * s0[((t66 + 24u)) + sgm3k * (72u) + sgmColumn];
            sgm3.y += sgm3l * s0[((t66 + 24u)) + sgm3k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm4k = 0u; sgm4k < 8u; ++sgm4k)
        {
            float sgm4l = s0[((t61 + 8u)) + sgmRow * (24u) + sgm4k];
            sgm4.x += sgm4l * s0[((t66 + 0u)) + sgm4k * (72u) + sgmColumn];
            sgm4.y += sgm4l * s0[((t66 + 0u)) + sgm4k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm5k = 0u; sgm5k < 8u; ++sgm5k)
        {
            float sgm5l = s0[((t61 + 8u)) + sgmRow * (24u) + sgm5k];
            sgm5.x += sgm5l * s0[((t66 + 8u)) + sgm5k * (72u) + sgmColumn];
            sgm5.y += sgm5l * s0[((t66 + 8u)) + sgm5k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm6k = 0u; sgm6k < 8u; ++sgm6k)
        {
            float sgm6l = s0[((t61 + 8u)) + sgmRow * (24u) + sgm6k];
            sgm6.x += sgm6l * s0[((t66 + 16u)) + sgm6k * (72u) + sgmColumn];
            sgm6.y += sgm6l * s0[((t66 + 16u)) + sgm6k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm7k = 0u; sgm7k < 8u; ++sgm7k)
        {
            float sgm7l = s0[((t61 + 8u)) + sgmRow * (24u) + sgm7k];
            sgm7.x += sgm7l * s0[((t66 + 24u)) + sgm7k * (72u) + sgmColumn];
            sgm7.y += sgm7l * s0[((t66 + 24u)) + sgm7k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm8k = 0u; sgm8k < 8u; ++sgm8k)
        {
            float sgm8l = s0[((t62 + 8u)) + sgmRow * (24u) + sgm8k];
            sgm8.x += sgm8l * s0[((t66 + 0u)) + sgm8k * (72u) + sgmColumn];
            sgm8.y += sgm8l * s0[((t66 + 0u)) + sgm8k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm9k = 0u; sgm9k < 8u; ++sgm9k)
        {
            float sgm9l = s0[((t62 + 8u)) + sgmRow * (24u) + sgm9k];
            sgm9.x += sgm9l * s0[((t66 + 8u)) + sgm9k * (72u) + sgmColumn];
            sgm9.y += sgm9l * s0[((t66 + 8u)) + sgm9k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm10k = 0u; sgm10k < 8u; ++sgm10k)
        {
            float sgm10l = s0[((t62 + 8u)) + sgmRow * (24u) + sgm10k];
            sgm10.x += sgm10l * s0[((t66 + 16u)) + sgm10k * (72u) + sgmColumn];
            sgm10.y += sgm10l * s0[((t66 + 16u)) + sgm10k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm11k = 0u; sgm11k < 8u; ++sgm11k)
        {
            float sgm11l = s0[((t62 + 8u)) + sgmRow * (24u) + sgm11k];
            sgm11.x += sgm11l * s0[((t66 + 24u)) + sgm11k * (72u) + sgmColumn];
            sgm11.y += sgm11l * s0[((t66 + 24u)) + sgm11k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm12k = 0u; sgm12k < 8u; ++sgm12k)
        {
            float sgm12l = s0[((t63 + 8u)) + sgmRow * (24u) + sgm12k];
            sgm12.x += sgm12l * s0[((t66 + 0u)) + sgm12k * (72u) + sgmColumn];
            sgm12.y += sgm12l * s0[((t66 + 0u)) + sgm12k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm13k = 0u; sgm13k < 8u; ++sgm13k)
        {
            float sgm13l = s0[((t63 + 8u)) + sgmRow * (24u) + sgm13k];
            sgm13.x += sgm13l * s0[((t66 + 8u)) + sgm13k * (72u) + sgmColumn];
            sgm13.y += sgm13l * s0[((t66 + 8u)) + sgm13k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm14k = 0u; sgm14k < 8u; ++sgm14k)
        {
            float sgm14l = s0[((t63 + 8u)) + sgmRow * (24u) + sgm14k];
            sgm14.x += sgm14l * s0[((t66 + 16u)) + sgm14k * (72u) + sgmColumn];
            sgm14.y += sgm14l * s0[((t66 + 16u)) + sgm14k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm15k = 0u; sgm15k < 8u; ++sgm15k)
        {
            float sgm15l = s0[((t63 + 8u)) + sgmRow * (24u) + sgm15k];
            sgm15.x += sgm15l * s0[((t66 + 24u)) + sgm15k * (72u) + sgmColumn];
            sgm15.y += sgm15l * s0[((t66 + 24u)) + sgm15k * (72u) + sgmColumn + 1u];
        }
        v8 = (v8 + 16u);
    }
    uint t67 = (((groupLane / 32u) % 2u) * 32u);
    uint t68 = (t5 + t67);
    uint t69 = (t68 + 0u);
    uint t70 = (t69 + 8u);
    uint t71 = (((groupLane / 32u) / 2u) * 32u);
    uint t72 = (t24 + t71);
    uint t73 = (t72 + 0u);
    uint t74 = (t73 + 8u);
    bool t75 = ((t70 <= uniforms.u0) && (t74 <= uniforms.u1));
    if (t75)
    {
        buffer2[(((t69 * uniforms.u1) + t73)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm0.x;
        buffer2[(((t69 * uniforms.u1) + t73)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm0.y;
    }
    else
    {
        s0[((2688u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm0.x;
        s0[((2688u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm0.y;
    }
    GroupMemoryBarrierWithGroupSync();
    if ((!(t75)))
    {
        uint t76 = (lid.x % 32u);
        uint t77 = (t76 + 0u);
        uint t78 = (t69 + (t77 / 8u));
        uint t79 = (t73 + (t77 % 8u));
        if (((t78 < uniforms.u0) && (t79 < uniforms.u1)))
        {
            buffer2[((t78 * uniforms.u1) + t79)] = s0[((2688u + ((groupLane / 32u) * 64u)) + t77)];
        }
        uint t80 = (t76 + 32u);
        uint t81 = (t69 + (t80 / 8u));
        uint t82 = (t73 + (t80 % 8u));
        if (((t81 < uniforms.u0) && (t82 < uniforms.u1)))
        {
            buffer2[((t81 * uniforms.u1) + t82)] = s0[((2688u + ((groupLane / 32u) * 64u)) + t80)];
        }
    }
    GroupMemoryBarrierWithGroupSync();
    uint t83 = (t72 + 8u);
    uint t84 = (t83 + 8u);
    bool t85 = ((t70 <= uniforms.u0) && (t84 <= uniforms.u1));
    if (t85)
    {
        buffer2[(((t69 * uniforms.u1) + t83)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm1.x;
        buffer2[(((t69 * uniforms.u1) + t83)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm1.y;
    }
    else
    {
        s0[((2688u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm1.x;
        s0[((2688u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm1.y;
    }
    GroupMemoryBarrierWithGroupSync();
    if ((!(t85)))
    {
        uint t86 = (lid.x % 32u);
        uint t87 = (t86 + 0u);
        uint t88 = (t69 + (t87 / 8u));
        uint t89 = (t83 + (t87 % 8u));
        if (((t88 < uniforms.u0) && (t89 < uniforms.u1)))
        {
            buffer2[((t88 * uniforms.u1) + t89)] = s0[((2688u + ((groupLane / 32u) * 64u)) + t87)];
        }
        uint t90 = (t86 + 32u);
        uint t91 = (t69 + (t90 / 8u));
        uint t92 = (t83 + (t90 % 8u));
        if (((t91 < uniforms.u0) && (t92 < uniforms.u1)))
        {
            buffer2[((t91 * uniforms.u1) + t92)] = s0[((2688u + ((groupLane / 32u) * 64u)) + t90)];
        }
    }
    GroupMemoryBarrierWithGroupSync();
    uint t93 = (t72 + 16u);
    uint t94 = (t93 + 8u);
    bool t95 = ((t70 <= uniforms.u0) && (t94 <= uniforms.u1));
    if (t95)
    {
        buffer2[(((t69 * uniforms.u1) + t93)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm2.x;
        buffer2[(((t69 * uniforms.u1) + t93)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm2.y;
    }
    else
    {
        s0[((2688u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm2.x;
        s0[((2688u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm2.y;
    }
    GroupMemoryBarrierWithGroupSync();
    if ((!(t95)))
    {
        uint t96 = (lid.x % 32u);
        uint t97 = (t96 + 0u);
        uint t98 = (t69 + (t97 / 8u));
        uint t99 = (t93 + (t97 % 8u));
        if (((t98 < uniforms.u0) && (t99 < uniforms.u1)))
        {
            buffer2[((t98 * uniforms.u1) + t99)] = s0[((2688u + ((groupLane / 32u) * 64u)) + t97)];
        }
        uint t100 = (t96 + 32u);
        uint t101 = (t69 + (t100 / 8u));
        uint t102 = (t93 + (t100 % 8u));
        if (((t101 < uniforms.u0) && (t102 < uniforms.u1)))
        {
            buffer2[((t101 * uniforms.u1) + t102)] = s0[((2688u + ((groupLane / 32u) * 64u)) + t100)];
        }
    }
    GroupMemoryBarrierWithGroupSync();
    uint t103 = (t72 + 24u);
    uint t104 = (t103 + 8u);
    bool t105 = ((t70 <= uniforms.u0) && (t104 <= uniforms.u1));
    if (t105)
    {
        buffer2[(((t69 * uniforms.u1) + t103)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm3.x;
        buffer2[(((t69 * uniforms.u1) + t103)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm3.y;
    }
    else
    {
        s0[((2688u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm3.x;
        s0[((2688u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm3.y;
    }
    GroupMemoryBarrierWithGroupSync();
    if ((!(t105)))
    {
        uint t106 = (lid.x % 32u);
        uint t107 = (t106 + 0u);
        uint t108 = (t69 + (t107 / 8u));
        uint t109 = (t103 + (t107 % 8u));
        if (((t108 < uniforms.u0) && (t109 < uniforms.u1)))
        {
            buffer2[((t108 * uniforms.u1) + t109)] = s0[((2688u + ((groupLane / 32u) * 64u)) + t107)];
        }
        uint t110 = (t106 + 32u);
        uint t111 = (t69 + (t110 / 8u));
        uint t112 = (t103 + (t110 % 8u));
        if (((t111 < uniforms.u0) && (t112 < uniforms.u1)))
        {
            buffer2[((t111 * uniforms.u1) + t112)] = s0[((2688u + ((groupLane / 32u) * 64u)) + t110)];
        }
    }
    GroupMemoryBarrierWithGroupSync();
    uint t113 = (t68 + 8u);
    uint t114 = (t113 + 8u);
    bool t115 = ((t114 <= uniforms.u0) && (t74 <= uniforms.u1));
    if (t115)
    {
        buffer2[(((t113 * uniforms.u1) + t73)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm4.x;
        buffer2[(((t113 * uniforms.u1) + t73)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm4.y;
    }
    else
    {
        s0[((2688u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm4.x;
        s0[((2688u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm4.y;
    }
    GroupMemoryBarrierWithGroupSync();
    if ((!(t115)))
    {
        uint t116 = (lid.x % 32u);
        uint t117 = (t116 + 0u);
        uint t118 = (t113 + (t117 / 8u));
        uint t119 = (t73 + (t117 % 8u));
        if (((t118 < uniforms.u0) && (t119 < uniforms.u1)))
        {
            buffer2[((t118 * uniforms.u1) + t119)] = s0[((2688u + ((groupLane / 32u) * 64u)) + t117)];
        }
        uint t120 = (t116 + 32u);
        uint t121 = (t113 + (t120 / 8u));
        uint t122 = (t73 + (t120 % 8u));
        if (((t121 < uniforms.u0) && (t122 < uniforms.u1)))
        {
            buffer2[((t121 * uniforms.u1) + t122)] = s0[((2688u + ((groupLane / 32u) * 64u)) + t120)];
        }
    }
    GroupMemoryBarrierWithGroupSync();
    bool t123 = ((t114 <= uniforms.u0) && (t84 <= uniforms.u1));
    if (t123)
    {
        buffer2[(((t113 * uniforms.u1) + t83)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm5.x;
        buffer2[(((t113 * uniforms.u1) + t83)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm5.y;
    }
    else
    {
        s0[((2688u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm5.x;
        s0[((2688u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm5.y;
    }
    GroupMemoryBarrierWithGroupSync();
    if ((!(t123)))
    {
        uint t124 = (lid.x % 32u);
        uint t125 = (t124 + 0u);
        uint t126 = (t113 + (t125 / 8u));
        uint t127 = (t83 + (t125 % 8u));
        if (((t126 < uniforms.u0) && (t127 < uniforms.u1)))
        {
            buffer2[((t126 * uniforms.u1) + t127)] = s0[((2688u + ((groupLane / 32u) * 64u)) + t125)];
        }
        uint t128 = (t124 + 32u);
        uint t129 = (t113 + (t128 / 8u));
        uint t130 = (t83 + (t128 % 8u));
        if (((t129 < uniforms.u0) && (t130 < uniforms.u1)))
        {
            buffer2[((t129 * uniforms.u1) + t130)] = s0[((2688u + ((groupLane / 32u) * 64u)) + t128)];
        }
    }
    GroupMemoryBarrierWithGroupSync();
    bool t131 = ((t114 <= uniforms.u0) && (t94 <= uniforms.u1));
    if (t131)
    {
        buffer2[(((t113 * uniforms.u1) + t93)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm6.x;
        buffer2[(((t113 * uniforms.u1) + t93)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm6.y;
    }
    else
    {
        s0[((2688u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm6.x;
        s0[((2688u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm6.y;
    }
    GroupMemoryBarrierWithGroupSync();
    if ((!(t131)))
    {
        uint t132 = (lid.x % 32u);
        uint t133 = (t132 + 0u);
        uint t134 = (t113 + (t133 / 8u));
        uint t135 = (t93 + (t133 % 8u));
        if (((t134 < uniforms.u0) && (t135 < uniforms.u1)))
        {
            buffer2[((t134 * uniforms.u1) + t135)] = s0[((2688u + ((groupLane / 32u) * 64u)) + t133)];
        }
        uint t136 = (t132 + 32u);
        uint t137 = (t113 + (t136 / 8u));
        uint t138 = (t93 + (t136 % 8u));
        if (((t137 < uniforms.u0) && (t138 < uniforms.u1)))
        {
            buffer2[((t137 * uniforms.u1) + t138)] = s0[((2688u + ((groupLane / 32u) * 64u)) + t136)];
        }
    }
    GroupMemoryBarrierWithGroupSync();
    bool t139 = ((t114 <= uniforms.u0) && (t104 <= uniforms.u1));
    if (t139)
    {
        buffer2[(((t113 * uniforms.u1) + t103)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm7.x;
        buffer2[(((t113 * uniforms.u1) + t103)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm7.y;
    }
    else
    {
        s0[((2688u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm7.x;
        s0[((2688u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm7.y;
    }
    GroupMemoryBarrierWithGroupSync();
    if ((!(t139)))
    {
        uint t140 = (lid.x % 32u);
        uint t141 = (t140 + 0u);
        uint t142 = (t113 + (t141 / 8u));
        uint t143 = (t103 + (t141 % 8u));
        if (((t142 < uniforms.u0) && (t143 < uniforms.u1)))
        {
            buffer2[((t142 * uniforms.u1) + t143)] = s0[((2688u + ((groupLane / 32u) * 64u)) + t141)];
        }
        uint t144 = (t140 + 32u);
        uint t145 = (t113 + (t144 / 8u));
        uint t146 = (t103 + (t144 % 8u));
        if (((t145 < uniforms.u0) && (t146 < uniforms.u1)))
        {
            buffer2[((t145 * uniforms.u1) + t146)] = s0[((2688u + ((groupLane / 32u) * 64u)) + t144)];
        }
    }
    GroupMemoryBarrierWithGroupSync();
    uint t147 = (t68 + 16u);
    uint t148 = (t147 + 8u);
    bool t149 = ((t148 <= uniforms.u0) && (t74 <= uniforms.u1));
    if (t149)
    {
        buffer2[(((t147 * uniforms.u1) + t73)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm8.x;
        buffer2[(((t147 * uniforms.u1) + t73)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm8.y;
    }
    else
    {
        s0[((2688u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm8.x;
        s0[((2688u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm8.y;
    }
    GroupMemoryBarrierWithGroupSync();
    if ((!(t149)))
    {
        uint t150 = (lid.x % 32u);
        uint t151 = (t150 + 0u);
        uint t152 = (t147 + (t151 / 8u));
        uint t153 = (t73 + (t151 % 8u));
        if (((t152 < uniforms.u0) && (t153 < uniforms.u1)))
        {
            buffer2[((t152 * uniforms.u1) + t153)] = s0[((2688u + ((groupLane / 32u) * 64u)) + t151)];
        }
        uint t154 = (t150 + 32u);
        uint t155 = (t147 + (t154 / 8u));
        uint t156 = (t73 + (t154 % 8u));
        if (((t155 < uniforms.u0) && (t156 < uniforms.u1)))
        {
            buffer2[((t155 * uniforms.u1) + t156)] = s0[((2688u + ((groupLane / 32u) * 64u)) + t154)];
        }
    }
    GroupMemoryBarrierWithGroupSync();
    bool t157 = ((t148 <= uniforms.u0) && (t84 <= uniforms.u1));
    if (t157)
    {
        buffer2[(((t147 * uniforms.u1) + t83)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm9.x;
        buffer2[(((t147 * uniforms.u1) + t83)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm9.y;
    }
    else
    {
        s0[((2688u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm9.x;
        s0[((2688u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm9.y;
    }
    GroupMemoryBarrierWithGroupSync();
    if ((!(t157)))
    {
        uint t158 = (lid.x % 32u);
        uint t159 = (t158 + 0u);
        uint t160 = (t147 + (t159 / 8u));
        uint t161 = (t83 + (t159 % 8u));
        if (((t160 < uniforms.u0) && (t161 < uniforms.u1)))
        {
            buffer2[((t160 * uniforms.u1) + t161)] = s0[((2688u + ((groupLane / 32u) * 64u)) + t159)];
        }
        uint t162 = (t158 + 32u);
        uint t163 = (t147 + (t162 / 8u));
        uint t164 = (t83 + (t162 % 8u));
        if (((t163 < uniforms.u0) && (t164 < uniforms.u1)))
        {
            buffer2[((t163 * uniforms.u1) + t164)] = s0[((2688u + ((groupLane / 32u) * 64u)) + t162)];
        }
    }
    GroupMemoryBarrierWithGroupSync();
    bool t165 = ((t148 <= uniforms.u0) && (t94 <= uniforms.u1));
    if (t165)
    {
        buffer2[(((t147 * uniforms.u1) + t93)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm10.x;
        buffer2[(((t147 * uniforms.u1) + t93)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm10.y;
    }
    else
    {
        s0[((2688u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm10.x;
        s0[((2688u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm10.y;
    }
    GroupMemoryBarrierWithGroupSync();
    if ((!(t165)))
    {
        uint t166 = (lid.x % 32u);
        uint t167 = (t166 + 0u);
        uint t168 = (t147 + (t167 / 8u));
        uint t169 = (t93 + (t167 % 8u));
        if (((t168 < uniforms.u0) && (t169 < uniforms.u1)))
        {
            buffer2[((t168 * uniforms.u1) + t169)] = s0[((2688u + ((groupLane / 32u) * 64u)) + t167)];
        }
        uint t170 = (t166 + 32u);
        uint t171 = (t147 + (t170 / 8u));
        uint t172 = (t93 + (t170 % 8u));
        if (((t171 < uniforms.u0) && (t172 < uniforms.u1)))
        {
            buffer2[((t171 * uniforms.u1) + t172)] = s0[((2688u + ((groupLane / 32u) * 64u)) + t170)];
        }
    }
    GroupMemoryBarrierWithGroupSync();
    bool t173 = ((t148 <= uniforms.u0) && (t104 <= uniforms.u1));
    if (t173)
    {
        buffer2[(((t147 * uniforms.u1) + t103)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm11.x;
        buffer2[(((t147 * uniforms.u1) + t103)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm11.y;
    }
    else
    {
        s0[((2688u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm11.x;
        s0[((2688u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm11.y;
    }
    GroupMemoryBarrierWithGroupSync();
    if ((!(t173)))
    {
        uint t174 = (lid.x % 32u);
        uint t175 = (t174 + 0u);
        uint t176 = (t147 + (t175 / 8u));
        uint t177 = (t103 + (t175 % 8u));
        if (((t176 < uniforms.u0) && (t177 < uniforms.u1)))
        {
            buffer2[((t176 * uniforms.u1) + t177)] = s0[((2688u + ((groupLane / 32u) * 64u)) + t175)];
        }
        uint t178 = (t174 + 32u);
        uint t179 = (t147 + (t178 / 8u));
        uint t180 = (t103 + (t178 % 8u));
        if (((t179 < uniforms.u0) && (t180 < uniforms.u1)))
        {
            buffer2[((t179 * uniforms.u1) + t180)] = s0[((2688u + ((groupLane / 32u) * 64u)) + t178)];
        }
    }
    GroupMemoryBarrierWithGroupSync();
    uint t181 = (t68 + 24u);
    uint t182 = (t181 + 8u);
    bool t183 = ((t182 <= uniforms.u0) && (t74 <= uniforms.u1));
    if (t183)
    {
        buffer2[(((t181 * uniforms.u1) + t73)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm12.x;
        buffer2[(((t181 * uniforms.u1) + t73)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm12.y;
    }
    else
    {
        s0[((2688u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm12.x;
        s0[((2688u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm12.y;
    }
    GroupMemoryBarrierWithGroupSync();
    if ((!(t183)))
    {
        uint t184 = (lid.x % 32u);
        uint t185 = (t184 + 0u);
        uint t186 = (t181 + (t185 / 8u));
        uint t187 = (t73 + (t185 % 8u));
        if (((t186 < uniforms.u0) && (t187 < uniforms.u1)))
        {
            buffer2[((t186 * uniforms.u1) + t187)] = s0[((2688u + ((groupLane / 32u) * 64u)) + t185)];
        }
        uint t188 = (t184 + 32u);
        uint t189 = (t181 + (t188 / 8u));
        uint t190 = (t73 + (t188 % 8u));
        if (((t189 < uniforms.u0) && (t190 < uniforms.u1)))
        {
            buffer2[((t189 * uniforms.u1) + t190)] = s0[((2688u + ((groupLane / 32u) * 64u)) + t188)];
        }
    }
    GroupMemoryBarrierWithGroupSync();
    bool t191 = ((t182 <= uniforms.u0) && (t84 <= uniforms.u1));
    if (t191)
    {
        buffer2[(((t181 * uniforms.u1) + t83)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm13.x;
        buffer2[(((t181 * uniforms.u1) + t83)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm13.y;
    }
    else
    {
        s0[((2688u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm13.x;
        s0[((2688u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm13.y;
    }
    GroupMemoryBarrierWithGroupSync();
    if ((!(t191)))
    {
        uint t192 = (lid.x % 32u);
        uint t193 = (t192 + 0u);
        uint t194 = (t181 + (t193 / 8u));
        uint t195 = (t83 + (t193 % 8u));
        if (((t194 < uniforms.u0) && (t195 < uniforms.u1)))
        {
            buffer2[((t194 * uniforms.u1) + t195)] = s0[((2688u + ((groupLane / 32u) * 64u)) + t193)];
        }
        uint t196 = (t192 + 32u);
        uint t197 = (t181 + (t196 / 8u));
        uint t198 = (t83 + (t196 % 8u));
        if (((t197 < uniforms.u0) && (t198 < uniforms.u1)))
        {
            buffer2[((t197 * uniforms.u1) + t198)] = s0[((2688u + ((groupLane / 32u) * 64u)) + t196)];
        }
    }
    GroupMemoryBarrierWithGroupSync();
    bool t199 = ((t182 <= uniforms.u0) && (t94 <= uniforms.u1));
    if (t199)
    {
        buffer2[(((t181 * uniforms.u1) + t93)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm14.x;
        buffer2[(((t181 * uniforms.u1) + t93)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm14.y;
    }
    else
    {
        s0[((2688u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm14.x;
        s0[((2688u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm14.y;
    }
    GroupMemoryBarrierWithGroupSync();
    if ((!(t199)))
    {
        uint t200 = (lid.x % 32u);
        uint t201 = (t200 + 0u);
        uint t202 = (t181 + (t201 / 8u));
        uint t203 = (t93 + (t201 % 8u));
        if (((t202 < uniforms.u0) && (t203 < uniforms.u1)))
        {
            buffer2[((t202 * uniforms.u1) + t203)] = s0[((2688u + ((groupLane / 32u) * 64u)) + t201)];
        }
        uint t204 = (t200 + 32u);
        uint t205 = (t181 + (t204 / 8u));
        uint t206 = (t93 + (t204 % 8u));
        if (((t205 < uniforms.u0) && (t206 < uniforms.u1)))
        {
            buffer2[((t205 * uniforms.u1) + t206)] = s0[((2688u + ((groupLane / 32u) * 64u)) + t204)];
        }
    }
    GroupMemoryBarrierWithGroupSync();
    bool t207 = ((t182 <= uniforms.u0) && (t104 <= uniforms.u1));
    if (t207)
    {
        buffer2[(((t181 * uniforms.u1) + t103)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm15.x;
        buffer2[(((t181 * uniforms.u1) + t103)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm15.y;
    }
    else
    {
        s0[((2688u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm15.x;
        s0[((2688u + ((groupLane / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm15.y;
    }
    GroupMemoryBarrierWithGroupSync();
    if ((!(t207)))
    {
        uint t208 = (lid.x % 32u);
        uint t209 = (t208 + 0u);
        uint t210 = (t181 + (t209 / 8u));
        uint t211 = (t103 + (t209 % 8u));
        if (((t210 < uniforms.u0) && (t211 < uniforms.u1)))
        {
            buffer2[((t210 * uniforms.u1) + t211)] = s0[((2688u + ((groupLane / 32u) * 64u)) + t209)];
        }
        uint t212 = (t208 + 32u);
        uint t213 = (t181 + (t212 / 8u));
        uint t214 = (t103 + (t212 % 8u));
        if (((t213 < uniforms.u0) && (t214 < uniforms.u1)))
        {
            buffer2[((t213 * uniforms.u1) + t214)] = s0[((2688u + ((groupLane / 32u) * 64u)) + t212)];
        }
    }
    GroupMemoryBarrierWithGroupSync();
}
