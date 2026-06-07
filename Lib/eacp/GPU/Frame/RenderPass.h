#pragma once

#include <eacp/Core/Utils/Common.h>

namespace eacp::GPU
{
class RenderPipeline;
class Buffer;

// Records draw commands for a single render pass (MTLRenderCommandEncoder on
// Metal). Ends the encoder automatically on destruction. Obtained from
// Frame::beginPass.
class RenderPass
{
public:
    explicit RenderPass(void* encoder);
    ~RenderPass();

    RenderPass(const RenderPass&) = delete;
    RenderPass& operator=(const RenderPass&) = delete;

    void setPipeline(const RenderPipeline& pipeline);
    void setVertexBuffer(const Buffer& buffer, int index = 0);
    void draw(int vertexCount, int firstVertex = 0);

    void end();

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU
