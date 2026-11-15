#include "TextureFunctions.h"

namespace eacp::GPU
{
TextureIndex::TextureIndex(const Int& value)
    : node(value.node)
{
}

TextureIndex::TextureIndex(const UInt& value)
    : node(value.node)
{
}

TextureIndex::TextureIndex(int value)
    : literal(value)
{
}

int TextureIndex::nodeOn(ShaderGraph& graph) const
{
    return node >= 0 ? node : graph.addIntConstant(literal);
}

TextureScalar::TextureScalar(const Float& value)
    : node(value.node)
{
}

TextureScalar::TextureScalar(float value)
    : literal(value)
{
}

int TextureScalar::nodeOn(ShaderGraph& graph) const
{
    return node >= 0 ? node : graph.addConstant(literal);
}

TexelCoordinates2::TexelCoordinates2(const Int2& value)
    : node(value.node)
{
}

TexelCoordinates2::TexelCoordinates2(const UInt2& value)
    : node(value.node)
{
}

TexelCoordinates3::TexelCoordinates3(const Int3& value)
    : node(value.node)
{
}

TexelCoordinates3::TexelCoordinates3(const UInt3& value)
    : node(value.node)
{
}

namespace
{
template <typename Handle>
bool textureCompares(const Handle& texture)
{
    return texture.graph->textureSampling(texture.slot).compare
           != TextureCompare::None;
}

// GLSL spells a comparing depth texture as a shadow sampler, which has no
// plain read, and a plain one as a sampler with no comparison.
template <typename Handle>
void assertTextureCompares(const Handle& texture, bool compares)
{
    assert(textureCompares(texture) == compares);
    (void) texture;
    (void) compares;
}

template <typename Result, typename Handle>
Result textureFunctionOf(const Handle& texture,
                         TextureFunction function,
                         const Vector<int>& args,
                         int component = 0)
{
    auto result = Result {};
    result.graph = texture.graph;
    result.node =
        texture.graph->addTextureFunction(texture.slot, function, args, component);
    return result;
}

template <typename Result, typename Handle>
Result textureFunctionOf(const Handle& texture,
                         TextureFunction function,
                         int coordinates,
                         const TextureIndex& layer,
                         const Vector<int>& extras,
                         int component = 0)
{
    auto args = Vector<int> {coordinates, layer.nodeOn(*texture.graph)};

    for (auto extra: extras)
        args.add(extra);

    return textureFunctionOf<Result>(texture, function, args, component);
}
} // namespace

Float4 sample(const Texture3D& texture, const Float3& coordinates)
{
    return textureFunctionOf<Float4>(
        texture, TextureFunction::Sample, {coordinates.node});
}

Float4 sample(const TextureCubeArray& texture,
              const Float3& direction,
              const TextureIndex& layer)
{
    return textureFunctionOf<Float4>(
        texture, TextureFunction::Sample, direction.node, layer, {});
}

Float sample(const TextureDepthCube& texture, const Float3& direction)
{
    assertTextureCompares(texture, false);
    return textureFunctionOf<Float>(
        texture, TextureFunction::Sample, {direction.node});
}

Float sample(const TextureDepthCubeArray& texture,
             const Float3& direction,
             const TextureIndex& layer)
{
    assertTextureCompares(texture, false);
    return textureFunctionOf<Float>(
        texture, TextureFunction::Sample, direction.node, layer, {});
}

Float4 sampleLevel(const Texture2D& texture,
                   const Float2& coordinates,
                   const TextureScalar& level)
{
    return textureFunctionOf<Float4>(
        texture,
        TextureFunction::Level,
        {coordinates.node, level.nodeOn(*texture.graph)});
}

Float4 sampleLevel(const Texture2DArray& texture,
                   const Float2& coordinates,
                   const TextureIndex& layer,
                   const TextureScalar& level)
{
    return textureFunctionOf<Float4>(texture,
                                     TextureFunction::Level,
                                     coordinates.node,
                                     layer,
                                     {level.nodeOn(*texture.graph)});
}

Float4 sampleLevel(const Texture3D& texture,
                   const Float3& coordinates,
                   const TextureScalar& level)
{
    return textureFunctionOf<Float4>(
        texture,
        TextureFunction::Level,
        {coordinates.node, level.nodeOn(*texture.graph)});
}

Float4 sampleLevel(const TextureCube& texture,
                   const Float3& direction,
                   const TextureScalar& level)
{
    return textureFunctionOf<Float4>(texture,
                                     TextureFunction::Level,
                                     {direction.node, level.nodeOn(*texture.graph)});
}

Float4 sampleLevel(const TextureCubeArray& texture,
                   const Float3& direction,
                   const TextureIndex& layer,
                   const TextureScalar& level)
{
    return textureFunctionOf<Float4>(texture,
                                     TextureFunction::Level,
                                     direction.node,
                                     layer,
                                     {level.nodeOn(*texture.graph)});
}

Float sampleLevel(const TextureDepth2D& texture,
                  const Float2& coordinates,
                  const TextureIndex& level)
{
    assertTextureCompares(texture, false);
    return textureFunctionOf<Float>(
        texture,
        TextureFunction::Level,
        {coordinates.node, level.nodeOn(*texture.graph)});
}

Float sampleLevel(const TextureDepth2DArray& texture,
                  const Float2& coordinates,
                  const TextureIndex& layer,
                  const TextureIndex& level)
{
    assertTextureCompares(texture, false);
    return textureFunctionOf<Float>(texture,
                                    TextureFunction::Level,
                                    coordinates.node,
                                    layer,
                                    {level.nodeOn(*texture.graph)});
}

Float sampleLevel(const TextureDepthCube& texture,
                  const Float3& direction,
                  const TextureIndex& level)
{
    assertTextureCompares(texture, false);
    return textureFunctionOf<Float>(texture,
                                    TextureFunction::Level,
                                    {direction.node, level.nodeOn(*texture.graph)});
}

Float sampleLevel(const TextureDepthCubeArray& texture,
                  const Float3& direction,
                  const TextureIndex& layer,
                  const TextureIndex& level)
{
    assertTextureCompares(texture, false);
    return textureFunctionOf<Float>(texture,
                                    TextureFunction::Level,
                                    direction.node,
                                    layer,
                                    {level.nodeOn(*texture.graph)});
}

Float4 sampleBias(const Texture2D& texture,
                  const Float2& coordinates,
                  const TextureScalar& bias)
{
    return textureFunctionOf<Float4>(
        texture,
        TextureFunction::Bias,
        {coordinates.node, bias.nodeOn(*texture.graph)});
}

Float4 sampleBias(const Texture2DArray& texture,
                  const Float2& coordinates,
                  const TextureIndex& layer,
                  const TextureScalar& bias)
{
    return textureFunctionOf<Float4>(texture,
                                     TextureFunction::Bias,
                                     coordinates.node,
                                     layer,
                                     {bias.nodeOn(*texture.graph)});
}

Float4 sampleBias(const Texture3D& texture,
                  const Float3& coordinates,
                  const TextureScalar& bias)
{
    return textureFunctionOf<Float4>(
        texture,
        TextureFunction::Bias,
        {coordinates.node, bias.nodeOn(*texture.graph)});
}

Float4 sampleBias(const TextureCube& texture,
                  const Float3& direction,
                  const TextureScalar& bias)
{
    return textureFunctionOf<Float4>(texture,
                                     TextureFunction::Bias,
                                     {direction.node, bias.nodeOn(*texture.graph)});
}

Float4 sampleBias(const TextureCubeArray& texture,
                  const Float3& direction,
                  const TextureIndex& layer,
                  const TextureScalar& bias)
{
    return textureFunctionOf<Float4>(texture,
                                     TextureFunction::Bias,
                                     direction.node,
                                     layer,
                                     {bias.nodeOn(*texture.graph)});
}

Float4 sampleGrad(const Texture2D& texture,
                  const Float2& coordinates,
                  const Float2& ddx,
                  const Float2& ddy)
{
    return textureFunctionOf<Float4>(
        texture, TextureFunction::Grad, {coordinates.node, ddx.node, ddy.node});
}

Float4 sampleGrad(const Texture2DArray& texture,
                  const Float2& coordinates,
                  const TextureIndex& layer,
                  const Float2& ddx,
                  const Float2& ddy)
{
    return textureFunctionOf<Float4>(texture,
                                     TextureFunction::Grad,
                                     coordinates.node,
                                     layer,
                                     {ddx.node, ddy.node});
}

Float4 sampleGrad(const Texture3D& texture,
                  const Float3& coordinates,
                  const Float3& ddx,
                  const Float3& ddy)
{
    return textureFunctionOf<Float4>(
        texture, TextureFunction::Grad, {coordinates.node, ddx.node, ddy.node});
}

Float4 sampleGrad(const TextureCube& texture,
                  const Float3& direction,
                  const Float3& ddx,
                  const Float3& ddy)
{
    return textureFunctionOf<Float4>(
        texture, TextureFunction::Grad, {direction.node, ddx.node, ddy.node});
}

Float4 sampleGrad(const TextureCubeArray& texture,
                  const Float3& direction,
                  const TextureIndex& layer,
                  const Float3& ddx,
                  const Float3& ddy)
{
    return textureFunctionOf<Float4>(
        texture, TextureFunction::Grad, direction.node, layer, {ddx.node, ddy.node});
}

Float sampleCompare(const TextureDepthCube& texture,
                    const Float3& direction,
                    const TextureScalar& reference)
{
    assertTextureCompares(texture, true);
    return textureFunctionOf<Float>(
        texture,
        TextureFunction::Compare,
        {direction.node, reference.nodeOn(*texture.graph)});
}

Float sampleCompare(const TextureDepthCubeArray& texture,
                    const Float3& direction,
                    const TextureIndex& layer,
                    const TextureScalar& reference)
{
    assertTextureCompares(texture, true);
    return textureFunctionOf<Float>(texture,
                                    TextureFunction::Compare,
                                    direction.node,
                                    layer,
                                    {reference.nodeOn(*texture.graph)});
}

Float sampleCompareLevel(const TextureDepth2D& texture,
                         const Float2& coordinates,
                         const TextureScalar& reference)
{
    assertTextureCompares(texture, true);
    return textureFunctionOf<Float>(
        texture,
        TextureFunction::CompareLevel,
        {coordinates.node, reference.nodeOn(*texture.graph)});
}

Float sampleCompareLevel(const TextureDepth2DArray& texture,
                         const Float2& coordinates,
                         const TextureIndex& layer,
                         const TextureScalar& reference)
{
    assertTextureCompares(texture, true);
    return textureFunctionOf<Float>(texture,
                                    TextureFunction::CompareLevel,
                                    coordinates.node,
                                    layer,
                                    {reference.nodeOn(*texture.graph)});
}

Float sampleCompareLevel(const TextureDepthCube& texture,
                         const Float3& direction,
                         const TextureScalar& reference)
{
    assertTextureCompares(texture, true);
    return textureFunctionOf<Float>(
        texture,
        TextureFunction::CompareLevel,
        {direction.node, reference.nodeOn(*texture.graph)});
}

Float sampleCompareLevel(const TextureDepthCubeArray& texture,
                         const Float3& direction,
                         const TextureIndex& layer,
                         const TextureScalar& reference)
{
    assertTextureCompares(texture, true);
    return textureFunctionOf<Float>(texture,
                                    TextureFunction::CompareLevel,
                                    direction.node,
                                    layer,
                                    {reference.nodeOn(*texture.graph)});
}

Float4 gather(const Texture2D& texture, const Float2& coordinates, int component)
{
    return textureFunctionOf<Float4>(
        texture, TextureFunction::Gather, {coordinates.node}, component);
}

Float4 gather(const Texture2DArray& texture,
              const Float2& coordinates,
              const TextureIndex& layer,
              int component)
{
    return textureFunctionOf<Float4>(
        texture, TextureFunction::Gather, coordinates.node, layer, {}, component);
}

Float4 gather(const TextureCube& texture, const Float3& direction, int component)
{
    return textureFunctionOf<Float4>(
        texture, TextureFunction::Gather, {direction.node}, component);
}

Float4 gather(const TextureCubeArray& texture,
              const Float3& direction,
              const TextureIndex& layer,
              int component)
{
    return textureFunctionOf<Float4>(
        texture, TextureFunction::Gather, direction.node, layer, {}, component);
}

Float4 gather(const TextureDepth2D& texture, const Float2& coordinates)
{
    assertTextureCompares(texture, false);
    return textureFunctionOf<Float4>(
        texture, TextureFunction::Gather, {coordinates.node});
}

Float4 gather(const TextureDepth2DArray& texture,
              const Float2& coordinates,
              const TextureIndex& layer)
{
    assertTextureCompares(texture, false);
    return textureFunctionOf<Float4>(
        texture, TextureFunction::Gather, coordinates.node, layer, {});
}

Float4 gather(const TextureDepthCube& texture, const Float3& direction)
{
    assertTextureCompares(texture, false);
    return textureFunctionOf<Float4>(
        texture, TextureFunction::Gather, {direction.node});
}

Float4 gather(const TextureDepthCubeArray& texture,
              const Float3& direction,
              const TextureIndex& layer)
{
    assertTextureCompares(texture, false);
    return textureFunctionOf<Float4>(
        texture, TextureFunction::Gather, direction.node, layer, {});
}

Float4 gatherCompare(const TextureDepth2D& texture,
                     const Float2& coordinates,
                     const TextureScalar& reference)
{
    assertTextureCompares(texture, true);
    return textureFunctionOf<Float4>(
        texture,
        TextureFunction::GatherCompare,
        {coordinates.node, reference.nodeOn(*texture.graph)});
}

Float4 gatherCompare(const TextureDepth2DArray& texture,
                     const Float2& coordinates,
                     const TextureIndex& layer,
                     const TextureScalar& reference)
{
    assertTextureCompares(texture, true);
    return textureFunctionOf<Float4>(texture,
                                     TextureFunction::GatherCompare,
                                     coordinates.node,
                                     layer,
                                     {reference.nodeOn(*texture.graph)});
}

Float4 gatherCompare(const TextureDepthCube& texture,
                     const Float3& direction,
                     const TextureScalar& reference)
{
    assertTextureCompares(texture, true);
    return textureFunctionOf<Float4>(
        texture,
        TextureFunction::GatherCompare,
        {direction.node, reference.nodeOn(*texture.graph)});
}

Float4 gatherCompare(const TextureDepthCubeArray& texture,
                     const Float3& direction,
                     const TextureIndex& layer,
                     const TextureScalar& reference)
{
    assertTextureCompares(texture, true);
    return textureFunctionOf<Float4>(texture,
                                     TextureFunction::GatherCompare,
                                     direction.node,
                                     layer,
                                     {reference.nodeOn(*texture.graph)});
}

Float4 load(const Texture2D& texture,
            const TexelCoordinates2& coordinates,
            const TextureIndex& level)
{
    return textureFunctionOf<Float4>(
        texture,
        TextureFunction::Load,
        {coordinates.node, level.nodeOn(*texture.graph)});
}

Float4 load(const Texture2DArray& texture,
            const TexelCoordinates2& coordinates,
            const TextureIndex& layer,
            const TextureIndex& level)
{
    return textureFunctionOf<Float4>(texture,
                                     TextureFunction::Load,
                                     coordinates.node,
                                     layer,
                                     {level.nodeOn(*texture.graph)});
}

Float4 load(const Texture3D& texture,
            const TexelCoordinates3& coordinates,
            const TextureIndex& level)
{
    return textureFunctionOf<Float4>(
        texture,
        TextureFunction::Load,
        {coordinates.node, level.nodeOn(*texture.graph)});
}

Float load(const TextureDepth2D& texture,
           const TexelCoordinates2& coordinates,
           const TextureIndex& level)
{
    assertTextureCompares(texture, false);
    return textureFunctionOf<Float>(
        texture,
        TextureFunction::Load,
        {coordinates.node, level.nodeOn(*texture.graph)});
}

Float load(const TextureDepth2DArray& texture,
           const TexelCoordinates2& coordinates,
           const TextureIndex& layer,
           const TextureIndex& level)
{
    assertTextureCompares(texture, false);
    return textureFunctionOf<Float>(texture,
                                    TextureFunction::Load,
                                    coordinates.node,
                                    layer,
                                    {level.nodeOn(*texture.graph)});
}
} // namespace eacp::GPU
