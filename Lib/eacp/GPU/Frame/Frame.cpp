#include "Frame.h"

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

bool sharesShapeWith(const Texture& texture, const Texture& first)
{
    return texture.width() == first.width() && texture.height() == first.height()
           && texture.sampleCount() == first.sampleCount();
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

        if (!sharesShapeWith(*texture, *targets[0].texture))
        {
            LOG("eacp: colour target ",
                index,
                " of a pass differs from target 0 in size or sample count");
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
} // namespace eacp::GPU
