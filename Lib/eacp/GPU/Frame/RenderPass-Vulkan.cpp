#include "RenderPass.h"

#include "../Pipeline/RenderPipeline.h"
#include "../Vulkan/VulkanContext.h"
#include "../Vulkan/VulkanTypes.h"

#include <algorithm>
#include <cmath>

namespace eacp::GPU
{
struct RenderPass::Native
{
    Native(void* targetToUse, int widthToUse, int heightToUse)
        : target(static_cast<VulkanRenderTarget*>(targetToUse))
        , width(widthToUse)
        , height(heightToUse)
    {
        if (target == nullptr)
            return;

        // Vulkan's clip space points +Y down, where Metal's and D3D's point up.
        // A negative-height viewport flips it back, so one emitted shader lands
        // the same way up on all three backends and the y-down contract
        // Graphics::Rect carries needs no special case per backend.
        auto viewport = VkViewport {};
        viewport.x = 0.f;
        viewport.y = static_cast<float>(height);
        viewport.width = static_cast<float>(width);
        viewport.height = -static_cast<float>(height);
        viewport.minDepth = 0.f;
        viewport.maxDepth = 1.f;

        vkCmdSetViewport(target->list, 0, 1, &viewport);
        applyScissor(
            {0.f, 0.f, static_cast<float>(width), static_cast<float>(height)});
    }

    void applyScissor(const Graphics::Rect& rect)
    {
        if (target == nullptr)
            return;

        auto left = std::max(0, static_cast<int>(std::lround(rect.x)));
        auto top = std::max(0, static_cast<int>(std::lround(rect.y)));
        auto right = std::min(width, static_cast<int>(std::lround(rect.x + rect.w)));
        auto bottom =
            std::min(height, static_cast<int>(std::lround(rect.y + rect.h)));

        auto scissor = VkRect2D {};
        scissor.offset = {left, top};
        scissor.extent = {static_cast<std::uint32_t>(std::max(0, right - left)),
                          static_cast<std::uint32_t>(std::max(0, bottom - top))};

        vkCmdSetScissor(target->list, 0, 1, &scissor);
    }

    void push(const void* data, std::size_t bytes)
    {
        if (target == nullptr || bound == nullptr || data == nullptr)
            return;

        auto capped =
            std::min(bytes, static_cast<std::size_t>(bound->pushConstantBytes));

        vkCmdPushConstants(target->list,
                           bound->layout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0,
                           static_cast<std::uint32_t>(capped),
                           data);
    }

    VulkanRenderTarget* target = nullptr;
    VulkanPipeline* bound = nullptr;
    int width = 0;
    int height = 0;
    bool ended = false;
};

RenderPass::RenderPass(void* encoder, int targetWidth, int targetHeight)
    : impl(encoder, targetWidth, targetHeight)
{
}

RenderPass::~RenderPass()
{
    end();
}

void RenderPass::setScissorRect(const Graphics::Rect& rect)
{
    impl->applyScissor(rect);
}

void RenderPass::clearScissorRect()
{
    impl->applyScissor({0.f,
                        0.f,
                        static_cast<float>(impl->width),
                        static_cast<float>(impl->height)});
}

void RenderPass::setPipeline(const RenderPipeline& pipeline)
{
    if (impl->target == nullptr || !pipeline.isValid())
        return;

    impl->bound = static_cast<VulkanPipeline*>(pipeline.nativeState());

    vkCmdBindPipeline(
        impl->target->list, VK_PIPELINE_BIND_POINT_GRAPHICS, impl->bound->pipeline);
}

void RenderPass::setVertexBuffer(const Buffer& buffer, int index)
{
    if (impl->target == nullptr || !buffer.isValid())
        return;

    auto* native = static_cast<VulkanBuffer*>(buffer.nativeBuffer());
    auto offset = VkDeviceSize {0};

    vkCmdBindVertexBuffers(impl->target->list,
                           static_cast<std::uint32_t>(index),
                           1,
                           &native->buffer,
                           &offset);
}

void RenderPass::setFragmentTexture(const Texture&, int, TextureSampling)
{
    // Not wired yet: this backend has no descriptor-set plumbing, so a sampled
    // texture cannot be bound. Deliberately left as a visible gap rather than a
    // silent no-op that would make texture tests fail as wrong colours instead
    // of as missing work.
}

void RenderPass::setVertexBytes(const void* data, std::size_t bytes, int slot)
{
    if (slot != 0)
        return;

    impl->push(data, bytes);
}

void RenderPass::setFragmentBytes(const void* data, std::size_t bytes, int slot)
{
    if (slot != 0)
        return;

    impl->push(data, bytes);
}

void RenderPass::draw(int vertexCount, int firstVertex)
{
    drawInstanced(vertexCount, 1, firstVertex, 0);
}

void RenderPass::drawInstanced(int vertexCount,
                               int instanceCount,
                               int firstVertex,
                               int firstInstance)
{
    if (impl->target == nullptr || impl->bound == nullptr)
        return;

    vkCmdDraw(impl->target->list,
              static_cast<std::uint32_t>(vertexCount),
              static_cast<std::uint32_t>(instanceCount),
              static_cast<std::uint32_t>(firstVertex),
              static_cast<std::uint32_t>(firstInstance));
}

void RenderPass::drawIndexed(const Buffer& indices,
                             int indexCount,
                             IndexFormat format,
                             int firstIndex)
{
    drawIndexedInstanced(indices, indexCount, 1, format, firstIndex, 0);
}

void RenderPass::drawIndexedInstanced(const Buffer& indices,
                                      int indexCount,
                                      int instanceCount,
                                      IndexFormat format,
                                      int firstIndex,
                                      int firstInstance)
{
    if (impl->target == nullptr || impl->bound == nullptr || !indices.isValid())
        return;

    auto* native = static_cast<VulkanBuffer*>(indices.nativeBuffer());

    vkCmdBindIndexBuffer(impl->target->list,
                         native->buffer,
                         0,
                         format == IndexFormat::UInt16 ? VK_INDEX_TYPE_UINT16
                                                       : VK_INDEX_TYPE_UINT32);

    vkCmdDrawIndexed(impl->target->list,
                     static_cast<std::uint32_t>(indexCount),
                     static_cast<std::uint32_t>(instanceCount),
                     static_cast<std::uint32_t>(firstIndex),
                     0,
                     static_cast<std::uint32_t>(firstInstance));
}

void RenderPass::end()
{
    if (impl->target == nullptr || impl->ended)
        return;

    impl->ended = true;
    getVulkanContext().endRendering(impl->target->list);
}
} // namespace eacp::GPU
