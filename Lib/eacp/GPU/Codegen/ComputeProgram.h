#pragma once

#include "../Device/Device.h"
#include "../Frame/ComputePass.h"
#include "../Pipeline/ComputePipeline.h"
#include "ComputeKernel.h"
#include "ShaderProgram.h"

#include <eacp/Core/Utils/Logging.h>

// A compute kernel authored as a struct, the compute sibling of ShaderProgram.
// Uniforms are named, typed members set by name; storage buffers are members
// assigned the GPU::Buffer to bind - or a BufferRange, to bind a slice of one
// with the kernel's element zero at the offset - with slots taken from
// declaration order.
// define() writes the kernel body: read inputs at threadId() (or, over a grid,
// at threadPosition(), or over a volume at threadPosition3()), write the result
// with write(). The generated kernel guards against the rounded-up dispatch
// with implicit extents, supplied automatically at dispatch - one count for a
// 1D kernel, a width and a height for a 2D one, a depth as well for a 3D one.
//
//   struct ScaleKernel final : ComputeProgram
//   {
//       Uniform<InputBuffer> input;
//       Uniform<OutputBuffer> output;
//       Uniform<Float> scale;
//       EACP_SHADER(input, output, scale)
//
//       ScaleKernel() { compile(); }
//
//       void define() override
//       {
//           auto i = threadId();
//           write(output, i, input[i] * scale);
//       }
//   };
//
//   ScaleKernel kernel;
//   kernel.input = inputBuffer;     // GPU::Buffer, Storage usage
//   kernel.output = outputBuffer;   // or BufferRange {&cache, row * bytes, bytes}
//   kernel.scale = 3.0f;
//   kernel.prepare();               // builds library + compute pipeline
//   ...
//   pass.dispatch(kernel, count);   // pipeline + buffers + uniforms + dispatch
//
// Everything above prepare() - the members, define(), compile(), graph() and
// source() - is ComputeKernel's, which needs no device (see ComputeKernel.h).

namespace eacp::GPU
{
// Resource bind walk: hand each assigned buffer and texture member to the
// compute pass at the slot its handle was declared with. One walk rather than
// one per resource kind - the members are visited in declaration order either
// way, and the slots are already carried by the handles.
class ComputeBindVisitor final : public ShaderVisitor
{
public:
    explicit ComputeBindVisitor(ComputePass& passToUse)
        : pass(passToUse)
    {
    }

    void
        onUniform(const char*, ValueType, detail::ValueHandle&, const void*) override
    {
    }

    void onInputBuffer(const char*,
                       InputBuffer& handle,
                       const BufferRange& range) override
    {
        if (range.isValid())
            pass.setInputBuffer(range, handle.slot);
    }

    void onOutputBuffer(const char*,
                        OutputBuffer& handle,
                        const BufferRange& range) override
    {
        if (range.isValid())
            pass.setOutputBuffer(range, handle.slot);
    }

    // The integer buffers bind through the same two calls the float ones do:
    // what the elements are is settled by the kernel's declaration, not by how
    // the pass hands the buffer over.
    void onUIntInputBuffer(const char*,
                           UIntInputBuffer& handle,
                           const BufferRange& range) override
    {
        if (range.isValid())
            pass.setInputBuffer(range, handle.slot);
    }

    void onUIntOutputBuffer(const char*,
                            UIntOutputBuffer& handle,
                            const BufferRange& range) override
    {
        if (range.isValid())
            pass.setOutputBuffer(range, handle.slot);
    }

    // An atomic buffer binds exactly as an output does - a Metal device buffer,
    // a D3D UAV - since what makes it atomic is the type the kernel declares it
    // through and not how the pass hands it over.
    void onAtomicBuffer(const char*,
                        AtomicBuffer& handle,
                        const BufferRange& range) override
    {
        if (range.isValid())
            pass.setOutputBuffer(range, handle.slot);
    }

    void onTexture(const char*,
                   Texture2D& handle,
                   const Texture* texture,
                   TextureSampling sampling) override
    {
        if (texture != nullptr)
            pass.setInputTexture(*texture, handle.slot, sampling);
    }

    // The same call the 2D one takes, for the reason the render bind visitor
    // gives: a cube is one texture on one slot of one index space on both
    // backends, and its dimensionality was settled when it was created and when
    // the kernel was compiled.
    void onCubeTexture(const char*,
                       TextureCube& handle,
                       const Texture* texture,
                       TextureSampling sampling) override
    {
        if (texture != nullptr)
            pass.setInputTexture(*texture, handle.slot, sampling);
    }

    void onWritableTexture(const char*,
                           WritableTexture2D& handle,
                           const Texture* texture) override
    {
        if (texture != nullptr)
            pass.setOutputTexture(*texture, handle.slot);
    }

private:
    ComputePass& pass;
};

// Base for struct-authored compute kernels that run on a GPU: a ComputeKernel
// plus the library and pipeline prepare() builds from it on a Device, and the
// bind ComputePass::dispatch runs. Derive, declare uniform and buffer members,
// list them with EACP_SHADER, write define(), and call compile() from the
// constructor.
class ComputeProgram : public ComputeKernel
{
public:
    ComputeProgram() = default;

    // The threadgroup this kernel is dispatched in, in place of the stock shape
    // for its rank: ComputeProgram({256}) over a 1D grid, ComputeProgram({16,
    // 16}) over a 2D one. The body reads it back through groupShape().
    explicit ComputeProgram(ThreadGroupShape shape)
        : ComputeKernel(shape)
    {
    }

