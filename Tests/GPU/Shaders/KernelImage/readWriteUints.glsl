#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 8, r32ui) uniform uimage2D image;

void main()
{
    ivec2 gid = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(image);

    if (gid.x >= size.x || gid.y >= size.y)
        return;

    uint value = imageLoad(image, gid).x;
    imageStore(image, gid, uvec4(value * 3u + 1u, 0u, 0u, 0u));
}
