#include "RenderPass.h"

#include "../Pipeline/RenderPipeline.h"
#include "../Vulkan/VulkanTypes.h"

// Linux/Vulkan placeholder. Stage 3 of the Linux plan replaces this file with
// the real thing: descriptor writes and barriers per bind, the scissor clamped
// with outward rounding, the viewport with a negative height (the one axis
// Vulkan differs on - see the plan's coordinate section), vkCmdSetCullMode and
// vkCmdSetFrontFace on every setPipeline, and the draw calls.
//
// Until then a pass records nothing. It is never handed a real encoder - Frame
// hands it null (Frame-Linux.cpp) - so every entry point below is reached only
// by a caller that got a pass it cannot draw with, and the honest answer is to
// drop the command rather than to pretend it was recorded.
//
// The target size is still reported, because it is what the caller passed in
// rather than anything about a pass: portable code sizes a scissor or a
// projection from it, and answering zero there would be a second, different
// wrongness on top of the missing draw.

namespace eacp::GPU
{
struct RenderPass::Native
{
    Native(void*, int width, int height)
        : targetWidth(width)
        , targetHeight(height)
    {
    }

    int targetWidth = 0;
    int targetHeight = 0;
};

RenderPass::RenderPass(void* encoder, int targetWidth, int targetHeight)
    : impl(encoder, targetWidth, targetHeight)
{
}

RenderPass::~RenderPass()
{
    end();
}

void RenderPass::setScissorRect(const Graphics::Rect&) {}

void RenderPass::clearScissorRect() {}

void RenderPass::setViewport(const Graphics::Rect&, float, float) {}

void RenderPass::clearViewport() {}

int RenderPass::targetWidth() const
{
    return impl->targetWidth;
}

int RenderPass::targetHeight() const
{
    return impl->targetHeight;
}

void RenderPass::setPipeline(const RenderPipeline&) {}

void RenderPass::setStencilReference(unsigned int) {}

void RenderPass::setVertexBuffer(const Buffer&, int) {}

void RenderPass::setVertexBuffer(const BufferRange&, int) {}

void RenderPass::setFragmentTexture(const Texture&, int, TextureSampling) {}

void RenderPass::setFragmentDepthTexture(const Texture&, int, TextureSampling) {}

void RenderPass::setVertexStorageBuffer(const Buffer&, int) {}

void RenderPass::setFragmentStorageBuffer(const Buffer&, int) {}

void RenderPass::setVertexBytes(const void*, std::size_t, int) {}

void RenderPass::setFragmentBytes(const void*, std::size_t, int) {}

void RenderPass::draw(int, int) {}

void RenderPass::drawInstanced(int, int, int, int) {}

void RenderPass::drawIndexed(const Buffer&, int, IndexFormat, int, int) {}

void RenderPass::drawIndexed(const BufferRange&, int, IndexFormat, int, int) {}

void RenderPass::drawIndexedInstanced(
    const Buffer&, int, int, IndexFormat, int, int, int)
{
}

void RenderPass::drawIndexedInstanced(
    const BufferRange&, int, int, IndexFormat, int, int, int)
{
}

void RenderPass::end()
{
    // Still drained, even though nothing is recorded. A participant is told
    // exactly once that its last chance has come, and a batching renderer that
    // is never told keeps its queue for the next pass and draws it twice - so
    // the rule is the same whether or not the pass has an encoder.
    drainParticipants();
}
} // namespace eacp::GPU
