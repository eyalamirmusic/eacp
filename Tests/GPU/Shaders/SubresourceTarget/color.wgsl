struct VertexOut
{
    @builtin(position) position: vec4f,
    @location(0) color: vec4f,
}

@vertex fn vertexMain(@location(0) position: vec3f, @location(1) color: vec4f)
    -> VertexOut
{
    var out: VertexOut;
    out.position = vec4f(position, 1.0);
    out.color = color;
    return out;
}

@fragment fn fragmentMain(in: VertexOut) -> @location(0) vec4f
{
    return in.color;
}
