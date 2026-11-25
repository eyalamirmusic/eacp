float4 vertexMain(uint id : SV_VertexID) : SV_Position
{
    return float4(0.0, 0.0, 0.0, 1.0);
}

float4 fragmentMain() : SV_Target
{
    return missingColour;
}
