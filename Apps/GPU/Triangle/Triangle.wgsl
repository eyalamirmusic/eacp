struct VertexIn
{
    @location(0) position: vec2<f32>,
    @location(1) color: vec3<f32>,
};

struct VertexOut
{
    @builtin(position) position: vec4<f32>,
    @location(0) color: vec3<f32>,
};

@vertex
fn vertexMain(input: VertexIn) -> VertexOut
{
    var output: VertexOut;
    output.position = vec4<f32>(input.position, 0.0, 1.0);
    output.color = input.color;
    return output;
}

@fragment
fn fragmentMain(input: VertexOut) -> @location(0) vec4<f32>
{
    return vec4<f32>(input.color, 1.0);
}
