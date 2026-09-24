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

shared float s0[2176];

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
    uint t1 = (lid.x + 0u);
    uint t2 = ((t1 % 4u) * 4u);
    uint t3 = (0u + t2);
    bool t4 = (t3 < uniforms.u2);
    uint t5 = (tgid.y * 32u);
    uint t6 = (t1 / 4u);
    uint t7 = (t5 + t6);
    uint t8 = (uniforms.u0 - 1u);
    uint t9 = (uniforms.u2 - 4u);
    uint t10 = ((((min(t7, t8) * uniforms.u2) + min(t3, t9)) / 4u) * 4u);
    vec4 t11 = vec4(buffer0[t10], buffer0[t10 + 1u], buffer0[t10 + 2u], buffer0[t10 + 3u]);
    v0 = vec4((t4 ? (t11).x : 0.0), (t4 ? (t11).y : 0.0), (t4 ? (t11).z : 0.0), (t4 ? (t11).w : 0.0));
    bool t12 = (t3 < uniforms.u2);
    uint t13 = (tgid.x * 64u);
    uint t14 = (t13 + t6);
    uint t15 = (uniforms.u1 - 1u);
    uint t16 = ((((min(t14, t15) * uniforms.u2) + min(t3, t9)) / 4u) * 4u);
    vec4 t17 = vec4(buffer1[t16], buffer1[t16 + 1u], buffer1[t16 + 2u], buffer1[t16 + 3u]);
    v4 = vec4((t12 ? (t17).x : 0.0), (t12 ? (t17).y : 0.0), (t12 ? (t17).z : 0.0), (t12 ? (t17).w : 0.0));
    uint t18 = (lid.x + 128u);
    uint t19 = ((t18 % 4u) * 4u);
    uint t20 = (0u + t19);
    bool t21 = (t20 < uniforms.u2);
    uint t22 = (t18 / 4u);
    uint t23 = (t13 + t22);
    uint t24 = ((((min(t23, t15) * uniforms.u2) + min(t20, t9)) / 4u) * 4u);
    vec4 t25 = vec4(buffer1[t24], buffer1[t24 + 1u], buffer1[t24 + 2u], buffer1[t24 + 3u]);
    v5 = vec4((t21 ? (t25).x : 0.0), (t21 ? (t25).y : 0.0), (t21 ? (t25).z : 0.0), (t21 ? (t25).w : 0.0));
    uint v8 = 0u;
    while ((v8 < uniforms.u2))
    {
        memoryBarrierShared();
        barrier();
        uint t26 = ((t6 * 24u) + t2);
        s0[t26] = (v0).x;
        s0[(t26 + 1u)] = (v0).y;
        s0[(t26 + 2u)] = (v0).z;
        s0[(t26 + 3u)] = (v0).w;
        uint t27 = ((768u + (t2 * 72u)) + t6);
        s0[t27] = (v4).x;
        s0[(t27 + 72u)] = (v4).y;
        s0[(t27 + 144u)] = (v4).z;
        s0[(t27 + 216u)] = (v4).w;
        uint t28 = ((768u + (t19 * 72u)) + t22);
        s0[t28] = (v5).x;
        s0[(t28 + 72u)] = (v5).y;
        s0[(t28 + 144u)] = (v5).z;
        s0[(t28 + 216u)] = (v5).w;
        memoryBarrierShared();
        barrier();
        uint t29 = (v8 + 16u);
        uint t30 = (t29 + t2);
        bool t31 = (t30 < uniforms.u2);
        uint t32 = ((((min(t7, t8) * uniforms.u2) + min(t30, t9)) / 4u) * 4u);
        vec4 t33 = vec4(buffer0[t32], buffer0[t32 + 1u], buffer0[t32 + 2u], buffer0[t32 + 3u]);
        v0 = vec4((t31 ? (t33).x : 0.0), (t31 ? (t33).y : 0.0), (t31 ? (t33).z : 0.0), (t31 ? (t33).w : 0.0));
        uint t34 = (t29 + t2);
        bool t35 = (t34 < uniforms.u2);
        uint t36 = ((((min(t14, t15) * uniforms.u2) + min(t34, t9)) / 4u) * 4u);
        vec4 t37 = vec4(buffer1[t36], buffer1[t36 + 1u], buffer1[t36 + 2u], buffer1[t36 + 3u]);
        v4 = vec4((t35 ? (t37).x : 0.0), (t35 ? (t37).y : 0.0), (t35 ? (t37).z : 0.0), (t35 ? (t37).w : 0.0));
        uint t38 = (t29 + t19);
        bool t39 = (t38 < uniforms.u2);
        uint t40 = ((((min(t23, t15) * uniforms.u2) + min(t38, t9)) / 4u) * 4u);
        vec4 t41 = vec4(buffer1[t40], buffer1[t40 + 1u], buffer1[t40 + 2u], buffer1[t40 + 3u]);
        v5 = vec4((t39 ? (t41).x : 0.0), (t39 ? (t41).y : 0.0), (t39 ? (t41).z : 0.0), (t39 ? (t41).w : 0.0));
        uint t42 = (((gl_LocalInvocationIndex / 32u) % 1u) * 32u);
        uint t43 = ((t42 + 0u) * 24u);
        vec2 sgm8 = vec2(s0[((t43 + 0u)) + sgmRow * (24u) + sgmColumn], s0[((t43 + 0u)) + sgmRow * (24u) + sgmColumn + 1u]);
        uint t44 = ((t42 + 8u) * 24u);
        vec2 sgm9 = vec2(s0[((t44 + 0u)) + sgmRow * (24u) + sgmColumn], s0[((t44 + 0u)) + sgmRow * (24u) + sgmColumn + 1u]);
        uint t45 = ((t42 + 16u) * 24u);
        vec2 sgm10 = vec2(s0[((t45 + 0u)) + sgmRow * (24u) + sgmColumn], s0[((t45 + 0u)) + sgmRow * (24u) + sgmColumn + 1u]);
        uint t46 = ((t42 + 24u) * 24u);
        vec2 sgm11 = vec2(s0[((t46 + 0u)) + sgmRow * (24u) + sgmColumn], s0[((t46 + 0u)) + sgmRow * (24u) + sgmColumn + 1u]);
        uint t47 = (((gl_LocalInvocationIndex / 32u) / 1u) * 16u);
        uint t48 = (768u + t47);
        vec2 sgm12 = vec2(s0[((t48 + 0u)) + sgmRow * (72u) + sgmColumn], s0[((t48 + 0u)) + sgmRow * (72u) + sgmColumn + 1u]);
        vec2 sgm13 = vec2(s0[((t48 + 8u)) + sgmRow * (72u) + sgmColumn], s0[((t48 + 8u)) + sgmRow * (72u) + sgmColumn + 1u]);
        for (uint sgm0k = 0u; sgm0k < 8u; ++sgm0k)
        {
            float sgm0l = s0[((t43 + 0u)) + sgmRow * (24u) + sgm0k];
            sgm0.x += sgm0l * s0[((t48 + 0u)) + sgm0k * (72u) + sgmColumn];
            sgm0.y += sgm0l * s0[((t48 + 0u)) + sgm0k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm1k = 0u; sgm1k < 8u; ++sgm1k)
        {
            float sgm1l = s0[((t43 + 0u)) + sgmRow * (24u) + sgm1k];
            sgm1.x += sgm1l * s0[((t48 + 8u)) + sgm1k * (72u) + sgmColumn];
            sgm1.y += sgm1l * s0[((t48 + 8u)) + sgm1k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm2k = 0u; sgm2k < 8u; ++sgm2k)
        {
            float sgm2l = s0[((t44 + 0u)) + sgmRow * (24u) + sgm2k];
            sgm2.x += sgm2l * s0[((t48 + 0u)) + sgm2k * (72u) + sgmColumn];
            sgm2.y += sgm2l * s0[((t48 + 0u)) + sgm2k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm3k = 0u; sgm3k < 8u; ++sgm3k)
        {
            float sgm3l = s0[((t44 + 0u)) + sgmRow * (24u) + sgm3k];
            sgm3.x += sgm3l * s0[((t48 + 8u)) + sgm3k * (72u) + sgmColumn];
            sgm3.y += sgm3l * s0[((t48 + 8u)) + sgm3k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm4k = 0u; sgm4k < 8u; ++sgm4k)
        {
            float sgm4l = s0[((t45 + 0u)) + sgmRow * (24u) + sgm4k];
            sgm4.x += sgm4l * s0[((t48 + 0u)) + sgm4k * (72u) + sgmColumn];
            sgm4.y += sgm4l * s0[((t48 + 0u)) + sgm4k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm5k = 0u; sgm5k < 8u; ++sgm5k)
        {
            float sgm5l = s0[((t45 + 0u)) + sgmRow * (24u) + sgm5k];
            sgm5.x += sgm5l * s0[((t48 + 8u)) + sgm5k * (72u) + sgmColumn];
            sgm5.y += sgm5l * s0[((t48 + 8u)) + sgm5k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm6k = 0u; sgm6k < 8u; ++sgm6k)
        {
            float sgm6l = s0[((t46 + 0u)) + sgmRow * (24u) + sgm6k];
            sgm6.x += sgm6l * s0[((t48 + 0u)) + sgm6k * (72u) + sgmColumn];
            sgm6.y += sgm6l * s0[((t48 + 0u)) + sgm6k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm7k = 0u; sgm7k < 8u; ++sgm7k)
        {
            float sgm7l = s0[((t46 + 0u)) + sgmRow * (24u) + sgm7k];
            sgm7.x += sgm7l * s0[((t48 + 8u)) + sgm7k * (72u) + sgmColumn];
            sgm7.y += sgm7l * s0[((t48 + 8u)) + sgm7k * (72u) + sgmColumn + 1u];
        }
        vec2 sgm14 = vec2(s0[((t43 + 8u)) + sgmRow * (24u) + sgmColumn], s0[((t43 + 8u)) + sgmRow * (24u) + sgmColumn + 1u]);
        vec2 sgm15 = vec2(s0[((t44 + 8u)) + sgmRow * (24u) + sgmColumn], s0[((t44 + 8u)) + sgmRow * (24u) + sgmColumn + 1u]);
        vec2 sgm16 = vec2(s0[((t45 + 8u)) + sgmRow * (24u) + sgmColumn], s0[((t45 + 8u)) + sgmRow * (24u) + sgmColumn + 1u]);
        vec2 sgm17 = vec2(s0[((t46 + 8u)) + sgmRow * (24u) + sgmColumn], s0[((t46 + 8u)) + sgmRow * (24u) + sgmColumn + 1u]);
        uint t49 = (1344u + t47);
        vec2 sgm18 = vec2(s0[((t49 + 0u)) + sgmRow * (72u) + sgmColumn], s0[((t49 + 0u)) + sgmRow * (72u) + sgmColumn + 1u]);
        vec2 sgm19 = vec2(s0[((t49 + 8u)) + sgmRow * (72u) + sgmColumn], s0[((t49 + 8u)) + sgmRow * (72u) + sgmColumn + 1u]);
        for (uint sgm0k = 0u; sgm0k < 8u; ++sgm0k)
        {
            float sgm0l = s0[((t43 + 8u)) + sgmRow * (24u) + sgm0k];
            sgm0.x += sgm0l * s0[((t49 + 0u)) + sgm0k * (72u) + sgmColumn];
            sgm0.y += sgm0l * s0[((t49 + 0u)) + sgm0k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm1k = 0u; sgm1k < 8u; ++sgm1k)
        {
            float sgm1l = s0[((t43 + 8u)) + sgmRow * (24u) + sgm1k];
            sgm1.x += sgm1l * s0[((t49 + 8u)) + sgm1k * (72u) + sgmColumn];
            sgm1.y += sgm1l * s0[((t49 + 8u)) + sgm1k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm2k = 0u; sgm2k < 8u; ++sgm2k)
        {
            float sgm2l = s0[((t44 + 8u)) + sgmRow * (24u) + sgm2k];
            sgm2.x += sgm2l * s0[((t49 + 0u)) + sgm2k * (72u) + sgmColumn];
            sgm2.y += sgm2l * s0[((t49 + 0u)) + sgm2k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm3k = 0u; sgm3k < 8u; ++sgm3k)
        {
            float sgm3l = s0[((t44 + 8u)) + sgmRow * (24u) + sgm3k];
            sgm3.x += sgm3l * s0[((t49 + 8u)) + sgm3k * (72u) + sgmColumn];
            sgm3.y += sgm3l * s0[((t49 + 8u)) + sgm3k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm4k = 0u; sgm4k < 8u; ++sgm4k)
        {
            float sgm4l = s0[((t45 + 8u)) + sgmRow * (24u) + sgm4k];
            sgm4.x += sgm4l * s0[((t49 + 0u)) + sgm4k * (72u) + sgmColumn];
            sgm4.y += sgm4l * s0[((t49 + 0u)) + sgm4k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm5k = 0u; sgm5k < 8u; ++sgm5k)
        {
            float sgm5l = s0[((t45 + 8u)) + sgmRow * (24u) + sgm5k];
            sgm5.x += sgm5l * s0[((t49 + 8u)) + sgm5k * (72u) + sgmColumn];
            sgm5.y += sgm5l * s0[((t49 + 8u)) + sgm5k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm6k = 0u; sgm6k < 8u; ++sgm6k)
        {
            float sgm6l = s0[((t46 + 8u)) + sgmRow * (24u) + sgm6k];
            sgm6.x += sgm6l * s0[((t49 + 0u)) + sgm6k * (72u) + sgmColumn];
            sgm6.y += sgm6l * s0[((t49 + 0u)) + sgm6k * (72u) + sgmColumn + 1u];
        }
        for (uint sgm7k = 0u; sgm7k < 8u; ++sgm7k)
        {
            float sgm7l = s0[((t46 + 8u)) + sgmRow * (24u) + sgm7k];
            sgm7.x += sgm7l * s0[((t49 + 8u)) + sgm7k * (72u) + sgmColumn];
            sgm7.y += sgm7l * s0[((t49 + 8u)) + sgm7k * (72u) + sgmColumn + 1u];
        }
        v8 = (v8 + 16u);
    }
    uint t50 = (((gl_LocalInvocationIndex / 32u) % 1u) * 32u);
    uint t51 = (t5 + t50);
    uint t52 = (t51 + 0u);
    uint t53 = (t52 + 8u);
    uint t54 = (((gl_LocalInvocationIndex / 32u) / 1u) * 16u);
    uint t55 = (t13 + t54);
    uint t56 = (t55 + 0u);
    uint t57 = (t56 + 8u);
    bool t58 = ((t53 <= uniforms.u0) && (t57 <= uniforms.u1));
    if (t58)
    {
        buffer2[(((t52 * uniforms.u1) + t56)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm0.x;
        buffer2[(((t52 * uniforms.u1) + t56)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm0.y;
    }
    else
    {
        s0[((1920u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm0.x;
        s0[((1920u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm0.y;
    }
    memoryBarrierShared();
    barrier();
    if ((!(t58)))
    {
        uint t59 = (lid.x % 32u);
        uint t60 = (t59 + 0u);
        uint t61 = (t52 + (t60 / 8u));
        uint t62 = (t56 + (t60 % 8u));
        if (((t61 < uniforms.u0) && (t62 < uniforms.u1)))
        {
            buffer2[((t61 * uniforms.u1) + t62)] = s0[((1920u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t60)];
        }
        uint t63 = (t59 + 32u);
        uint t64 = (t52 + (t63 / 8u));
        uint t65 = (t56 + (t63 % 8u));
        if (((t64 < uniforms.u0) && (t65 < uniforms.u1)))
        {
            buffer2[((t64 * uniforms.u1) + t65)] = s0[((1920u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t63)];
        }
    }
    memoryBarrierShared();
    barrier();
    uint t66 = (t55 + 8u);
    uint t67 = (t66 + 8u);
    bool t68 = ((t53 <= uniforms.u0) && (t67 <= uniforms.u1));
    if (t68)
    {
        buffer2[(((t52 * uniforms.u1) + t66)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm1.x;
        buffer2[(((t52 * uniforms.u1) + t66)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm1.y;
    }
    else
    {
        s0[((1920u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm1.x;
        s0[((1920u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm1.y;
    }
    memoryBarrierShared();
    barrier();
    if ((!(t68)))
    {
        uint t69 = (lid.x % 32u);
        uint t70 = (t69 + 0u);
        uint t71 = (t52 + (t70 / 8u));
        uint t72 = (t66 + (t70 % 8u));
        if (((t71 < uniforms.u0) && (t72 < uniforms.u1)))
        {
            buffer2[((t71 * uniforms.u1) + t72)] = s0[((1920u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t70)];
        }
        uint t73 = (t69 + 32u);
        uint t74 = (t52 + (t73 / 8u));
        uint t75 = (t66 + (t73 % 8u));
        if (((t74 < uniforms.u0) && (t75 < uniforms.u1)))
        {
            buffer2[((t74 * uniforms.u1) + t75)] = s0[((1920u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t73)];
        }
    }
    memoryBarrierShared();
    barrier();
    uint t76 = (t51 + 8u);
    uint t77 = (t76 + 8u);
    bool t78 = ((t77 <= uniforms.u0) && (t57 <= uniforms.u1));
    if (t78)
    {
        buffer2[(((t76 * uniforms.u1) + t56)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm2.x;
        buffer2[(((t76 * uniforms.u1) + t56)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm2.y;
    }
    else
    {
        s0[((1920u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm2.x;
        s0[((1920u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm2.y;
    }
    memoryBarrierShared();
    barrier();
    if ((!(t78)))
    {
        uint t79 = (lid.x % 32u);
        uint t80 = (t79 + 0u);
        uint t81 = (t76 + (t80 / 8u));
        uint t82 = (t56 + (t80 % 8u));
        if (((t81 < uniforms.u0) && (t82 < uniforms.u1)))
        {
            buffer2[((t81 * uniforms.u1) + t82)] = s0[((1920u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t80)];
        }
        uint t83 = (t79 + 32u);
        uint t84 = (t76 + (t83 / 8u));
        uint t85 = (t56 + (t83 % 8u));
        if (((t84 < uniforms.u0) && (t85 < uniforms.u1)))
        {
            buffer2[((t84 * uniforms.u1) + t85)] = s0[((1920u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t83)];
        }
    }
    memoryBarrierShared();
    barrier();
    bool t86 = ((t77 <= uniforms.u0) && (t67 <= uniforms.u1));
    if (t86)
    {
        buffer2[(((t76 * uniforms.u1) + t66)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm3.x;
        buffer2[(((t76 * uniforms.u1) + t66)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm3.y;
    }
    else
    {
        s0[((1920u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm3.x;
        s0[((1920u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm3.y;
    }
    memoryBarrierShared();
    barrier();
    if ((!(t86)))
    {
        uint t87 = (lid.x % 32u);
        uint t88 = (t87 + 0u);
        uint t89 = (t76 + (t88 / 8u));
        uint t90 = (t66 + (t88 % 8u));
        if (((t89 < uniforms.u0) && (t90 < uniforms.u1)))
        {
            buffer2[((t89 * uniforms.u1) + t90)] = s0[((1920u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t88)];
        }
        uint t91 = (t87 + 32u);
        uint t92 = (t76 + (t91 / 8u));
        uint t93 = (t66 + (t91 % 8u));
        if (((t92 < uniforms.u0) && (t93 < uniforms.u1)))
        {
            buffer2[((t92 * uniforms.u1) + t93)] = s0[((1920u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t91)];
        }
    }
    memoryBarrierShared();
    barrier();
    uint t94 = (t51 + 16u);
    uint t95 = (t94 + 8u);
    bool t96 = ((t95 <= uniforms.u0) && (t57 <= uniforms.u1));
    if (t96)
    {
        buffer2[(((t94 * uniforms.u1) + t56)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm4.x;
        buffer2[(((t94 * uniforms.u1) + t56)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm4.y;
    }
    else
    {
        s0[((1920u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm4.x;
        s0[((1920u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm4.y;
    }
    memoryBarrierShared();
    barrier();
    if ((!(t96)))
    {
        uint t97 = (lid.x % 32u);
        uint t98 = (t97 + 0u);
        uint t99 = (t94 + (t98 / 8u));
        uint t100 = (t56 + (t98 % 8u));
        if (((t99 < uniforms.u0) && (t100 < uniforms.u1)))
        {
            buffer2[((t99 * uniforms.u1) + t100)] = s0[((1920u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t98)];
        }
        uint t101 = (t97 + 32u);
        uint t102 = (t94 + (t101 / 8u));
        uint t103 = (t56 + (t101 % 8u));
        if (((t102 < uniforms.u0) && (t103 < uniforms.u1)))
        {
            buffer2[((t102 * uniforms.u1) + t103)] = s0[((1920u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t101)];
        }
    }
    memoryBarrierShared();
    barrier();
    bool t104 = ((t95 <= uniforms.u0) && (t67 <= uniforms.u1));
    if (t104)
    {
        buffer2[(((t94 * uniforms.u1) + t66)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm5.x;
        buffer2[(((t94 * uniforms.u1) + t66)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm5.y;
    }
    else
    {
        s0[((1920u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm5.x;
        s0[((1920u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm5.y;
    }
    memoryBarrierShared();
    barrier();
    if ((!(t104)))
    {
        uint t105 = (lid.x % 32u);
        uint t106 = (t105 + 0u);
        uint t107 = (t94 + (t106 / 8u));
        uint t108 = (t66 + (t106 % 8u));
        if (((t107 < uniforms.u0) && (t108 < uniforms.u1)))
        {
            buffer2[((t107 * uniforms.u1) + t108)] = s0[((1920u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t106)];
        }
        uint t109 = (t105 + 32u);
        uint t110 = (t94 + (t109 / 8u));
        uint t111 = (t66 + (t109 % 8u));
        if (((t110 < uniforms.u0) && (t111 < uniforms.u1)))
        {
            buffer2[((t110 * uniforms.u1) + t111)] = s0[((1920u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t109)];
        }
    }
    memoryBarrierShared();
    barrier();
    uint t112 = (t51 + 24u);
    uint t113 = (t112 + 8u);
    bool t114 = ((t113 <= uniforms.u0) && (t57 <= uniforms.u1));
    if (t114)
    {
        buffer2[(((t112 * uniforms.u1) + t56)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm6.x;
        buffer2[(((t112 * uniforms.u1) + t56)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm6.y;
    }
    else
    {
        s0[((1920u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm6.x;
        s0[((1920u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm6.y;
    }
    memoryBarrierShared();
    barrier();
    if ((!(t114)))
    {
        uint t115 = (lid.x % 32u);
        uint t116 = (t115 + 0u);
        uint t117 = (t112 + (t116 / 8u));
        uint t118 = (t56 + (t116 % 8u));
        if (((t117 < uniforms.u0) && (t118 < uniforms.u1)))
        {
            buffer2[((t117 * uniforms.u1) + t118)] = s0[((1920u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t116)];
        }
        uint t119 = (t115 + 32u);
        uint t120 = (t112 + (t119 / 8u));
        uint t121 = (t56 + (t119 % 8u));
        if (((t120 < uniforms.u0) && (t121 < uniforms.u1)))
        {
            buffer2[((t120 * uniforms.u1) + t121)] = s0[((1920u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t119)];
        }
    }
    memoryBarrierShared();
    barrier();
    bool t122 = ((t113 <= uniforms.u0) && (t67 <= uniforms.u1));
    if (t122)
    {
        buffer2[(((t112 * uniforms.u1) + t66)) + sgmRow * (uniforms.u1) + sgmColumn] = sgm7.x;
        buffer2[(((t112 * uniforms.u1) + t66)) + sgmRow * (uniforms.u1) + sgmColumn + 1u] = sgm7.y;
    }
    else
    {
        s0[((1920u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn] = sgm7.x;
        s0[((1920u + ((gl_LocalInvocationIndex / 32u) * 64u))) + sgmRow * (8u) + sgmColumn + 1u] = sgm7.y;
    }
    memoryBarrierShared();
    barrier();
    if ((!(t122)))
    {
        uint t123 = (lid.x % 32u);
        uint t124 = (t123 + 0u);
        uint t125 = (t112 + (t124 / 8u));
        uint t126 = (t66 + (t124 % 8u));
        if (((t125 < uniforms.u0) && (t126 < uniforms.u1)))
        {
            buffer2[((t125 * uniforms.u1) + t126)] = s0[((1920u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t124)];
        }
        uint t127 = (t123 + 32u);
        uint t128 = (t112 + (t127 / 8u));
        uint t129 = (t66 + (t127 % 8u));
        if (((t128 < uniforms.u0) && (t129 < uniforms.u1)))
        {
            buffer2[((t128 * uniforms.u1) + t129)] = s0[((1920u + ((gl_LocalInvocationIndex / 32u) * 64u)) + t127)];
        }
    }
    memoryBarrierShared();
    barrier();
}
