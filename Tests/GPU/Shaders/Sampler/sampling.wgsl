@group(0) @binding(17) var image: texture_2d<f32>;
@group(0) @binding(33) var imageSampler: sampler;

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
    return textureSample(image, imageSampler, in.uv);
}
