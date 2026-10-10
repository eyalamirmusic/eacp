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

    std::int64_t rowBytes() const { return (std::int64_t) width * bytesPerTexel; }
};

int textureSlices(const Texture& texture);
int levelWidth(const Texture& texture, int level);
int levelHeight(const Texture& texture, int level);

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

std::optional<BufferTextureCopyPlan> planBufferTextureCopy(
    const Buffer& buffer, const Texture& texture, const BufferTextureCopy& layout);
} // namespace eacp::GPU
