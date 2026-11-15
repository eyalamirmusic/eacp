#pragma once

#include "../Common.h"

#include "../Shader/PipelineConstants.h"
#include "../Shader/ShaderSource.h"

namespace eacp::GPU
{
class Device;
class ShaderLibrary;

// A compiled compute pipeline state (MTLComputePipelineState on Metal). The
// compute sibling of RenderPipeline, built from a library's kernel entry point;
// it carries none of the render fixed-function state. Create via
// Device::makeComputePipeline.
class ComputePipeline
{
public:
    ComputePipeline(Device& device, const ShaderLibrary& library);

    // The library's kernel specialised with `constants`, its pipeline-
    // overridable constants (PipelineConstants.h); the ones left out take the
    // shader's defaults. A library with none ignores them.
    ComputePipeline(Device& device,
                    const ShaderLibrary& library,
                    const PipelineConstants& constants);

    bool isValid() const;

    // The group the kernel was compiled for, which is the group the pass
    // dispatches it in. Unset for a hand-written source that named none.
    constexpr ThreadGroupShape threadGroupShape() const { return groupShape; }

    // How many threads this pipeline's SIMD groups really hold on this device -
    // Metal's threadExecutionWidth, which is a property of the compiled kernel
    // and not of the EDSL: 32 on every Apple GPU and 8 or 16 on an Intel Mac.
    //
    // Zero where the backend reports none, which is both of the others: they
    // emulate a SIMD group at ComputeProgram::simdWidth lanes rather than
    // lowering to one, so there is no hardware width for them to disagree with.
    // ComputeProgram::prepare is what reads this, for the kernels whose
    // correctness depends on the two numbers agreeing.
    int threadExecutionWidth() const;

    // Opaque native handle for cross-translation-unit use by the compute pass.
    void* nativeState() const;

private:
    ThreadGroupShape groupShape;

    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU
