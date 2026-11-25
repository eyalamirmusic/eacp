@id(0) override eacpOverride0: i32 = 1;
@group(0) @binding(1) var<storage, read_write> values: array<f32>;

@compute @workgroup_size(64)
fn computeMain(@builtin(global_invocation_id) gid: vec3u)
{
    values[gid.x] = f32(eacpOverride0);
}
