@group(0) @binding(1) var<storage, read_write> values: array<f32>;
@group(0) @binding(17) var depth: texture_depth_2d;

@compute @workgroup_size(8, 8)
fn computeMain(@builtin(global_invocation_id) gid: vec3u)
{
    let size = textureDimensions(depth);

    if (gid.x >= size.x || gid.y >= size.y)
    {
        return;
    }

    values[gid.y * size.x + gid.x] = textureLoad(depth, gid.xy, 0);
}
