#version 450

layout(local_size_x = 4, local_size_y = 4, local_size_z = 4) in;

layout(set = 0, binding = 8, rgba8) uniform writeonly image2DArray faces;

void main()
{
    ivec3 gid = ivec3(gl_GlobalInvocationID);
    ivec3 size = imageSize(faces);

    if (gid.x >= size.x || gid.y >= size.y || gid.z >= size.z)
        return;

    float face = float(gid.z);
    imageStore(faces, gid, vec4((40.0 * face + 20.0) / 255.0,
                                (200.0 - 30.0 * face) / 255.0, 64.0 / 255.0, 1.0));
}
