#version 450

layout(std140, set = 0, binding = 16) uniform Uniforms
{
    float u0;
    float u1;
    float u2;
    float u3;
    uint u4;
    uint count;
} uniforms;

layout(std430, set = 0, binding = 0) buffer Buffer0
{
    float buffer0[];
};

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

void main()
{
    uint gid = gl_GlobalInvocationID.x;
    if (gid >= uniforms.count)
        return;
    float t0 = ((uniforms.u0 * exp((((float(gid) / uniforms.u3) * (uniforms.u2 - uniforms.u1)) + uniforms.u1))) * 6.28319);
    buffer0[gid] = cos(t0);
    buffer0[(uniforms.u4 + gid)] = sin(t0);
}
