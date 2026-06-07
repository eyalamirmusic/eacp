#include "RenderPass.h"

#include "../Buffer/Buffer.h"
#include "../Pipeline/RenderPipeline.h"

// Windows/DirectX stub. See Device-Windows.cpp.

namespace eacp::GPU
{
struct RenderPass::Native
{
};

RenderPass::RenderPass(void*)
    : impl()
{
}

RenderPass::~RenderPass() = default;

void RenderPass::setPipeline(const RenderPipeline&) {}

void RenderPass::setVertexBuffer(const Buffer&, int) {}

void RenderPass::draw(int, int) {}

void RenderPass::end() {}
} // namespace eacp::GPU
