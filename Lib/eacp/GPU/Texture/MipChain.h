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
int mipLevelCount(int width, int height);

// One level's dimensions, never below 1.
int mipExtent(int extent, int level);

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
// matching Texture::update. Returns an empty chain for a format this does not
// know how to average, which is what keeps a texture from being created with
// levels full of nothing.
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
