@group(0) @binding(17) var cube: texture_cube<f32>;
@group(0) @binding(33) var cubeSampler: sampler;

var<private> directions: array<vec3f, 6> = array<vec3f, 6>(
    vec3f(1.0, 0.0, 0.0), vec3f(-1.0, 0.0, 0.0),
    vec3f(0.0, 1.0, 0.0), vec3f(0.0, -1.0, 0.0),
    vec3f(0.0, 0.0, 1.0), vec3f(0.0, 0.0, -1.0));

@vertex fn vertexMain(@location(0) position: vec2f) -> @builtin(position) vec4f
{
    return vec4f(position, 0.0, 1.0);
}

@fragment fn fragmentMain(@builtin(position) position: vec4f) -> @location(0) vec4f
{
    return textureSample(cube, cubeSampler, directions[u32(position.x)]);
}
