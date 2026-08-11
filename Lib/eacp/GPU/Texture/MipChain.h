#pragma once

#include "Texture.h"

#include <eacp/Core/Utils/Containers.h>

#include <cstdint>

namespace eacp::GPU
{
// Counts level 0 and stops at 1x1, halving the larger axis on after the
// smaller bottoms out - what both APIs and GetCopyableFootprints assume.
int mipLevelCount(int width, int height);

// One level's dimensions, never below 1.
int mipExtent(int extent, int level);

// Every level of a texture, tightly packed, level 0 first. Built on the CPU and
// shared by both backends so they produce identical bytes; D3D12 has no
// generateMipmapsForTexture equivalent to match Metal's against.
struct MipChain
{
    bool isValid() const { return !levels.empty(); }
    int levelCount() const { return levels.size(); }

    const void* level(int index) const { return levels[index].data(); }

    Vector<Vector<std::uint8_t>> levels;
};

// Halves each level into the next by averaging 2x2 blocks of stored values (not
// light, as every GPU's own generator does). Rows of `pixels` are tightly packed
// unless bytesPerRow says otherwise; an unsupported format returns nothing.
MipChain buildMipChain(const void* pixels,
                       int width,
                       int height,
                       TextureFormat format,
                       std::size_t bytesPerRow = 0);
} // namespace eacp::GPU
