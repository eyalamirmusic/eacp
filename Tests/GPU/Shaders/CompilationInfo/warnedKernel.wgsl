@group(0) @binding(1) var<storage, read_write> values: array<f32>;

@compute @workgroup_size(64)
fn computeMain(@builtin(global_invocation_id) gid: vec3u)
{
    values[gid.x] = 1.0;
    return;
    values[gid.x] = 2.0;
}
