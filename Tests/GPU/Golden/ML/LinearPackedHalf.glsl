#version 450

float eacpReadHalf(uint bits, uint parity)
{
    return unpackHalf2x16(bits >> (16u * parity)).x;
}

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

shared float s0[4096];

layout(local_size_x = 256, local_size_y = 1, local_size_z = 1) in;

void main()
{
    uvec2 gid = gl_GlobalInvocationID.xy;
    uvec2 lid = gl_LocalInvocationID.xy;
    uvec2 tgid = gl_WorkGroupID.xy;
    uint sgmLane = gl_LocalInvocationIndex % 32u;
    uint sgmRow = sgmLane / 4u;
    uint sgmColumn = (sgmLane % 4u) * 2u;
    uint sgmBase = (gl_LocalInvocationIndex / 32u) * 128u;
    vec2 sgm0 = vec2(0.0, 0.0);
    vec2 sgm1 = vec2(0.0, 0.0);
    vec2 sgm2 = vec2(0.0, 0.0);
    vec2 sgm3 = vec2(0.0, 0.0);
    vec2 sgm4 = vec2(0.0, 0.0);
    vec2 sgm5 = vec2(0.0, 0.0);
    vec2 sgm6 = vec2(0.0, 0.0);
    vec2 sgm7 = vec2(0.0, 0.0);
    uint v0 = 0u;
    while ((v0 < uniforms.u2))
    {
        uint t0 = (lid.x / 4u);
        uint t1 = ((lid.x % 4u) * 8u);
        uint t2 = ((t0 * 32u) + t1);
        uint t3 = ((v0 + t1) + 0u);
        bool t4 = (t3 < uniforms.u2);
        uint t5 = (min(((tgid.y * 64u) + t0), (uniforms.u0 - 1u)) * uniforms.u2);
        uint t6 = (uniforms.u2 - 1u);
        uint t7 = min(t3, t6);
        s0[(t2 + 0u)] = (t4 ? buffer0[(t5 + t7)] : 0.0);
        uint t8 = (min(((tgid.x * 64u) + t0), (uniforms.u1 - 1u)) * uniforms.u2);
        uint t9 = (t8 + t7);
        s0[((2048u + ((t1 + 0u) * 64u)) + t0)] = (t4 ? eacpReadHalf(floatBitsToUint(buffer1[(t9 / 2u)]), (t9 % 2u)) : 0.0);
        uint t10 = ((v0 + t1) + 1u);
        bool t11 = (t10 < uniforms.u2);
        uint t12 = min(t10, t6);
        s0[(t2 + 1u)] = (t11 ? buffer0[(t5 + t12)] : 0.0);
        uint t13 = (t8 + t12);
        s0[((2048u + ((t1 + 1u) * 64u)) + t0)] = (t11 ? eacpReadHalf(floatBitsToUint(buffer1[(t13 / 2u)]), (t13 % 2u)) : 0.0);
        uint t14 = ((v0 + t1) + 2u);
        bool t15 = (t14 < uniforms.u2);
        uint t16 = min(t14, t6);
        s0[(t2 + 2u)] = (t15 ? buffer0[(t5 + t16)] : 0.0);
        uint t17 = (t8 + t16);
        s0[((2048u + ((t1 + 2u) * 64u)) + t0)] = (t15 ? eacpReadHalf(floatBitsToUint(buffer1[(t17 / 2u)]), (t17 % 2u)) : 0.0);
        uint t18 = ((v0 + t1) + 3u);
        bool t19 = (t18 < uniforms.u2);
        uint t20 = min(t18, t6);
        s0[(t2 + 3u)] = (t19 ? buffer0[(t5 + t20)] : 0.0);
        uint t21 = (t8 + t20);
        s0[((2048u + ((t1 + 3u) * 64u)) + t0)] = (t19 ? eacpReadHalf(floatBitsToUint(buffer1[(t21 / 2u)]), (t21 % 2u)) : 0.0);
        uint t22 = ((v0 + t1) + 4u);
        bool t23 = (t22 < uniforms.u2);
        uint t24 = min(t22, t6);
        s0[(t2 + 4u)] = (t23 ? buffer0[(t5 + t24)] : 0.0);
        uint t25 = (t8 + t24);
        s0[((2048u + ((t1 + 4u) * 64u)) + t0)] = (t23 ? eacpReadHalf(floatBitsToUint(buffer1[(t25 / 2u)]), (t25 % 2u)) : 0.0);
        uint t26 = ((v0 + t1) + 5u);
        bool t27 = (t26 < uniforms.u2);
        uint t28 = min(t26, t6);
        s0[(t2 + 5u)] = (t27 ? buffer0[(t5 + t28)] : 0.0);
        uint t29 = (t8 + t28);
        s0[((2048u + ((t1 + 5u) * 64u)) + t0)] = (t27 ? eacpReadHalf(floatBitsToUint(buffer1[(t29 / 2u)]), (t29 % 2u)) : 0.0);
        uint t30 = ((v0 + t1) + 6u);
        bool t31 = (t30 < uniforms.u2);
        uint t32 = min(t30, t6);
        s0[(t2 + 6u)] = (t31 ? buffer0[(t5 + t32)] : 0.0);
        uint t33 = (t8 + t32);
        s0[((2048u + ((t1 + 6u) * 64u)) + t0)] = (t31 ? eacpReadHalf(floatBitsToUint(buffer1[(t33 / 2u)]), (t33 % 2u)) : 0.0);
        uint t34 = ((v0 + t1) + 7u);
        bool t35 = (t34 < uniforms.u2);
        uint t36 = min(t34, t6);
        s0[(t2 + 7u)] = (t35 ? buffer0[(t5 + t36)] : 0.0);
        uint t37 = (t8 + t36);
        s0[((2048u + ((t1 + 7u) * 64u)) + t0)] = (t35 ? eacpReadHalf(floatBitsToUint(buffer1[(t37 / 2u)]), (t37 % 2u)) : 0.0);
        memoryBarrierShared();
        barrier();
        uint t38 = (((gl_LocalInvocationIndex / 32u) % 2u) * 32u);
        uint t39 = ((t38 + 0u) * 32u);
        vec2 sgm8 = vec2(s0[((t39 + 0u)) + sgmRow * (32u) + sgmColumn], s0[((t39 + 0u)) + sgmRow * (32u) + sgmColumn + 1u]);
        uint t40 = ((t38 + 8u) * 32u);
        vec2 sgm9 = vec2(s0[((t40 + 0u)) + sgmRow * (32u) + sgmColumn], s0[((t40 + 0u)) + sgmRow * (32u) + sgmColumn + 1u]);
        uint t41 = ((t38 + 16u) * 32u);
        vec2 sgm10 = vec2(s0[((t41 + 0u)) + sgmRow * (32u) + sgmColumn], s0[((t41 + 0u)) + sgmRow * (32u) + sgmColumn + 1u]);
        uint t42 = ((t38 + 24u) * 32u);
        vec2 sgm11 = vec2(s0[((t42 + 0u)) + sgmRow * (32u) + sgmColumn], s0[((t42 + 0u)) + sgmRow * (32u) + sgmColumn + 1u]);
        uint t43 = (((gl_LocalInvocationIndex / 32u) / 2u) * 16u);
        uint t44 = (2048u + t43);
        vec2 sgm12 = vec2(s0[((t44 + 0u)) + sgmRow * (64u) + sgmColumn], s0[((t44 + 0u)) + sgmRow * (64u) + sgmColumn + 1u]);
        vec2 sgm13 = vec2(s0[((t44 + 8u)) + sgmRow * (64u) + sgmColumn], s0[((t44 + 8u)) + sgmRow * (64u) + sgmColumn + 1u]);
        for (uint sgm0k = 0u; sgm0k < 8u; ++sgm0k)
        {
            float sgm0l = s0[((t39 + 0u)) + sgmRow * (32u) + sgm0k];
            sgm0.x += sgm0l * s0[((t44 + 0u)) + sgm0k * (64u) + sgmColumn];
            sgm0.y += sgm0l * s0[((t44 + 0u)) + sgm0k * (64u) + sgmColumn + 1u];
        }
        for (uint sgm1k = 0u; sgm1k < 8u; ++sgm1k)
        {
            float sgm1l = s0[((t39 + 0u)) + sgmRow * (32u) + sgm1k];
            sgm1.x += sgm1l * s0[((t44 + 8u)) + sgm1k * (64u) + sgmColumn];
            sgm1.y += sgm1l * s0[((t44 + 8u)) + sgm1k * (64u) + sgmColumn + 1u];
        }
        for (uint sgm2k = 0u; sgm2k < 8u; ++sgm2k)
        {
            float sgm2l = s0[((t40 + 0u)) + sgmRow * (32u) + sgm2k];
            sgm2.x += sgm2l * s0[((t44 + 0u)) + sgm2k * (64u) + sgmColumn];
            sgm2.y += sgm2l * s0[((t44 + 0u)) + sgm2k * (64u) + sgmColumn + 1u];
        }
        for (uint sgm3k = 0u; sgm3k < 8u; ++sgm3k)
        {
            float sgm3l = s0[((t40 + 0u)) + sgmRow * (32u) + sgm3k];
            sgm3.x += sgm3l * s0[((t44 + 8u)) + sgm3k * (64u) + sgmColumn];
            sgm3.y += sgm3l * s0[((t44 + 8u)) + sgm3k * (64u) + sgmColumn + 1u];
        }
        for (uint sgm4k = 0u; sgm4k < 8u; ++sgm4k)
        {
            float sgm4l = s0[((t41 + 0u)) + sgmRow * (32u) + sgm4k];
            sgm4.x += sgm4l * s0[((t44 + 0u)) + sgm4k * (64u) + sgmColumn];
            sgm4.y += sgm4l * s0[((t44 + 0u)) + sgm4k * (64u) + sgmColumn + 1u];
        }
        for (uint sgm5k = 0u; sgm5k < 8u; ++sgm5k)
        {
            float sgm5l = s0[((t41 + 0u)) + sgmRow * (32u) + sgm5k];
            sgm5.x += sgm5l * s0[((t44 + 8u)) + sgm5k * (64u) + sgmColumn];
            sgm5.y += sgm5l * s0[((t44 + 8u)) + sgm5k * (64u) + sgmColumn + 1u];
        }
        for (uint sgm6k = 0u; sgm6k < 8u; ++sgm6k)
        {
            float sgm6l = s0[((t42 + 0u)) + sgmRow * (32u) + sgm6k];
            sgm6.x += sgm6l * s0[((t44 + 0u)) + sgm6k * (64u) + sgmColumn];
            sgm6.y += sgm6l * s0[((t44 + 0u)) + sgm6k * (64u) + sgmColumn + 1u];
        }
        for (uint sgm7k = 0u; sgm7k < 8u; ++sgm7k)
        {
            float sgm7l = s0[((t42 + 0u)) + sgmRow * (32u) + sgm7k];
            sgm7.x += sgm7l * s0[((t44 + 8u)) + sgm7k * (64u) + sgmColumn];
            sgm7.y += sgm7l * s0[((t44 + 8u)) + sgm7k * (64u) + sgmColumn + 1u];
        }
        vec2 sgm14 = vec2(s0[((t39 + 8u)) + sgmRow * (32u) + sgmColumn], s0[((t39 + 8u)) + sgmRow * (32u) + sgmColumn + 1u]);
        vec2 sgm15 = vec2(s0[((t40 + 8u)) + sgmRow * (32u) + sgmColumn], s0[((t40 + 8u)) + sgmRow * (32u) + sgmColumn + 1u]);
        vec2 sgm16 = vec2(s0[((t41 + 8u)) + sgmRow * (32u) + sgmColumn], s0[((t41 + 8u)) + sgmRow * (32u) + sgmColumn + 1u]);
        vec2 sgm17 = vec2(s0[((t42 + 8u)) + sgmRow * (32u) + sgmColumn], s0[((t42 + 8u)) + sgmRow * (32u) + sgmColumn + 1u]);
        uint t45 = (2560u + t43);
        vec2 sgm18 = vec2(s0[((t45 + 0u)) + sgmRow * (64u) + sgmColumn], s0[((t45 + 0u)) + sgmRow * (64u) + sgmColumn + 1u]);
        vec2 sgm19 = vec2(s0[((t45 + 8u)) + sgmRow * (64u) + sgmColumn], s0[((t45 + 8u)) + sgmRow * (64u) + sgmColumn + 1u]);
        for (uint sgm0k = 0u; sgm0k < 8u; ++sgm0k)
        {
            float sgm0l = s0[((t39 + 8u)) + sgmRow * (32u) + sgm0k];
            sgm0.x += sgm0l * s0[((t45 + 0u)) + sgm0k * (64u) + sgmColumn];
            sgm0.y += sgm0l * s0[((t45 + 0u)) + sgm0k * (64u) + sgmColumn + 1u];
        }
        for (uint sgm1k = 0u; sgm1k < 8u; ++sgm1k)
        {
            float sgm1l = s0[((t39 + 8u)) + sgmRow * (32u) + sgm1k];
            sgm1.x += sgm1l * s0[((t45 + 8u)) + sgm1k * (64u) + sgmColumn];
            sgm1.y += sgm1l * s0[((t45 + 8u)) + sgm1k * (64u) + sgmColumn + 1u];
        }
        for (uint sgm2k = 0u; sgm2k < 8u; ++sgm2k)
        {
            float sgm2l = s0[((t40 + 8u)) + sgmRow * (32u) + sgm2k];
            sgm2.x += sgm2l * s0[((t45 + 0u)) + sgm2k * (64u) + sgmColumn];
            sgm2.y += sgm2l * s0[((t45 + 0u)) + sgm2k * (64u) + sgmColumn + 1u];
        }
        for (uint sgm3k = 0u; sgm3k < 8u; ++sgm3k)
        {
            float sgm3l = s0[((t40 + 8u)) + sgmRow * (32u) + sgm3k];
            sgm3.x += sgm3l * s0[((t45 + 8u)) + sgm3k * (64u) + sgmColumn];
            sgm3.y += sgm3l * s0[((t45 + 8u)) + sgm3k * (64u) + sgmColumn + 1u];
        }
        for (uint sgm4k = 0u; sgm4k < 8u; ++sgm4k)
        {
            float sgm4l = s0[((t41 + 8u)) + sgmRow * (32u) + sgm4k];
            sgm4.x += sgm4l * s0[((t45 + 0u)) + sgm4k * (64u) + sgmColumn];
            sgm4.y += sgm4l * s0[((t45 + 0u)) + sgm4k * (64u) + sgmColumn + 1u];
        }
        for (uint sgm5k = 0u; sgm5k < 8u; ++sgm5k)
        {
            float sgm5l = s0[((t41 + 8u)) + sgmRow * (32u) + sgm5k];
            sgm5.x += sgm5l * s0[((t45 + 8u)) + sgm5k * (64u) + sgmColumn];
            sgm5.y += sgm5l * s0[((t45 + 8u)) + sgm5k * (64u) + sgmColumn + 1u];
        }
        for (uint sgm6k = 0u; sgm6k < 8u; ++sgm6k)
        {
            float sgm6l = s0[((t42 + 8u)) + sgmRow * (32u) + sgm6k];
            sgm6.x += sgm6l * s0[((t45 + 0u)) + sgm6k * (64u) + sgmColumn];
            sgm6.y += sgm6l * s0[((t45 + 0u)) + sgm6k * (64u) + sgmColumn + 1u];
        }
        for (uint sgm7k = 0u; sgm7k < 8u; ++sgm7k)
        {
            float sgm7l = s0[((t42 + 8u)) + sgmRow * (32u) + sgm7k];
            sgm7.x += sgm7l * s0[((t45 + 8u)) + sgm7k * (64u) + sgmColumn];
            sgm7.y += sgm7l * s0[((t45 + 8u)) + sgm7k * (64u) + sgmColumn + 1u];
        }
        vec2 sgm20 = vec2(s0[((t39 + 16u)) + sgmRow * (32u) + sgmColumn], s0[((t39 + 16u)) + sgmRow * (32u) + sgmColumn + 1u]);
        vec2 sgm21 = vec2(s0[((t40 + 16u)) + sgmRow * (32u) + sgmColumn], s0[((t40 + 16u)) + sgmRow * (32u) + sgmColumn + 1u]);
        vec2 sgm22 = vec2(s0[((t41 + 16u)) + sgmRow * (32u) + sgmColumn], s0[((t41 + 16u)) + sgmRow * (32u) + sgmColumn + 1u]);
        vec2 sgm23 = vec2(s0[((t42 + 16u)) + sgmRow * (32u) + sgmColumn], s0[((t42 + 16u)) + sgmRow * (32u) + sgmColumn + 1u]);
        uint t46 = (3072u + t43);
        vec2 sgm24 = vec2(s0[((t46 + 0u)) + sgmRow * (64u) + sgmColumn], s0[((t46 + 0u)) + sgmRow * (64u) + sgmColumn + 1u]);
        vec2 sgm25 = vec2(s0[((t46 + 8u)) + sgmRow * (64u) + sgmColumn], s0[((t46 + 8u)) + sgmRow * (64u) + sgmColumn + 1u]);
        for (uint sgm0k = 0u; sgm0k < 8u; ++sgm0k)
        {
            float sgm0l = s0[((t39 + 16u)) + sgmRow * (32u) + sgm0k];
            sgm0.x += sgm0l * s0[((t46 + 0u)) + sgm0k * (64u) + sgmColumn];
            sgm0.y += sgm0l * s0[((t46 + 0u)) + sgm0k * (64u) + sgmColumn + 1u];
        }
        for (uint sgm1k = 0u; sgm1k < 8u; ++sgm1k)
        {
            float sgm1l = s0[((t39 + 16u)) + sgmRow * (32u) + sgm1k];
            sgm1.x += sgm1l * s0[((t46 + 8u)) + sgm1k * (64u) + sgmColumn];
            sgm1.y += sgm1l * s0[((t46 + 8u)) + sgm1k * (64u) + sgmColumn + 1u];
        }
        for (uint sgm2k = 0u; sgm2k < 8u; ++sgm2k)
        {
            float sgm2l = s0[((t40 + 16u)) + sgmRow * (32u) + sgm2k];
            sgm2.x += sgm2l * s0[((t46 + 0u)) + sgm2k * (64u) + sgmColumn];
            sgm2.y += sgm2l * s0[((t46 + 0u)) + sgm2k * (64u) + sgmColumn + 1u];
        }
        for (uint sgm3k = 0u; sgm3k < 8u; ++sgm3k)
        {
            float sgm3l = s0[((t40 + 16u)) + sgmRow * (32u) + sgm3k];
            sgm3.x += sgm3l * s0[((t46 + 8u)) + sgm3k * (64u) + sgmColumn];
            sgm3.y += sgm3l * s0[((t46 + 8u)) + sgm3k * (64u) + sgmColumn + 1u];
        }
        for (uint sgm4k = 0u; sgm4k < 8u; ++sgm4k)
        {
            float sgm4l = s0[((t41 + 16u)) + sgmRow * (32u) + sgm4k];
            sgm4.x += sgm4l * s0[((t46 + 0u)) + sgm4k * (64u) + sgmColumn];
            sgm4.y += sgm4l * s0[((t46 + 0u)) + sgm4k * (64u) + sgmColumn + 1u];
        }
        for (uint sgm5k = 0u; sgm5k < 8u; ++sgm5k)
        {
            float sgm5l = s0[((t41 + 16u)) + sgmRow * (32u) + sgm5k];
            sgm5.x += sgm5l * s0[((t46 + 8u)) + sgm5k * (64u) + sgmColumn];
            sgm5.y += sgm5l * s0[((t46 + 8u)) + sgm5k * (64u) + sgmColumn + 1u];
        }
        for (uint sgm6k = 0u; sgm6k < 8u; ++sgm6k)
        {
            float sgm6l = s0[((t42 + 16u)) + sgmRow * (32u) + sgm6k];
            sgm6.x += sgm6l * s0[((t46 + 0u)) + sgm6k * (64u) + sgmColumn];
            sgm6.y += sgm6l * s0[((t46 + 0u)) + sgm6k * (64u) + sgmColumn + 1u];
        }
        for (uint sgm7k = 0u; sgm7k < 8u; ++sgm7k)
        {
            float sgm7l = s0[((t42 + 16u)) + sgmRow * (32u) + sgm7k];
            sgm7.x += sgm7l * s0[((t46 + 8u)) + sgm7k * (64u) + sgmColumn];
            sgm7.y += sgm7l * s0[((t46 + 8u)) + sgm7k * (64u) + sgmColumn + 1u];
        }
        vec2 sgm26 = vec2(s0[((t39 + 24u)) + sgmRow * (32u) + sgmColumn], s0[((t39 + 24u)) + sgmRow * (32u) + sgmColumn + 1u]);
        vec2 sgm27 = vec2(s0[((t40 + 24u)) + sgmRow * (32u) + sgmColumn], s0[((t40 + 24u)) + sgmRow * (32u) + sgmColumn + 1u]);
        vec2 sgm28 = vec2(s0[((t41 + 24u)) + sgmRow * (32u) + sgmColumn], s0[((t41 + 24u)) + sgmRow * (32u) + sgmColumn + 1u]);
        vec2 sgm29 = vec2(s0[((t42 + 24u)) + sgmRow * (32u) + sgmColumn], s0[((t42 + 24u)) + sgmRow * (32u) + sgmColumn + 1u]);
        uint t47 = (3584u + t43);
        vec2 sgm30 = vec2(s0[((t47 + 0u)) + sgmRow * (64u) + sgmColumn], s0[((t47 + 0u)) + sgmRow * (64u) + sgmColumn + 1u]);
        vec2 sgm31 = vec2(s0[((t47 + 8u)) + sgmRow * (64u) + sgmColumn], s0[((t47 + 8u)) + sgmRow * (64u) + sgmColumn + 1u]);
        for (uint sgm0k = 0u; sgm0k < 8u; ++sgm0k)
        {
            float sgm0l = s0[((t39 + 24u)) + sgmRow * (32u) + sgm0k];
            sgm0.x += sgm0l * s0[((t47 + 0u)) + sgm0k * (64u) + sgmColumn];
            sgm0.y += sgm0l * s0[((t47 + 0u)) + sgm0k * (64u) + sgmColumn + 1u];
        }
        for (uint sgm1k = 0u; sgm1k < 8u; ++sgm1k)
        {
            float sgm1l = s0[((t39 + 24u)) + sgmRow * (32u) + sgm1k];
            sgm1.x += sgm1l * s0[((t47 + 8u)) + sgm1k * (64u) + sgmColumn];
            sgm1.y += sgm1l * s0[((t47 + 8u)) + sgm1k * (64u) + sgmColumn + 1u];
        }
        for (uint sgm2k = 0u; sgm2k < 8u; ++sgm2k)
        {
            float sgm2l = s0[((t40 + 24u)) + sgmRow * (32u) + sgm2k];
            sgm2.x += sgm2l * s0[((t47 + 0u)) + sgm2k * (64u) + sgmColumn];
            sgm2.y += sgm2l * s0[((t47 + 0u)) + sgm2k * (64u) + sgmColumn + 1u];
        }
        for (uint sgm3k = 0u; sgm3k < 8u; ++sgm3k)
        {
            float sgm3l = s0[((t40 + 24u)) + sgmRow * (32u) + sgm3k];
            sgm3.x += sgm3l * s0[((t47 + 8u)) + sgm3k * (64u) + sgmColumn];
            sgm3.y += sgm3l * s0[((t47 + 8u)) + sgm3k * (64u) + sgmColumn + 1u];
        }
        for (uint sgm4k = 0u; sgm4k < 8u; ++sgm4k)
        {
            float sgm4l = s0[((t41 + 24u)) + sgmRow * (32u) + sgm4k];
            sgm4.x += sgm4l * s0[((t47 + 0u)) + sgm4k * (64u) + sgmColumn];
            sgm4.y += sgm4l * s0[((t47 + 0u)) + sgm4k * (64u) + sgmColumn + 1u];
        }
        for (uint sgm5k = 0u; sgm5k < 8u; ++sgm5k)
        {
            float sgm5l = s0[((t41 + 24u)) + sgmRow * (32u) + sgm5k];
            sgm5.x += sgm5l * s0[((t47 + 8u)) + sgm5k * (64u) + sgmColumn];
            sgm5.y += sgm5l * s0[((t47 + 8u)) + sgm5k * (64u) + sgmColumn + 1u];
        }
        for (uint sgm6k = 0u; sgm6k < 8u; ++sgm6k)
        {
            float sgm6l = s0[((t42 + 24u)) + sgmRow * (32u) + sgm6k];
            sgm6.x += sgm6l * s0[((t47 + 0u)) + sgm6k * (64u) + sgmColumn];
            sgm6.y += sgm6l * s0[((t47 + 0u)) + sgm6k * (64u) + sgmColumn + 1u];
        }
        for (uint sgm7k = 0u; sgm7k < 8u; ++sgm7k)
        {
            float sgm7l = s0[((t42 + 24u)) + sgmRow * (32u) + sgm7k];
            sgm7.x += sgm7l * s0[((t47 + 8u)) + sgm7k * (64u) + sgmColumn];
            sgm7.y += sgm7l * s0[((t47 + 8u)) + sgm7k * (64u) + sgmColumn + 1u];
        }
        memoryBarrierShared();
        barrier();
        v0 = (v0 + 32u);
    }
    memoryBarrierShared();
    barrier();
    uint t48 = (((gl_LocalInvocationIndex / 32u) % 2u) * 32u);
    uint t49 = (t48 + 0u);
    uint t50 = (((gl_LocalInvocationIndex / 32u) / 2u) * 16u);
    uint t51 = ((t49 * 64u) + t50);
    s0[((t51 + 0u)) + sgmRow * (64u) + sgmColumn] = sgm0.x;
    s0[((t51 + 0u)) + sgmRow * (64u) + sgmColumn + 1u] = sgm0.y;
    s0[((t51 + 8u)) + sgmRow * (64u) + sgmColumn] = sgm1.x;
    s0[((t51 + 8u)) + sgmRow * (64u) + sgmColumn + 1u] = sgm1.y;
    uint t52 = (t48 + 8u);
    uint t53 = ((t52 * 64u) + t50);
    s0[((t53 + 0u)) + sgmRow * (64u) + sgmColumn] = sgm2.x;
    s0[((t53 + 0u)) + sgmRow * (64u) + sgmColumn + 1u] = sgm2.y;
    s0[((t53 + 8u)) + sgmRow * (64u) + sgmColumn] = sgm3.x;
    s0[((t53 + 8u)) + sgmRow * (64u) + sgmColumn + 1u] = sgm3.y;
    uint t54 = (t48 + 16u);
    uint t55 = ((t54 * 64u) + t50);
    s0[((t55 + 0u)) + sgmRow * (64u) + sgmColumn] = sgm4.x;
    s0[((t55 + 0u)) + sgmRow * (64u) + sgmColumn + 1u] = sgm4.y;
    s0[((t55 + 8u)) + sgmRow * (64u) + sgmColumn] = sgm5.x;
    s0[((t55 + 8u)) + sgmRow * (64u) + sgmColumn + 1u] = sgm5.y;
    uint t56 = (t48 + 24u);
    uint t57 = ((t56 * 64u) + t50);
    s0[((t57 + 0u)) + sgmRow * (64u) + sgmColumn] = sgm6.x;
    s0[((t57 + 0u)) + sgmRow * (64u) + sgmColumn + 1u] = sgm6.y;
    s0[((t57 + 8u)) + sgmRow * (64u) + sgmColumn] = sgm7.x;
    s0[((t57 + 8u)) + sgmRow * (64u) + sgmColumn + 1u] = sgm7.y;
    memoryBarrierShared();
    barrier();
    uint t58 = (tgid.y * 64u);
    uint t59 = (lid.x + 0u);
    uint t60 = (t58 + (t59 / 64u));
    uint t61 = (tgid.x * 64u);
    uint t62 = (t61 + (t59 % 64u));
    if (((t60 < uniforms.u0) && (t62 < uniforms.u1)))
    {
        buffer2[((t60 * uniforms.u1) + t62)] = s0[t59];
    }
    uint t63 = (lid.x + 256u);
    uint t64 = (t58 + (t63 / 64u));
    uint t65 = (t61 + (t63 % 64u));
    if (((t64 < uniforms.u0) && (t65 < uniforms.u1)))
    {
        buffer2[((t64 * uniforms.u1) + t65)] = s0[t63];
    }
    uint t66 = (lid.x + 512u);
    uint t67 = (t58 + (t66 / 64u));
    uint t68 = (t61 + (t66 % 64u));
    if (((t67 < uniforms.u0) && (t68 < uniforms.u1)))
    {
        buffer2[((t67 * uniforms.u1) + t68)] = s0[t66];
    }
    uint t69 = (lid.x + 768u);
    uint t70 = (t58 + (t69 / 64u));
    uint t71 = (t61 + (t69 % 64u));
    if (((t70 < uniforms.u0) && (t71 < uniforms.u1)))
    {
        buffer2[((t70 * uniforms.u1) + t71)] = s0[t69];
    }
    uint t72 = (lid.x + 1024u);
    uint t73 = (t58 + (t72 / 64u));
    uint t74 = (t61 + (t72 % 64u));
    if (((t73 < uniforms.u0) && (t74 < uniforms.u1)))
    {
        buffer2[((t73 * uniforms.u1) + t74)] = s0[t72];
    }
    uint t75 = (lid.x + 1280u);
    uint t76 = (t58 + (t75 / 64u));
    uint t77 = (t61 + (t75 % 64u));
    if (((t76 < uniforms.u0) && (t77 < uniforms.u1)))
    {
        buffer2[((t76 * uniforms.u1) + t77)] = s0[t75];
    }
    uint t78 = (lid.x + 1536u);
    uint t79 = (t58 + (t78 / 64u));
    uint t80 = (t61 + (t78 % 64u));
    if (((t79 < uniforms.u0) && (t80 < uniforms.u1)))
    {
        buffer2[((t79 * uniforms.u1) + t80)] = s0[t78];
    }
    uint t81 = (lid.x + 1792u);
    uint t82 = (t58 + (t81 / 64u));
    uint t83 = (t61 + (t81 % 64u));
    if (((t82 < uniforms.u0) && (t83 < uniforms.u1)))
    {
        buffer2[((t82 * uniforms.u1) + t83)] = s0[t81];
    }
    uint t84 = (lid.x + 2048u);
    uint t85 = (t58 + (t84 / 64u));
    uint t86 = (t61 + (t84 % 64u));
    if (((t85 < uniforms.u0) && (t86 < uniforms.u1)))
    {
        buffer2[((t85 * uniforms.u1) + t86)] = s0[t84];
    }
    uint t87 = (lid.x + 2304u);
    uint t88 = (t58 + (t87 / 64u));
    uint t89 = (t61 + (t87 % 64u));
    if (((t88 < uniforms.u0) && (t89 < uniforms.u1)))
    {
        buffer2[((t88 * uniforms.u1) + t89)] = s0[t87];
    }
    uint t90 = (lid.x + 2560u);
    uint t91 = (t58 + (t90 / 64u));
    uint t92 = (t61 + (t90 % 64u));
    if (((t91 < uniforms.u0) && (t92 < uniforms.u1)))
    {
        buffer2[((t91 * uniforms.u1) + t92)] = s0[t90];
    }
    uint t93 = (lid.x + 2816u);
    uint t94 = (t58 + (t93 / 64u));
    uint t95 = (t61 + (t93 % 64u));
    if (((t94 < uniforms.u0) && (t95 < uniforms.u1)))
    {
        buffer2[((t94 * uniforms.u1) + t95)] = s0[t93];
    }
    uint t96 = (lid.x + 3072u);
    uint t97 = (t58 + (t96 / 64u));
    uint t98 = (t61 + (t96 % 64u));
    if (((t97 < uniforms.u0) && (t98 < uniforms.u1)))
    {
        buffer2[((t97 * uniforms.u1) + t98)] = s0[t96];
    }
    uint t99 = (lid.x + 3328u);
    uint t100 = (t58 + (t99 / 64u));
    uint t101 = (t61 + (t99 % 64u));
    if (((t100 < uniforms.u0) && (t101 < uniforms.u1)))
    {
        buffer2[((t100 * uniforms.u1) + t101)] = s0[t99];
    }
    uint t102 = (lid.x + 3584u);
    uint t103 = (t58 + (t102 / 64u));
    uint t104 = (t61 + (t102 % 64u));
    if (((t103 < uniforms.u0) && (t104 < uniforms.u1)))
    {
        buffer2[((t103 * uniforms.u1) + t104)] = s0[t102];
    }
    uint t105 = (lid.x + 3840u);
    uint t106 = (t58 + (t105 / 64u));
    uint t107 = (t61 + (t105 % 64u));
    if (((t106 < uniforms.u0) && (t107 < uniforms.u1)))
    {
        buffer2[((t106 * uniforms.u1) + t107)] = s0[t105];
    }
}
