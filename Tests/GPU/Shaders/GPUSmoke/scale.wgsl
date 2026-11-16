struct Params { scale: f32, count: u32, }

@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(1) var<storage, read> sourceValues: array<f32>;
@group(0) @binding(2) var<storage, read_write> targetValues: array<f32>;

@compute @workgroup_size(64)
fn computeMain(@builtin(global_invocation_id) gid: vec3u)
{
    if (gid.x >= params.count)
    {
        return;
    }

    targetValues[gid.x] = sourceValues[gid.x] * params.scale;
}
