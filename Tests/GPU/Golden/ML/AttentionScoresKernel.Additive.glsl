#version 450

layout(std140, set = 0, binding = 16) uniform Uniforms
{
    uint u0;
    uint u1;
    uint u2;
    uint u3;
    float u4;
    uint width;
    uint height;
    uint depth;
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
    float buffer3[];
};

layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

void main()
{
    uvec3 gid = gl_GlobalInvocationID.xyz;
    if (gid.x >= uniforms.width || gid.y >= uniforms.height || gid.z >= uniforms.depth)
        return;
    vec4 t0 = vec4(0.0, 0.0, 0.0, 0.0);
    vec4 v0 = t0;
    vec4 v1 = t0;
    vec4 v2 = t0;
    vec4 v3 = t0;
    uint v4 = 0u;
    while ((v4 < uniforms.u1))
    {
        uint t1 = (gid.y * 4u);
        uint t2 = (uniforms.u2 - 1u);
        uint t3 = ((((((min((t1 + 0u), t2) * uniforms.u0) + gid.z) * uniforms.u1) + v4) / 4u) * 4u);
        vec4 t4 = vec4(buffer0[t3], buffer0[t3 + 1u], buffer0[t3 + 2u], buffer0[t3 + 3u]);
        uint t5 = (gid.x * 4u);
        uint t6 = (uniforms.u3 - 1u);
        uint t7 = ((((((min((t5 + 0u), t6) * uniforms.u0) + gid.z) * uniforms.u1) + v4) / 4u) * 4u);
        vec4 t8 = vec4(buffer1[t7], buffer1[t7 + 1u], buffer1[t7 + 2u], buffer1[t7 + 3u]);
        uint t9 = ((((((min((t5 + 1u), t6) * uniforms.u0) + gid.z) * uniforms.u1) + v4) / 4u) * 4u);
        vec4 t10 = vec4(buffer1[t9], buffer1[t9 + 1u], buffer1[t9 + 2u], buffer1[t9 + 3u]);
        uint t11 = ((((((min((t5 + 2u), t6) * uniforms.u0) + gid.z) * uniforms.u1) + v4) / 4u) * 4u);
        vec4 t12 = vec4(buffer1[t11], buffer1[t11 + 1u], buffer1[t11 + 2u], buffer1[t11 + 3u]);
        uint t13 = ((((((min((t5 + 3u), t6) * uniforms.u0) + gid.z) * uniforms.u1) + v4) / 4u) * 4u);
        vec4 t14 = vec4(buffer1[t13], buffer1[t13 + 1u], buffer1[t13 + 2u], buffer1[t13 + 3u]);
        vec4 t15 = vec4((t8).x, (t10).x, (t12).x, (t14).x);
        v0 = (v0 + ((t4).x * t15));
        uint t16 = ((((((min((t1 + 1u), t2) * uniforms.u0) + gid.z) * uniforms.u1) + v4) / 4u) * 4u);
        vec4 t17 = vec4(buffer0[t16], buffer0[t16 + 1u], buffer0[t16 + 2u], buffer0[t16 + 3u]);
        v1 = (v1 + ((t17).x * t15));
        uint t18 = ((((((min((t1 + 2u), t2) * uniforms.u0) + gid.z) * uniforms.u1) + v4) / 4u) * 4u);
        vec4 t19 = vec4(buffer0[t18], buffer0[t18 + 1u], buffer0[t18 + 2u], buffer0[t18 + 3u]);
        v2 = (v2 + ((t19).x * t15));
        uint t20 = ((((((min((t1 + 3u), t2) * uniforms.u0) + gid.z) * uniforms.u1) + v4) / 4u) * 4u);
        vec4 t21 = vec4(buffer0[t20], buffer0[t20 + 1u], buffer0[t20 + 2u], buffer0[t20 + 3u]);
        v3 = (v3 + ((t21).x * t15));
        vec4 t22 = vec4((t8).y, (t10).y, (t12).y, (t14).y);
        v0 = (v0 + ((t4).y * t22));
        v1 = (v1 + ((t17).y * t22));
        v2 = (v2 + ((t19).y * t22));
        v3 = (v3 + ((t21).y * t22));
        vec4 t23 = vec4((t8).z, (t10).z, (t12).z, (t14).z);
        v0 = (v0 + ((t4).z * t23));
        v1 = (v1 + ((t17).z * t23));
        v2 = (v2 + ((t19).z * t23));
        v3 = (v3 + ((t21).z * t23));
        vec4 t24 = vec4((t8).w, (t10).w, (t12).w, (t14).w);
        v0 = (v0 + ((t4).w * t24));
        v1 = (v1 + ((t17).w * t24));
        v2 = (v2 + ((t19).w * t24));
        v3 = (v3 + ((t21).w * t24));
        v4 = (v4 + 4u);
    }
    uint t25 = (gid.y * 4u);
    uint t26 = (t25 + 0u);
    uint t27 = (gid.x * 4u);
    uint t28 = (t27 + 0u);
    if (((t26 < uniforms.u2) && (t28 < uniforms.u3)))
    {
        buffer3[((((t26 * uniforms.u0) + gid.z) * uniforms.u3) + t28)] = (((v0).x * uniforms.u4) + buffer2[((t26 * uniforms.u3) + t28)]);
    }
    uint t29 = (t27 + 1u);
    if (((t26 < uniforms.u2) && (t29 < uniforms.u3)))
    {
        buffer3[((((t26 * uniforms.u0) + gid.z) * uniforms.u3) + t29)] = (((v0).y * uniforms.u4) + buffer2[((t26 * uniforms.u3) + t29)]);
    }
    uint t30 = (t27 + 2u);
    if (((t26 < uniforms.u2) && (t30 < uniforms.u3)))
    {
        buffer3[((((t26 * uniforms.u0) + gid.z) * uniforms.u3) + t30)] = (((v0).z * uniforms.u4) + buffer2[((t26 * uniforms.u3) + t30)]);
    }
    uint t31 = (t27 + 3u);
    if (((t26 < uniforms.u2) && (t31 < uniforms.u3)))
    {
        buffer3[((((t26 * uniforms.u0) + gid.z) * uniforms.u3) + t31)] = (((v0).w * uniforms.u4) + buffer2[((t26 * uniforms.u3) + t31)]);
    }
    uint t32 = (t25 + 1u);
    if (((t32 < uniforms.u2) && (t28 < uniforms.u3)))
    {
        buffer3[((((t32 * uniforms.u0) + gid.z) * uniforms.u3) + t28)] = (((v1).x * uniforms.u4) + buffer2[((t32 * uniforms.u3) + t28)]);
    }
    if (((t32 < uniforms.u2) && (t29 < uniforms.u3)))
    {
        buffer3[((((t32 * uniforms.u0) + gid.z) * uniforms.u3) + t29)] = (((v1).y * uniforms.u4) + buffer2[((t32 * uniforms.u3) + t29)]);
    }
    if (((t32 < uniforms.u2) && (t30 < uniforms.u3)))
    {
        buffer3[((((t32 * uniforms.u0) + gid.z) * uniforms.u3) + t30)] = (((v1).z * uniforms.u4) + buffer2[((t32 * uniforms.u3) + t30)]);
    }
    if (((t32 < uniforms.u2) && (t31 < uniforms.u3)))
    {
        buffer3[((((t32 * uniforms.u0) + gid.z) * uniforms.u3) + t31)] = (((v1).w * uniforms.u4) + buffer2[((t32 * uniforms.u3) + t31)]);
    }
    uint t33 = (t25 + 2u);
    if (((t33 < uniforms.u2) && (t28 < uniforms.u3)))
    {
        buffer3[((((t33 * uniforms.u0) + gid.z) * uniforms.u3) + t28)] = (((v2).x * uniforms.u4) + buffer2[((t33 * uniforms.u3) + t28)]);
    }
    if (((t33 < uniforms.u2) && (t29 < uniforms.u3)))
    {
        buffer3[((((t33 * uniforms.u0) + gid.z) * uniforms.u3) + t29)] = (((v2).y * uniforms.u4) + buffer2[((t33 * uniforms.u3) + t29)]);
    }
    if (((t33 < uniforms.u2) && (t30 < uniforms.u3)))
    {
        buffer3[((((t33 * uniforms.u0) + gid.z) * uniforms.u3) + t30)] = (((v2).z * uniforms.u4) + buffer2[((t33 * uniforms.u3) + t30)]);
    }
    if (((t33 < uniforms.u2) && (t31 < uniforms.u3)))
    {
        buffer3[((((t33 * uniforms.u0) + gid.z) * uniforms.u3) + t31)] = (((v2).w * uniforms.u4) + buffer2[((t33 * uniforms.u3) + t31)]);
    }
    uint t34 = (t25 + 3u);
    if (((t34 < uniforms.u2) && (t28 < uniforms.u3)))
    {
        buffer3[((((t34 * uniforms.u0) + gid.z) * uniforms.u3) + t28)] = (((v3).x * uniforms.u4) + buffer2[((t34 * uniforms.u3) + t28)]);
    }
    if (((t34 < uniforms.u2) && (t29 < uniforms.u3)))
    {
        buffer3[((((t34 * uniforms.u0) + gid.z) * uniforms.u3) + t29)] = (((v3).y * uniforms.u4) + buffer2[((t34 * uniforms.u3) + t29)]);
    }
    if (((t34 < uniforms.u2) && (t30 < uniforms.u3)))
    {
        buffer3[((((t34 * uniforms.u0) + gid.z) * uniforms.u3) + t30)] = (((v3).z * uniforms.u4) + buffer2[((t34 * uniforms.u3) + t30)]);
    }
    if (((t34 < uniforms.u2) && (t31 < uniforms.u3)))
    {
        buffer3[((((t34 * uniforms.u0) + gid.z) * uniforms.u3) + t31)] = (((v3).w * uniforms.u4) + buffer2[((t34 * uniforms.u3) + t31)]);
    }
}
