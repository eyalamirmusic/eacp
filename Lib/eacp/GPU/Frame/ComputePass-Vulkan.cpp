#include "ComputePass.h"

#include "../Buffer/Buffer.h"
#include "../Pipeline/ComputePipeline.h"

// See ComputePipeline-Vulkan.cpp: compute waits on descriptor-set plumbing.

namespace eacp::GPU
{
struct ComputePass::Native
{
};

ComputePass::ComputePass(void*)
    : impl()
{
}

ComputePass::~ComputePass() = default;

void ComputePass::setPipeline(const ComputePipeline&) {}
void ComputePass::setInputBuffer(const Buffer&, int) {}
void ComputePass::setOutputBuffer(const Buffer&, int) {}
void ComputePass::setBytes(const void*, std::size_t, int) {}
void ComputePass::dispatch(int) {}
void ComputePass::end() {}
} // namespace eacp::GPU
