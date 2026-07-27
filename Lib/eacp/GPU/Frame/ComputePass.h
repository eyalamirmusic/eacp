#pragma once

#include "../Common.h"

#include "../Texture/Texture.h"

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
    explicit ComputePass(void* encoder);
    ~ComputePass();

    ComputePass(const ComputePass&) = delete;
    ComputePass& operator=(const ComputePass&) = delete;

    void setPipeline(const ComputePipeline& pipeline);

    // A read-only input (Metal device buffer / D3D shader-resource view) and a
    // read-write output (Metal device buffer / D3D unordered-access view).
    void setInputBuffer(const Buffer& buffer, int slot);
    void setOutputBuffer(const Buffer& buffer, int slot);

    // The texture siblings, on a slot space of their own: a texture the kernel
    // samples or fetches, and one it writes. sampling is the configuration the
    // shader declared, exactly as in RenderPass::setFragmentTexture.
    //
    // An output texture must have been created with
    // TextureDescriptor::computeWrite; one that was not is dropped rather than
    // bound, since the resource has no view to bind through.
    void setInputTexture(const Texture& texture,
                         int slot,
                         TextureSampling sampling = {});
    void setOutputTexture(const Texture& texture, int slot);

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

    // The 2D sibling, over a width × height grid in groups of threadGroupSize2D
    // squared. What anything image-shaped is dispatched with, and what a kernel
    // authored against threadPosition() needs.
    void dispatch(int width, int height);

    // Binds and dispatches a prepared ComputeProgram in one call: its pipeline,
    // storage buffers and uniform block (including the implicit element count
    // its generated bounds guard reads), then a dispatch over count work items.
    // Templated so this header stays independent of the codegen layer.
    template <typename Program>
    void dispatch(Program& program, int count)
    {
        setPipeline(program.pipeline());
        program.bindResources(*this);

        // Sequenced separately: packing must happen before the size is read,
        // and argument evaluation order would not guarantee that.
        const auto* uniforms = program.packedUniforms(count);
        setBytes(uniforms, (std::size_t) program.uniformByteSize());
        dispatch(count);
    }

    // The 2D form: the same binding, with the grid extents its guard reads in
    // place of the element count.
    template <typename Program>
    void dispatch(Program& program, int width, int height)
    {
        setPipeline(program.pipeline());
        program.bindResources(*this);

        const auto* uniforms = program.packedUniforms(width, height);
        setBytes(uniforms, (std::size_t) program.uniformByteSize());
        dispatch(width, height);
    }

    void end();

    // The Metal buffer index the first uniform block binds to. Storage buffers
    // take the low indices, so uniforms start above them.
    static constexpr int uniformBase = 16;

    // Threadgroup width the 1D dispatch uses; the example kernels declare a
    // matching [numthreads(64,1,1)] on D3D.
    static constexpr int threadGroupWidth = 64;

    // The 2D dispatch's group is this squared, which is the same 64 threads the
    // 1D path already budgets for - and square, so a group covers a tile rather
    // than a strip, which is what a kernel reading its neighbours wants.
    static constexpr int threadGroupSize2D = 8;

    // The D3D shader register a kernel's first texture takes. A texture shares
    // the t/u register spaces with the storage buffers there, and the two slot
    // spaces are counted separately, so textures start above every buffer slot
    // - the emitter writes these registers and the root signature declares
    // them, and D3D12Types.h holds the two to the same number. Metal is
    // unaffected: its texture indices are a space of their own.
    static constexpr int textureRegisterBase = 4;

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU
