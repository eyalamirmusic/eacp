@group(0) @binding(17) var depth: texture_depth_2d;

@vertex fn vertexMain(@location(0) position: vec2f) -> @builtin(position) vec4f
{
    return vec4f(position, 0.5, 1.0);
}

@fragment fn fragmentMain(@builtin(position) position: vec4f) -> @location(0) vec4f
{
    return vec4f(textureLoad(depth, vec2u(position.xy), 0), 0.0, 0.0, 1.0);
}
