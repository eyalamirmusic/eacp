#pragma once

/** @file ComputeProgram, a struct-authored compute kernel that builds its pipeline
    on a Device and dispatches through ComputePass.

    A compute kernel authored as a struct, the compute sibling of ShaderProgram.
    Uniforms are named, typed members set by name; storage buffers are members
    assigned the GPU::Buffer to bind - or a BufferRange, to bind a slice of one
    with the kernel's element zero at the offset - with slots taken from
    declaration order.
    define() writes the kernel body: read inputs at threadId() (or, over a grid,
    at threadPosition(), or over a volume at threadPosition3()), write the result
    with write(). The generated kernel guards against the rounded-up dispatch
    with implicit extents, supplied automatically at dispatch - one count for a
    1D kernel, a width and a height for a 2D one, a depth as well for a 3D one.

      struct ScaleKernel final : ComputeProgram
      {
          Uniform<InputBuffer> input;
          Uniform<OutputBuffer> output;
          Uniform<Float> scale;
          EACP_SHADER(input, output, scale)

          ScaleKernel() { compile(); }

          void define() override
          {
              auto i = threadId();
              write(output, i, input[i] * scale);
          }
      };

      ScaleKernel kernel;
      kernel.input = inputBuffer;     // GPU::Buffer, Storage usage
      kernel.output = outputBuffer;   // or BufferRange {&cache, row * bytes, bytes}
      kernel.scale = 3.0f;
      kernel.prepare();               // builds library + compute pipeline
      ...
      pass.dispatch(kernel, count);   // pipeline + buffers + uniforms + dispatch

    Everything above prepare() - the members, define(), compile(), graph() and
    source() - is ComputeKernel's, which needs no device (see ComputeKernel.h).
*/

#include "../Pipeline/ComputePipeline.h"
#include "ComputeKernel.h"
#include "ShaderProgram.h"

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
class ComputePass;
class Device;

// Base for struct-authored compute kernels that run on a GPU: a ComputeKernel
// plus the library and pipeline prepare() builds from it on a Device, and the
// bind ComputePass::dispatch runs. Derive, declare uniform and buffer members,
// list them with EACP_SHADER, write define(), and call compile() from the
// constructor.
class ComputeProgram : public ComputeKernel
{
public:
    ComputeProgram();
    ~ComputeProgram() override;

    // The threadgroup this kernel is dispatched in, in place of the stock shape
    // for its rank: ComputeProgram({256}) over a 1D grid, ComputeProgram({16,
    // 16}) over a 2D one. The body reads it back through groupShape().
    explicit ComputeProgram(ThreadGroupShape shape);

    // Builds the shader library and compute pipeline from the generated kernel,
    // on the Device whose passes will dispatch it. A pipeline belongs to the
    // device that compiled it, so a kernel a worker Device dispatches is
    // compiled on that Device rather than on the process-wide one.
    void prepare(Device& device);

    // prepare(Device::shared()).
    void prepare();

    // The same, with values for the kernel's overrides (OverrideValues.h): the
    // pipeline built is the kernel specialised with them, and the ones left
    // out take their defaults. Values the kernel refuses - an unknown name, a
    // value of the wrong type, an override with no default given none - leave
    // a pipeline that is not valid, and log why. Every prepare() compiles the
    // library afresh and forgets the pipelines built before it, which is what
    // a restored device needs.
    void prepare(Device& device, const OverrideValues& overrides);
    void prepare(const OverrideValues& overrides);

    // Makes the kernel specialised with these values the one a dispatch runs,
    // on the library and the device the last prepare() was given: the pipeline
    // already built for the same values since then, or one built now. Value
    // sets that come to the same constants - a default given or left out -
    // share one pipeline; every set shares the library, which on D3D compiles
    // a variant of its source once per set instead (PipelineConstants.h).
    // Refused as prepare() refuses; false before any prepare(), after one that
    // refused the kernel, and for values it refuses. Returns isValid().
    bool specialize(const OverrideValues& overrides);

    // Whether threadgroupMemoryBytes() is inside what the device allows. The
    // two are what a kernel author sizes a tile against, together with
    // Device::maxThreadgroupMemory, instead of carrying the backend's number in
    // a comment. An invalid Device has no budget to be inside, so it fits.
    bool fitsThreadgroupMemory(const Device& device) const;

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
    bool fitsPackedSimdMatrix(const Device& device) const;

    // Whether every storage texture this kernel declares is one the device
    // takes: a Write one in a format Device::supportsComputeWrite names, a
    // ReadWrite one in a format Device::supportsComputeReadWrite names. A Read
    // one loads through the read binding, which every format has. prepare()
    // refuses a kernel that does not fit, as it refuses a packed fragment the
    // device has no instruction for, and logs the slot and the query.
    bool fitsStorageTextures(const Device& device) const;

    // Whether the device runs this kernel's f16 values, if it has any:
    // Device::supportsShaderFloat16 answers. prepare() refuses one that does
    // not fit, as it refuses a storage texture the device takes no store of.
    bool fitsShaderFloat16(const Device& device) const;

    const ComputePipeline& pipeline() const;

    // Whether prepare() left something dispatchable. False before prepare(), of
    // a refused build, and of a shader that would not compile - all three being
    // states in which a dispatch of this program does nothing, so a caller that
    // would rather know than find out asks here.
    bool isValid() const;

    // Whether prepare() loaded the kernel from a blob compiled at build time
    // rather than compiling its source. False before prepare().
    bool isPrecompiled() const;

    // Binds every assigned buffer and texture member to the pass at its
    // declared slot. ComputePass::dispatch(program, ...) calls this.
    void bindResources(ComputePass& pass);

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
    void reportSimdWidthMismatch() const;

    // What a kernel gets instead of the one it asked for when the device has no
    // instruction for a fragment it loads: an empty library, and so a pipeline
    // that is not valid, which ComputePass::dispatch drops rather than encodes.
    // Built rather than left unset so that everything holding this program
    // still has a pipeline to name, and empty rather than the generated source
    // because every backend's ShaderLibrary declines an empty one in silence -
    // so the only thing logged is the reason above, not a shader compiler's
    // complaint about a type.
    void buildRefusedPipeline(Device& device);

    void reportUnsupportedPackedSimdMatrix(const Device& device) const;

    void reportUnsupportedStorageTexture(const Device& device) const;

    void reportUnsupportedShaderFloat16() const;

    // Named here rather than left to the backend, which reports a threadgroup
    // allocation it cannot make as a pipeline that would not build - on Metal
    // after the library compiled clean, which points at the wrong thing.
    void reportThreadgroupMemoryOverBudget(const Device& device) const;

    // Whether the device takes this kernel at all, logging why not.
    bool fitsDevice(Device& device) const;

    // An invalid pipeline made current, for values specialize() refused.
    void useRefusedPipeline(Device& device);

    // One pipeline per set of constants the kernel was specialised with.
    struct Specialization
    {
        Specialization(Device& device,
                       const ShaderLibrary& library,
                       const PipelineConstants& constantsToUse)
            : constants(constantsToUse)
            , pipeline(device, library, constantsToUse)
        {
        }

        PipelineConstants constants;
        ComputePipeline pipeline;
    };

    Device* preparedDevice = nullptr;
    bool refused = false;
    std::optional<ShaderLibrary> shaderLibrary;
    OwnedVector<Specialization> specializations;
    std::optional<ShaderLibrary> refusedLibrary;
    std::optional<ComputePipeline> refusedPipeline;
    const ComputePipeline* currentPipeline = nullptr;
};
} // namespace eacp::GPU
