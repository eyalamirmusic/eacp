@group(0) @binding(17) var src: texture_2d<f32>;
@group(0) @binding(18) var dst: texture_storage_2d<rgba8unorm, write>;

@compute @workgroup_size(8, 8)
fn computeMain(@builtin(global_invocation_id) gid: vec3u)
{
    let size = textureDimensions(dst);

    if (gid.x >= size.x || gid.y >= size.y)
    {
        return;
    }

    let at = gid.xy * 2u;
    let sum = textureLoad(src, at, 0) + textureLoad(src, at + vec2u(1u, 0u), 0)
              + textureLoad(src, at + vec2u(0u, 1u), 0)
              + textureLoad(src, at + vec2u(1u, 1u), 0);
    textureStore(dst, gid.xy, sum * 0.25);
}
