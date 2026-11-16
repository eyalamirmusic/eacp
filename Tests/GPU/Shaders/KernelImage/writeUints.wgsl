@group(0) @binding(17) var image: texture_storage_2d<r32uint, write>;

@compute @workgroup_size(8, 8)
fn computeMain(@builtin(global_invocation_id) gid: vec3u)
{
    let size = textureDimensions(image);

    if (gid.x >= size.x || gid.y >= size.y)
    {
        return;
    }

    textureStore(image, gid.xy, vec4u((gid.x + gid.y * 4u) * 1000003u + 7u, 0u, 0u, 0u));
}
