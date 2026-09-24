#version 450

float eacpErf(float x)
{
    float a = abs(x);
    float t = 1.0 / (1.0 + 0.3275911 * a);
    float e = 1.0 - t * (0.254829592 + t * (-0.284496736 + t * (1.421413741
              + t * (-1.453152027 + t * 1.061405429)))) * exp(-a * a);
    return a == 0.0 ? x : (x < 0.0 ? -e : e);
}

vec2 eacpErf(vec2 x)
{
    return vec2(eacpErf(x.x), eacpErf(x.y));
}

vec3 eacpErf(vec3 x)
{
    return vec3(eacpErf(x.x), eacpErf(x.y), eacpErf(x.z));
}

vec4 eacpErf(vec4 x)
{
    return vec4(eacpErf(x.x), eacpErf(x.y), eacpErf(x.z), eacpErf(x.w));
}

layout(std140, set = 0, binding = 16) uniform Uniforms
{
    uint count;
} uniforms;

layout(std430, set = 0, binding = 0) readonly buffer Buffer0
{
    float buffer0[];
};
layout(std430, set = 0, binding = 1) buffer Buffer1
{
    float buffer1[];
};

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

void main()
{
    uint gid = gl_GlobalInvocationID.x;
    if (gid >= uniforms.count)
        return;
    float t0 = buffer0[gid];
    buffer1[gid] = ((0.5 * t0) * (1.0 + eacpErf((t0 * 0.707107))));
}
