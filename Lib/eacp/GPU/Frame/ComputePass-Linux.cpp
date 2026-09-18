#include "ComputePass.h"

#include "../Buffer/Buffer.h"
#include "../Linux/GPUBackend-Linux.h"
#include "../Pipeline/ComputePipeline.h"

namespace eacp::GPU
{
// The encoder a Frame or a CommandBuffer handed over is the backend itself:
// whichever of them opened the recording made it, so the pass never asks which
// backend it is on.
struct ComputePass::Native
{
    explicit Native(void* passBackend)
        : backend(static_cast<ComputePassBackend*>(passBackend))
    {
    }

    std::unique_ptr<ComputePassBackend> backend;
};

// The order was taken when the backend was made, so it is not kept here.
ComputePass::ComputePass(void* encoder, DispatchOrder)
    : impl(encoder)
{
}

ComputePass::~ComputePass()
{
    end();
}

void ComputePass::setPipeline(const ComputePipeline& pipeline)
{
    boundGroup = pipeline.threadGroupShape();
    boundPipeline = false;

    if (impl->backend == nullptr)
        return;

    boundPipeline = impl->backend->setPipeline(pipeline);
}

void ComputePass::setInputBuffer(const Buffer& buffer, int slot)
{
    setInputBuffer(BufferRange::of(buffer), slot);
}

void ComputePass::setInputBuffer(const BufferRange& range, int slot)
{
    if (impl->backend != nullptr)
        impl->backend->setInputBuffer(range, slot);
}

void ComputePass::setOutputBuffer(const Buffer& buffer, int slot)
{
    setOutputBuffer(BufferRange::of(buffer), slot);
}

void ComputePass::setOutputBuffer(const BufferRange& range, int slot)
{
    if (impl->backend != nullptr)
        impl->backend->setOutputBuffer(range, slot);
}

void ComputePass::setInputTexture(const Texture& texture,
                                  int slot,
                                  TextureSampling sampling)
{
    if (impl->backend != nullptr)
        impl->backend->setInputTexture(texture, slot, sampling);
}

void ComputePass::setOutputTexture(const Texture& texture, int slot)
{
    if (impl->backend != nullptr)
        impl->backend->setOutputTexture(texture, slot);
}

void ComputePass::setBytes(const void* data, std::int64_t bytes, int slot)
{
    if (impl->backend != nullptr)
        impl->backend->setBytes(data, bytes, slot);
}

void ComputePass::dispatch(int count)
{
    if (impl->backend == nullptr || !boundPipeline || count <= 0)
        return;

    impl->backend->dispatch(count, 1, 1, groupFor1D());
}

void ComputePass::dispatch(int width, int height)
{
    if (impl->backend == nullptr || !boundPipeline || width <= 0 || height <= 0)
        return;

    impl->backend->dispatch(width, height, 1, groupFor2D());
}

void ComputePass::dispatch(int width, int height, int depth)
{
    if (impl->backend == nullptr || !boundPipeline || width <= 0 || height <= 0
        || depth <= 0)
        return;

    impl->backend->dispatch(width, height, depth, groupFor3D());
}

void ComputePass::dispatchIndirect(const Buffer& arguments,
                                   std::int64_t offsetInBytes)
{
    if (impl->backend == nullptr || !boundPipeline || offsetInBytes < 0
        || offsetInBytes % 4 != 0
        || offsetInBytes
               > arguments.size() - (std::int64_t) sizeof(DispatchArguments))
        return;

    impl->backend->dispatchIndirect(arguments, offsetInBytes);
}

void ComputePass::barrier()
{
    if (impl->backend != nullptr)
        impl->backend->barrier();
}

void ComputePass::end()
{
    if (impl->backend != nullptr)
        impl->backend->end();
}
} // namespace eacp::GPU
