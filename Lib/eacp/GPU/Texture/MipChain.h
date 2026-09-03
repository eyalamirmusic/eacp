#pragma once

#include "Texture.h"

#include <eacp/Core/Utils/Containers.h>

#include <cstdint>

namespace eacp::GPU
{
// How many mip levels a texture of this size has, counting level 0 and stopping
// at 1x1. A 64x16 texture has 7: 64x16, 32x8, 16x4, 8x2, 4x1, 2x1, 1x1 - the
// chain continues halving the larger axis after the smaller one bottoms out,
// which is what both APIs expect and what GetCopyableFootprints assumes.
constexpr int mipLevelCount(int width, int height)
{
    if (width <= 0 || height <= 0)
        return 0;

    auto levels = 1;
    auto largest = width > height ? width : height;

    while (largest > 1)
    {
        largest /= 2;
        ++levels;
    }

    return levels;
}

// One level's dimensions, never below 1.
constexpr int mipExtent(int extent, int level)
{
    const auto shifted = extent >> level;
    return shifted > 1 ? shifted : 1;
}

// The bytes a whole chain of this many levels occupies, tightly packed with
// level 0 first - the layout MipChain holds, and the one
// TextureDescriptor::mipLevels takes. What a caller assembling its own chain
// sizes the block with.
//
// constexpr, as the two above are, so that a caller with a size known at compile
// time can size a buffer with it rather than a magic number.
constexpr std::size_t
    mipChainBytes(TextureFormat format, int width, int height, int levels)
{
    auto total = std::size_t {0};

    for (auto level = 0; level < levels; ++level)
        total +=
            levelBytes(format, mipExtent(width, level), mipExtent(height, level));

    return total;
}

// Whether buildMipChain knows how to average this format, which is the question
// a texture asking to be mipmapped actually turns on. False for every
// block-compressed format: a 4x4 block is a decode, not four numbers to take a
// mean of, so such a texture gets one level rather than lower ones nothing ever
// wrote. Both backends ask this rather than each deciding for itself.
constexpr bool canBuildMipChain(TextureFormat format)
{
    return !isCompressedFormat(format);
}

// Every level of a texture, tightly packed, level 0 first.
//
// **Built on the CPU, and shared by both backends, deliberately.** Metal has
// generateMipmapsForTexture and D3D12 has no equivalent at all - a chain there
// is a compute shader, a UAV per level and a root signature to bind them. So
// the two obvious routes are a GPU chain on Metal against a hand-written one on
// Windows, which is two different filters producing two different pictures for
// the same texture, or one filter on the CPU producing the same bytes for both.
//
// The second is the only one a conformance test can check, and this library has
// been wrong about a cross-backend detail often enough to prefer the version
// that can be checked. The cost is that a mipmapped upload moves 4/3 of the
// pixels rather than 1, once, when the texture is created.
//
// **It is the default rather than the only way**: TextureDescriptor::mipLevels
// takes a chain the caller built, in exactly this layout, and the filter
// argument above still holds where it does - the two backends upload the same
// bytes because they were handed the same bytes. What that field is for is the
// two cases this cannot serve at all: a compressed texture, whose blocks have
// no average, and a caller whose own filter differs on purpose.
//
// Level 0 is a repacked copy of the source rather than a pointer into it, so
// the backends upload every level through one loop with one set of indices.
// One memcpy of a texture that is about to cross the bus anyway is not the
// expensive part of this.
struct MipChain
{
    bool isValid() const { return !levels.empty(); }
    int levelCount() const { return levels.size(); }

    const void* level(int index) const { return levels[index].data(); }

    Vector<Vector<std::uint8_t>> levels;
};

// Halves each level into the next by averaging 2x2 blocks, down to 1x1.
//
// Rows of `pixels` are tightly packed unless bytesPerRow gives a larger stride,
// matching Texture::update. Returns an empty chain for a format canBuildMipChain
// refuses, which is what keeps a texture from being created with levels full of
// nothing.
//
// The average is of the stored values, not of light: an 8-bit texture holding
// sRGB is averaged in sRGB, which darkens a mip slightly against the physically
// correct answer. That is what every GPU's own mip generator does, eacp has no
// sRGB formats to distinguish the two cases with, and doing otherwise here
// would make eacp's mips disagree with every other tool that produced them.
MipChain buildMipChain(const void* pixels,
                       int width,
                       int height,
                       TextureFormat format,
                       std::size_t bytesPerRow = 0);
} // namespace eacp::GPU
