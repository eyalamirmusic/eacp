@vertex fn vertexMain(@location(0) position: vec4f) -> @builtin(position) vec4f
{
    return position;
}

@fragment fn fragmentMain() -> @location(0) vec4f
{
    return vec4f(1.0, 1.0, 1.0, 1.0);
}
