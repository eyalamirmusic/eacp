@group(0) @binding(17) var faces: texture_storage_2d_array<rgba8unorm, write>;

@compute @workgroup_size(4, 4, 4)
fn computeMain(@builtin(global_invocation_id) gid: vec3u)
{
    let size = textureDimensions(faces);

    if (gid.x >= size.x || gid.y >= size.y || gid.z >= textureNumLayers(faces))
    {
        return;
    }

    let face = f32(gid.z);
    textureStore(faces,
                 gid.xy,
                 gid.z,
                 vec4f((40.0 * face + 20.0) / 255.0,
                       (200.0 - 30.0 * face) / 255.0,
                       64.0 / 255.0,
                       1.0));
}
