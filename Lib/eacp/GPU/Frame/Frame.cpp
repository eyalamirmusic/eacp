#include "Frame.h"

#include "../CommandBuffer/CopyRecorder.h"

#include <eacp/Core/Utils/Logging.h>

namespace eacp::GPU
{
namespace
{
ColorTarget colorTargetFor(const Texture* texture,
                           const RenderPassDescriptor& descriptor)
{
    return {texture, descriptor.clearColor, descriptor.clear};
}

bool sharesShapeWith(const ColorTarget& target, const ColorTarget& first)
{
    const auto& texture = *target.texture;
    const auto& other = *first.texture;

    return levelWidth(texture, target.level) == levelWidth(other, first.level)
           && levelHeight(texture, target.level) == levelHeight(other, first.level)
           && texture.sampleCount() == other.sampleCount()
           && target.layerCount == first.layerCount;
}

// The level, the slice and the run of layers from it, all inside the texture;
// a volume's run starts at z 0, Metal having no way to begin a layered pass
// part-way through one.
bool namesImagesItHas(const ColorTarget& target)
{
    const auto& texture = *target.texture;

    if (target.level < 0 || target.level >= texture.mipLevels() || target.slice < 0
        || target.layerCount < 1)
        return false;

    if (texture.kind() == TextureType::ThreeD && target.layerCount > 1
        && target.slice != 0)
        return false;

    return target.slice + target.layerCount <= levelSlices(texture, target.level);
}
} // namespace

float Frame::backingScale() const
{
    return scale;
}

Graphics::Point Frame::logicalSize() const
{
    const auto pixels = pixelSize();
    const auto divisor = scale > 0.f ? scale : 1.f;

    return {pixels.x / divisor, pixels.y / divisor};
}

RenderPass Frame::beginPass(const Texture& target,
                            const RenderPassDescriptor& descriptor)
{
    const ColorTarget targets[] = {colorTargetFor(&target, descriptor)};
    return beginMultiTargetPass(std::span<const ColorTarget> {targets}, descriptor);
}

RenderPass Frame::beginMultiTargetPass(std::span<const Texture* const> targets,
                                       const RenderPassDescriptor& descriptor)
{
    if (targets.size() > (std::size_t) maxColorAttachments)
    {
        LOG("eacp: a pass over ",
            targets.size(),
            " targets is refused; the most is ",
            maxColorAttachments);
        return RenderPass(nullptr);
    }

    auto colorTargets = std::array<ColorTarget, maxColorAttachments> {};

    for (auto index = std::size_t {0}; index < targets.size(); ++index)
        colorTargets[index] = colorTargetFor(targets[index], descriptor);

    return beginMultiTargetPass(
        std::span<const ColorTarget> {colorTargets.data(), targets.size()},
        descriptor);
}

bool Frame::canRenderInto(std::span<const ColorTarget> targets)
{
    if (targets.empty() || targets.size() > (std::size_t) maxColorAttachments)
    {
        LOG("eacp: a pass needs between 1 and ",
            maxColorAttachments,
            " colour targets, and was given ",
            targets.size());
        return false;
    }

    for (auto index = std::size_t {0}; index < targets.size(); ++index)
    {
        const auto* texture = targets[index].texture;

        if (texture == nullptr || !texture->isValid() || !texture->isRenderTarget())
        {
            LOG("eacp: colour target ", index, " of a pass is not a render target");
            return false;
        }

        if (!namesImagesItHas(targets[index]))
        {
            LOG("eacp: colour target ",
                index,
                " of a pass names a level, slice or layer run its texture lacks");
            return false;
        }

        if (!sharesShapeWith(targets[index], targets[0]))
        {
            LOG("eacp: colour target ",
                index,
                " of a pass differs from target 0 in size, sample count or layer "
                "count");
            return false;
        }

        for (auto other = std::size_t {0}; other < index; ++other)
        {
            if (targets[other].texture == texture)
            {
                LOG("eacp: a pass names one texture as two colour targets");
                return false;
            }
        }
    }

    return true;
}

void Frame::copy(const Texture& source, const Texture& destination)
{
    for (const auto& plan: planWholeTextureCopy(source, destination))
        recordCopy(nativeRecording(), plan);
}

void Frame::copy(const Texture& source,
                 const Texture& destination,
                 const TextureCopyRegion& region)
{
    if (auto plan = planTextureCopy(source, destination, region))
        recordCopy(nativeRecording(), *plan);
}

void Frame::copy(const Buffer& source, const Buffer& destination)
{
    copy(source, destination, 0, 0, source.size());
}

void Frame::copy(const Buffer& source,
                 const Buffer& destination,
                 std::int64_t sourceOffset,
                 std::int64_t destinationOffset,
                 std::int64_t bytes)
{
    if (auto plan = planBufferCopy(
            source, destination, sourceOffset, destinationOffset, bytes))
        recordCopy(nativeRecording(), *plan);
}

void Frame::copy(const Buffer& source,
                 const Texture& destination,
                 const BufferTextureCopy& layout)
{
    if (auto plan = planBufferTextureCopy(source, destination, layout))
        recordCopyToTexture(nativeRecording(), *plan);
}

void Frame::copy(const Texture& source,
                 const Buffer& destination,
                 const BufferTextureCopy& layout)
{
    if (auto plan = planBufferTextureCopy(destination, source, layout))
        recordCopyToBuffer(nativeRecording(), *plan);
}
} // namespace eacp::GPU
