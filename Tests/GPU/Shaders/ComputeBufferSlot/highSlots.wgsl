struct Params { scale: f32, count: u32, }

@group(0) @binding(0) var<uniform> params: Params;
@group(0) @binding(8) var<storage, read> low: array<f32>;
@group(0) @binding(13) var<storage, read> high: array<f32>;
@group(0) @binding(16) var<storage, read_write> result: array<f32>;

@compute @workgroup_size(64)
fn computeMain(@builtin(global_invocation_id) gid: vec3u)
{
    if (gid.x >= params.count)
    {
        return;
    }

    result[gid.x] = low[gid.x] + high[gid.x] * params.scale;
}
