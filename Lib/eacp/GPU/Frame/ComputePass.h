#pragma once

#include <eacp/Core/Utils/Common.h>

#include <cstddef>

namespace eacp::GPU
{
class ComputePipeline;
class Buffer;

// Records dispatch commands for a single compute pass (MTLComputeCommandEncoder
// on Metal). Ends the encoder automatically on destruction. Obtained from
// CommandBuffer::beginCompute.
//
// Binding model: Metal uses one flat buffer-index space, D3D uses separate
// SRV/UAV/CBV register spaces. setInputBuffer/setOutputBuffer take a slot
// that maps to
// Metal buffer(slot) and to a D3D SRV t<slot> / UAV u<slot>; because Metal
// shares the space, an input and an output must use distinct slots. setBytes
// uploads a uniform block at Metal buffer(uniformBase + slot) and D3D CBV
// b<slot>, mirroring the render pass's hidden offset.
class ComputePass
{
public:
    // The Metal buffer index the first uniform block binds to. Storage buffers
    // take the low indices, so uniforms start above them.
    static constexpr int uniformBase = 16;

    explicit ComputePass(void* encoder);
    ~ComputePass();

    ComputePass(const ComputePass&) = delete;
    ComputePass& operator=(const ComputePass&) = delete;

    void setPipeline(const ComputePipeline& pipeline);

    // A read-only input (Metal device buffer / D3D shader-resource view) and a
    // read-write output (Metal device buffer / D3D unordered-access view).
    void setInputBuffer(const Buffer& buffer, int slot);
    void setOutputBuffer(const Buffer& buffer, int slot);

    // Uploads a small uniform block without a buffer object, like the render
    // pass's setVertexBytes. slot is the uniform-block slot (0 = first block).
    void setBytes(const void* data, std::size_t bytes, int slot = 0);

    template <typename T>
    void setUniform(const T& value, int slot = 0)
    {
        setBytes(&value, sizeof(T), slot);
    }

    // Runs the kernel over count work items, in groups of threadGroupWidth.
    void dispatch(int count);

    // Binds and dispatches a prepared ComputeProgram in one call: its pipeline,
    // storage buffers and uniform block (including the implicit element count
    // its generated bounds guard reads), then a dispatch over count work items.
    // Templated so this header stays independent of the codegen layer.
    template <typename Program>
    void dispatch(Program& program, int count)
    {
        setPipeline(program.pipeline());
        program.bindBuffers(*this);

        // Sequenced separately: packing must happen before the size is read,
        // and argument evaluation order would not guarantee that.
        const auto* uniforms = program.packedUniforms(count);
        setBytes(uniforms, (std::size_t) program.uniformByteSize());
        dispatch(count);
    }

    void end();

    // Threadgroup width the 1D dispatch uses; the example kernels declare a
    // matching [numthreads(64,1,1)] on D3D.
    static constexpr int threadGroupWidth = 64;

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU
