#pragma once

#include "ShaderValue.h"

#include <concepts>
#include <type_traits>

// WGSL's texture functions past the plain sample, and the kinds that have no
// sample of their own above: a volume, an array of cubes, and the depth plane
// of a cube or cube-array target. Every one is an expression node, so each
// reads wherever the stage declares textures. Sample, Bias and Compare take
// the level from the fragment's derivatives and are fragment-stage only; a
// kernel asserts it has none of them.

namespace eacp::GPU
{
namespace detail
{
struct TextureHandle
{
    ShaderGraph* graph = nullptr;
    int slot = -1;
};
} // namespace detail

// A ThreeD texture, sampled at a Float3 in [0, 1] on all three axes and loaded
// at an Int3. Bound with setFragmentTexture / setInputTexture.
struct Texture3D : detail::TextureHandle
{
};

// A CubeArray texture: a direction and the cube it is looked up in. Bound like
// a cube. The device has to support cube arrays (Device::supportsCubeArrays).
struct TextureCubeArray : detail::TextureHandle
{
};

// The depth plane of a Cube or a CubeArray render target, assigned the target
// and bound with setFragmentDepthTexture / setInputDepthTexture, as a
// TextureDepth2D is. Level 0 of every face.
struct TextureDepthCube : detail::TextureHandle
{
};

struct TextureDepthCubeArray : detail::TextureHandle
{
};

// An integer a texture function takes - a mip level, a layer - as an Int, a
// UInt or an int literal.
class TextureIndex
{
public:
    TextureIndex(const Int& value);
    TextureIndex(const UInt& value);
    TextureIndex(int value);

    int nodeOn(ShaderGraph& graph) const;

private:
    int node = -1;
    int literal = 0;
};

// A float a texture function takes - a level, a bias, a depth reference - as a
// Float or a float literal.
class TextureScalar
{
public:
    TextureScalar(const Float& value);
    TextureScalar(float value);

    int nodeOn(ShaderGraph& graph) const;

private:
    int node = -1;
    float literal = 0.f;
};

// A texel address for load(), signed or unsigned.
class TexelCoordinates2
{
public:
    TexelCoordinates2(const Int2& value);
    TexelCoordinates2(const UInt2& value);

    int node = -1;
};

class TexelCoordinates3
{
public:
    TexelCoordinates3(const Int3& value);
    TexelCoordinates3(const UInt3& value);

    int node = -1;
};

// A handle or the Uniform member holding one.
template <typename T>
concept SampledTextureHandle =
    std::derived_from<T, Texture2D> || std::derived_from<T, TextureCube>
    || std::derived_from<T, TextureDepth2D> || std::derived_from<T, Texture2DArray>
    || std::derived_from<T, TextureDepth2DArray> || std::derived_from<T, Texture3D>
    || std::derived_from<T, TextureCubeArray>
    || std::derived_from<T, TextureDepthCube>
    || std::derived_from<T, TextureDepthCubeArray>;

template <typename T>
concept LayeredTextureHandle =
    std::derived_from<T, Texture2DArray> || std::derived_from<T, TextureDepth2DArray>
    || std::derived_from<T, TextureCubeArray>
    || std::derived_from<T, TextureDepthCubeArray>;

template <typename T>
using TextureSizeOf =
    std::conditional_t<std::derived_from<T, Texture3D>, UInt3, UInt2>;

namespace detail
{
template <typename Result>
Result textureQuery(ShaderGraph* graph, int slot, const char* query, int level)
{
    auto result = Result {};
    result.graph = graph;
    result.node = graph->addTextureQuery(slot, query, level);
    return result;
}
} // namespace detail

// textureDimensions: the width and height (and depth of a volume) of level 0,
// or of `level`. A cube's is one face's.
template <SampledTextureHandle T>
TextureSizeOf<T> dimensions(const T& texture)
{
    return detail::textureQuery<TextureSizeOf<T>>(
        texture.graph, texture.slot, "size", -1);
}

template <SampledTextureHandle T>
TextureSizeOf<T> dimensions(const T& texture, const TextureIndex& level)
{
    return detail::textureQuery<TextureSizeOf<T>>(
        texture.graph, texture.slot, "size", level.nodeOn(*texture.graph));
}

// textureNumLevels: the mip levels the bound view has.
template <SampledTextureHandle T>
UInt levelCount(const T& texture)
{
    return detail::textureQuery<UInt>(texture.graph, texture.slot, "levels", -1);
}

// textureNumLayers: the layers of an array, the cubes of a cube array.
template <LayeredTextureHandle T>
UInt layerCount(const T& texture)
{
    return detail::textureQuery<UInt>(texture.graph, texture.slot, "layers", -1);
}

// textureSample on the kinds that had none. Fragment stage only.
Float4 sample(const Texture3D& texture, const Float3& coordinates);
Float4 sample(const TextureCubeArray& texture,
              const Float3& direction,
              const TextureIndex& layer);
Float sample(const TextureDepthCube& texture, const Float3& direction);
Float sample(const TextureDepthCubeArray& texture,
             const Float3& direction,
             const TextureIndex& layer);

// textureSampleLevel, at a level the shader chooses. A depth kind's level is
// an integer, as WGSL has it.
Float4 sampleLevel(const Texture2D& texture,
                   const Float2& coordinates,
                   const TextureScalar& level);
Float4 sampleLevel(const Texture2DArray& texture,
                   const Float2& coordinates,
                   const TextureIndex& layer,
                   const TextureScalar& level);
Float4 sampleLevel(const Texture3D& texture,
                   const Float3& coordinates,
                   const TextureScalar& level);
Float4 sampleLevel(const TextureCube& texture,
                   const Float3& direction,
                   const TextureScalar& level);
Float4 sampleLevel(const TextureCubeArray& texture,
                   const Float3& direction,
                   const TextureIndex& layer,
                   const TextureScalar& level);
Float sampleLevel(const TextureDepth2D& texture,
                  const Float2& coordinates,
                  const TextureIndex& level);
Float sampleLevel(const TextureDepth2DArray& texture,
                  const Float2& coordinates,
                  const TextureIndex& layer,
                  const TextureIndex& level);
Float sampleLevel(const TextureDepthCube& texture,
                  const Float3& direction,
                  const TextureIndex& level);
Float sampleLevel(const TextureDepthCubeArray& texture,
                  const Float3& direction,
                  const TextureIndex& layer,
                  const TextureIndex& level);

// textureSampleBias: the derived level plus `bias`. Fragment stage only.
Float4 sampleBias(const Texture2D& texture,
                  const Float2& coordinates,
                  const TextureScalar& bias);
Float4 sampleBias(const Texture2DArray& texture,
                  const Float2& coordinates,
                  const TextureIndex& layer,
                  const TextureScalar& bias);
Float4 sampleBias(const Texture3D& texture,
                  const Float3& coordinates,
                  const TextureScalar& bias);
Float4 sampleBias(const TextureCube& texture,
                  const Float3& direction,
                  const TextureScalar& bias);
Float4 sampleBias(const TextureCubeArray& texture,
                  const Float3& direction,
                  const TextureIndex& layer,
                  const TextureScalar& bias);

// textureSampleGrad: the level the given derivatives of the coordinate pick.
Float4 sampleGrad(const Texture2D& texture,
                  const Float2& coordinates,
                  const Float2& ddx,
                  const Float2& ddy);
Float4 sampleGrad(const Texture2DArray& texture,
                  const Float2& coordinates,
                  const TextureIndex& layer,
                  const Float2& ddx,
                  const Float2& ddy);
Float4 sampleGrad(const Texture3D& texture,
                  const Float3& coordinates,
                  const Float3& ddx,
                  const Float3& ddy);
Float4 sampleGrad(const TextureCube& texture,
                  const Float3& direction,
                  const Float3& ddx,
                  const Float3& ddy);
Float4 sampleGrad(const TextureCubeArray& texture,
                  const Float3& direction,
                  const TextureIndex& layer,
                  const Float3& ddx,
                  const Float3& ddy);

// textureSampleCompare on the cube depth kinds (the 2D ones are in
// ShaderValue.h). Fragment stage only; declared with TextureCompare.
Float sampleCompare(const TextureDepthCube& texture,
                    const Float3& direction,
                    const TextureScalar& reference);
Float sampleCompare(const TextureDepthCubeArray& texture,
                    const Float3& direction,
                    const TextureIndex& layer,
                    const TextureScalar& reference);

// textureSampleCompareLevel: the comparison at level 0, in any stage.
Float sampleCompareLevel(const TextureDepth2D& texture,
                         const Float2& coordinates,
                         const TextureScalar& reference);
Float sampleCompareLevel(const TextureDepth2DArray& texture,
                         const Float2& coordinates,
                         const TextureIndex& layer,
                         const TextureScalar& reference);
Float sampleCompareLevel(const TextureDepthCube& texture,
                         const Float3& direction,
                         const TextureScalar& reference);
Float sampleCompareLevel(const TextureDepthCubeArray& texture,
                         const Float3& direction,
                         const TextureIndex& layer,
                         const TextureScalar& reference);

// textureGather: one component (0 to 3, x to w) of the four texels a bilinear
// sample would weigh, in WGSL's order - (umin, vmax), (umax, vmax),
// (umax, vmin), (umin, vmin), v growing down the image - which is every
// backend's own. A depth kind gathers its one channel.
Float4
    gather(const Texture2D& texture, const Float2& coordinates, int component = 0);
Float4 gather(const Texture2DArray& texture,
              const Float2& coordinates,
              const TextureIndex& layer,
              int component = 0);
Float4
    gather(const TextureCube& texture, const Float3& direction, int component = 0);
Float4 gather(const TextureCubeArray& texture,
              const Float3& direction,
              const TextureIndex& layer,
              int component = 0);
Float4 gather(const TextureDepth2D& texture, const Float2& coordinates);
Float4 gather(const TextureDepth2DArray& texture,
              const Float2& coordinates,
              const TextureIndex& layer);
Float4 gather(const TextureDepthCube& texture, const Float3& direction);
Float4 gather(const TextureDepthCubeArray& texture,
              const Float3& direction,
              const TextureIndex& layer);

// textureGatherCompare: those four texels compared, each 1 or 0.
Float4 gatherCompare(const TextureDepth2D& texture,
                     const Float2& coordinates,
                     const TextureScalar& reference);
Float4 gatherCompare(const TextureDepth2DArray& texture,
                     const Float2& coordinates,
                     const TextureIndex& layer,
                     const TextureScalar& reference);
Float4 gatherCompare(const TextureDepthCube& texture,
                     const Float3& direction,
                     const TextureScalar& reference);
Float4 gatherCompare(const TextureDepthCubeArray& texture,
                     const Float3& direction,
                     const TextureIndex& layer,
                     const TextureScalar& reference);

// textureLoad: one texel of one level, no sampler. An address outside the
// level is the caller's bug: WGSL lets it read zero or any texel, and Metal
// leaves it undefined. A cube has no load, in WGSL as here.
Float4 load(const Texture2D& texture,
            const TexelCoordinates2& coordinates,
            const TextureIndex& level);
Float4 load(const Texture2DArray& texture,
            const TexelCoordinates2& coordinates,
            const TextureIndex& layer,
            const TextureIndex& level);
Float4 load(const Texture3D& texture,
            const TexelCoordinates3& coordinates,
            const TextureIndex& level);
Float load(const TextureDepth2D& texture,
           const TexelCoordinates2& coordinates,
           const TextureIndex& level);
Float load(const TextureDepth2DArray& texture,
           const TexelCoordinates2& coordinates,
           const TextureIndex& layer,
           const TextureIndex& level);
} // namespace eacp::GPU
