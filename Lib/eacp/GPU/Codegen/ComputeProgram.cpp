#include "ComputeProgram.h"
#include "../Device/Device.h"
#include "../Frame/ComputePass.h"

#include <eacp/Core/Utils/Logging.h>

namespace eacp::GPU
{
Uniform<InputBuffer>& Uniform<InputBuffer>::operator=(const Buffer& newBuffer)
{
    value = BufferRange::of(newBuffer);
    return *this;
}

Uniform<OutputBuffer>& Uniform<OutputBuffer>::operator=(const Buffer& newBuffer)
{
    value = BufferRange::of(newBuffer);
    return *this;
}

Uniform<UIntInputBuffer>&
    Uniform<UIntInputBuffer>::operator=(const Buffer& newBuffer)
{
    value = BufferRange::of(newBuffer);
    return *this;
}

Uniform<UIntOutputBuffer>&
    Uniform<UIntOutputBuffer>::operator=(const Buffer& newBuffer)
{
    value = BufferRange::of(newBuffer);
    return *this;
}

Uniform<IntInputBuffer>& Uniform<IntInputBuffer>::operator=(const Buffer& newBuffer)
{
    value = BufferRange::of(newBuffer);
    return *this;
}

Uniform<IntOutputBuffer>&
    Uniform<IntOutputBuffer>::operator=(const Buffer& newBuffer)
{
    value = BufferRange::of(newBuffer);
    return *this;
}

Uniform<HalfInputBuffer>&
    Uniform<HalfInputBuffer>::operator=(const Buffer& newBuffer)
{
    value = BufferRange::of(newBuffer);
    return *this;
}

Uniform<HalfOutputBuffer>&
    Uniform<HalfOutputBuffer>::operator=(const Buffer& newBuffer)
{
    value = BufferRange::of(newBuffer);
    return *this;
}

Uniform<AtomicBuffer>& Uniform<AtomicBuffer>::operator=(const Buffer& newBuffer)
{
    value = BufferRange::of(newBuffer);
    return *this;
}

Uniform<IntAtomicBuffer>&
    Uniform<IntAtomicBuffer>::operator=(const Buffer& newBuffer)
{
    value = BufferRange::of(newBuffer);
    return *this;
}

ComputeProgram::ComputeProgram() = default;

ComputeProgram::~ComputeProgram() = default;

namespace
{
// Resource bind walk: hand each assigned buffer and texture member to the
// compute pass at the slot its handle was declared with. One walk rather than
// one per resource kind - the members are visited in declaration order either
// way, and the slots are already carried by the handles.
class ComputeBindVisitor final : public ShaderVisitor
{
public:
    explicit ComputeBindVisitor(ComputePass& passToUse);

    void onUniform(const char*,
                   ValueType,
                   detail::ValueHandle&,
                   const void*) override;

    void onInputBuffer(const char*,
                       InputBuffer& handle,
                       const BufferRange& range) override;

    void onOutputBuffer(const char*,
                        OutputBuffer& handle,
                        const BufferRange& range) override;

    // The integer buffers bind through the same two calls the float ones do:
    // what the elements are is settled by the kernel's declaration, not by how
    // the pass hands the buffer over.
    void onUIntInputBuffer(const char*,
                           UIntInputBuffer& handle,
                           const BufferRange& range) override;

    void onUIntOutputBuffer(const char*,
                            UIntOutputBuffer& handle,
                            const BufferRange& range) override;

    void onIntInputBuffer(const char*,
                          IntInputBuffer& handle,
                          const BufferRange& range) override;

    void onIntOutputBuffer(const char*,
                           IntOutputBuffer& handle,
                           const BufferRange& range) override;

    void onHalfInputBuffer(const char*,
                           HalfInputBuffer& handle,
                           const BufferRange& range) override;

    void onHalfOutputBuffer(const char*,
                            HalfOutputBuffer& handle,
                            const BufferRange& range) override;

    // An atomic buffer binds exactly as an output does - a Metal device buffer,
    // a D3D UAV - since what makes it atomic is the type the kernel declares it
    // through and not how the pass hands it over.
    void onAtomicBuffer(const char*,
                        AtomicBuffer& handle,
                        const BufferRange& range) override;

