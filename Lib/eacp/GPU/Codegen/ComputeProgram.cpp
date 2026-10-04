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

Uniform<AtomicBuffer>& Uniform<AtomicBuffer>::operator=(const Buffer& newBuffer)
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

    // An atomic buffer binds exactly as an output does - a Metal device buffer,
    // a D3D UAV - since what makes it atomic is the type the kernel declares it
    // through and not how the pass hands it over.
    void onAtomicBuffer(const char*,
                        AtomicBuffer& handle,
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

    void onWritableTexture(const char*,
                           WritableTexture2D& handle,
                           const Texture* texture) override;

private:
    ComputePass& pass;
};

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

void ComputeBindVisitor::onAtomicBuffer(const char*,
                                        AtomicBuffer& handle,
                                        const BufferRange& range)
{
    if (range.isValid())
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

void ComputeBindVisitor::onWritableTexture(const char*,
                                           WritableTexture2D& handle,
                                           const Texture* texture)
{
    if (texture != nullptr)
        pass.setOutputTexture(*texture, handle.slot);
}
} // namespace

ComputeProgram::ComputeProgram(ThreadGroupShape shape)
    : ComputeKernel(shape)
{
}

void ComputeProgram::prepare(Device& device)
{
    reportThreadgroupMemoryOverBudget(device);

    // Refused here rather than handed to the backend. A packed fragment
    // this device has no instruction for is a kernel built against the
    // wrong answer to a question it was supposed to ask first, and what
    // the shader compiler would say about it names a type, not the query.
    if (!fitsPackedSimdMatrix(device))
    {
        reportUnsupportedPackedSimdMatrix(device);
        buildRefusedPipeline(device);
        return;
    }

    shaderLibrary.emplace(device, source());
    pipelineState.emplace(device, *shaderLibrary);

    reportSimdWidthMismatch();
}

void ComputeProgram::prepare()
{
    prepare(Device::shared());
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

const ComputePipeline& ComputeProgram::pipeline() const
{
    return *pipelineState;
}

bool ComputeProgram::isValid() const
{
    return pipelineState.has_value() && pipelineState->isValid();
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

    auto width = pipelineState->threadExecutionWidth();

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
    pipelineState.emplace(device, *shaderLibrary);
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
