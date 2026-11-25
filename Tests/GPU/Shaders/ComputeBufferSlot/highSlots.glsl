#version 450

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(std140, set = 0, binding = 16) uniform Params
{
    float scale;
    uint count;
} params;

layout(std430, set = 0, binding = 7) readonly buffer Low { float low[]; };
layout(std430, set = 0, binding = 21) readonly buffer High { float high[]; };
layout(std430, set = 0, binding = 24) buffer Result { float result[]; };

void main()
{
    uint gid = gl_GlobalInvocationID.x;

    if (gid >= params.count)
        return;

    result[gid] = low[gid] + high[gid] * params.scale;
}
