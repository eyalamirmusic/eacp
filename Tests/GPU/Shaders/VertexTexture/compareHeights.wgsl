@group(0) @binding(17) var depthImage: texture_depth_2d;
@group(0) @binding(33) var depthSampler: sampler_comparison;

@vertex fn vertexMain(@location(0) column: vec3f) -> @builtin(position) vec4f
{
    let uv = vec2f(column.y, 0.5);
    let passed = textureSampleCompareLevel(depthImage, depthSampler, uv, 0.5);
    let h = 0.5 + 0.5 * passed;
    return vec4f(column.x, -1.0 + 2.0 * h * column.z, 0.0, 1.0);
}

@fragment fn fragmentMain() -> @location(0) vec4f
{
    return vec4f(1.0);
}
