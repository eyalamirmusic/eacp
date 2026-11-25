#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 8) uniform sampler2DArray depth;
layout(std430, set = 0, binding = 0) buffer Values
{
    float values[];
};
layout(set = 0, binding = 16) uniform Params
{
    uint layer;
};

void main()
{
    ivec2 gid = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = textureSize(depth, 0).xy;

    if (gid.x >= size.x || gid.y >= size.y)
        return;

    values[gid.y * size.x + gid.x] = texelFetch(depth, ivec3(gid, int(layer)), 0).r;
}
