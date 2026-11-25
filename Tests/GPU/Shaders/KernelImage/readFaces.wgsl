@group(0) @binding(1) var<storage, read_write> values: array<vec4f>;
@group(0) @binding(17) var faces: texture_2d_array<f32>;

@compute @workgroup_size(64)
fn computeMain(@builtin(global_invocation_id) gid: vec3u)
{
    if (gid.x >= textureNumLayers(faces))
    {
        return;
    }

    values[gid.x] = textureLoad(faces, vec2u(1u, 1u), gid.x, 0);
}
