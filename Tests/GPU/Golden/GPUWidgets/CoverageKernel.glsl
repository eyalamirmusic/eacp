#version 450

layout(std140, set = 0, binding = 16) uniform Uniforms
{
    uint u0;
    int u1;
    uint width;
    uint height;
} uniforms;

layout(std430, set = 0, binding = 0) readonly buffer Buffer0
{
    float buffer0[];
};
layout(std430, set = 0, binding = 1) buffer Buffer1
{
    uint buffer1[];
};
layout(std430, set = 0, binding = 2) buffer Buffer2
{
    uint buffer2[];
};
layout(std430, set = 0, binding = 3) readonly buffer Buffer3
{
    float buffer3[];
};
layout(std430, set = 0, binding = 4) readonly buffer Buffer4
{
    float buffer4[];
};

layout(set = 0, binding = 8) uniform writeonly image2D texture0;

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

void main()
{
    uvec2 gid = gl_GlobalInvocationID.xy;
    uvec2 lid = gl_LocalInvocationID.xy;
    uvec2 tgid = gl_WorkGroupID.xy;
    if (gid.x >= uniforms.width || gid.y >= uniforms.height)
        return;
    int v0 = 0;
    int v1 = uniforms.u1;
    while ((v0 < v1))
    {
        int t0 = ((v0 + v1) >> 1);
        if ((buffer4[uint(t0)] <= float(((tgid.y * uniforms.u0) + tgid.x))))
        {
            v0 = (t0 + 1);
        }
        else
        {
            v1 = t0;
        }
    }
    uint t1 = ((tgid.y * uniforms.u0) + tgid.x);
    uint t2 = uint((max(v0, 1) - 1));
    uint t3 = (t1 - uint(buffer4[t2]));
    uint t4 = ((t2 * 2u) * 4u);
    vec4 t5 = vec4(buffer3[t4], buffer3[t4 + 1u], buffer3[t4 + 2u], buffer3[t4 + 3u]);
    uint t6 = uint((t5).y);
    uint t7 = ((t6 + 7u) / 8u);
    uint t8 = (((t3 % t7) * 8u) + lid.x);
    uint t9 = (((t3 / t7) * 8u) + lid.y);
    uint t10 = uint((t5).z);
    if (((t8 < t6) && (t9 < t10)))
    {
        uint t11 = (t8 / 16u);
        float v2 = (float(int(buffer2[((uint((t5).x) + (t11 * t10)) + t9)])) * 9.53674e-07);
        uint t12 = ((uint((t5).w) + ((t9 / 16u) * ((t6 + 15u) / 16u))) + t11);
        uint v3 = buffer1[t12];
        uint v4 = buffer1[(t12 + 1u)];
        while ((v3 < v4))
        {
            uint t13 = (v3 * 4u);
            vec4 t14 = vec4(buffer0[t13], buffer0[t13 + 1u], buffer0[t13 + 2u], buffer0[t13 + 3u]);
            float t15 = float(t9);
            float t16 = ((t14).y - t15);
            float t17 = ((t14).w - t15);
            float t18 = clamp(max(t16, t17), 0.0, 1.0);
            float t19 = clamp(min(t16, t17), 0.0, 1.0);
            float t20 = (t18 - t19);
            if ((t20 > 0.0))
            {
                float t21 = float(t8);
                float t22 = ((t14).x - t21);
                float t23 = (1.0 / (t17 - t16));
                float t24 = ((t14).z - t21);
                float t25 = (t22 + (((t18 - t16) * t23) * (t24 - t22)));
                float t26 = (t22 + (((t19 - t16) * t23) * (t24 - t22)));
                float t27 = (t25 - t26);
                bool t28 = (abs(t27) < 1e-06);
                float t29 = clamp(t25, 0.0, 1.0);
                float t30 = clamp(t26, 0.0, 1.0);
                v2 = (v2 + (((t17 > t16) ? t20 : (-(t20))) * (1.0 - (t28 ? clamp(t26, 0.0, 1.0) : (((((t29 * t29) * 0.5) + max((t25 - 1.0), 0.0)) - (((t30 * t30) * 0.5) + max((t26 - 1.0), 0.0))) / (t28 ? 1.0 : t27))))));
            }
            v3 = (v3 + 1u);
        }
        uint t31 = (((t2 * 2u) + 1u) * 4u);
        vec4 t32 = vec4(buffer3[t31], buffer3[t31 + 1u], buffer3[t31 + 2u], buffer3[t31 + 3u]);
        float t33 = abs(v2);
        float t34 = (fract((t33 * 0.5)) * 2.0);
        float t35 = (((t32).x != 0.0) ? min(t34, (2.0 - t34)) : min(t33, 1.0));
        imageStore(texture0, ivec2((t8 + uint((t32).y)), (t9 + uint((t32).z))), vec4(t35, t35, t35, t35));
    }
}
