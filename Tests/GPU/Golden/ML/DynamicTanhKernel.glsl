#version 450

float eacpSaturatingTanh(float x)
{
    return x >= 10.0 ? 1.0 : (x <= -10.0 ? -1.0 : tanh(x));
}

vec2 eacpSaturatingTanh(vec2 x)
{
    return vec2(eacpSaturatingTanh(x.x), eacpSaturatingTanh(x.y));
}

vec3 eacpSaturatingTanh(vec3 x)
{
    return vec3(eacpSaturatingTanh(x.x), eacpSaturatingTanh(x.y),
                eacpSaturatingTanh(x.z));
}

vec4 eacpSaturatingTanh(vec4 x)
{
    return vec4(eacpSaturatingTanh(x.x), eacpSaturatingTanh(x.y),
                eacpSaturatingTanh(x.z), eacpSaturatingTanh(x.w));
}

layout(std140, set = 0, binding = 16) uniform Uniforms
{
    uint u0;
    float u1;
    uint u2;
    uint u3;
    uint u4;
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
layout(std430, set = 0, binding = 2) readonly buffer Buffer2
{
    float buffer2[];
};
layout(std430, set = 0, binding = 3) buffer Buffer3
{
    float buffer3[];
};

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

void main()
{
    uvec2 gid = gl_GlobalInvocationID.xy;
    if (gid.x >= uniforms.width || gid.y >= uniforms.height)
        return;
    buffer3[((gid.y * uniforms.u0) + gid.x)] = ((eacpSaturatingTanh((uniforms.u1 * buffer0[(((((gid.y / uniforms.u2) * uniforms.u3) + uniforms.u4) + ((gid.y % uniforms.u2) * uniforms.u0)) + gid.x)])) * buffer1[gid.x]) + buffer2[gid.x]);
}
