#include "ComputePass.h"

#include "../Buffer/Buffer.h"
#include "../Pipeline/ComputePipeline.h"
#include "../WebGPU/WebGPUTypes.h"

#include <memory>

namespace eacp::GPU
{
namespace
{
std::uint32_t groupsFor(int count, int size)
{
    const auto width = static_cast<std::uint32_t>(size > 0 ? size : 1);

    return (static_cast<std::uint32_t>(count) + width - 1) / width;
}
} // namespace

// WebGPU orders every dispatch after the one before it that wrote what it
// reads, so DispatchOrder and barrier() have nothing to record here.
struct ComputePass::Native
{
    Native(void* encoderHandle, DispatchOrder)
        : encoder(static_cast<WebComputeEncoder*>(encoderHandle))
    {
    }

    ~Native() { releaseGroup(); }

    bool canRecord() const
    {
        return encoder != nullptr && encoder->pass != nullptr && pipeline != nullptr;
    }

    WGPUComputePassEncoder pass() const { return encoder->pass; }

    bool bindGroup()
    {
        const auto& layout = pipeline->bindings;

        if (layout.entries.empty())
            return true;

        auto group = makeWebBindGroup(layout, pipeline->groupLayout, bound);

        if (group == nullptr)
            return false;

        releaseGroup();
        currentGroup = group;

        const auto offset = webUniformOffset(layout, bound);

        wgpuComputePassEncoderSetBindGroup(
            pass(), 0, currentGroup, layout.hasUniforms() ? 1 : 0, &offset);

        return true;
    }

    void releaseGroup()
    {
        if (currentGroup != nullptr)
            wgpuBindGroupRelease(currentGroup);

        currentGroup = nullptr;
    }

    void bindBuffer(const BufferRange& range, int slot)
    {
        if (encoder == nullptr || slot < 0 || slot >= webMaxBufferSlots)
            return;

        const auto bind = webStorageBufferBind(range);

        if (bind.isValid())
            bound.buffers[slot] = bind;
    }

    std::unique_ptr<WebComputeEncoder> encoder;
    const WebComputePipeline* pipeline = nullptr;
    WebBoundResources bound;
    WGPUBindGroup currentGroup = nullptr;
};

ComputePass::ComputePass(void* encoder, DispatchOrder order)
    : impl(encoder, order)
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

    if (!impl->encoder || impl->encoder->pass == nullptr)
        return;

    auto* state = static_cast<WebComputePipeline*>(pipeline.nativeState());

    if (state == nullptr || !state->isValid())
        return;

    boundPipeline = true;
    impl->pipeline = state;
    wgpuComputePassEncoderSetPipeline(impl->pass(), state->pipeline);
}

void ComputePass::setInputBuffer(const Buffer& buffer, int slot)
{
    setInputBuffer(BufferRange::of(buffer), slot);
}

void ComputePass::setInputBuffer(const BufferRange& range, int slot)
{
    impl->bindBuffer(range, slot);
}

void ComputePass::setOutputBuffer(const Buffer& buffer, int slot)
{
    setOutputBuffer(BufferRange::of(buffer), slot);
}

void ComputePass::setOutputBuffer(const BufferRange& range, int slot)
{
    impl->bindBuffer(range, slot);
}

void ComputePass::setInputTexture(const Texture& texture,
                                  int slot,
                                  TextureSampling sampling)
{
    if (!impl->encoder || slot < 0 || slot >= maxTextureSlots)
        return;

    auto* data = static_cast<WebTextureData*>(texture.nativeTexture());

    if (data == nullptr || !data->isValid())
        return;

    impl->bound.textures[slot] = {data->sampledView, sampling};
}

void ComputePass::setOutputTexture(const Texture& texture, int slot)
{
    if (!impl->encoder || slot < 0 || slot >= maxTextureSlots)
        return;

    auto* data = static_cast<WebTextureData*>(texture.nativeTexture());

    if (data == nullptr || !data->isValid() || !data->isComputeWritable())
        return;

    impl->bound.textures[slot] = {data->storageView, {}};
}

void ComputePass::setBytes(const void* data, std::int64_t bytes, int slot)
{
    if (!impl->encoder || bytes <= 0 || slot != 0)
        return;

    impl->bound.uniforms = impl->encoder->context->uploadUniforms(
        data, static_cast<std::size_t>(bytes));
}

void ComputePass::dispatch(int count)
{
    if (!impl->canRecord() || !boundPipeline || count <= 0 || !impl->bindGroup())
        return;

    wgpuComputePassEncoderDispatchWorkgroups(
        impl->pass(), groupsFor(count, groupFor1D().x), 1, 1);
}

void ComputePass::dispatch(int width, int height)
{
    if (!impl->canRecord() || !boundPipeline || width <= 0 || height <= 0
        || !impl->bindGroup())
        return;

    const auto group = groupFor2D();

    wgpuComputePassEncoderDispatchWorkgroups(
        impl->pass(), groupsFor(width, group.x), groupsFor(height, group.y), 1);
}

void ComputePass::dispatch(int width, int height, int depth)
{
    if (!impl->canRecord() || !boundPipeline || width <= 0 || height <= 0
        || depth <= 0 || !impl->bindGroup())
        return;

    const auto group = groupFor3D();

    wgpuComputePassEncoderDispatchWorkgroups(impl->pass(),
                                             groupsFor(width, group.x),
                                             groupsFor(height, group.y),
                                             groupsFor(depth, group.z));
}

void ComputePass::dispatchIndirect(const Buffer& arguments,
                                   std::int64_t offsetInBytes)
{
    if (!impl->canRecord() || !boundPipeline || offsetInBytes < 0
        || offsetInBytes % 4 != 0
        || offsetInBytes
               > arguments.size() - (std::int64_t) sizeof(DispatchArguments))
        return;

    auto* data = static_cast<WebBufferData*>(arguments.nativeBuffer());

    if (data == nullptr || data->buffer == nullptr || !impl->bindGroup())
        return;

    wgpuComputePassEncoderDispatchWorkgroupsIndirect(
        impl->pass(), data->buffer, static_cast<std::uint64_t>(offsetInBytes));
}

void ComputePass::barrier() {}

void ComputePass::end()
{
    if (impl->encoder && impl->encoder->pass != nullptr)
    {
        wgpuComputePassEncoderEnd(impl->pass());
        wgpuComputePassEncoderRelease(impl->pass());
    }

    impl->releaseGroup();
    impl->encoder.reset();
    impl->pipeline = nullptr;
}
} // namespace eacp::GPU
