#pragma once

#include "TextureFunctions.h"

#include <concepts>
#include <type_traits>

// WGSL's sampler and sampler_comparison: a sampler the shader declares as a
// binding of its own and hands to a sampling function, in place of the
// TextureSampling baked into the texture member. What the sampler does is
// whatever Device::makeSampler made the one bound at its slot - filter, mip
// filter, address mode per axis, border colour, anisotropy, LOD clamp and,
// for a comparison sampler, the CompareFunction.
//
//     Uniform<Texture2D> image;
//     Uniform<SamplerBinding> smooth;
//
//     setFragment(sample(image, smooth, uv));
//     ...
//     shader.smooth = device.makeSampler(linearRepeat);
//
// A sampler takes its slot from the texture counter: MSL sampler(slot), HLSL
// register(s<RenderPass::samplerRegisterBase + slot>), and on Vulkan the
// sampler at binding 8 + slot (the compute range for a kernel). It is bound
// with RenderPass::setFragmentSampler or ComputePass::setSampler, which a
// Uniform member's bind does. See "Samplers as bindings" in
// Lib/eacp/GPU/README.md.

namespace eacp::GPU
{
namespace detail
{
struct SamplerHandle
{
    ShaderGraph* graph = nullptr;
    int slot = -1;
};

template <typename T>
constexpr bool isDepthTextureHandle =
    std::derived_from<T, TextureDepth2D> || std::derived_from<T, TextureDepth2DArray>
    || std::derived_from<T, TextureDepthCube>
    || std::derived_from<T, TextureDepthCubeArray>;

template <typename T>
constexpr bool isPlanarTextureHandle =
    std::derived_from<T, Texture2D> || std::derived_from<T, Texture2DArray>
    || std::derived_from<T, TextureDepth2D>
    || std::derived_from<T, TextureDepth2DArray>;

int addSampledFunction(ShaderGraph* graph,
                       int textureSlot,
                       const SamplerHandle& sampler,
                       TextureFunction function,
                       const Vector<int>& args,
                       int component);
} // namespace detail

// A plain sampler: every sampling function but the comparisons.
struct SamplerBinding : detail::SamplerHandle
{
};

// A comparison sampler, for the comparison functions alone; the Sampler bound
// at its slot has a SamplerDescriptor::compare.
struct ComparisonSamplerBinding : detail::SamplerHandle
{
};

template <typename T>
concept ColourTextureHandle =
    SampledTextureHandle<T> && !detail::isDepthTextureHandle<T>;

template <typename T>
concept DepthTextureHandle =
    SampledTextureHandle<T> && detail::isDepthTextureHandle<T>;

template <typename T>
concept FlatTextureHandle = SampledTextureHandle<T> && !LayeredTextureHandle<T>;

// What a sample of T is addressed with, what it gives, and what names its
// level - an integer on a depth kind, as WGSL has it.
template <typename T>
using SamplingCoordinatesOf =
    std::conditional_t<detail::isPlanarTextureHandle<T>, Float2, Float3>;

template <typename T>
using SampledValueOf =
    std::conditional_t<detail::isDepthTextureHandle<T>, Float, Float4>;

template <typename T>
using SamplingLevelOf =
    std::conditional_t<detail::isDepthTextureHandle<T>, TextureIndex, TextureScalar>;

namespace detail
{
template <typename Result, typename Texture>
Result sampleThrough(const Texture& texture,
                     const SamplerHandle& sampler,
                     TextureFunction function,
                     const Vector<int>& args,
                     int component = 0)
{
    auto result = Result {};
    result.graph = texture.graph;
    result.node = addSampledFunction(
        texture.graph, texture.slot, sampler, function, args, component);
    return result;
}

template <typename Result, typename Texture>
Result sampleLayerThrough(const Texture& texture,
                          const SamplerHandle& sampler,
                          TextureFunction function,
                          int coordinates,
                          const TextureIndex& layer,
                          const Vector<int>& extras,
                          int component = 0)
{
    auto args = Vector<int> {coordinates, layer.nodeOn(*texture.graph)};

    for (auto extra: extras)
        args.add(extra);

    return sampleThrough<Result>(texture, sampler, function, args, component);
}
} // namespace detail

// textureSample. Fragment stage only.
template <FlatTextureHandle T>
SampledValueOf<T> sample(const T& texture,
                         const SamplerBinding& sampler,
                         const SamplingCoordinatesOf<T>& coordinates)
{
    return detail::sampleThrough<SampledValueOf<T>>(
        texture, sampler, TextureFunction::Sample, {coordinates.node});
}

template <LayeredTextureHandle T>
SampledValueOf<T> sample(const T& texture,
                         const SamplerBinding& sampler,
                         const SamplingCoordinatesOf<T>& coordinates,
                         const TextureIndex& layer)
{
    return detail::sampleLayerThrough<SampledValueOf<T>>(
        texture, sampler, TextureFunction::Sample, coordinates.node, layer, {});
}

// textureSampleLevel, in any stage.
template <FlatTextureHandle T>
SampledValueOf<T> sampleLevel(const T& texture,
                              const SamplerBinding& sampler,
                              const SamplingCoordinatesOf<T>& coordinates,
                              const SamplingLevelOf<T>& level)
{
    return detail::sampleThrough<SampledValueOf<T>>(
        texture,
        sampler,
        TextureFunction::Level,
        {coordinates.node, level.nodeOn(*texture.graph)});
}

template <LayeredTextureHandle T>
SampledValueOf<T> sampleLevel(const T& texture,
                              const SamplerBinding& sampler,
                              const SamplingCoordinatesOf<T>& coordinates,
                              const TextureIndex& layer,
                              const SamplingLevelOf<T>& level)
{
    return detail::sampleLayerThrough<SampledValueOf<T>>(
        texture,
        sampler,
        TextureFunction::Level,
        coordinates.node,
        layer,
        {level.nodeOn(*texture.graph)});
}

// textureSampleBias, on the colour kinds. Fragment stage only.
template <ColourTextureHandle T>
    requires FlatTextureHandle<T>
Float4 sampleBias(const T& texture,
                  const SamplerBinding& sampler,
                  const SamplingCoordinatesOf<T>& coordinates,
                  const TextureScalar& bias)
{
    return detail::sampleThrough<Float4>(
        texture,
        sampler,
        TextureFunction::Bias,
        {coordinates.node, bias.nodeOn(*texture.graph)});
}

template <ColourTextureHandle T>
    requires LayeredTextureHandle<T>
Float4 sampleBias(const T& texture,
                  const SamplerBinding& sampler,
                  const SamplingCoordinatesOf<T>& coordinates,
                  const TextureIndex& layer,
                  const TextureScalar& bias)
{
    return detail::sampleLayerThrough<Float4>(texture,
                                              sampler,
                                              TextureFunction::Bias,
                                              coordinates.node,
                                              layer,
                                              {bias.nodeOn(*texture.graph)});
}

// textureSampleGrad, on the colour kinds, in any stage.
template <ColourTextureHandle T>
    requires FlatTextureHandle<T>
Float4 sampleGrad(const T& texture,
                  const SamplerBinding& sampler,
                  const SamplingCoordinatesOf<T>& coordinates,
                  const SamplingCoordinatesOf<T>& ddx,
                  const SamplingCoordinatesOf<T>& ddy)
{
    return detail::sampleThrough<Float4>(texture,
                                         sampler,
                                         TextureFunction::Grad,
                                         {coordinates.node, ddx.node, ddy.node});
}

template <ColourTextureHandle T>
    requires LayeredTextureHandle<T>
Float4 sampleGrad(const T& texture,
                  const SamplerBinding& sampler,
                  const SamplingCoordinatesOf<T>& coordinates,
                  const TextureIndex& layer,
                  const SamplingCoordinatesOf<T>& ddx,
                  const SamplingCoordinatesOf<T>& ddy)
{
    return detail::sampleLayerThrough<Float4>(texture,
                                              sampler,
                                              TextureFunction::Grad,
                                              coordinates.node,
                                              layer,
                                              {ddx.node, ddy.node});
}

// textureSampleCompare, on the depth kinds. Fragment stage only.
template <DepthTextureHandle T>
    requires FlatTextureHandle<T>
Float sampleCompare(const T& texture,
                    const ComparisonSamplerBinding& sampler,
                    const SamplingCoordinatesOf<T>& coordinates,
                    const TextureScalar& reference)
{
    return detail::sampleThrough<Float>(
        texture,
        sampler,
        TextureFunction::Compare,
        {coordinates.node, reference.nodeOn(*texture.graph)});
}

template <DepthTextureHandle T>
    requires LayeredTextureHandle<T>
Float sampleCompare(const T& texture,
                    const ComparisonSamplerBinding& sampler,
                    const SamplingCoordinatesOf<T>& coordinates,
                    const TextureIndex& layer,
                    const TextureScalar& reference)
{
    return detail::sampleLayerThrough<Float>(texture,
                                             sampler,
                                             TextureFunction::Compare,
                                             coordinates.node,
                                             layer,
                                             {reference.nodeOn(*texture.graph)});
}

// textureSampleCompareLevel: the comparison at level 0, in any stage.
template <DepthTextureHandle T>
    requires FlatTextureHandle<T>
Float sampleCompareLevel(const T& texture,
                         const ComparisonSamplerBinding& sampler,
                         const SamplingCoordinatesOf<T>& coordinates,
                         const TextureScalar& reference)
{
    return detail::sampleThrough<Float>(
        texture,
        sampler,
        TextureFunction::CompareLevel,
        {coordinates.node, reference.nodeOn(*texture.graph)});
}

template <DepthTextureHandle T>
    requires LayeredTextureHandle<T>
Float sampleCompareLevel(const T& texture,
                         const ComparisonSamplerBinding& sampler,
                         const SamplingCoordinatesOf<T>& coordinates,
                         const TextureIndex& layer,
                         const TextureScalar& reference)
{
    return detail::sampleLayerThrough<Float>(texture,
                                             sampler,
                                             TextureFunction::CompareLevel,
                                             coordinates.node,
                                             layer,
                                             {reference.nodeOn(*texture.graph)});
}

// textureGather: one component (0 to 3) of a colour kind's four texels, a
// depth kind's one channel. No volume, as in WGSL.
template <ColourTextureHandle T>
    requires FlatTextureHandle<T> && (!std::derived_from<T, Texture3D>)
Float4 gather(const T& texture,
              const SamplerBinding& sampler,
              const SamplingCoordinatesOf<T>& coordinates,
              int component = 0)
{
    return detail::sampleThrough<Float4>(
        texture, sampler, TextureFunction::Gather, {coordinates.node}, component);
}

template <ColourTextureHandle T>
    requires LayeredTextureHandle<T>
Float4 gather(const T& texture,
              const SamplerBinding& sampler,
              const SamplingCoordinatesOf<T>& coordinates,
              const TextureIndex& layer,
              int component = 0)
{
    return detail::sampleLayerThrough<Float4>(texture,
                                              sampler,
                                              TextureFunction::Gather,
                                              coordinates.node,
                                              layer,
                                              {},
                                              component);
}

template <DepthTextureHandle T>
    requires FlatTextureHandle<T>
Float4 gather(const T& texture,
              const SamplerBinding& sampler,
              const SamplingCoordinatesOf<T>& coordinates)
{
    return detail::sampleThrough<Float4>(
        texture, sampler, TextureFunction::Gather, {coordinates.node});
}

template <DepthTextureHandle T>
    requires LayeredTextureHandle<T>
Float4 gather(const T& texture,
              const SamplerBinding& sampler,
              const SamplingCoordinatesOf<T>& coordinates,
              const TextureIndex& layer)
{
    return detail::sampleLayerThrough<Float4>(
        texture, sampler, TextureFunction::Gather, coordinates.node, layer, {});
}

// textureGatherCompare: a depth kind's four texels compared, each 1 or 0.
template <DepthTextureHandle T>
    requires FlatTextureHandle<T>
Float4 gatherCompare(const T& texture,
                     const ComparisonSamplerBinding& sampler,
                     const SamplingCoordinatesOf<T>& coordinates,
                     const TextureScalar& reference)
{
    return detail::sampleThrough<Float4>(
        texture,
        sampler,
        TextureFunction::GatherCompare,
        {coordinates.node, reference.nodeOn(*texture.graph)});
}

template <DepthTextureHandle T>
    requires LayeredTextureHandle<T>
Float4 gatherCompare(const T& texture,
                     const ComparisonSamplerBinding& sampler,
                     const SamplingCoordinatesOf<T>& coordinates,
                     const TextureIndex& layer,
                     const TextureScalar& reference)
{
    return detail::sampleLayerThrough<Float4>(texture,
                                              sampler,
                                              TextureFunction::GatherCompare,
                                              coordinates.node,
                                              layer,
                                              {reference.nodeOn(*texture.graph)});
}
} // namespace eacp::GPU
