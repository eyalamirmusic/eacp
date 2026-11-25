#include "ComputePass.h"

#include "../Buffer/Buffer.h"
#include "../Codegen/ShaderBindings.h"
#include "../Pipeline/ComputePipeline.h"
#include "../Texture/Sampler.h"
#include "../WebGPU/WebGPUContext.h"

#include <algorithm>

namespace eacp::GPU
{
namespace
{
WebGPUTextureData* boundTextureDataOf(const Texture& texture)
{
    auto* data = static_cast<WebGPUTextureData*>(texture.nativeTexture());
    return data != nullptr && data->isValid() ? data : nullptr;
}

bool isFiltering(const SamplerDescriptor& descriptor)
{
    return descriptor.minFilter == SamplerFilter::Linear
           || descriptor.magFilter == SamplerFilter::Linear
           || descriptor.mipFilter == SamplerMipFilter::Linear;
}

bool isFiltering(TextureSampling sampling)
{
    return sampling.filter == TextureFilter::Linear;
}
} // namespace

struct ComputePass::Native
{
    Native(void* encoderHandle, DispatchOrder)
        : encoder(static_cast<WebGPUComputeEncoder*>(encoderHandle))
        , contextPointer(encoder != nullptr ? encoder->context : nullptr)
    {
    }

    bool canRecord() const { return encoder != nullptr && pipeline != nullptr; }

    bool isSlot(int slot, int limit) const
    {
        return encoder != nullptr && slot >= 0 && slot < limit;
    }

    void bindBuffer(const BufferRange& range, int slot)
    {
        if (!isSlot(slot, maxBufferSlots) || range.buffer == nullptr)
            return;

        auto* data = static_cast<WebGPUBufferData*>(range.buffer->nativeBuffer());

        if (data == nullptr || data->buffer == nullptr || range.offset < 0
            || static_cast<std::uint64_t>(range.offset) >= data->size)
            return;

        const auto offset = static_cast<std::uint64_t>(range.offset);

        if (contextPointer == nullptr || !contextPointer->isStorageOffset(offset))
            return;

        const auto available = data->size - offset;
        const auto wanted =
            range.bytes > 0 ? static_cast<std::uint64_t>(range.bytes) : available;
        const auto size = alignTo(std::min(wanted, available), 4);

        bindings.bindBuffer(webgpuBufferBinding(slot), data->buffer, offset, size);
    }

    void bindSampled(const wgpu::TextureView& view,
                     int slot,
                     const wgpu::Sampler& sampler,
                     bool filtering,
                     bool filterable = true)
    {
        if (!isSlot(slot, maxTextureSlots) || view == nullptr)
            return;

        bindings.bindView(webgpuTextureBinding(slot), view, filterable);
        bindings.bindSampler(webgpuSamplerBinding(slot), sampler, filtering);
    }

    WebGPUContext& context() const { return *contextPointer; }

    void noteStorageFormat(int binding, wgpu::TextureFormat format)
    {
        for (auto& bound: storageFormats)
        {
            if (bound.binding == binding)
            {
                bound.format = format;
                return;
            }
        }

        storageFormats.create(binding, format);
    }

    wgpu::TextureFormat boundStorageFormat(int binding) const
    {
        for (const auto& bound: storageFormats)
            if (bound.binding == binding)
                return bound.format;

        return wgpu::TextureFormat::Undefined;
    }

    // The bound pipeline, or its variant for the storage formats bound where
    // they differ from the ones its WGSL declares, and for the float textures
    // bound that WebGPU will not filter.
    const WebGPUComputePipeline* pipelineForBoundFormats() const
    {
        auto key = WebGPUComputeVariantKey {};
        key.unfilterable = bindings.unfilterableBindings(pipeline->layout.bindings);

        if (differingStorageFormats(key.storageFormats) || !key.unfilterable.empty())
            return pipeline->variantFor(key);

        return pipeline;
    }