    // Builds the shader library and compute pipeline from the generated kernel,
    // on the Device whose passes will dispatch it. A pipeline belongs to the
    // device that compiled it, so a kernel a worker Device dispatches is
    // compiled on that Device rather than on the process-wide one.
    void prepare(Device& device)
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

    void prepare() { prepare(Device::shared()); }

    // Whether threadgroupMemoryBytes() is inside what the device allows. The
    // two are what a kernel author sizes a tile against, together with
    // Device::maxThreadgroupMemory, instead of carrying the backend's number in
    // a comment. An invalid Device has no budget to be inside, so it fits.
    bool fitsThreadgroupMemory(const Device& device) const
    {
        auto budget = device.maxThreadgroupMemory();

        return budget <= 0 || threadgroupMemoryBytes() <= budget;
    }

    // Whether this kernel's packed fragments are ones this device can **build**
    // - a different question from whether it has instructions for them, and the
    // two are worth keeping apart.
    //
    // Device::supportsHalfSimdMatrix and supportsBFloat16SimdMatrix answer
    // "natively, in one instruction". They are what a kernel author picks a
    // tiling around, and they are false on D3D12 and Vulkan. This answers "at
    // all", and on those two backends it is true whatever they said: a packed
    // load lowers there to the same two-floats-per-lane emulation every other
    // fragment operation lowers to, each lane widening the pair it holds. Only
    // Metal has a shader that would literally not compile - the packed fragment
    // is a type the dialect either has or does not - so only Metal refuses.
    //
    // A kernel that loads no packed fragment builds anywhere.
    bool fitsPackedSimdMatrix(const Device& device) const
    {
        if (source().backend != ShaderBackend::Metal)
            return true;

        auto needsHalf = graph().usesPackedSimdMatrix(SimdMatrixElement::Half);
        auto needsBFloat16 =
            graph().usesPackedSimdMatrix(SimdMatrixElement::BFloat16);

        return (!needsHalf || device.supportsHalfSimdMatrix())
               && (!needsBFloat16 || device.supportsBFloat16SimdMatrix());
    }

    const ComputePipeline& pipeline() const { return *pipelineState; }

    // Whether prepare() left something dispatchable. False before prepare(), of
    // a refused build, and of a shader that would not compile - all three being
    // states in which a dispatch of this program does nothing, so a caller that
    // would rather know than find out asks here.
    bool isValid() const
    {
        return pipelineState.has_value() && pipelineState->isValid();
    }

    // Binds every assigned buffer and texture member to the pass at its
    // declared slot. ComputePass::dispatch(program, ...) calls this.
    void bindResources(ComputePass& pass)
    {
        auto bindVisitor = ComputeBindVisitor {pass};
        reflectMembers(bindVisitor);
    }

private:
    // The one thing a kernel using simdSum/simdMax/simdMin or a SIMD-group
    // matrix cannot check for itself: those lower to intrinsics collective over
    // the *hardware* SIMD group, and the EDSL's arithmetic - simdWidth, the
    // fragment layout, simdGroupIndex - is written against a fixed 32. Every
    // Apple GPU agrees; an Intel Mac dispatches at eight or sixteen and the two
    // stop meaning the same thing, silently, since an intrinsic over a narrower
    // SIMD group is a well-formed fold of the wrong set of threads.
    //
    // Only the compiled pipeline knows the number, which is why this is here
    // and not in the emitter. The backends that emulate a SIMD group report
    // nothing, and there is nothing for them to disagree with.
    void reportSimdWidthMismatch() const
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

    // What a kernel gets instead of the one it asked for when the device has no
    // instruction for a fragment it loads: an empty library, and so a pipeline
    // that is not valid, which ComputePass::dispatch drops rather than encodes.
    // Built rather than left unset so that everything holding this program
    // still has a pipeline to name, and empty rather than the generated source
    // because every backend's ShaderLibrary declines an empty one in silence -
    // so the only thing logged is the reason above, not a shader compiler's
    // complaint about a type.
    void buildRefusedPipeline(Device& device)
    {
        shaderLibrary.emplace(device, ShaderSource {});
        pipelineState.emplace(device, *shaderLibrary);
    }

    void reportUnsupportedPackedSimdMatrix(const Device& device) const
    {
        auto missingBFloat16 =
            graph().usesPackedSimdMatrix(SimdMatrixElement::BFloat16)
            && !device.supportsBFloat16SimdMatrix();

        const auto* load = missingBFloat16 ? "simdMatrixBFloat16" : "simdMatrixHalf";

        const auto* query = missingBFloat16 ? "supportsBFloat16SimdMatrix"
                                            : "supportsHalfSimdMatrix";

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

    // Named here rather than left to the backend, which reports a threadgroup
    // allocation it cannot make as a pipeline that would not build - on Metal
    // after the library compiled clean, which points at the wrong thing.
    void reportThreadgroupMemoryOverBudget(const Device& device) const
    {
        if (fitsThreadgroupMemory(device))
            return;

        LOG("eacp: this kernel declares ",
            threadgroupMemoryBytes(),
            " bytes of threadgroup memory and this device allows ",
            device.maxThreadgroupMemory(),
            ". Size its shared<> arrays against Device::maxThreadgroupMemory().");
    }

    std::optional<ShaderLibrary> shaderLibrary;
    std::optional<ComputePipeline> pipelineState;
};
} // namespace eacp::GPU
