@vertex fn vertexMain() -> @builtin(position) vec4f
{
    return vec4f(0.0, 0.0, 0.0, 1.0);
}

@fragment fn fragmentMain() -> @location(0) vec4f
{
    return missingColour;
}
