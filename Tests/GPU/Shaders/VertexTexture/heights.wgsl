@group(0) @binding(17) var heights: texture_2d<f32>;
@group(0) @binding(33) var heightSampler: sampler;

@vertex fn vertexMain(@location(0) column: vec3f) -> @builtin(position) vec4f
{
    let uv = vec2f(column.y, 0.5);
    let h = textureSampleLevel(heights, heightSampler, uv, 0.0).r;
    return vec4f(column.x, -1.0 + 2.0 * h * column.z, 0.0, 1.0);
}

@fragment fn fragmentMain() -> @location(0) vec4f
{
    return vec4f(1.0);
}
