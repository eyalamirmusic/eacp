#version 450

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(std140, set = 0, binding = 16) uniform Params
{
    float scale;
    uint count;
} params;

layout(std430, set = 0, binding = 0) readonly buffer Source
{
    float sourceValues[];
};

layout(std430, set = 0, binding = 1) buffer Target
{
    float targetValues[];
};

void main()
{
    uint gid = gl_GlobalInvocationID.x;

    if (gid >= params.count)
        return;

    targetValues[gid] = sourceValues[gid] * params.scale;
}
