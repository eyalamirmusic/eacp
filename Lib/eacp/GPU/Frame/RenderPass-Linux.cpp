#include "RenderPass.h"

#include "../Buffer/Buffer.h"
#include "../Linux/GPUBackend-Linux.h"
#include "../Pipeline/RenderPipeline.h"
#include "../Texture/Texture.h"

namespace eacp::GPU
{
// The encoder the Frame handed over is the backend itself - the Frame's own
// backend made it - so a pass never asks which backend it is on.
struct RenderPass::Native
{
    explicit Native(void* passBackend)
        : backend(static_cast<RenderPassBackend*>(passBackend))
    {
    }

    std::unique_ptr<RenderPassBackend> backend;
};

RenderPass::RenderPass(void* encoder, int, int)
    : impl(encoder)
{
}

RenderPass::~RenderPass()
{
    end();
}

void RenderPass::setScissorRect(const Graphics::Rect& rect)
{
    if (impl->backend != nullptr)
        impl->backend->setScissorRect(rect);
}

void RenderPass::clearScissorRect()
{
    if (impl->backend != nullptr)
        impl->backend->clearScissorRect();
}

void RenderPass::setViewport(const Graphics::Rect& rect,
                             float nearDepth,
                             float farDepth)
{
    if (impl->backend != nullptr)
        impl->backend->setViewport(rect, nearDepth, farDepth);
}

void RenderPass::clearViewport()
{
    if (impl->backend != nullptr)
        impl->backend->clearViewport();
}

int RenderPass::targetWidth() const
{
    return impl->backend != nullptr ? impl->backend->targetWidth() : 0;
}

int RenderPass::targetHeight() const
{
    return impl->backend != nullptr ? impl->backend->targetHeight() : 0;
}

void RenderPass::setPipeline(const RenderPipeline& pipeline)
{
    if (impl->backend != nullptr)
        impl->backend->setPipeline(pipeline);
}

void RenderPass::setStencilReference(unsigned int value)
{
    if (impl->backend != nullptr)
        impl->backend->setStencilReference(value);
}

void RenderPass::setVertexBuffer(const Buffer& buffer, int index)
{
    setVertexBuffer(BufferRange::of(buffer), index);
}

void RenderPass::setVertexBuffer(const BufferRange& range, int index)
{
    if (impl->backend != nullptr)
        impl->backend->setVertexBuffer(range, index);
}

void RenderPass::setFragmentTexture(const Texture& texture,
                                    int slot,
                                    TextureSampling sampling)
{
    if (impl->backend != nullptr)
        impl->backend->setFragmentTexture(texture, slot, sampling);
}

void RenderPass::setFragmentDepthTexture(const Texture& renderTarget,
                                         int slot,
                                         TextureSampling sampling)
{
    if (impl->backend != nullptr)
        impl->backend->setFragmentDepthTexture(renderTarget, slot, sampling);
}

void RenderPass::setVertexStorageBuffer(const Buffer& buffer, int slot)
{
    setVertexStorageBuffer(BufferRange::of(buffer), slot);
}

void RenderPass::setVertexStorageBuffer(const BufferRange& range, int slot)
{
    if (impl->backend != nullptr)
        impl->backend->setStorageBuffer(range, slot);
}

void RenderPass::setFragmentStorageBuffer(const Buffer& buffer, int slot)
{
    setFragmentStorageBuffer(BufferRange::of(buffer), slot);
}

void RenderPass::setFragmentStorageBuffer(const BufferRange& range, int slot)
{
    if (impl->backend != nullptr)
        impl->backend->setStorageBuffer(range, slot);
}

void RenderPass::setVertexBytes(const void* data, int bytes, int slot)
{
    if (impl->backend != nullptr)
        impl->backend->setBytes(data, bytes, slot);
}

void RenderPass::setFragmentBytes(const void* data, int bytes, int slot)
{
    if (impl->backend != nullptr)
        impl->backend->setBytes(data, bytes, slot);
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
    if (impl->backend != nullptr)
        impl->backend->drawInstanced(
            vertexCount, instanceCount, firstVertex, firstInstance);
}

void RenderPass::drawIndexed(const Buffer& indices,
                             int indexCount,
                             IndexFormat format,
                             int firstIndex,
                             int baseVertex)
{
    drawIndexed(
        BufferRange::of(indices), indexCount, format, firstIndex, baseVertex);
}

void RenderPass::drawIndexed(const BufferRange& indices,
                             int indexCount,
                             IndexFormat format,
                             int firstIndex,
                             int baseVertex)
{
    drawIndexedInstanced(indices, indexCount, 1, format, firstIndex, 0, baseVertex);
}

void RenderPass::drawIndexedInstanced(const Buffer& indices,
                                      int indexCount,
                                      int instanceCount,
                                      IndexFormat format,
                                      int firstIndex,
                                      int firstInstance,
                                      int baseVertex)
{
    drawIndexedInstanced(BufferRange::of(indices),
                         indexCount,
                         instanceCount,
                         format,
                         firstIndex,
                         firstInstance,
                         baseVertex);
}

void RenderPass::drawIndexedInstanced(const BufferRange& indices,
                                      int indexCount,
                                      int instanceCount,
                                      IndexFormat format,
                                      int firstIndex,
                                      int firstInstance,
                                      int baseVertex)
{
    if (impl->backend != nullptr)
        impl->backend->drawIndexedInstanced(indices,
                                            indexCount,
                                            instanceCount,
                                            format,
                                            firstIndex,
                                            firstInstance,
                                            baseVertex);
}

void RenderPass::end()
{
    // Before the pass is ended: a participant's queued draws are still draws.
    drainParticipants();

    if (impl->backend != nullptr)
        impl->backend->end();
}
} // namespace eacp::GPU