    void onIntAtomicBuffer(const char*,
                           IntAtomicBuffer& handle,
                           const BufferRange& range) override;

    void onStructBuffer(const char*,
                        BufferAccess access,
                        detail::StructBufferHandle& handle,
                        int (*)(ShaderGraph&),
                        const BufferRange& range) override;

    void onTexture(const char*,
                   Texture2D& handle,
                   const Texture* texture,
                   TextureSampling sampling) override;

    // The same call the 2D one takes, for the reason the render bind visitor
    // gives: a cube is one texture on one slot of one index space on both
    // backends, and its dimensionality was settled when it was created and when
    // the kernel was compiled.
    void onCubeTexture(const char*,
                       TextureCube& handle,
                       const Texture* texture,
                       TextureSampling sampling) override;

    void onArrayTexture(const char*,
                        Texture2DArray& handle,
                        const Texture* texture,
                        TextureSampling sampling) override;

    void onDepthTexture(const char*,
                        TextureDepth2D& handle,
                        const Texture* renderTarget,
                        TextureSampling sampling) override;

    void onDepthArrayTexture(const char*,
                             TextureDepth2DArray& handle,
                             const Texture* renderTarget,
                             TextureSampling sampling) override;

    void onTextureOfKind(const char*,
                         TextureKind kind,
                         detail::TextureHandle& handle,
                         const Texture* texture,
                         TextureSampling sampling) override;

    void onWritableTexture(const char*,
                           WritableTexture2D& handle,
                           const Texture* texture) override;

    // Read through the read binding with no filtering, Write and ReadWrite
    // through the write one; a cube or a cube array as the 2D array of its
    // faces, at level 0.
    void onStorageTexture(const char*,
                          const StorageTextureLayout& layout,
                          detail::TextureHandle& handle,
                          const Texture* texture) override;

