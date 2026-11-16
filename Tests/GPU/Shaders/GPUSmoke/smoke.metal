#include <metal_stdlib>
using namespace metal;

struct VertexIn { float4 position [[attribute(0)]]; };

vertex float4 vertexMain(VertexIn in [[stage_in]]) { return in.position; }
fragment float4 fragmentMain() { return float4(1.0, 1.0, 1.0, 1.0); }
