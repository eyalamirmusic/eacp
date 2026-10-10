#include "CopyPlan.h"

#include <eacp/Core/Utils/Logging.h>

#include <algorithm>

namespace eacp::GPU
{
namespace
{
bool isWithin(int origin, int extent, int limit)
{
    return origin >= 0 && extent > 0 && origin + extent <= limit;
}

bool hasLevelAndSlice(const Texture& texture, int level, int slice)
{
    return level >= 0 && level < texture.mipLevels() && slice >= 0
           && slice < textureSlices(texture);
}

bool isFlat(const TextureOrigin& origin, const TextureExtent& size)
{
    return origin.z == 0 && size.depth == 1;
}

int resolvedExtent(int requested, int origin, int levelExtent)
{
    return requested > 0 ? requested : levelExtent - origin;
}

bool coversWholeLevel(const Texture& texture, int level, int x, int y, int w, int h)
{
    return x == 0 && y == 0 && w == levelWidth(texture, level)
           && h == levelHeight(texture, level);
}
} // namespace

int textureSlices(const Texture& texture)
{
    return texture.isCube() ? 6 : 1;
}

int levelWidth(const Texture& texture, int level)
{
    return std::max(1, texture.width() >> level);
}

int levelHeight(const Texture& texture, int level)
{
    return std::max(1, texture.height() >> level);
}

std::optional<TextureCopyPlan> planTextureCopy(const Texture& source,
                                               const Texture& destination,
                                               const TextureCopyRegion& region)
{
    if (!source.isValid() || !destination.isValid())
    {
        LOG("eacp: a texture copy names an invalid texture");
        return {};
    }

    if (source.format() != destination.format())
    {
        LOG("eacp: a texture copy is refused between two formats");
        return {};
    }

    if (!hasLevelAndSlice(source, region.sourceLevel, region.sourceSlice)
        || !hasLevelAndSlice(
            destination, region.destinationLevel, region.destinationSlice))
    {
        LOG("eacp: a texture copy names a level or slice the texture lacks");
        return {};
    }

    if (!isFlat(region.sourceOrigin, region.size) || region.destinationOrigin.z != 0)
    {
        LOG("eacp: a texture copy with depth needs 3D textures, which eacp lacks");
        return {};
    }

    auto plan = TextureCopyPlan {};
    plan.source = &source;
    plan.destination = &destination;
    plan.sourceLevel = region.sourceLevel;
    plan.sourceSlice = region.sourceSlice;
    plan.sourceX = region.sourceOrigin.x;
    plan.sourceY = region.sourceOrigin.y;
    plan.destinationLevel = region.destinationLevel;
    plan.destinationSlice = region.destinationSlice;
    plan.destinationX = region.destinationOrigin.x;
    plan.destinationY = region.destinationOrigin.y;
    plan.width = resolvedExtent(
        region.size.width, plan.sourceX, levelWidth(source, plan.sourceLevel));
    plan.height = resolvedExtent(
        region.size.height, plan.sourceY, levelHeight(source, plan.sourceLevel));

    if (!isWithin(plan.sourceX, plan.width, levelWidth(source, plan.sourceLevel))
        || !isWithin(
            plan.sourceY, plan.height, levelHeight(source, plan.sourceLevel))
        || !isWithin(plan.destinationX,
                     plan.width,
                     levelWidth(destination, plan.destinationLevel))
        || !isWithin(plan.destinationY,
                     plan.height,
                     levelHeight(destination, plan.destinationLevel)))
    {
        LOG("eacp: a texture copy region leaves the source or destination level");
        return {};
    }

    if (isCompressedFormat(source.format())
        && (!coversWholeLevel(source,
                              plan.sourceLevel,
                              plan.sourceX,
                              plan.sourceY,
                              plan.width,
                              plan.height)
            || !coversWholeLevel(destination,
                                 plan.destinationLevel,
                                 plan.destinationX,
                                 plan.destinationY,
                                 plan.width,
                                 plan.height)))
    {
        LOG("eacp: a compressed texture is copied only a whole level at a time");
        return {};
    }

    if (&source == &destination)
    {
        LOG("eacp: a texture copy within one texture is refused");
        return {};
    }

    return plan;
}

Vector<TextureCopyPlan> planWholeTextureCopy(const Texture& source,
                                             const Texture& destination)
{
    auto plans = Vector<TextureCopyPlan> {};

    if (source.width() != destination.width()
        || source.height() != destination.height()
        || source.mipLevels() != destination.mipLevels()
        || source.isCube() != destination.isCube())
    {
        LOG("eacp: a whole-texture copy needs two textures of one size, level "
            "count and shape");
        return plans;
    }

    for (auto level = 0; level < source.mipLevels(); ++level)
    {
        for (auto slice = 0; slice < textureSlices(source); ++slice)
        {
            auto plan = planTextureCopy(
                source, destination, TextureCopyRegion::wholeLevel(level, slice));

            if (!plan)
                return {};

            plans.add(*plan);
        }
    }

    return plans;
}

std::optional<BufferCopyPlan> planBufferCopy(const Buffer& source,
                                             const Buffer& destination,
                                             std::int64_t sourceOffset,
                                             std::int64_t destinationOffset,
                                             std::int64_t bytes)
{
    if (!source.isValid() || !destination.isValid())
    {
        LOG("eacp: a buffer copy names an invalid buffer");
        return {};
    }

    if (bytes <= 0 || sourceOffset < 0 || destinationOffset < 0
        || sourceOffset + bytes > source.size()
        || destinationOffset + bytes > destination.size())
    {
        LOG("eacp: a buffer copy leaves the source or destination buffer");
        return {};
    }

    if (sourceOffset % 4 != 0 || destinationOffset % 4 != 0 || bytes % 4 != 0)
    {
        LOG("eacp: a buffer copy's offsets and length must be multiples of four");
        return {};
    }

    if (&source == &destination)
    {
        LOG("eacp: a buffer copy within one buffer is refused");
        return {};
    }

    return BufferCopyPlan {
        &source, &destination, sourceOffset, destinationOffset, bytes};
}

std::optional<BufferTextureCopyPlan> planBufferTextureCopy(
    const Buffer& buffer, const Texture& texture, const BufferTextureCopy& layout)
{
    if (!buffer.isValid() || !texture.isValid())
    {
        LOG("eacp: a buffer-texture copy names an invalid resource");
        return {};
    }

    if (isCompressedFormat(texture.format()))
    {
        LOG("eacp: a buffer-texture copy of a compressed texture is refused");
        return {};
    }

    if (!hasLevelAndSlice(texture, layout.level, layout.slice))
    {
        LOG("eacp: a buffer-texture copy names a level or slice the texture lacks");
        return {};
    }

    if (!isFlat(layout.origin, layout.size))
    {
        LOG("eacp: a buffer-texture copy with depth needs 3D textures, which eacp "
            "lacks");
        return {};
    }

    auto plan = BufferTextureCopyPlan {};
    plan.buffer = &buffer;
    plan.texture = &texture;
    plan.bufferOffset = layout.bufferOffset;
    plan.bytesPerTexel = bytesPerPixel(texture.format());
    plan.level = layout.level;
    plan.slice = layout.slice;
    plan.x = layout.origin.x;
    plan.y = layout.origin.y;
    plan.width =
        resolvedExtent(layout.size.width, plan.x, levelWidth(texture, plan.level));
    plan.height =
        resolvedExtent(layout.size.height, plan.y, levelHeight(texture, plan.level));
    plan.bytesPerRow =
        layout.bytesPerRow > 0 ? layout.bytesPerRow : (int) plan.rowBytes();

    if (!isWithin(plan.x, plan.width, levelWidth(texture, plan.level))
        || !isWithin(plan.y, plan.height, levelHeight(texture, plan.level)))
    {
        LOG("eacp: a buffer-texture copy region leaves the texture level");
        return {};
    }

    if (plan.bytesPerRow < plan.rowBytes()
        || plan.bytesPerRow % plan.bytesPerTexel != 0 || plan.bufferOffset < 0
        || plan.bufferOffset % plan.bytesPerTexel != 0 || plan.bufferOffset % 4 != 0)
    {
        LOG("eacp: a buffer-texture copy's row pitch or offset is off the texel "
            "grid");
        return {};
    }

    const auto lastByte = plan.bufferOffset
                          + (std::int64_t) plan.bytesPerRow * (plan.height - 1)
                          + plan.rowBytes();

    if (lastByte > buffer.size())
    {
        LOG("eacp: a buffer-texture copy runs past the end of the buffer");
        return {};
    }

    return plan;
}
} // namespace eacp::GPU