    void onSampler(const char* name,
                   detail::SamplerHandle& handle,
                   bool comparison,
                   const Sampler& sampler) override;

private:
    ComputePass& pass;
};

constexpr auto texelSampling =
    TextureSampling {TextureFilter::Nearest, TextureAddressMode::Clamp};

ComputeBindVisitor::ComputeBindVisitor(ComputePass& passToUse)
    : pass(passToUse)
{
}

void ComputeBindVisitor::onUniform(const char*,
                                   ValueType,
                                   detail::ValueHandle&,
                                   const void*)
{
}

void ComputeBindVisitor::onInputBuffer(const char*,
                                       InputBuffer& handle,
                                       const BufferRange& range)
{
    if (range.isValid())
        pass.setInputBuffer(range, handle.slot);
}

void ComputeBindVisitor::onOutputBuffer(const char*,
                                        OutputBuffer& handle,
                                        const BufferRange& range)
{
    if (range.isValid())
        pass.setOutputBuffer(range, handle.slot);
}

void ComputeBindVisitor::onUIntInputBuffer(const char*,
                                           UIntInputBuffer& handle,
                                           const BufferRange& range)
{
    if (range.isValid())
        pass.setInputBuffer(range, handle.slot);
}

void ComputeBindVisitor::onUIntOutputBuffer(const char*,
                                            UIntOutputBuffer& handle,
                                            const BufferRange& range)
{
    if (range.isValid())
        pass.setOutputBuffer(range, handle.slot);
}

void ComputeBindVisitor::onIntInputBuffer(const char*,
                                          IntInputBuffer& handle,
                                          const BufferRange& range)
{
    if (range.isValid())
        pass.setInputBuffer(range, handle.slot);
}

void ComputeBindVisitor::onIntOutputBuffer(const char*,
                                           IntOutputBuffer& handle,
                                           const BufferRange& range)
{
    if (range.isValid())
        pass.setOutputBuffer(range, handle.slot);
}

void ComputeBindVisitor::onHalfInputBuffer(const char*,
                                           HalfInputBuffer& handle,
                                           const BufferRange& range)
{
    if (range.isValid())
        pass.setInputBuffer(range, handle.slot);
}

void ComputeBindVisitor::onHalfOutputBuffer(const char*,
                                            HalfOutputBuffer& handle,
                                            const BufferRange& range)
{
    if (range.isValid())
        pass.setOutputBuffer(range, handle.slot);
}

void ComputeBindVisitor::onAtomicBuffer(const char*,
                                        AtomicBuffer& handle,
                                        const BufferRange& range)
{
    if (range.isValid())
        pass.setOutputBuffer(range, handle.slot);
}

void ComputeBindVisitor::onIntAtomicBuffer(const char*,
                                           IntAtomicBuffer& handle,
                                           const BufferRange& range)
{
    if (range.isValid())
        pass.setOutputBuffer(range, handle.slot);
}

void ComputeBindVisitor::onStructBuffer(const char*,
                                        BufferAccess access,
                                        detail::StructBufferHandle& handle,
                                        int (*)(ShaderGraph&),
                                        const BufferRange& range)
{
    if (!range.isValid())
        return;

    if (access == BufferAccess::Read)
        pass.setInputBuffer(range, handle.slot);
    else
        pass.setOutputBuffer(range, handle.slot);
}

void ComputeBindVisitor::onTexture(const char*,
                                   Texture2D& handle,
                                   const Texture* texture,
                                   TextureSampling sampling)
{
    if (texture != nullptr)
        pass.setInputTexture(*texture, handle.slot, sampling);
}

void ComputeBindVisitor::onCubeTexture(const char*,
                                       TextureCube& handle,
                                       const Texture* texture,
                                       TextureSampling sampling)
{
    if (texture != nullptr)
        pass.setInputTexture(*texture, handle.slot, sampling);
}

void ComputeBindVisitor::onArrayTexture(const char*,
                                        Texture2DArray& handle,
                                        const Texture* texture,
                                        TextureSampling sampling)
{
    if (texture != nullptr)
        pass.setInputTexture(*texture, handle.slot, sampling);
}

void ComputeBindVisitor::onDepthTexture(const char*,
                                        TextureDepth2D& handle,
                                        const Texture* renderTarget,
                                        TextureSampling sampling)
{
    if (renderTarget != nullptr)
        pass.setInputDepthTexture(*renderTarget, handle.slot, sampling);
}

void ComputeBindVisitor::onDepthArrayTexture(const char*,
                                             TextureDepth2DArray& handle,
                                             const Texture* renderTarget,
                                             TextureSampling sampling)
{
    if (renderTarget != nullptr)
        pass.setInputDepthTexture(*renderTarget, handle.slot, sampling);
}

void ComputeBindVisitor::onTextureOfKind(const char*,
                                         TextureKind kind,
                                         detail::TextureHandle& handle,
                                         const Texture* texture,
                                         TextureSampling sampling)
{
    if (texture == nullptr)
        return;

    if (isDepth(kind))
        pass.setInputDepthTexture(*texture, handle.slot, sampling);
    else
        pass.setInputTexture(*texture, handle.slot, sampling);
}

void ComputeBindVisitor::onWritableTexture(const char*,
                                           WritableTexture2D& handle,
                                           const Texture* texture)
{
    if (texture != nullptr)
        pass.setOutputTexture(*texture, handle.slot);
}

void ComputeBindVisitor::onStorageTexture(const char*,
                                          const StorageTextureLayout& layout,
                                          detail::TextureHandle& handle,
                                          const Texture* texture)
{
    if (texture == nullptr || !detail::fitsStorageMember(*texture, layout))
        return;

    const auto asArray = detail::isCubeType(texture->kind());

    if (layout.access == TextureAccess::Read)
    {
        if (asArray)
            pass.setInputTextureAsArray(*texture, handle.slot, 0, texelSampling);
        else
            pass.setInputTexture(*texture, handle.slot, texelSampling);

        return;
    }

    if (asArray)
        pass.setOutputTextureAsArray(*texture, handle.slot);
    else
        pass.setOutputTexture(*texture, handle.slot);
}

void ComputeBindVisitor::onSampler(const char* name,
                                   detail::SamplerHandle& handle,
                                   bool comparison,
                                   const Sampler& sampler)
{
    if (detail::fitsSamplerMember(name, sampler, comparison))
        pass.setSampler(sampler, handle.slot);
}

bool fitsStorageTexture(const Device& device,
                        TextureAccess access,
                        TextureFormat format)
{
    if (access == TextureAccess::ReadWrite)
        return device.supportsComputeReadWrite(format);

    if (access == TextureAccess::Write)
        return device.supportsComputeWrite(format);

    return true;
}

// The first storage slot the device refuses, or -1.
int findRefusedStorageTexture(const Device& device, const ShaderGraph& graph)
{
    if (!device.isValid())
        return -1;

    for (auto slot = 0; slot < graph.textureCount(); ++slot)
        if (!fitsStorageTexture(
                device, graph.textureAccess(slot), graph.textureFormat(slot)))
            return slot;

    return -1;
}
} // namespace

ComputeProgram::ComputeProgram(ThreadGroupShape shape)
    : ComputeKernel(shape)
{
}

void ComputeProgram::prepare(Device& device)
{
    prepare(device, OverrideValues {});
}

void ComputeProgram::prepare()
{
    prepare(Device::shared());
}

void ComputeProgram::prepare(const OverrideValues& overrides)
{
    prepare(Device::shared(), overrides);
}

void ComputeProgram::prepare(Device& device, const OverrideValues& overrides)
{
    reportThreadgroupMemoryOverBudget(device);

    specializations.clear();
    currentPipeline = nullptr;
    preparedDevice = &device;
    refused = !fitsDevice(device);

    if (refused)
    {
        buildRefusedPipeline(device);
        return;
    }

    shaderLibrary.emplace(device, source());
    specialize(overrides);
}

bool ComputeProgram::specialize(const OverrideValues& overrides)
{
    if (preparedDevice == nullptr || refused)
        return false;

    const auto resolved = resolveOverrides(graph(), overrides);

    if (!resolved.isValid())
    {
        LOG("eacp: this kernel refuses its override values (",
            resolved.error,
            "), so no pipeline was built for them.");
        useRefusedPipeline(*preparedDevice);
        return false;
    }

    for (const auto& specialization: specializations)
    {
        if (specialization->constants == resolved.specialized)
        {
            currentPipeline = &specialization->pipeline;
            return isValid();
        }
    }

    auto& built = specializations.createNew(
        *preparedDevice, *shaderLibrary, resolved.specialized);
    currentPipeline = &built.pipeline;

    reportSimdWidthMismatch();
    return isValid();
}

bool ComputeProgram::fitsDevice(Device& device) const
{
    // Refused here rather than handed to the backend. A packed fragment
    // this device has no instruction for is a kernel built against the
    // wrong answer to a question it was supposed to ask first, and what
    // the shader compiler would say about it names a type, not the query.
    if (!fitsPackedSimdMatrix(device))
    {
        reportUnsupportedPackedSimdMatrix(device);
        return false;
    }

    // Refused for the same reason: a store or a read-modify-write in a format
    // the device has no typed access for is a kernel whose writes would vanish
    // or whose loads would read garbage, not one that fails to compile.
    if (!fitsStorageTextures(device))
    {
        reportUnsupportedStorageTexture(device);
        return false;
    }

    // And for an f16 value on a device whose shaders have none: GLSL's
    // float16_t is a type the driver either enabled or did not.
    if (!fitsShaderFloat16(device))
    {
        reportUnsupportedShaderFloat16();
        return false;
    }

    return true;
}

bool ComputeProgram::fitsThreadgroupMemory(const Device& device) const
{
    auto budget = device.maxThreadgroupMemory();

    return budget <= 0 || threadgroupMemoryBytes() <= budget;
}

bool ComputeProgram::fitsPackedSimdMatrix(const Device& device) const
{
    if (source().backend != ShaderBackend::Metal)
        return true;

    auto needsHalf = graph().usesPackedSimdMatrix(SimdMatrixElement::Half);
    auto needsBFloat16 = graph().usesPackedSimdMatrix(SimdMatrixElement::BFloat16);

    return (!needsHalf || device.supportsHalfSimdMatrix())
           && (!needsBFloat16 || device.supportsBFloat16SimdMatrix());
}

bool ComputeProgram::fitsStorageTextures(const Device& device) const
{
    return findRefusedStorageTexture(device, graph()) < 0;
}

bool ComputeProgram::fitsShaderFloat16(const Device& device) const
{
    return !device.isValid() || !graph().usesHalf()
           || device.supportsShaderFloat16();
}

const ComputePipeline& ComputeProgram::pipeline() const
{
    return *currentPipeline;
}

bool ComputeProgram::isValid() const
{
    return currentPipeline != nullptr && currentPipeline->isValid();
}

bool ComputeProgram::isPrecompiled() const
{
    return shaderLibrary.has_value() && shaderLibrary->isPrecompiled();
}

void ComputeProgram::bindResources(ComputePass& pass)
{
    auto bindVisitor = ComputeBindVisitor {pass};
    reflectMembers(bindVisitor);
}

void ComputeProgram::reportSimdWidthMismatch() const
{
    if (!graph().usesSimdReduction() && !graph().usesSimdGroups())
        return;

    auto width = currentPipeline->threadExecutionWidth();

    if (width <= 0 || width == ComputeProgram::simdWidth)
        return;

    LOG("eacp: this kernel folds or multiplies over SIMD groups of ",
        ComputeProgram::simdWidth,
        " threads and this device runs it at ",
        width,
        ". simdSum/simdMax/simdMin and SimdMatrix need the two to agree; "
        "use the whole-group groupSum/groupMax/groupMin, which is correct "
        "at any width.");
}

void ComputeProgram::buildRefusedPipeline(Device& device)
{
    shaderLibrary.emplace(device, ShaderSource {});
    useRefusedPipeline(device);
}

void ComputeProgram::useRefusedPipeline(Device& device)
{
    refusedLibrary.emplace(device, ShaderSource {});
    refusedPipeline.emplace(device, *refusedLibrary);
    currentPipeline = &*refusedPipeline;
}

void ComputeProgram::reportUnsupportedPackedSimdMatrix(const Device& device) const
{
    auto missingBFloat16 = graph().usesPackedSimdMatrix(SimdMatrixElement::BFloat16)
                           && !device.supportsBFloat16SimdMatrix();

    const auto* load = missingBFloat16 ? "simdMatrixBFloat16" : "simdMatrixHalf";

    const auto* query =
        missingBFloat16 ? "supportsBFloat16SimdMatrix" : "supportsHalfSimdMatrix";

    LOG("eacp: this kernel loads a packed SIMD-group matrix fragment "
        "through ",
        load,
        ", and Device::",
        query,
        " answers no, so no pipeline was built for it. Ask that query "
        "before recording the load, and where it answers no build the "
        "kernel that stages the weight into a shared<Float> tile instead. "
        "The two are different kernels rather than two arms of one, "
        "because staging carries barriers and the packed load does not.");
}

void ComputeProgram::reportUnsupportedStorageTexture(const Device& device) const
{
    const auto slot = findRefusedStorageTexture(device, graph());
    const auto readWrite = graph().textureAccess(slot) == TextureAccess::ReadWrite;

    LOG("eacp: this kernel declares a ",
        readWrite ? "read-write" : "write-only",
        " storage texture at slot ",
        slot,
        " in a format Device::",
        readWrite ? "supportsComputeReadWrite" : "supportsComputeWrite",
        " answers no for, so no pipeline was built for it. Ask that query "
        "before choosing the format, and where it answers no pick one it "
        "takes - R32Float, R32Uint and R32Sint read and write through one "
        "bind on every device eacp runs on.");
}

void ComputeProgram::reportUnsupportedShaderFloat16() const
{
    LOG("eacp: this kernel computes in f16 (Half, a half matrix or a "
        "HalfInputBuffer/HalfOutputBuffer) and Device::supportsShaderFloat16 "
        "answers no, so no pipeline was built for it. Ask that query first, "
        "and where it answers no build the Float kernel instead.");
}

void ComputeProgram::reportThreadgroupMemoryOverBudget(const Device& device) const
{
    if (fitsThreadgroupMemory(device))
        return;

    LOG("eacp: this kernel declares ",
        threadgroupMemoryBytes(),
        " bytes of threadgroup memory and this device allows ",
        device.maxThreadgroupMemory(),
        ". Size its shared<> arrays against Device::maxThreadgroupMemory().");
}
} // namespace eacp::GPU
