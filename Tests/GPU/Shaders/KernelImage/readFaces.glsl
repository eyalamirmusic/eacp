#version 450

layout(local_size_x = 64, local_size_y = 1, local_size_z = 1) in;

layout(set = 0, binding = 8) uniform sampler2DArray faces;
layout(std430, set = 0, binding = 0) buffer Values
{
    vec4 values[];
};

void main()
{
    int gid = int(gl_GlobalInvocationID.x);

    if (gid >= textureSize(faces, 0).z)
        return;

    values[gid] = texelFetch(faces, ivec3(1, 1, gid), 0);
}
