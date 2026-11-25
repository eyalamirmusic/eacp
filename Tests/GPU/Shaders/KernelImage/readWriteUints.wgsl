@group(0) @binding(17) var image: texture_storage_2d<r32uint, read_write>;

@compute @workgroup_size(8, 8)
fn computeMain(@builtin(global_invocation_id) gid: vec3u)
{
    let size = textureDimensions(image);

    if (gid.x >= size.x || gid.y >= size.y)
    {
        return;
    }

    let value = textureLoad(image, gid.xy).x;
    textureStore(image, gid.xy, vec4u(value * 3u + 1u, 0u, 0u, 0u));
}
