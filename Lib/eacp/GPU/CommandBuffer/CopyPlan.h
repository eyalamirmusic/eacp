#pragma once

#include "Copy.h"

#include "../Buffer/Buffer.h"
#include "../Texture/Texture.h"

#include <eacp/Core/Utils/Containers.h>

#include <optional>

namespace eacp::GPU
{
// A copy checked against both resources and reduced to numbers every backend
// records as they stand: no zero sizes left to resolve, nothing out of range.
// Built by the plan functions below, which trace and return nothing for a copy
// they refuse, so a backend never has to.
struct TextureCopyPlan
{
    const Texture* source = nullptr;
    const Texture* destination = nullptr;

    int sourceLevel = 0;
    int sourceSlice = 0;
    int sourceX = 0;
    int sourceY = 0;

    int destinationLevel = 0;
    int destinationSlice = 0;
    int destinationX = 0;
    int destinationY = 0;

    int width = 0;
    int height = 0;

    // A volume's images: z on each side and how many. 0, 0 and 1 elsewhere.
    int sourceZ = 0;
    int destinationZ = 0;
    int depth = 1;

    // Consecutive slices from each side's, the same box of each.
    int sliceCount = 1;
};

struct BufferCopyPlan
{
    const Buffer* source = nullptr;
    const Buffer* destination = nullptr;

    std::int64_t sourceOffset = 0;
    std::int64_t destinationOffset = 0;
    std::int64_t bytes = 0;
};

struct BufferTextureCopyPlan
{
    const Buffer* buffer = nullptr;
    const Texture* texture = nullptr;

    std::int64_t bufferOffset = 0;
    int bytesPerRow = 0;
    int bytesPerTexel = 0;

    int level = 0;
    int slice = 0;
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    std::int64_t rowBytes() const;
};

int textureSlices(const Texture& texture);
int levelWidth(const Texture& texture, int level);
int levelHeight(const Texture& texture, int level);

// The 2D images a level holds: its slices, or a volume's images at that level.
int levelSlices(const Texture& texture, int level);

std::optional<TextureCopyPlan> planTextureCopy(const Texture& source,
                                               const Texture& destination,
                                               const TextureCopyRegion& region);

Vector<TextureCopyPlan> planWholeTextureCopy(const Texture& source,
                                             const Texture& destination);

std::optional<BufferCopyPlan> planBufferCopy(const Buffer& source,
                                             const Buffer& destination,
                                             std::int64_t sourceOffset,
                                             std::int64_t destinationOffset,
                                             std::int64_t bytes);

// Whether Frame::generateMipmaps has work to record for `texture`: true for a
// valid texture of more than one level in a format the device generates a
// chain for (Device::supportsMipGeneration). A single level is nothing to do,
// silently; anything else is refused with a trace.
bool planMipGeneration(const Texture& texture);

std::optional<BufferTextureCopyPlan> planBufferTextureCopy(
    const Buffer& buffer, const Texture& texture, const BufferTextureCopy& layout);
} // namespace eacp::GPU
