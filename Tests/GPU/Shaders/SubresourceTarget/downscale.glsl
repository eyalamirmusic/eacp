#version 450

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(set = 0, binding = 8) uniform sampler2D source;
layout(set = 0, binding = 9, rgba8) uniform writeonly image2D target;

void main()
{
    ivec2 gid = ivec2(gl_GlobalInvocationID.xy);
    ivec2 size = imageSize(target);

    if (gid.x >= size.x || gid.y >= size.y)
        return;

    ivec2 at = gid * 2;
    vec4 sum = texelFetch(source, at, 0) + texelFetch(source, at + ivec2(1, 0), 0)
               + texelFetch(source, at + ivec2(0, 1), 0)
               + texelFetch(source, at + ivec2(1, 1), 0);
    imageStore(target, gid, sum * 0.25);
}
