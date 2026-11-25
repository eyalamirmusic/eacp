@vertex fn vertexMain(@location(0) position: vec2f) -> @builtin(position) vec4f
{
    return vec4f(position, 0.25, 1.0);
}
