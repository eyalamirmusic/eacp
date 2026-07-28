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
        assert(dispatchRank() == DispatchRank::OneD
               && "eacp: a kernel written against threadPosition() is "
                  "dispatched with dispatch(width, height)");

        assert(coversWholeGroups(count, ComputePass::threadGroupWidth)
               && "eacp: a kernel with a barrier must be dispatched over a whole "
                  "number of threadgroups - the bounds guard returns early, and a "
                  "thread that returns before a barrier its neighbours are waiting "
                  "at is undefined. Round the count up and guard the writes.");

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

        assert(coversWholeGroups(width, ComputePass::threadGroupSize2D)
               && coversWholeGroups(height, ComputePass::threadGroupSize2D)
               && "eacp: a kernel with a barrier must be dispatched over a whole "
                  "number of threadgroups in both axes");

        const std::uint32_t extents[] = {(std::uint32_t) width,
                                         (std::uint32_t) height};
        return packWithExtents(extents, 2);
    }

    // The grid shape this kernel's body asked for, which decides which dispatch
    // it takes.
    DispatchRank dispatchRank() const { return generated.dispatchRank; }

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
    UInt threadIndexInGroup() { return builder.threadIndexInGroup(); }

    // Threadgroup memory and the barrier that makes one thread's writes to it
    // visible to the rest. See ShaderBuilder::barrier for the one rule: every
    // thread in the group reaches it, or none does.
    template <typename T, int Size>
    SharedArray<T, Size> sharedArray()
    {
        return builder.sharedArray<T, Size>();
    }

    void barrier() { builder.barrier(); }

    Float constant(float value) { return builder.constant(value); }
    Bool boolean(bool value) { return builder.boolean(value); }
    Int integer(int value) { return builder.integer(value); }

    template <ShaderHandleLike T, SameShaderHandle<T>... Rest>
    ConstantArray<ShaderHandle<T>, 1 + (int) sizeof...(Rest)>
        array(const T& first, const Rest&... rest)
    {
        return builder.array(first, rest...);
    }

    // Control flow, forwarded from the builder exactly as ShaderProgram
    // forwards it. A kernel needs it more than a fragment stage does: walking a
    // list whose length only the CPU knows is what a buffer input is for, and
    // that is a loop over a mutable counter or it is nothing.
    template <ShaderHandleLike T>
    Var<ShaderHandle<T>> var(const T& initialValue)
    {
        return builder.var(initialValue);
    }

    template <typename T>
        requires(isMatrix(ValueTypeOf<T>::value))
    Var<T> var(const T& initialValue)
    {
        return builder.var(initialValue);
    }

    Var<Float> var(float initialValue) { return builder.var(initialValue); }
    Var<Bool> var(bool initialValue) { return builder.var(initialValue); }
    Var<Int> var(int initialValue) { return builder.var(initialValue); }

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

    template <typename T, int Size>
    void write(const SharedArray<T, Size>& array, const UInt& index, const T& value)
    {
        builder.write(array, index, value);
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
    // Only a kernel that waits for its group is held to this: every other one is
    // free to be dispatched over any count at all, the guard simply retiring the
    // threads past the end.
    bool coversWholeGroups(int extent, int groupSize) const
    {
        return !generated.usesBarriers || extent % groupSize == 0;
    }

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