    bool differingStorageFormats(Vector<WebGPUStorageFormat>& wanted) const
    {
        auto differs = false;

        for (const auto& binding: pipeline->layout.bindings)
        {
            if (binding.kind != WebGPUBindingKind::StorageTexture)
                continue;

            const auto bound = boundStorageFormat(binding.binding);

            if (bound == wgpu::TextureFormat::Undefined)
                continue;

            wanted.create(binding.binding, bound);
            differs = differs || bound != binding.storageFormat;
        }

        if (!differs)
            wanted.clear();

        return differs;
    }

    // WebGPU puts a dispatch's indirect buffer in the dispatch's usage scope
    // beside its bindings, and one buffer may not be both indirect and writable
    // storage in a scope. Where the arguments live in a buffer the dispatch
    // also binds read_write, the pass ends, the arguments are copied into the
    // context's scratch buffer, and a new pass begins for the dispatch to read
    // them from there; the copy lands after every dispatch recorded before it,
    // so it reads what they wrote. The pipeline and bind group are set again
    // by the next prepareDispatch.
    wgpu::Buffer splitAroundArgumentCopy(const wgpu::Buffer& arguments,
                                         std::uint64_t offset)
    {
        const auto& scratch = context().getIndirectScratch();

        if (scratch == nullptr || encoder->recording == nullptr)
            return {};

        encoder->pass.End();
        encoder->recording->encoder.CopyBufferToBuffer(
            arguments, offset, scratch, 0, sizeof(DispatchArguments));
        encoder->pass = encoder->recording->encoder.BeginComputePass();

        active = nullptr;
        group = nullptr;
        return scratch;
    }

    bool prepareDispatch()
    {
        if (!canRecord())
            return false;

        const auto* target = pipelineForBoundFormats();

        if (target == nullptr || target->pipeline == nullptr)
            return false;

        if (target != active)
        {
            active = target;
            group = nullptr;
            encoder->pass.SetPipeline(target->pipeline);
        }

        if (bindings.isDirty() || group == nullptr)
        {
            group = bindings.makeGroup(context(), active->layout);

            if (group == nullptr && active->layout.group != nullptr)
                return false;

            if (group != nullptr)
                encoder->pass.SetBindGroup(0, group, 0, nullptr);
        }

        return true;
    }

