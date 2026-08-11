#pragma once

#include "../Common.h"

#include "../Texture/Texture.h"

namespace eacp::GPU
{
class ComputePipeline;
class Buffer;

// Layout-compatible with MTLDispatchThreadgroupsIndirectArguments and
// D3D12_DISPATCH_ARGUMENTS. Counts threadgroups, not threads.
struct DispatchArguments
{
    std::uint32_t groupsX = 1;
    std::uint32_t groupsY = 1;
    std::uint32_t groupsZ = 1;
};

// Obtained from CommandBuffer::beginCompute; ends the encoder on destruction.
// A slot maps to Metal buffer(slot) and to D3D SRV t<slot> / UAV u<slot>, so an
// input and an output must differ, Metal sharing one index space.
class ComputePass
{
public:
    explicit ComputePass(void* encoder);
    ~ComputePass();

    ComputePass(const ComputePass&) = delete;
    ComputePass& operator=(const ComputePass&) = delete;

    void setPipeline(const ComputePipeline& pipeline);

    void setInputBuffer(const Buffer& buffer, int slot);
    void setOutputBuffer(const Buffer& buffer, int slot);

    // Textures use a slot space of their own. An output texture must have been
    // created with TextureDescriptor::computeWrite, or the bind is dropped.
    void setInputTexture(const Texture& texture,
                         int slot,
                         TextureSampling sampling = {});
    void setOutputTexture(const Texture& texture, int slot);

    // A uniform block without a buffer object; slot 0 is the first block.
    void setBytes(const void* data, std::size_t bytes, int slot = 0);

    template <typename T>
    void setUniform(const T& value, int slot = 0)
    {
        setBytes(&value, sizeof(T), slot);
    }

    // Runs the kernel over count work items, in groups of threadGroupWidth.
    void dispatch(int count);

    // The 2D sibling, in groups of threadGroupSize2D squared.
    void dispatch(int width, int height);

    // Takes its threadgroup counts from DispatchArguments a kernel earlier on
    // this command buffer wrote, so the CPU never sees the number.
    void dispatchIndirect(const Buffer& arguments, int offsetInBytes = 0);

    template <typename Program>
    void dispatch(Program& program, int count)
    {
        setPipeline(program.pipeline());
        program.bindResources(*this);

        // Sequenced separately: packing must happen before the size is read,
        // which argument evaluation order would not guarantee.
        const auto* uniforms = program.packedUniforms(count);
        setBytes(uniforms, (std::size_t) program.uniformByteSize());
        dispatch(count);
    }

    template <typename Program>
    void dispatch(Program& program, int width, int height)
    {
        setPipeline(program.pipeline());
        program.bindResources(*this);

        const auto* uniforms = program.packedUniforms(width, height);
        setBytes(uniforms, (std::size_t) program.uniformByteSize());
        dispatch(width, height);
    }

    // 1D only. guardCount is the generated bounds guard's limit and must be the
    // capacity, not the real count, which nothing on this side knows; a kernel
    // that must stop at the real count reads it from a buffer itself.
    template <typename Program>
    void dispatchIndirect(Program& program,
                          const Buffer& arguments,
                          int guardCount,
                          int offsetInBytes = 0)
    {
        setPipeline(program.pipeline());
        program.bindResources(*this);

        const auto* uniforms = program.packedUniforms(guardCount);
        setBytes(uniforms, (std::size_t) program.uniformByteSize());
        dispatchIndirect(arguments, offsetInBytes);
    }

    void end();

    // Metal buffer index of the first uniform block, above the storage buffers.
    static constexpr int uniformBase = 16;

    // Kernels must declare a matching [numthreads(...)] on D3D.
    static constexpr int threadGroupWidth = 64;
    static constexpr int threadGroupSize2D = 8;

    // D3D root signature limit: a slot past this binds nowhere, and a kernel
    // reading an unbound buffer silently reads zeroes, so the emitter asserts.
    static constexpr int maxBufferSlots = 8;

    // Textures share the t/u register spaces with storage buffers on D3D, so
    // they start above every buffer slot; D3D12Types.h holds the root signature
    // to this number. Metal's texture indices are a space of their own.
    static constexpr int textureRegisterBase = maxBufferSlots;

private:
    struct Native;
    Pimpl<Native> impl;
};
} // namespace eacp::GPU
