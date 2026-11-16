#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 8, r32ui) uniform writeonly uimage2D target;

void main()
{
    ivec2 gid = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(target);

    if (gid.x >= size.x || gid.y >= size.y)
        return;

    uint value = uint(gid.x + gid.y * 4) * 1000003u + 7u;
    imageStore(target, gid, uvec4(value, 0u, 0u, 0u));
}