    OwningPointer<WebGPUComputeEncoder> encoder;
    WebGPUContext* contextPointer = nullptr;
    const WebGPUComputePipeline* pipeline = nullptr;
    const WebGPUComputePipeline* active = nullptr;
    WebGPUBindingTable bindings;
    Vector<WebGPUStorageFormat> storageFormats;
    wgpu::BindGroup group;
};

ComputePass::ComputePass(void* encoder, DispatchOrder order)
    : impl(encoder, order)
{
}

ComputePass::~ComputePass()
{
    end();
}

void ComputePass::encodePipeline(const ComputePipeline& pipeline)
{
    boundGroup = pipeline.threadGroupShape();
    boundPipeline = false;

    if (!impl->encoder)
        return;

    auto* state = static_cast<WebGPUComputePipeline*>(pipeline.nativeState());

    if (state == nullptr || state->pipeline == nullptr)
        return;

    boundPipeline = true;

    if (impl->pipeline != state)
        impl->group = nullptr;

    impl->pipeline = state;
    impl->active = state;
    impl->encoder->pass.SetPipeline(state->pipeline);
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
    auto* data = boundTextureDataOf(texture);

    if (data == nullptr || impl->contextPointer == nullptr)
        return;

    impl->bindSampled(webgpuSampledView(*data),
                      slot,
                      impl->context().getSampler(sampling),
                      isFiltering(sampling),
                      impl->context().isFilterable(data->format));
}

void ComputePass::setInputTexture(const Texture& texture,
                                  int slot,
                                  const Sampler& sampler)
{
    auto* data = boundTextureDataOf(texture);

    if (data == nullptr || !sampler.isValid() || impl->contextPointer == nullptr)
        return;

    auto native = wgpu::Sampler {static_cast<WGPUSampler>(sampler.nativeSampler())};
    impl->bindSampled(webgpuSampledView(*data),
                      slot,
                      native,
                      isFiltering(sampler.descriptor()),
                      impl->context().isFilterable(data->format));
}

void ComputePass::setSampler(const Sampler& sampler, int slot)
{
    if (!impl->isSlot(slot, maxTextureSlots) || !sampler.isValid())
        return;

    auto native = wgpu::Sampler {static_cast<WGPUSampler>(sampler.nativeSampler())};
    impl->bindings.bindSampler(
        webgpuSamplerBinding(slot), native, isFiltering(sampler.descriptor()));
}

void ComputePass::setOutputTexture(const Texture& texture, int slot)
{
    setOutputTextureLevel(texture, slot, 0);
}

void ComputePass::setInputTextureLevel(const Texture& texture,
                                       int slot,
                                       int level,
                                       TextureSampling sampling)
{
    auto* data = boundTextureDataOf(texture);

    if (data == nullptr || level < 0 || level >= data->mipLevels
        || impl->contextPointer == nullptr)
        return;

    impl->bindSampled(webgpuSampledView(*data, level),
                      slot,
                      impl->context().getSampler(sampling),
                      isFiltering(sampling),
                      impl->context().isFilterable(data->format));
}

void ComputePass::setOutputTextureLevel(const Texture& texture, int slot, int level)
{
    auto* data = boundTextureDataOf(texture);

    if (!impl->isSlot(slot, maxTextureSlots) || data == nullptr
        || !data->computeWrite || level < 0 || level >= data->mipLevels)
        return;

    impl->bindings.bindView(webgpuTextureBinding(slot),
                            webgpuStorageView(*data, level));
    impl->noteStorageFormat(webgpuTextureBinding(slot), data->format);
}

void ComputePass::setInputTextureAsArray(const Texture& texture,
                                         int slot,
                                         int level,
                                         TextureSampling sampling)
{
    auto* data = boundTextureDataOf(texture);

    if (data == nullptr || level < 0 || level >= data->mipLevels
        || impl->contextPointer == nullptr)
        return;

    impl->bindSampled(webgpuSampledView(*data, level, true),
                      slot,
                      impl->context().getSampler(sampling),
                      isFiltering(sampling),
                      impl->context().isFilterable(data->format));
}

// A cube's storage view is a 2D array of its slices already.
void ComputePass::setOutputTextureAsArray(const Texture& texture,
                                          int slot,
                                          int level)
{
    const auto kind = texture.kind();

    if (kind == TextureType::TwoDArray || kind == TextureType::Cube
        || kind == TextureType::CubeArray)
        setOutputTextureLevel(texture, slot, level);
}

void ComputePass::setInputDepthTexture(const Texture& renderTarget,
                                       int slot,
                                       TextureSampling sampling)
{
    setInputDepthTextureSlice(renderTarget, -1, slot, sampling);
}

void ComputePass::setInputDepthTexture(const Texture& renderTarget,
                                       int slot,
                                       const Sampler& sampler)
{
    setInputDepthTextureSlice(renderTarget, -1, slot, sampler);
}

void ComputePass::setInputDepthTextureSlice(const Texture& renderTarget,
                                            int slice,
                                            int slot,
                                            TextureSampling sampling)
{
    auto* data = boundTextureDataOf(renderTarget);

    if (data == nullptr || data->depth == nullptr || impl->contextPointer == nullptr)
        return;

    impl->bindSampled(webgpuDepthView(*data, slice),
                      slot,
                      impl->context().getSampler(sampling),
                      isFiltering(sampling));
}

void ComputePass::setInputDepthTextureSlice(const Texture& renderTarget,
                                            int slice,
                                            int slot,
                                            const Sampler& sampler)
{
    auto* data = boundTextureDataOf(renderTarget);

    if (data == nullptr || data->depth == nullptr || !sampler.isValid())
        return;

    auto native = wgpu::Sampler {static_cast<WGPUSampler>(sampler.nativeSampler())};
    impl->bindSampled(webgpuDepthView(*data, slice),
                      slot,
                      native,
                      isFiltering(sampler.descriptor()));
}

void ComputePass::setBytes(const void* data, std::int64_t bytes, int slot)
{
    if (!impl->encoder || bytes <= 0 || slot != 0 || impl->contextPointer == nullptr)
        return;

    auto uniforms = impl->context().uploadUniforms(
        *impl->encoder->recording, data, static_cast<std::size_t>(bytes));

    if (uniforms.isValid())
        impl->bindings.bindBuffer(
            webgpuUniformBinding, uniforms.buffer, uniforms.offset, uniforms.size);
}

void ComputePass::encodeDispatch(int count)
{
    if (!boundPipeline || count <= 0 || !impl->prepareDispatch())
        return;

    const auto width = static_cast<std::uint32_t>(groupFor1D().x);
    const auto groups = (static_cast<std::uint32_t>(count) + width - 1) / width;

    // WebGPU caps each grid dimension, so a long 1D dispatch folds into rows;
    // the emitted kernel reads its index linearly across them, and its bounds
    // guard drops the tail of the last row.
    const auto limit = std::max<std::uint32_t>(
        impl->context().getLimits().maxComputeWorkgroupsPerDimension, 1);
    const auto columns = std::min(groups, limit);
    const auto rows = (groups + columns - 1) / columns;

    impl->encoder->pass.DispatchWorkgroups(columns, rows, 1);
}

void ComputePass::encodeDispatch(int width, int height)
{
    if (!boundPipeline || width <= 0 || height <= 0 || !impl->prepareDispatch())
        return;

    const auto group = groupFor2D();
    const auto sizeX = static_cast<std::uint32_t>(group.x);
    const auto sizeY = static_cast<std::uint32_t>(group.y);

    impl->encoder->pass.DispatchWorkgroups(
        (static_cast<std::uint32_t>(width) + sizeX - 1) / sizeX,
        (static_cast<std::uint32_t>(height) + sizeY - 1) / sizeY,
        1);
}

void ComputePass::encodeDispatch(int width, int height, int depth)
{
    if (!boundPipeline || width <= 0 || height <= 0 || depth <= 0
        || !impl->prepareDispatch())
        return;

    const auto group = groupFor3D();
    const auto sizeX = static_cast<std::uint32_t>(group.x);
    const auto sizeY = static_cast<std::uint32_t>(group.y);
    const auto sizeZ = static_cast<std::uint32_t>(group.z);

    impl->encoder->pass.DispatchWorkgroups(
        (static_cast<std::uint32_t>(width) + sizeX - 1) / sizeX,
        (static_cast<std::uint32_t>(height) + sizeY - 1) / sizeY,
        (static_cast<std::uint32_t>(depth) + sizeZ - 1) / sizeZ);
}

void ComputePass::encodeDispatchIndirect(const Buffer& arguments,
                                         std::int64_t offsetInBytes)
{
    if (!boundPipeline || offsetInBytes < 0 || offsetInBytes % 4 != 0
        || offsetInBytes
               > arguments.size() - (std::int64_t) sizeof(DispatchArguments))
        return;

    auto* data = static_cast<WebGPUBufferData*>(arguments.nativeBuffer());

    if (data == nullptr || data->buffer == nullptr || !impl->prepareDispatch())
        return;

    auto source = data->buffer;
    auto offset = static_cast<std::uint64_t>(offsetInBytes);

    if (impl->bindings.bindsWritable(impl->active->layout, source))
    {
        source = impl->splitAroundArgumentCopy(source, offset);
        offset = 0;

        if (source == nullptr || !impl->prepareDispatch())
            return;
    }

    impl->encoder->pass.DispatchWorkgroupsIndirect(source, offset);
}

// Each dispatch is its own usage scope, so WebGPU already orders a dispatch
// after the writes of the one before it.
void ComputePass::encodeBarrier() {}

void ComputePass::end()
{
    if (impl->encoder)
    {
        impl->encoder->pass.End();

        if (impl->encoder->recording != nullptr)
            impl->encoder->recording->passOpen = false;
    }

    impl->encoder.reset();
    impl->pipeline = nullptr;
}
} // namespace eacp::GPU
