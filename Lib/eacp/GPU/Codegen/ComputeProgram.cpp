#include "ComputeProgram.h"
#include "../Device/Device.h"
#include "../Frame/ComputePass.h"

#include <eacp/Core/Utils/Logging.h>

#include <stdexcept>

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
//
// A member nothing was assigned to is recorded rather than skipped, so the
// dispatch can refuse it: a kernel that runs with a slot left over from
// whatever the pass bound last reads memory nobody meant it to. A member
// assigned a buffer that never got storage is still skipped, as the pass's own
// bind would skip it.
//
// With releasing on, every member is cleared once it is bound, so the pointer
// it held into a buffer the caller is about to free does not outlive the
// dispatch. That is what makes a shared kernel safe: see sharedKernel.
class ComputeBindVisitor final : public ShaderVisitor
{
public:
    ComputeBindVisitor(ComputePass& passToUse, bool releaseAfterBinding);

    // The first member the walk found with nothing assigned, or null.
    const char* unassigned() const { return firstUnassigned; }

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
    bool isAssigned(const char* name, const void* resource);

    // Every handle the walk is given is the base of the Uniform member that
    // holds its binding - ShaderVisitor's operator() hands the member itself
    // over - so the member is reached back through it.
    template <typename Handle>
    void release(Handle& handle)
    {
        if (releasing)
            static_cast<Uniform<Handle>&>(handle).value = {};
    }

    ComputePass& pass;
    bool releasing = false;
    const char* firstUnassigned = nullptr;
};

ComputeBindVisitor::ComputeBindVisitor(ComputePass& passToUse,
                                       bool releaseAfterBinding)
    : pass(passToUse)
    , releasing(releaseAfterBinding)
{
}

bool ComputeBindVisitor::isAssigned(const char* name, const void* resource)
{
    if (resource != nullptr)
        return true;

    if (firstUnassigned == nullptr)
        firstUnassigned = name;

    return false;
}

void ComputeBindVisitor::onUniform(const char*,
                                   ValueType,
                                   detail::ValueHandle&,
                                   const void*)
{
}

void ComputeBindVisitor::onInputBuffer(const char* name,
                                       InputBuffer& handle,
                                       const BufferRange& range)
{
    if (isAssigned(name, range.buffer) && range.isValid())
        pass.setInputBuffer(range, handle.slot);

    release(handle);
}

void ComputeBindVisitor::onOutputBuffer(const char* name,
                                        OutputBuffer& handle,
                                        const BufferRange& range)
{
    if (isAssigned(name, range.buffer) && range.isValid())
        pass.setOutputBuffer(range, handle.slot);

    release(handle);
}

void ComputeBindVisitor::onUIntInputBuffer(const char* name,
                                           UIntInputBuffer& handle,
                                           const BufferRange& range)
{
    if (isAssigned(name, range.buffer) && range.isValid())
        pass.setInputBuffer(range, handle.slot);

    release(handle);
}

void ComputeBindVisitor::onUIntOutputBuffer(const char* name,
                                            UIntOutputBuffer& handle,
                                            const BufferRange& range)
{
    if (isAssigned(name, range.buffer) && range.isValid())
        pass.setOutputBuffer(range, handle.slot);

    release(handle);
}

void ComputeBindVisitor::onAtomicBuffer(const char* name,
                                        AtomicBuffer& handle,
                                        const BufferRange& range)
{
    if (isAssigned(name, range.buffer) && range.isValid())
        pass.setOutputBuffer(range, handle.slot);

    release(handle);
}

void ComputeBindVisitor::onTexture(const char* name,
                                   Texture2D& handle,
                                   const Texture* texture,
                                   TextureSampling sampling)
{
    if (isAssigned(name, texture))
        pass.setInputTexture(*texture, handle.slot, sampling);

    release(handle);
}

void ComputeBindVisitor::onCubeTexture(const char* name,
                                       TextureCube& handle,
                                       const Texture* texture,
                                       TextureSampling sampling)
{
    if (isAssigned(name, texture))
        pass.setInputTexture(*texture, handle.slot, sampling);

    release(handle);
}

void ComputeBindVisitor::onWritableTexture(const char* name,
                                           WritableTexture2D& handle,
                                           const Texture* texture)
{
    if (isAssigned(name, texture))
        pass.setOutputTexture(*texture, handle.slot);

    release(handle);
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

    compiled = compileComputeCached(device, source());

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
    return compiled->pipeline;
}

bool ComputeProgram::isValid() const
{
    return compiled != nullptr && compiled->pipeline.isValid();
}

void ComputeProgram::bindResources(ComputePass& pass)
{
    auto bindVisitor = ComputeBindVisitor {pass, releasesBindings};
    reflectMembers(bindVisitor);

    if (bindVisitor.unassigned() != nullptr)
        throwUnassigned(bindVisitor.unassigned());
}

void ComputeProgram::reportSimdWidthMismatch() const
{
    if (!graph().usesSimdReduction() && !graph().usesSimdGroups())
        return;

    auto width = compiled->pipeline.threadExecutionWidth();

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
    compiled = std::make_shared<const CompiledCompute>(device, ShaderSource {});
}

void ComputeProgram::throwUnassigned(const char* member) const
{
    auto message = "eacp: " + name() + " was dispatched with nothing assigned "
                   + "to its '" + member + "' member.";

    if (releasesBindings)
        message += " It is a shared kernel, which lets go of every buffer "
                   "and texture after each dispatch, so each call assigns "
                   "all of them.";

    throw std::logic_error {message};
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
