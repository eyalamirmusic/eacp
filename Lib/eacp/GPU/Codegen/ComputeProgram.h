#pragma once

#include "../Device/Device.h"
#include "../Frame/ComputePass.h"
#include "../Pipeline/ComputePipeline.h"
#include "ShaderProgram.h"

// A compute kernel authored as a struct, the compute sibling of ShaderProgram.
// Uniforms are named, typed members set by name; storage buffers are members
// assigned the GPU::Buffer to bind, with slots taken from declaration order.
// define() writes the kernel body: read inputs at threadId() (or, over a grid,
// at threadPosition()), write the result with write(). The generated kernel
// guards against the rounded-up dispatch with implicit extents, supplied
// automatically at dispatch - one count for a 1D kernel, a width and a height
// for a 2D one.
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

    // An atomic buffer binds exactly as an output does - a Metal device buffer,
    // a D3D UAV - since what makes it atomic is the type the kernel declares it
    // through and not how the pass hands it over.
    void onAtomicBuffer(const char*,
                        AtomicBuffer& handle,
                        const Buffer* buffer) override
    {
        if (buffer != nullptr)
            pass.setOutputBuffer(*buffer, handle.slot);
    }

    void onTexture(const char*,
                   Texture2D& handle,
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

    // Builds the shader library and compute pipeline from the generated kernel,
    // on the Device whose passes will dispatch it. A pipeline belongs to the
    // device that compiled it, so a kernel a worker Device dispatches is
    // compiled on that Device rather than on the process-wide one.
    void prepare(Device& device)
    {
        shaderLibrary.emplace(device, generated.source);
        pipelineState.emplace(device, *shaderLibrary);
    }

    void prepare() { prepare(Device::shared()); }

    const ComputePipeline& pipeline() const { return *pipelineState; }

    // Re-packs the current uniform values, appends the element count the
    // generated bounds guard reads, and returns the block, ready for
    // ComputePass::setBytes.
    const void* packedUniforms(int count)
    {
        assert(dispatchRank() == DispatchRank::OneD
               && "eacp: a kernel written against threadPosition() is "
                  "dispatched with dispatch(width, height)");

        const std::uint32_t extents[] = {(std::uint32_t) count};
        return packWithExtents(extents, 1);
    }

    // The 2D sibling: the grid extents the two-dimensional guard reads, in the
    // order the emitted block declares them.
    const void* packedUniforms(int width, int height)
    {
        assert(dispatchRank() == DispatchRank::TwoD
               && "eacp: a kernel written against threadId() is dispatched "
                  "with dispatch(count)");

        const std::uint32_t extents[] = {(std::uint32_t) width,
                                         (std::uint32_t) height};
        return packWithExtents(extents, 2);
    }

    // The grid shape this kernel's body asked for, which decides which dispatch
    // it takes.
    DispatchRank dispatchRank() const { return generated.dispatchRank; }

    // The fixed group shape every dispatch uses, restated here because it is
    // part of a shared-memory kernel's arithmetic: localId() runs to
    // groupWidth in a 1D kernel (groupSize2D per axis in a 2D one), and a
    // shared tile is sized in these units.
    static constexpr int groupWidth = ComputePass::threadGroupWidth;
    static constexpr int groupSize2D = ComputePass::threadGroupSize2D;

    int uniformByteSize() const { return uniformBytes.size(); }

    // Binds every assigned buffer and texture member to the pass at its
    // declared slot. ComputePass::dispatch(program, ...) calls this.
    void bindResources(ComputePass& pass)
    {
        auto bindVisitor = ComputeBindVisitor {pass};
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
    ThreadPosition threadPosition() { return builder.threadPosition(); }
    Float constant(float value) { return builder.constant(value); }

    // The threadgroup vocabulary, forwarded on the terms the ids above set:
    // where a thread sits in its group, which group it is in, the implicit
    // grid bound the dispatch supplied, a shared array, and the barrier that
    // orders access to it. The group shape is the fixed one the dispatch
    // uses, so shared tiles are sized against the constants below.
    UInt localId() { return builder.localId(); }
    ThreadPosition localPosition() { return builder.localPosition(); }
    UInt groupId() { return builder.groupId(); }
    ThreadPosition groupPosition() { return builder.groupPosition(); }
    UInt gridCount() { return builder.gridCount(); }
    UInt gridWidth() { return builder.gridWidth(); }
    UInt gridHeight() { return builder.gridHeight(); }
    void barrier() { builder.barrier(); }

    template <typename T>
    Shared<T> shared(int count)
    {
        return builder.shared<T>(count);
    }

    // Adds to one element of a shared counter and yields what it held before, so
    // threads that never meet each other still come away with distinct numbers.
    // See ShaderBuilder::atomicAdd for what it does and does not order.
    UInt atomicAdd(const AtomicBuffer& buffer, const UInt& index, const UInt& value)
    {
        return builder.atomicAdd(buffer, index, value);
    }

    UInt atomicAdd(const AtomicBuffer& buffer, unsigned index, const UInt& value)
    {
        return builder.atomicAdd(buffer, index, value);
    }

    UInt atomicAdd(const AtomicBuffer& buffer, const UInt& index, unsigned value)
    {
        return builder.atomicAdd(buffer, index, value);
    }

    UInt atomicAdd(const AtomicBuffer& buffer, unsigned index, unsigned value)
    {
        return builder.atomicAdd(buffer, index, value);
    }

    // Control flow, forwarded from the builder on the terms ShaderProgram
    // forwards it: a mutable local, the two branching statements, the loop and
    // its two jumps. The unsigned overload is the counter a reduction kernel
    // walks a buffer with - it lives beside the UInt indices threadId() hands
    // out, and the uint comparisons are what bound it.
    template <ShaderHandleLike T>
    Var<ShaderHandle<T>> var(const T& initialValue)
    {
        return builder.var(initialValue);
    }

    Var<Float> var(float initialValue) { return builder.var(initialValue); }
    Var<Bool> var(bool initialValue) { return builder.var(initialValue); }
    Var<Int> var(int initialValue) { return builder.var(initialValue); }
    Var<UInt> var(unsigned initialValue) { return builder.var(initialValue); }

    template <typename Body>
    void ifThen(const Bool& condition, Body&& body)
    {
        builder.ifThen(condition, std::forward<Body>(body));
    }

    template <typename Then, typename Else>
    void ifThen(const Bool& condition, Then&& whenTrue, Else&& whenFalse)
    {
        builder.ifThen(
            condition, std::forward<Then>(whenTrue), std::forward<Else>(whenFalse));
    }

    template <typename Body>
    void loop(const Bool& condition, Body&& body)
    {
        builder.loop(condition, std::forward<Body>(body));
    }

    void breakLoop() { builder.breakLoop(); }
    void continueLoop() { builder.continueLoop(); }

    void write(const OutputBuffer& buffer, const UInt& index, const Float& value)
    {
        builder.write(buffer, index, value);
    }

    // The vector writes, for a buffer whose elements are records of N floats.
    // The index is in records, so it pairs with InputBuffer::read2/3/4 and a
    // kernel never spells the stride itself.
    void write(const OutputBuffer& buffer, const UInt& index, const Float2& value)
    {
        builder.write(buffer, index, value);
    }

    void write(const OutputBuffer& buffer, const UInt& index, const Float3& value)
    {
        builder.write(buffer, index, value);
    }

    void write(const OutputBuffer& buffer, const UInt& index, const Float4& value)
    {
        builder.write(buffer, index, value);
    }

    // One element of a threadgroup-shared array, published to the rest of the
    // group by the next barrier().
    template <typename T>
    void write(const Shared<T>& array, const UInt& index, const T& value)
    {
        builder.write(array, index, value);
    }

    // An atomic buffer's element, set rather than added to - what a kernel
    // computing a dispatch size writes.
    void write(const AtomicBuffer& buffer, const UInt& index, const UInt& value)
    {
        builder.write(buffer, index, value);
    }

    void write(const AtomicBuffer& buffer, const UInt& index, unsigned value)
    {
        builder.write(buffer, index, value);
    }

    void write(const AtomicBuffer& buffer, unsigned index, const UInt& value)
    {
        builder.write(buffer, index, value);
    }

    void write(const AtomicBuffer& buffer, unsigned index, unsigned value)
    {
        builder.write(buffer, index, value);
    }

    // One texel of a kernel's output image, at the coordinates a 2D kernel
    // already has in hand from threadPosition().
    void write(const WritableTexture2D& texture,
               const UInt& x,
               const UInt& y,
               const Float4& color)
    {
        builder.write(texture, x, y, color);
    }

    // Generated by EACP_SHADER: visits each declared member in order.
    virtual void reflectMembers(ShaderVisitor& visitor) = 0;

    // Written by the user: the kernel body.
    virtual void define() = 0;

private:
    const void* packWithExtents(const std::uint32_t* extents, int count)
    {
        uniformBytes.clear();
        auto uploadVisitor = ShaderUploadVisitor {uniformBytes};
        reflectMembers(uploadVisitor);

        for (auto i = 0; i < count; ++i)
        {
            auto offset = alignUp(uniformBytes.size(), 4);
            uniformBytes.resize(offset + (int) sizeof(std::uint32_t));
            std::memcpy(
                uniformBytes.data() + offset, &extents[i], sizeof(extents[i]));
        }

        // After the extents, so the pad lands at the struct's end where MSL
        // puts it, not between the last member and them.
        uploadVisitor.finish();
        return uniformBytes.data();
    }

    ShaderBuilder builder;
    GeneratedShader generated;
    Vector<std::byte> uniformBytes;

    std::optional<ShaderLibrary> shaderLibrary;
    std::optional<ComputePipeline> pipelineState;
};
} // namespace eacp::GPU
