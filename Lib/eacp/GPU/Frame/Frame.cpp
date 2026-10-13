#include "Frame.h"

#include "../CommandBuffer/CopyRecorder.h"
#include "../Device/Device.h"

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

// Two targets on one texture are two attachments when they name different
// levels, or runs of slices that do not meet: two layers of an array, two faces
// of a cube, two images of a volume. A multisampled texture has one image.
bool overlaps(const ColorTarget& a, const ColorTarget& b)
{
    if (a.texture != b.texture)
        return false;

    if (a.texture->sampleCount() > 1)
        return true;

    if (a.level != b.level)
        return false;

    return a.slice < b.slice + b.layerCount && b.slice < a.slice + a.layerCount;
}

// A single-sampled target with a depth plane, at an image it has, the size of
// the first colour target's level; the run is the first target's.
bool canTakeDepthFrom(const ColorTarget& depth, const ColorTarget& first)
{
    const auto& texture = *depth.texture;

    return texture.isValid() && texture.isRenderTarget() && texture.hasDepth()
           && texture.sampleCount() == 1 && first.texture->sampleCount() == 1
           && namesImagesItHas(depth)
           && levelWidth(texture, depth.level)
                  == levelWidth(*first.texture, first.level)
           && levelHeight(texture, depth.level)
                  == levelHeight(*first.texture, first.level);
}
} // namespace

Frame::Nesting::Nesting(Device& deviceToUse, Frame& frame)
    : device(&deviceToUse)
    , place(deviceToUse.enterFrame(frame))
{
}

Frame::Nesting::~Nesting()
{
    device->leaveFrame(place);
}

Frame::Redirect::Redirect(Frame& frameToUse,
                          const Texture& target,
                          float backingScaleToUse,
                          std::string_view timingLabel)
    : frame(frameToUse)
    , previousTarget(frameToUse.redirectTarget)
    , previousScale(frameToUse.scale)
    , previousTimingLabel(
          frameToUse.nesting.device->frameTimer().setScope(timingLabel))
{
    frame.redirectTarget = &target;
    frame.scale = backingScaleToUse;
}

Frame::Redirect::~Redirect()
{
    frame.redirectTarget = previousTarget;
    frame.scale = previousScale;
    frame.nesting.device->frameTimer().setScope(previousTimingLabel);
}

bool Frame::isRedirected() const
{
    return redirectTarget != nullptr;
}

Graphics::Point Frame::pixelSize() const
{
    if (redirectTarget == nullptr)
        return nativePixelSize();

    return {(float) redirectTarget->width(), (float) redirectTarget->height()};
}

RenderPass Frame::beginPass(const RenderPassDescriptor& descriptor)
{
    if (redirectTarget == nullptr)
        return beginNativePass(descriptor);

    return beginPass(*redirectTarget, descriptor);
}

TextureFormat Frame::drawableFormat() const
{
    if (redirectTarget == nullptr)
        return nativeDrawableFormat();

    return redirectTarget->format();
}

bool Frame::copyDrawable(const Texture& destination)
{
    if (redirectTarget == nullptr)
        return copyNativeDrawable(destination);

    if (!isValid() || !canCopyDrawableInto(destination))
        return false;

    copy(*redirectTarget, destination, TextureCopyRegion::wholeLevel(0));
    return true;
}

bool Frame::isValid() const
{
    if (redirectTarget == nullptr)
        return isNativeValid();

    return nativeRecording() != nullptr && redirectTarget->isValid()
           && redirectTarget->isRenderTarget();
}

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

OcclusionQueries* Frame::occlusionQueriesFor(const RenderPassDescriptor& descriptor)
{
    auto* queries = descriptor.occlusionQueries;

    if (queries == nullptr)
        return nullptr;

    if (!queries->isValid())
    {
        LOG("eacp: a pass names an OcclusionQueries that is not valid; its "
            "queries are refused");
        return nullptr;
    }

    return queries;
}

ColorTarget Frame::depthSource(std::span<const ColorTarget> targets,
                               const RenderPassDescriptor& descriptor)
{
    auto source = targets[0];
    const auto& depth = descriptor.depthTarget;

    if (depth.texture != nullptr)
    {
        source.texture = depth.texture;
        source.level = depth.level;
        source.slice = depth.slice;
    }

    return source;
}

bool Frame::canRenderInto(std::span<const ColorTarget> targets,
                          const RenderPassDescriptor& descriptor)
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
            if (overlaps(targets[other], targets[index]))
            {
                LOG("eacp: a pass names one image of a texture as two colour "
                    "targets");
                return false;
            }
        }
    }

    if (descriptor.depthTarget.texture != nullptr
        && !canTakeDepthFrom(depthSource(targets, descriptor), targets[0]))
    {
        LOG("eacp: a pass's depth target is not a single-sampled render target "
            "with depth at an image of the pass's size");
        return false;
    }

    return true;
}

bool Frame::canCopyDrawableInto(const Texture& destination) const
{
    const auto size = pixelSize();

    if (!destination.isValid() || destination.kind() != TextureType::TwoD
        || destination.sampleCount() != 1)
    {
        LOG("eacp: copyDrawable needs a valid single-sampled 2D texture");
        return false;
    }

    if (destination.format() != drawableFormat())
    {
        LOG("eacp: copyDrawable needs a texture of the drawable's format, "
            "Frame::drawableFormat()");
        return false;
    }

    if ((float) destination.width() != size.x
        || (float) destination.height() != size.y)
    {
        LOG("eacp: copyDrawable needs a texture of the drawable's size, "
            "Frame::pixelSize()");
        return false;
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

void Frame::generateMipmaps(const Texture& texture)
{
    if (planMipGeneration(texture))
        recordMipGeneration(nativeRecording(), texture);
}
} // namespace eacp::GPU
