#include <metal_stdlib>
using namespace metal;

vertex float4 vertexMain(uint id [[vertex_id]])
{
    return float4(0.0, 0.0, 0.0, 1.0);
}

fragment float4 fragmentMain()
{
    return missingColour;
}
