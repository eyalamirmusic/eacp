#pragma once

#include "../Device/Device.h"
#include "../Frame/ComputePass.h"
#include "../Pipeline/ComputePipeline.h"
#include "ShaderProgram.h"

// A compute kernel authored as a struct, the compute sibling of ShaderProgram.
// Uniforms are named, typed members set by name; storage buffers are members
// assigned the GPU::Buffer to bind, with slots taken from declaration order.
// define() writes the kernel body: read inputs at threadId(), write the result
// with write(). The generated kernel guards against the rounded-up dispatch
// with an implicit element count, supplied automatically at dispatch.
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
//   kernel.output = outputBuffer;
//   kernel.scale = 3.0f;
//   kernel.prepare();               // builds library + compute pipeline
//   ...
//   pass.dispatch(kernel, count);   // pipeline + buffers + uniforms + dispatch

namespace eacp::GPU
{
// Buffer bind walk: hand each assigned storage-buffer member to the compute
// pass at the slot its handle was declared with.
class ComputeBufferBindVisitor final : public ShaderVisitor
{
public:
    explicit ComputeBufferBindVisitor(ComputePass& passToUse)
        : pass(passToUse)
    {
    }

    void
        onUniform(const char*, ValueType, detail::ValueHandle&, const void*) override
    {
    }

    void onInputBuffer(const char*,
                       InputBuffer& handle,
                       const Buffer* buffer) override
    {
        if (buffer != nullptr)
            pass.setInputBuffer(*buffer, handle.slot);
    }

    void onOutputBuffer(const char*,
                        OutputBuffer& handle,
                        const Buffer* buffer) override
    {
        if (buffer != nullptr)
            pass.setOutputBuffer(*buffer, handle.slot);
    }

private:
    ComputePass& pass;
};

// Base for struct-authored compute kernels. Derive, declare uniform and buffer
// members, list them with EACP_SHADER, write define(), and call compile() from
// the constructor.
class ComputeProgram
{
public:
    ComputeProgram() = default;
    virtual ~ComputeProgram() = default;

    // Members point into the owned builder's graph and the GPU resources are
    // non-copyable, so a program is pinned in place (like ShaderProgram).
    ComputeProgram(const ComputeProgram&) = delete;
    ComputeProgram& operator=(const ComputeProgram&) = delete;

    const ShaderSource& source() const { return generated.source; }

    // Builds the shader library and compute pipeline from the generated kernel.
    void prepare()
    {
        shaderLibrary.emplace(Device::shared(), generated.source);
        pipelineState.emplace(Device::shared(), *shaderLibrary);
    }

    const ComputePipeline& pipeline() const { return *pipelineState; }

    // Re-packs the current uniform values, appends the element count the
    // generated bounds guard reads, and returns the block, ready for
    // ComputePass::setBytes.
    const void* packedUniforms(int count)
    {
        uniformBytes.clear();
        auto uploadVisitor = ShaderUploadVisitor {uniformBytes};
        reflectMembers(uploadVisitor);

        auto offset = alignUp(uniformBytes.size(), 4);
        uniformBytes.resize(offset + (int) sizeof(std::uint32_t));

        auto value = (std::uint32_t) count;
        std::memcpy(uniformBytes.data() + offset, &value, sizeof(value));

        // After the count, so the pad lands at the struct's end where MSL puts
        // it, not between the last member and the count.
        uploadVisitor.finish();
        return uniformBytes.data();
    }

    int uniformByteSize() const { return uniformBytes.size(); }

    // Binds every assigned buffer member to the pass at its declared slot.
    // ComputePass::dispatch(program, count) calls this.
    void bindBuffers(ComputePass& pass)
    {
        auto bindVisitor = ComputeBufferBindVisitor {pass};
        reflectMembers(bindVisitor);
    }

protected:
    // Runs the member build walk (adopting uniform and buffer slots), the
    // user's define(), then emits the kernel source. Called from the
    // most-derived constructor.
    void compile()
    {
        auto buildVisitor = ShaderBuildVisitor {builder};
        reflectMembers(buildVisitor);
        define();
        generated = builder.build();
    }

    UInt threadId() { return builder.threadId(); }
    Float constant(float value) { return builder.constant(value); }

    void write(const OutputBuffer& buffer, const UInt& index, const Float& value)
    {
        builder.write(buffer, index, value);
    }

    // Generated by EACP_SHADER: visits each declared member in order.
    virtual void reflectMembers(ShaderVisitor& visitor) = 0;

    // Written by the user: the kernel body.
    virtual void define() = 0;

private:
    ShaderBuilder builder;
    GeneratedShader generated;
    Vector<std::byte> uniformBytes;

    std::optional<ShaderLibrary> shaderLibrary;
    std::optional<ComputePipeline> pipelineState;
};
} // namespace eacp::GPU
