#version 450

layout(std140, set = 0, binding = 16) uniform Uniforms
{
    uint u0;
    uint u1;
    uint u2;
    uint width;
    uint height;
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

shared float s0[2944];

layout(local_size_x = 128, local_size_y = 1, local_size_z = 1) in;

void main()
{
    uvec2 gid = gl_GlobalInvocationID.xy;
    uvec2 lid = gl_LocalInvocationID.xy;
    uvec2 tgid = gl_WorkGroupID.xy;
    uint sgmLane = gl_LocalInvocationIndex % 32u;
    uint sgmRow = sgmLane / 4u;
    uint sgmColumn = (sgmLane % 4u) * 2u;
    uint sgmBase = (gl_LocalInvocationIndex / 32u) * 128u;
    vec4 t0 = vec4(0.0, 0.0, 0.0, 0.0);
    vec4 v0 = t0;
    vec4 v1 = t0;
    vec4 v2 = t0;
    vec4 v3 = t0;
    vec4 v4 = t0;
    vec4 v5 = t0;
    vec4 v6 = t0;
    vec4 v7 = t0;
    vec2 sgm0 = vec2(0.0, 0.0);
    vec2 sgm1 = vec2(0.0, 0.0);
    vec2 sgm2 = vec2(0.0, 0.0);
    vec2 sgm3 = vec2(0.0, 0.0);
    vec2 sgm4 = vec2(0.0, 0.0);
    vec2 sgm5 = vec2(0.0, 0.0);
    vec2 sgm6 = vec2(0.0, 0.0);
    vec2 sgm7 = vec2(0.0, 0.0);
    vec2 sgm8 = vec2(0.0, 0.0);
    vec2 sgm9 = vec2(0.0, 0.0);
    vec2 sgm10 = vec2(0.0, 0.0);
    vec2 sgm11 = vec2(0.0, 0.0);
    vec2 sgm12 = vec2(0.0, 0.0);
    vec2 sgm13 = vec2(0.0, 0.0);
    vec2 sgm14 = vec2(0.0, 0.0);
    vec2 sgm15 = vec2(0.0, 0.0);
    uint t1 = (lid.x + 0u);
    uint t2 = ((t1 % 4u) * 4u);
    uint t3 = (0u + t2);
    bool t4 = (t3 < uniforms.u2);
    uint t5 = (tgid.y * 64u);
    uint t6 = (t1 / 4u);
    uint t7 = (t5 + t6);
    uint t8 = (uniforms.u0 - 1u);
    uint t9 = (uniforms.u2 - 4u);
    uint t10 = ((((min(t7, t8) * uniforms.u2) + min(t3, t9)) / 4u) * 4u);
    vec4 t11 = vec4(buffer0[t10], buffer0[t10 + 1u], buffer0[t10 + 2u], buffer0[t10 + 3u]);
    v0 = vec4((t4 ? (t11).x : 0.0), (t4 ? (t11).y : 0.0), (t4 ? (t11).z : 0.0), (t4 ? (t11).w : 0.0));
    uint t12 = (lid.x + 128u);
    uint t13 = ((t12 % 4u) * 4u);
    uint t14 = (0u + t13);
    bool t15 = (t14 < uniforms.u2);
    uint t16 = (t12 / 4u);
    uint t17 = (t5 + t16);
    uint t18 = ((((min(t17, t8) * uniforms.u2) + min(t14, t9)) / 4u) * 4u);
    vec4 t19 = vec4(buffer0[t18], buffer0[t18 + 1u], buffer0[t18 + 2u], buffer0[t18 + 3u]);
    v1 = vec4((t15 ? (t19).x : 0.0), (t15 ? (t19).y : 0.0), (t15 ? (t19).z : 0.0), (t15 ? (t19).w : 0.0));
    bool t20 = (t3 < uniforms.u2);
    uint t21 = (tgid.x * 64u);
    uint t22 = (t21 + t6);
    uint t23 = (uniforms.u1 - 1u);
    uint t24 = ((((min(t22, t23) * uniforms.u2) + min(t3, t9)) / 4u) * 4u);
    vec4 t25 = vec4(buffer1[t24], buffer1[t24 + 1u], buffer1[t24 + 2u], buffer1[t24 + 3u]);
    v4 = vec4((t20 ? (t25).x : 0.0), (t20 ? (t25).y : 0.0), (t20 ? (t25).z : 0.0), (t20 ? (t25).w : 0.0));
    bool t26 = (t14 < uniforms.u2);
    uint t27 = (t21 + t16);
    uint t28 = ((((min(t27, t23) * uniforms.u2) + min(t14, t9)) / 4u) * 4u);
    vec4 t29 = vec4(buffer1[t28], buffer1[t28 + 1u], buffer1[t28 + 2u], buffer1[t28 + 3u]);
    v5 = vec4((t26 ? (t29).x : 0.0), (t26 ? (t29).y : 0.0), (t26 ? (t29).z : 0.0), (t26 ? (t29).w : 0.0));
    uint v8 = 0u;
    while ((v8 < uniforms.u2))
    {
        memoryBarrierShared();
        barrier();
        uint t30 = ((t6 * 24u) + t2);
        s0[t30] = (v0).x;
        s0[(t30 + 1u)] = (v0).y;
        s0[(t30 + 2u)] = (v0).z;
        s0[(t30 + 3u)] = (v0).w;
        uint t31 = ((t16 * 24u) + t13);
        s0[t31] = (v1).x;
        s0[(t31 + 1u)] = (v1).y;
        s0[(t31 + 2u)] = (v1).z;
        s0[(t31 + 3u)] = (v1).w;
        uint t32 = ((1536u + (t2 * 72u)) + t6);
        s0[t32] = (v4).x;
        s0[(t32 + 72u)] = (v4).y;
        s0[(t32 + 144u)] = (v4).z;
        s0[(t32 + 216u)] = (v4).w;
        uint t33 = ((1536u + (t13 * 72u)) + t16);
        s0[t33] = (v5).x;
        s0[(t33 + 72u)] = (v5).y;
        s0[(t33 + 144u)] = (v5).z;
        s0[(t33 + 216u)] = (v5).w;
        memoryBarrierShared();
        barrier();
        uint t34 = (v8 + 16u);
        uint t35 = (t34 + t2);
        bool t36 = (t35 < uniforms.u2);
        uint t37 = ((((min(t7, t8) * uniforms.u2) + min(t35, t9)) / 4u) * 4u);
        vec4 t38 = vec4(buffer0[t37], buffer0[t37 + 1u], buffer0[t37 + 2u], buffer0[t37 + 3u]);
        v0 = vec4((t36 ? (t38).x : 0.0), (t36 ? (t38).y : 0.0), (t36 ? (t38).z : 0.0), (t36 ? (t38).w : 0.0));
        uint t39 = (t34 + t13);
        bool t40 = (t39 < uniforms.u2);
        uint t41 = ((((min(t17, t8) * uniforms.u2) + min(t39, t9)) / 4u) * 4u);
        vec4 t42 = vec4(buffer0[t41], buffer0[t41 + 1u], buffer0[t41 + 2u], buffer0[t41 + 3u]);
        v1 = vec4((t40 ? (t42).x : 0.0), (t40 ? (t42).y : 0.0), (t40 ? (t42).z : 0.0), (t40 ? (t42).w : 0.0));
        uint t43 = (t34 + t2);
        bool t44 = (t43 < uniforms.u2);
        uint t45 = ((((min(t22, t23) * uniforms.u2) + min(t43, t9)) / 4u) * 4u);
        vec4 t46 = vec4(buffer1[t45], buffer1[t45 + 1u], buffer1[t45 + 2u], buffer1[t45 + 3u]);
        v4 = vec4((t44 ? (t46).x : 0.0), (t44 ? (t46).y : 0.0), (t44 ? (t46).z : 0.0), (t44 ? (t46).w : 0.0));
        uint t47 = (t34 + t13);
        bool t48 = (t47 < uniforms.u2);
        uint t49 = ((((min(t27, t23) * uniforms.u2) + min(t47, t9)) / 4u) * 4u);
        vec4 t50 = vec4(buffer1[t49], buffer1[t49 + 1u], buffer1[t49 + 2u], buffer1[t49 + 3u]);
        v5 = vec4((t48 ? (t50).x : 0.0), (t48 ? (t50).y : 0.0), (t48 ? (t50).z : 0.0), (t48 ? (t50).w : 0.0));
        uint t51 = (((gl_LocalInvocationIndex / 32u) % 2u) * 32u);
        uint t52 = ((t51 + 0u) * 24u);
        vec2 sgm16 = vec2(s0[((t52 + 0u)) + sgmRow * (24u) + sgmColumn], s0[((t52 + 0u)) + sgmRow * (24u) + sgmColumn + 1u]);
        uint t53 = ((t51 + 8u) * 24u);
        vec2 sgm17 = vec2(s0[((t53 + 0u)) + sgmRow * (24u) + sgmColumn], s0[((t53 + 0u)) + sgmRow * (24u) + sgmColumn + 1u]);
        uint t54 = ((t51 + 16u) * 24u);
        vec2 sgm18 = vec2(s0[((t54 + 0u)) + sgmRow * (24u) + sgmColumn], s0[((t54 + 0u)) + sgmRow * (24u) + sgmColumn + 1u]);
        uint t55 = ((t51 + 24u) * 24u);
        vec2 sgm19 = vec2(s0[((t55 + 0u)) + sgmRow * (24u) + sgmColumn], s0[((t55 + 0u)) + sgmRow * (24u) + sgmColumn + 1u]);
        uint t56 = (((gl_LocalInvocationIndex / 32u) / 2u) * 32u);
        uint t57 = (1536u + t56);
        vec2 sgm20 = vec2(s0[((t57 + 0u)) + sgmRow * (72u) + sgmColumn], s0[((t57 + 0u)) + sgmRow * (72u) + sgmColumn + 1u]);
        vec2 sgm21 = vec2(s0[((t57 + 8u)) + sgmRow * (72u) + sgmColumn], s0[((t57 + 8u)) + sgmRow * (72u) + sgmColumn + 1u]);
        vec2 sgm22 = vec2(s0[((t57 + 16u)) + sgmRow * (72u) + sgmColumn], s0[((t57 + 16u)) + sgmRow * (72u) + sgmColumn + 1u]);
        vec2 sgm23 = vec2(s0[((t57 + 24u)) + sgmRow * (72u) + sgmColumn], s0[((t57 + 24u)) + sgmRow * (72u) + sgmColumn + 1u]);
        for (uint sgm0k = 0u; sgm0k < 8u; ++sgm0k)
        {
            float sgm0l = s0[((t52 + 0u)) + sgmRow * (24u) + sgm0k];
            sgm0.x += sgm0l * s0[((t57 + 0u)) + sgm0k * (72u) + sgmColumn];
            sgm0.y += sgm0l * s0[((t57 + 0u)) + sgm0k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm1k = 0u; sgm1k < 8u; ++sgm1k)
        {
            float sgm1l = s0[((t52 + 0u)) + sgmRow * (24u) + sgm1k];
            sgm1.x += sgm1l * s0[((t57 + 8u)) + sgm1k * (72u) + sgmColumn];
            sgm1.y += sgm1l * s0[((t57 + 8u)) + sgm1k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm2k = 0u; sgm2k < 8u; ++sgm2k)
        {
            float sgm2l = s0[((t52 + 0u)) + sgmRow * (24u) + sgm2k];
            sgm2.x += sgm2l * s0[((t57 + 16u)) + sgm2k * (72u) + sgmColumn];
            sgm2.y += sgm2l * s0[((t57 + 16u)) + sgm2k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm3k = 0u; sgm3k < 8u; ++sgm3k)
        {
            float sgm3l = s0[((t52 + 0u)) + sgmRow * (24u) + sgm3k];
            sgm3.x += sgm3l * s0[((t57 + 24u)) + sgm3k * (72u) + sgmColumn];
            sgm3.y += sgm3l * s0[((t57 + 24u)) + sgm3k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm4k = 0u; sgm4k < 8u; ++sgm4k)
        {
            float sgm4l = s0[((t53 + 0u)) + sgmRow * (24u) + sgm4k];
            sgm4.x += sgm4l * s0[((t57 + 0u)) + sgm4k * (72u) + sgmColumn];
            sgm4.y += sgm4l * s0[((t57 + 0u)) + sgm4k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm5k = 0u; sgm5k < 8u; ++sgm5k)
        {
            float sgm5l = s0[((t53 + 0u)) + sgmRow * (24u) + sgm5k];
            sgm5.x += sgm5l * s0[((t57 + 8u)) + sgm5k * (72u) + sgmColumn];
            sgm5.y += sgm5l * s0[((t57 + 8u)) + sgm5k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm6k = 0u; sgm6k < 8u; ++sgm6k)
        {
            float sgm6l = s0[((t53 + 0u)) + sgmRow * (24u) + sgm6k];
            sgm6.x += sgm6l * s0[((t57 + 16u)) + sgm6k * (72u) + sgmColumn];
            sgm6.y += sgm6l * s0[((t57 + 16u)) + sgm6k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm7k = 0u; sgm7k < 8u; ++sgm7k)
        {
            float sgm7l = s0[((t53 + 0u)) + sgmRow * (24u) + sgm7k];
            sgm7.x += sgm7l * s0[((t57 + 24u)) + sgm7k * (72u) + sgmColumn];
            sgm7.y += sgm7l * s0[((t57 + 24u)) + sgm7k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm8k = 0u; sgm8k < 8u; ++sgm8k)
        {
            float sgm8l = s0[((t54 + 0u)) + sgmRow * (24u) + sgm8k];
            sgm8.x += sgm8l * s0[((t57 + 0u)) + sgm8k * (72u) + sgmColumn];
            sgm8.y += sgm8l * s0[((t57 + 0u)) + sgm8k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm9k = 0u; sgm9k < 8u; ++sgm9k)
        {
            float sgm9l = s0[((t54 + 0u)) + sgmRow * (24u) + sgm9k];
            sgm9.x += sgm9l * s0[((t57 + 8u)) + sgm9k * (72u) + sgmColumn];
            sgm9.y += sgm9l * s0[((t57 + 8u)) + sgm9k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm10k = 0u; sgm10k < 8u; ++sgm10k)
        {
            float sgm10l = s0[((t54 + 0u)) + sgmRow * (24u) + sgm10k];
            sgm10.x += sgm10l * s0[((t57 + 16u)) + sgm10k * (72u) + sgmColumn];
            sgm10.y += sgm10l * s0[((t57 + 16u)) + sgm10k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm11k = 0u; sgm11k < 8u; ++sgm11k)
        {
            float sgm11l = s0[((t54 + 0u)) + sgmRow * (24u) + sgm11k];
            sgm11.x += sgm11l * s0[((t57 + 24u)) + sgm11k * (72u) + sgmColumn];
            sgm11.y += sgm11l * s0[((t57 + 24u)) + sgm11k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm12k = 0u; sgm12k < 8u; ++sgm12k)
        {
            float sgm12l = s0[((t55 + 0u)) + sgmRow * (24u) + sgm12k];
            sgm12.x += sgm12l * s0[((t57 + 0u)) + sgm12k * (72u) + sgmColumn];
            sgm12.y += sgm12l * s0[((t57 + 0u)) + sgm12k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm13k = 0u; sgm13k < 8u; ++sgm13k)
        {
            float sgm13l = s0[((t55 + 0u)) + sgmRow * (24u) + sgm13k];
            sgm13.x += sgm13l * s0[((t57 + 8u)) + sgm13k * (72u) + sgmColumn];
            sgm13.y += sgm13l * s0[((t57 + 8u)) + sgm13k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm14k = 0u; sgm14k < 8u; ++sgm14k)
        {
            float sgm14l = s0[((t55 + 0u)) + sgmRow * (24u) + sgm14k];
            sgm14.x += sgm14l * s0[((t57 + 16u)) + sgm14k * (72u) + sgmColumn];
            sgm14.y += sgm14l * s0[((t57 + 16u)) + sgm14k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm15k = 0u; sgm15k < 8u; ++sgm15k)
        {
            float sgm15l = s0[((t55 + 0u)) + sgmRow * (24u) + sgm15k];
            sgm15.x += sgm15l * s0[((t57 + 24u)) + sgm15k * (72u) + sgmColumn];
            sgm15.y += sgm15l * s0[((t57 + 24u)) + sgm15k * (72u) + sgmColumn + 1u];
        }
        vec2 sgm24 = vec2(s0[((t52 + 8u)) + sgmRow * (24u) + sgmColumn], s0[((t52 + 8u)) + sgmRow * (24u) + sgmColumn + 1u]);
        vec2 sgm25 = vec2(s0[((t53 + 8u)) + sgmRow * (24u) + sgmColumn], s0[((t53 + 8u)) + sgmRow * (24u) + sgmColumn + 1u]);
        vec2 sgm26 = vec2(s0[((t54 + 8u)) + sgmRow * (24u) + sgmColumn], s0[((t54 + 8u)) + sgmRow * (24u) + sgmColumn + 1u]);
        vec2 sgm27 = vec2(s0[((t55 + 8u)) + sgmRow * (24u) + sgmColumn], s0[((t55 + 8u)) + sgmRow * (24u) + sgmColumn + 1u]);
        uint t58 = (2112u + t56);
        vec2 sgm28 = vec2(s0[((t58 + 0u)) + sgmRow * (72u) + sgmColumn], s0[((t58 + 0u)) + sgmRow * (72u) + sgmColumn + 1u]);
        vec2 sgm29 = vec2(s0[((t58 + 8u)) + sgmRow * (72u) + sgmColumn], s0[((t58 + 8u)) + sgmRow * (72u) + sgmColumn + 1u]);
        vec2 sgm30 = vec2(s0[((t58 + 16u)) + sgmRow * (72u) + sgmColumn], s0[((t58 + 16u)) + sgmRow * (72u) + sgmColumn + 1u]);
        vec2 sgm31 = vec2(s0[((t58 + 24u)) + sgmRow * (72u) + sgmColumn], s0[((t58 + 24u)) + sgmRow * (72u) + sgmColumn + 1u]);
        for (uint sgm0k = 0u; sgm0k < 8u; ++sgm0k)
        {
            float sgm0l = s0[((t52 + 8u)) + sgmRow * (24u) + sgm0k];
            sgm0.x += sgm0l * s0[((t58 + 0u)) + sgm0k * (72u) + sgmColumn];
            sgm0.y += sgm0l * s0[((t58 + 0u)) + sgm0k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm1k = 0u; sgm1k < 8u; ++sgm1k)
        {
            float sgm1l = s0[((t52 + 8u)) + sgmRow * (24u) + sgm1k];
            sgm1.x += sgm1l * s0[((t58 + 8u)) + sgm1k * (72u) + sgmColumn];
            sgm1.y += sgm1l * s0[((t58 + 8u)) + sgm1k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm2k = 0u; sgm2k < 8u; ++sgm2k)
        {
            float sgm2l = s0[((t52 + 8u)) + sgmRow * (24u) + sgm2k];
            sgm2.x += sgm2l * s0[((t58 + 16u)) + sgm2k * (72u) + sgmColumn];
            sgm2.y += sgm2l * s0[((t58 + 16u)) + sgm2k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm3k = 0u; sgm3k < 8u; ++sgm3k)
        {
            float sgm3l = s0[((t52 + 8u)) + sgmRow * (24u) + sgm3k];
            sgm3.x += sgm3l * s0[((t58 + 24u)) + sgm3k * (72u) + sgmColumn];
            sgm3.y += sgm3l * s0[((t58 + 24u)) + sgm3k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm4k = 0u; sgm4k < 8u; ++sgm4k)
        {
            float sgm4l = s0[((t53 + 8u)) + sgmRow * (24u) + sgm4k];
            sgm4.x += sgm4l * s0[((t58 + 0u)) + sgm4k * (72u) + sgmColumn];
            sgm4.y += sgm4l * s0[((t58 + 0u)) + sgm4k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm5k = 0u; sgm5k < 8u; ++sgm5k)
        {
            float sgm5l = s0[((t53 + 8u)) + sgmRow * (24u) + sgm5k];
            sgm5.x += sgm5l * s0[((t58 + 8u)) + sgm5k * (72u) + sgmColumn];
            sgm5.y += sgm5l * s0[((t58 + 8u)) + sgm5k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm6k = 0u; sgm6k < 8u; ++sgm6k)
        {
            float sgm6l = s0[((t53 + 8u)) + sgmRow * (24u) + sgm6k];
            sgm6.x += sgm6l * s0[((t58 + 16u)) + sgm6k * (72u) + sgmColumn];
            sgm6.y += sgm6l * s0[((t58 + 16u)) + sgm6k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm7k = 0u; sgm7k < 8u; ++sgm7k)
        {
            float sgm7l = s0[((t53 + 8u)) + sgmRow * (24u) + sgm7k];
            sgm7.x += sgm7l * s0[((t58 + 24u)) + sgm7k * (72u) + sgmColumn];
            sgm7.y += sgm7l * s0[((t58 + 24u)) + sgm7k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm8k = 0u; sgm8k < 8u; ++sgm8k)
        {
            float sgm8l = s0[((t54 + 8u)) + sgmRow * (24u) + sgm8k];
            sgm8.x += sgm8l * s0[((t58 + 0u)) + sgm8k * (72u) + sgmColumn];
            sgm8.y += sgm8l * s0[((t58 + 0u)) + sgm8k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm9k = 0u; sgm9k < 8u; ++sgm9k)
        {
            float sgm9l = s0[((t54 + 8u)) + sgmRow * (24u) + sgm9k];
            sgm9.x += sgm9l * s0[((t58 + 8u)) + sgm9k * (72u) + sgmColumn];
            sgm9.y += sgm9l * s0[((t58 + 8u)) + sgm9k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm10k = 0u; sgm10k < 8u; ++sgm10k)
        {
            float sgm10l = s0[((t54 + 8u)) + sgmRow * (24u) + sgm10k];
            sgm10.x += sgm10l * s0[((t58 + 16u)) + sgm10k * (72u) + sgmColumn];
            sgm10.y += sgm10l * s0[((t58 + 16u)) + sgm10k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm11k = 0u; sgm11k < 8u; ++sgm11k)
        {
            float sgm11l = s0[((t54 + 8u)) + sgmRow * (24u) + sgm11k];
            sgm11.x += sgm11l * s0[((t58 + 24u)) + sgm11k * (72u) + sgmColumn];
            sgm11.y += sgm11l * s0[((t58 + 24u)) + sgm11k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm12k = 0u; sgm12k < 8u; ++sgm12k)
        {
            float sgm12l = s0[((t55 + 8u)) + sgmRow * (24u) + sgm12k];
            sgm12.x += sgm12l * s0[((t58 + 0u)) + sgm12k * (72u) + sgmColumn];
            sgm12.y += sgm12l * s0[((t58 + 0u)) + sgm12k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm13k = 0u; sgm13k < 8u; ++sgm13k)
        {
            float sgm13l = s0[((t55 + 8u)) + sgmRow * (24u) + sgm13k];
            sgm13.x += sgm13l * s0[((t58 + 8u)) + sgm13k * (72u) + sgmColumn];
            sgm13.y += sgm13l * s0[((t58 + 8u)) + sgm13k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm14k = 0u; sgm14k < 8u; ++sgm14k)
        {
            float sgm14l = s0[((t55 + 8u)) + sgmRow * (24u) + sgm14k];
            sgm14.x += sgm14l * s0[((t58 + 16u)) + sgm14k * (72u) + sgmColumn];
            sgm14.y += sgm14l * s0[((t58 + 16u)) + sgm14k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm15k = 0u; sgm15k < 8u; ++sgm15k)
        {
            float sgm15l = s0[((t55 + 8u)) + sgmRow * (24u) + sgm15k];
            sgm15.x += sgm15l * s0[((t58 + 24u)) + sgm15k * (72u) + sgmColumn];
            sgm15.y += sgm15l * s0[((t58 + 24u)) + sgm15k * (72u) + sgmColumn + 1u];
        }
        v8 = (v8 + 16u);
    }
    uint t59 = (((gl_LocalInvocationIndex / 32u) % 2u) * 32u);
    uint t60 = (t5 + t59);
    uint t61 = (t60 + 0u);
    uint t62 = (t61 + 8u);
    uint t63 = (((gl_LocalInvocationIndex / 32u) / 2u) * 32u);
    uint t64 = (t21 + t63);
    uint t65 = (t64 + 0u);
    uint t66 = (t65 + 8u);
    bool t67 = ((t62 <= uniforms.u0) && (t66 <= uniforms.u1));
    if (t67)
    {
        buffer2[(((t61 * uniforms.u1) + t65)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm0.x;
        buffer2[(((t61 * uniforms.u1) + t65)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm0.y;
    }
    else
    {
        s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm0.x;
        s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm0.y;
    }
    memoryBarrierShared();
    barrier();
    if ((!(t67)))
    {
        uint t68 = (lid.x % 32u);
        uint t69 = (t68 + 0u);
        uint t70 = (t61 + (t69 / 8u));
        uint t71 = (t65 + (t69 % 8u));
        if (((t70 < uniforms.u0) && (t71 < uniforms.u1)))
        {
            buffer2[((t70 * uniforms.u1) + t71)] = s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t69)];
        }
        uint t72 = (t68 + 32u);
        uint t73 = (t61 + (t72 / 8u));
        uint t74 = (t65 + (t72 % 8u));
        if (((t73 < uniforms.u0) && (t74 < uniforms.u1)))
        {
            buffer2[((t73 * uniforms.u1) + t74)] = s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t72)];
        }
    }
    memoryBarrierShared();
    barrier();
    uint t75 = (t64 + 8u);
    uint t76 = (t75 + 8u);
    bool t77 = ((t62 <= uniforms.u0) && (t76 <= uniforms.u1));
    if (t77)
    {
        buffer2[(((t61 * uniforms.u1) + t75)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm1.x;
        buffer2[(((t61 * uniforms.u1) + t75)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm1.y;
    }
    else
    {
        s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm1.x;
        s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm1.y;
    }
    memoryBarrierShared();
    barrier();
    if ((!(t77)))
    {
        uint t78 = (lid.x % 32u);
        uint t79 = (t78 + 0u);
        uint t80 = (t61 + (t79 / 8u));
        uint t81 = (t75 + (t79 % 8u));
        if (((t80 < uniforms.u0) && (t81 < uniforms.u1)))
        {
            buffer2[((t80 * uniforms.u1) + t81)] = s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t79)];
        }
        uint t82 = (t78 + 32u);
        uint t83 = (t61 + (t82 / 8u));
        uint t84 = (t75 + (t82 % 8u));
        if (((t83 < uniforms.u0) && (t84 < uniforms.u1)))
        {
            buffer2[((t83 * uniforms.u1) + t84)] = s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t82)];
        }
    }
    memoryBarrierShared();
    barrier();
    uint t85 = (t64 + 16u);
    uint t86 = (t85 + 8u);
    bool t87 = ((t62 <= uniforms.u0) && (t86 <= uniforms.u1));
    if (t87)
    {
        buffer2[(((t61 * uniforms.u1) + t85)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm2.x;
        buffer2[(((t61 * uniforms.u1) + t85)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm2.y;
    }
    else
    {
        s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm2.x;
        s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm2.y;
    }
    memoryBarrierShared();
    barrier();
    if ((!(t87)))
    {
        uint t88 = (lid.x % 32u);
        uint t89 = (t88 + 0u);
        uint t90 = (t61 + (t89 / 8u));
        uint t91 = (t85 + (t89 % 8u));
        if (((t90 < uniforms.u0) && (t91 < uniforms.u1)))
        {
            buffer2[((t90 * uniforms.u1) + t91)] = s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t89)];
        }
        uint t92 = (t88 + 32u);
        uint t93 = (t61 + (t92 / 8u));
        uint t94 = (t85 + (t92 % 8u));
        if (((t93 < uniforms.u0) && (t94 < uniforms.u1)))
        {
            buffer2[((t93 * uniforms.u1) + t94)] = s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t92)];
        }
    }
    memoryBarrierShared();
    barrier();
    uint t95 = (t64 + 24u);
    uint t96 = (t95 + 8u);
    bool t97 = ((t62 <= uniforms.u0) && (t96 <= uniforms.u1));
    if (t97)
    {
        buffer2[(((t61 * uniforms.u1) + t95)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm3.x;
        buffer2[(((t61 * uniforms.u1) + t95)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm3.y;
    }
    else
    {
        s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm3.x;
        s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm3.y;
    }
    memoryBarrierShared();
    barrier();
    if ((!(t97)))
    {
        uint t98 = (lid.x % 32u);
        uint t99 = (t98 + 0u);
        uint t100 = (t61 + (t99 / 8u));
        uint t101 = (t95 + (t99 % 8u));
        if (((t100 < uniforms.u0) && (t101 < uniforms.u1)))
        {
            buffer2[((t100 * uniforms.u1) + t101)] = s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t99)];
        }
        uint t102 = (t98 + 32u);
        uint t103 = (t61 + (t102 / 8u));
        uint t104 = (t95 + (t102 % 8u));
        if (((t103 < uniforms.u0) && (t104 < uniforms.u1)))
        {
            buffer2[((t103 * uniforms.u1) + t104)] = s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t102)];
        }
    }
    memoryBarrierShared();
    barrier();
    uint t105 = (t60 + 8u);
    uint t106 = (t105 + 8u);
    bool t107 = ((t106 <= uniforms.u0) && (t66 <= uniforms.u1));
    if (t107)
    {
        buffer2[(((t105 * uniforms.u1) + t65)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm4.x;
        buffer2[(((t105 * uniforms.u1) + t65)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm4.y;
    }
    else
    {
        s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm4.x;
        s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm4.y;
    }
    memoryBarrierShared();
    barrier();
    if ((!(t107)))
    {
        uint t108 = (lid.x % 32u);
        uint t109 = (t108 + 0u);
        uint t110 = (t105 + (t109 / 8u));
        uint t111 = (t65 + (t109 % 8u));
        if (((t110 < uniforms.u0) && (t111 < uniforms.u1)))
        {
            buffer2[((t110 * uniforms.u1) + t111)] = s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t109)];
        }
        uint t112 = (t108 + 32u);
        uint t113 = (t105 + (t112 / 8u));
        uint t114 = (t65 + (t112 % 8u));
        if (((t113 < uniforms.u0) && (t114 < uniforms.u1)))
        {
            buffer2[((t113 * uniforms.u1) + t114)] = s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t112)];
        }
    }
    memoryBarrierShared();
    barrier();
    bool t115 = ((t106 <= uniforms.u0) && (t76 <= uniforms.u1));
    if (t115)
    {
        buffer2[(((t105 * uniforms.u1) + t75)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm5.x;
        buffer2[(((t105 * uniforms.u1) + t75)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm5.y;
    }
    else
    {
        s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm5.x;
        s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm5.y;
    }
    memoryBarrierShared();
    barrier();
    if ((!(t115)))
    {
        uint t116 = (lid.x % 32u);
        uint t117 = (t116 + 0u);
        uint t118 = (t105 + (t117 / 8u));
        uint t119 = (t75 + (t117 % 8u));
        if (((t118 < uniforms.u0) && (t119 < uniforms.u1)))
        {
            buffer2[((t118 * uniforms.u1) + t119)] = s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t117)];
        }
        uint t120 = (t116 + 32u);
        uint t121 = (t105 + (t120 / 8u));
        uint t122 = (t75 + (t120 % 8u));
        if (((t121 < uniforms.u0) && (t122 < uniforms.u1)))
        {
            buffer2[((t121 * uniforms.u1) + t122)] = s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t120)];
        }
    }
    memoryBarrierShared();
    barrier();
    bool t123 = ((t106 <= uniforms.u0) && (t86 <= uniforms.u1));
    if (t123)
    {
        buffer2[(((t105 * uniforms.u1) + t85)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm6.x;
        buffer2[(((t105 * uniforms.u1) + t85)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm6.y;
    }
    else
    {
        s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm6.x;
        s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm6.y;
    }
    memoryBarrierShared();
    barrier();
    if ((!(t123)))
    {
        uint t124 = (lid.x % 32u);
        uint t125 = (t124 + 0u);
        uint t126 = (t105 + (t125 / 8u));
        uint t127 = (t85 + (t125 % 8u));
        if (((t126 < uniforms.u0) && (t127 < uniforms.u1)))
        {
            buffer2[((t126 * uniforms.u1) + t127)] = s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t125)];
        }
        uint t128 = (t124 + 32u);
        uint t129 = (t105 + (t128 / 8u));
        uint t130 = (t85 + (t128 % 8u));
        if (((t129 < uniforms.u0) && (t130 < uniforms.u1)))
        {
            buffer2[((t129 * uniforms.u1) + t130)] = s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t128)];
        }
    }
    memoryBarrierShared();
    barrier();
    bool t131 = ((t106 <= uniforms.u0) && (t96 <= uniforms.u1));
    if (t131)
    {
        buffer2[(((t105 * uniforms.u1) + t95)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm7.x;
        buffer2[(((t105 * uniforms.u1) + t95)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm7.y;
    }
    else
    {
        s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm7.x;
        s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm7.y;
    }
    memoryBarrierShared();
    barrier();
    if ((!(t131)))
    {
        uint t132 = (lid.x % 32u);
        uint t133 = (t132 + 0u);
        uint t134 = (t105 + (t133 / 8u));
        uint t135 = (t95 + (t133 % 8u));
        if (((t134 < uniforms.u0) && (t135 < uniforms.u1)))
        {
            buffer2[((t134 * uniforms.u1) + t135)] = s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t133)];
        }
        uint t136 = (t132 + 32u);
        uint t137 = (t105 + (t136 / 8u));
        uint t138 = (t95 + (t136 % 8u));
        if (((t137 < uniforms.u0) && (t138 < uniforms.u1)))
        {
            buffer2[((t137 * uniforms.u1) + t138)] = s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t136)];
        }
    }
    memoryBarrierShared();
    barrier();
    uint t139 = (t60 + 16u);
    uint t140 = (t139 + 8u);
    bool t141 = ((t140 <= uniforms.u0) && (t66 <= uniforms.u1));
    if (t141)
    {
        buffer2[(((t139 * uniforms.u1) + t65)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm8.x;
        buffer2[(((t139 * uniforms.u1) + t65)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm8.y;
    }
    else
    {
        s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm8.x;
        s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm8.y;
    }
    memoryBarrierShared();
    barrier();
    if ((!(t141)))
    {
        uint t142 = (lid.x % 32u);
        uint t143 = (t142 + 0u);
        uint t144 = (t139 + (t143 / 8u));
        uint t145 = (t65 + (t143 % 8u));
        if (((t144 < uniforms.u0) && (t145 < uniforms.u1)))
        {
            buffer2[((t144 * uniforms.u1) + t145)] = s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t143)];
        }
        uint t146 = (t142 + 32u);
        uint t147 = (t139 + (t146 / 8u));
        uint t148 = (t65 + (t146 % 8u));
        if (((t147 < uniforms.u0) && (t148 < uniforms.u1)))
        {
            buffer2[((t147 * uniforms.u1) + t148)] = s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t146)];
        }
    }
    memoryBarrierShared();
    barrier();
    bool t149 = ((t140 <= uniforms.u0) && (t76 <= uniforms.u1));
    if (t149)
    {
        buffer2[(((t139 * uniforms.u1) + t75)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm9.x;
        buffer2[(((t139 * uniforms.u1) + t75)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm9.y;
    }
    else
    {
        s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm9.x;
        s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm9.y;
    }
    memoryBarrierShared();
    barrier();
    if ((!(t149)))
    {
        uint t150 = (lid.x % 32u);
        uint t151 = (t150 + 0u);
        uint t152 = (t139 + (t151 / 8u));
        uint t153 = (t75 + (t151 % 8u));
        if (((t152 < uniforms.u0) && (t153 < uniforms.u1)))
        {
            buffer2[((t152 * uniforms.u1) + t153)] = s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t151)];
        }
        uint t154 = (t150 + 32u);
        uint t155 = (t139 + (t154 / 8u));
        uint t156 = (t75 + (t154 % 8u));
        if (((t155 < uniforms.u0) && (t156 < uniforms.u1)))
        {
            buffer2[((t155 * uniforms.u1) + t156)] = s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t154)];
        }
    }
    memoryBarrierShared();
    barrier();
    bool t157 = ((t140 <= uniforms.u0) && (t86 <= uniforms.u1));
    if (t157)
    {
        buffer2[(((t139 * uniforms.u1) + t85)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm10.x;
        buffer2[(((t139 * uniforms.u1) + t85)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm10.y;
    }
    else
    {
        s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm10.x;
        s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm10.y;
    }
    memoryBarrierShared();
    barrier();
    if ((!(t157)))
    {
        uint t158 = (lid.x % 32u);
        uint t159 = (t158 + 0u);
        uint t160 = (t139 + (t159 / 8u));
        uint t161 = (t85 + (t159 % 8u));
        if (((t160 < uniforms.u0) && (t161 < uniforms.u1)))
        {
            buffer2[((t160 * uniforms.u1) + t161)] = s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t159)];
        }
        uint t162 = (t158 + 32u);
        uint t163 = (t139 + (t162 / 8u));
        uint t164 = (t85 + (t162 % 8u));
        if (((t163 < uniforms.u0) && (t164 < uniforms.u1)))
        {
            buffer2[((t163 * uniforms.u1) + t164)] = s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t162)];
        }
    }
    memoryBarrierShared();
    barrier();
    bool t165 = ((t140 <= uniforms.u0) && (t96 <= uniforms.u1));
    if (t165)
    {
        buffer2[(((t139 * uniforms.u1) + t95)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm11.x;
        buffer2[(((t139 * uniforms.u1) + t95)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm11.y;
    }
    else
    {
        s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm11.x;
        s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm11.y;
    }
    memoryBarrierShared();
    barrier();
    if ((!(t165)))
    {
        uint t166 = (lid.x % 32u);
        uint t167 = (t166 + 0u);
        uint t168 = (t139 + (t167 / 8u));
        uint t169 = (t95 + (t167 % 8u));
        if (((t168 < uniforms.u0) && (t169 < uniforms.u1)))
        {
            buffer2[((t168 * uniforms.u1) + t169)] = s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t167)];
        }
        uint t170 = (t166 + 32u);
        uint t171 = (t139 + (t170 / 8u));
        uint t172 = (t95 + (t170 % 8u));
        if (((t171 < uniforms.u0) && (t172 < uniforms.u1)))
        {
            buffer2[((t171 * uniforms.u1) + t172)] = s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t170)];
        }
    }
    memoryBarrierShared();
    barrier();
    uint t173 = (t60 + 24u);
    uint t174 = (t173 + 8u);
    bool t175 = ((t174 <= uniforms.u0) && (t66 <= uniforms.u1));
    if (t175)
    {
        buffer2[(((t173 * uniforms.u1) + t65)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm12.x;
        buffer2[(((t173 * uniforms.u1) + t65)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm12.y;
    }
    else
    {
        s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm12.x;
        s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm12.y;
    }
    memoryBarrierShared();
    barrier();
    if ((!(t175)))
    {
        uint t176 = (lid.x % 32u);
        uint t177 = (t176 + 0u);
        uint t178 = (t173 + (t177 / 8u));
        uint t179 = (t65 + (t177 % 8u));
        if (((t178 < uniforms.u0) && (t179 < uniforms.u1)))
        {
            buffer2[((t178 * uniforms.u1) + t179)] = s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t177)];
        }
        uint t180 = (t176 + 32u);
        uint t181 = (t173 + (t180 / 8u));
        uint t182 = (t65 + (t180 % 8u));
        if (((t181 < uniforms.u0) && (t182 < uniforms.u1)))
        {
            buffer2[((t181 * uniforms.u1) + t182)] = s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t180)];
        }
    }
    memoryBarrierShared();
    barrier();
    bool t183 = ((t174 <= uniforms.u0) && (t76 <= uniforms.u1));
    if (t183)
    {
        buffer2[(((t173 * uniforms.u1) + t75)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm13.x;
        buffer2[(((t173 * uniforms.u1) + t75)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm13.y;
    }
    else
    {
        s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm13.x;
        s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm13.y;
    }
    memoryBarrierShared();
    barrier();
    if ((!(t183)))
    {
        uint t184 = (lid.x % 32u);
        uint t185 = (t184 + 0u);
        uint t186 = (t173 + (t185 / 8u));
        uint t187 = (t75 + (t185 % 8u));
        if (((t186 < uniforms.u0) && (t187 < uniforms.u1)))
        {
            buffer2[((t186 * uniforms.u1) + t187)] = s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t185)];
        }
        uint t188 = (t184 + 32u);
        uint t189 = (t173 + (t188 / 8u));
        uint t190 = (t75 + (t188 % 8u));
        if (((t189 < uniforms.u0) && (t190 < uniforms.u1)))
        {
            buffer2[((t189 * uniforms.u1) + t190)] = s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t188)];
        }
    }
    memoryBarrierShared();
    barrier();
    bool t191 = ((t174 <= uniforms.u0) && (t86 <= uniforms.u1));
    if (t191)
    {
        buffer2[(((t173 * uniforms.u1) + t85)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm14.x;
        buffer2[(((t173 * uniforms.u1) + t85)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm14.y;
    }
    else
    {
        s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm14.x;
        s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm14.y;
    }
    memoryBarrierShared();
    barrier();
    if ((!(t191)))
    {
        uint t192 = (lid.x % 32u);
        uint t193 = (t192 + 0u);
        uint t194 = (t173 + (t193 / 8u));
        uint t195 = (t85 + (t193 % 8u));
        if (((t194 < uniforms.u0) && (t195 < uniforms.u1)))
        {
            buffer2[((t194 * uniforms.u1) + t195)] = s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t193)];
        }
        uint t196 = (t192 + 32u);
        uint t197 = (t173 + (t196 / 8u));
        uint t198 = (t85 + (t196 % 8u));
        if (((t197 < uniforms.u0) && (t198 < uniforms.u1)))
        {
            buffer2[((t197 * uniforms.u1) + t198)] = s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t196)];
        }
    }
    memoryBarrierShared();
    barrier();
    bool t199 = ((t174 <= uniforms.u0) && (t96 <= uniforms.u1));
    if (t199)
    {
        buffer2[(((t173 * uniforms.u1) + t95)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm15.x;
        buffer2[(((t173 * uniforms.u1) + t95)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm15.y;
    }
    else
    {
        s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm15.x;
        s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm15.y;
    }
    memoryBarrierShared();
    barrier();
    if ((!(t199)))
    {
        uint t200 = (lid.x % 32u);
        uint t201 = (t200 + 0u);
        uint t202 = (t173 + (t201 / 8u));
        uint t203 = (t95 + (t201 % 8u));
        if (((t202 < uniforms.u0) && (t203 < uniforms.u1)))
        {
            buffer2[((t202 * uniforms.u1) + t203)] = s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t201)];
        }
        uint t204 = (t200 + 32u);
        uint t205 = (t173 + (t204 / 8u));
        uint t206 = (t95 + (t204 % 8u));
        if (((t205 < uniforms.u0) && (t206 < uniforms.u1)))
        {
            buffer2[((t205 * uniforms.u1) + t206)] = s0[((2688u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t204)];
        }
    }
    memoryBarrierShared();
    barrier();
}
