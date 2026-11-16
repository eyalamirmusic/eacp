@group(0) @binding(17) var depthImage: texture_depth_2d;
@group(0) @binding(33) var depthSampler: sampler_comparison;

struct VertexOut
{
    @builtin(position) position: vec4f,
    @location(0) uv: vec2f,
}

@vertex fn vertexMain(@location(0) position: vec2f, @location(1) uv: vec2f)
    -> VertexOut
{
    var out: VertexOut;
    out.position = vec4f(position, 0.0, 1.0);
    out.uv = uv;
    return out;
}

@fragment fn fragmentMain(in: VertexOut) -> @location(0) vec4f
{
    let passed = textureSampleCompare(depthImage, depthSampler, in.uv, 0.5);
    return vec4f(passed, 0.0, 0.0, 1.0);
}
