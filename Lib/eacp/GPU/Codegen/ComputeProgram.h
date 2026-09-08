#pragma once

#include "../Device/Device.h"
#include "../Frame/ComputePass.h"
#include "../Pipeline/ComputePipeline.h"
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

// Base for struct-authored compute kernels. Derive, declare uniform and buffer
// members, list them with EACP_SHADER, write define(), and call compile() from
// the constructor.
class ComputeProgram
{
public:
    ComputeProgram() = default;

    // The threadgroup this kernel is dispatched in, in place of the stock shape
    // for its rank: ComputeProgram({256}) over a 1D grid, ComputeProgram({16,
    // 16}) over a 2D one. The body reads it back through groupShape().
    explicit ComputeProgram(ThreadGroupShape shape)
    {
        builder.setThreadGroupShape(shape);
    }

    virtual ~ComputeProgram() = default;

    // Members point into the owned builder's graph and the GPU resources are
    // non-copyable, so a program is pinned in place (like ShaderProgram).
    ComputeProgram(const ComputeProgram&) = delete;
    ComputeProgram& operator=(const ComputeProgram&) = delete;

    const ShaderSource& source() const { return generated.source; }

    // The graph the body was recorded into, so either backend's text can be
    // emitted from the kernel that ships rather than from a copy of its body.
    const ShaderGraph& graph() const { return builder.graph(); }

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
               && "eacp: only a kernel written against threadId() is dispatched "
                  "with dispatch(count)");

        const std::uint32_t extents[] = {(std::uint32_t) count};
        return packWithExtents(extents, 1);
    }

    // The 2D sibling: the grid extents the two-dimensional guard reads, in the
    // order the emitted block declares them.
    const void* packedUniforms(int width, int height)
    {
        assert(dispatchRank() == DispatchRank::TwoD
               && "eacp: only a kernel written against threadPosition() is "
                  "dispatched with dispatch(width, height)");

        const std::uint32_t extents[] = {(std::uint32_t) width,
                                         (std::uint32_t) height};
        return packWithExtents(extents, 2);
    }

    // And the 3D one, whose guard reads three.
    const void* packedUniforms(int width, int height, int depth)
    {
        assert(dispatchRank() == DispatchRank::ThreeD
               && "eacp: only a kernel written against threadPosition3() is "
                  "dispatched with dispatch(width, height, depth)");

        const std::uint32_t extents[] = {
            (std::uint32_t) width, (std::uint32_t) height, (std::uint32_t) depth};
        return packWithExtents(extents, 3);
    }

    // The grid shape this kernel's body asked for, which decides which dispatch
    // it takes.
    DispatchRank dispatchRank() const { return generated.dispatchRank; }

    // This kernel's own group, which is what localId() runs to and what a
    // shared tile is sized in. Read it inside define() after the body has asked
    // for its thread index, since an unset shape resolves against the rank.
    ThreadGroupShape groupShape() const { return builder.threadGroupShape(); }

    // The stock group shape, which is what a kernel that named none is
    // dispatched in: groupWidth threads in a 1D kernel, groupSize2D squared in
    // a 2D one, groupSize3D cubed in a 3D one.
    static constexpr int groupWidth = ComputePass::threadGroupWidth;
    static constexpr int groupSize2D = ComputePass::threadGroupSize2D;
    static constexpr int groupSize3D = ComputePass::threadGroupSize3D;

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
    ThreadPosition3 threadPosition3() { return builder.threadPosition3(); }

    // The same work item as one value: a UInt2 over a grid, a UInt3 over a
    // volume, fixing the rank exactly as the two above do.
    UInt2 threadId2() { return builder.threadId2(); }
    UInt3 threadId3() { return builder.threadId3(); }
    Float constant(float value) { return builder.constant(value); }
    UInt unsignedInteger(unsigned value) { return builder.unsignedInteger(value); }

    // The threadgroup vocabulary, forwarded on the terms the ids above set:
    // where a thread sits in its group, which group it is in, the implicit
    // grid bound the dispatch supplied, a shared array, and the barrier that
    // orders access to it. Shared tiles are sized against groupShape(), which
    // is the group the dispatch really runs.
    UInt localId() { return builder.localId(); }
    ThreadPosition localPosition() { return builder.localPosition(); }
    ThreadPosition3 localPosition3() { return builder.localPosition3(); }
    UInt groupId() { return builder.groupId(); }
    ThreadPosition groupPosition() { return builder.groupPosition(); }
    ThreadPosition3 groupPosition3() { return builder.groupPosition3(); }

    // Their whole-vector forms, beside threadId2() and threadId3().
    UInt2 localId2() { return builder.localId2(); }
    UInt3 localId3() { return builder.localId3(); }
    UInt2 groupId2() { return builder.groupId2(); }
    UInt3 groupId3() { return builder.groupId3(); }

    UInt gridCount() { return builder.gridCount(); }
    UInt gridWidth() { return builder.gridWidth(); }
    UInt gridHeight() { return builder.gridHeight(); }
    UInt gridDepth() { return builder.gridDepth(); }
    void barrier() { builder.barrier(); }

    template <typename T>
    Shared<T> shared(int count)
    {
        return builder.shared<T>(count);
    }

    // The fold of what every thread of the group contributed, returned to
    // every thread. It barriers, so - like barrier() - every thread of the
    // group has to reach it or none of them.
    Float groupSum(const Float& value) { return builder.groupSum(value); }
    Float groupMax(const Float& value) { return builder.groupMax(value); }
    Float groupMin(const Float& value) { return builder.groupMin(value); }

    UInt groupSum(const UInt& value) { return builder.groupSum(value); }
    UInt groupMax(const UInt& value) { return builder.groupMax(value); }
    UInt groupMin(const UInt& value) { return builder.groupMin(value); }

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

    // Two values narrowed to fp16 and packed into the one float slot that
    // holds them, which InputBuffer::readHalf2 reads back at the same index.
    void writeHalf2(const OutputBuffer& buffer,
                    const UInt& index,
                    const Float2& value)
    {
        builder.writeHalf2(buffer, index, value);
    }

    // One element of a threadgroup-shared array, published to the rest of the
    // group by the next barrier().
    template <typename T>
    void write(const Shared<T>& array, const UInt& index, const T& value)
    {
        builder.write(array, index, value);
    }

    // One element of an integer output: the id or the count a kernel arrived
    // at, kept as an integer for the kernel after it to index with.
    void write(const UIntOutputBuffer& buffer, const UInt& index, const UInt& value)
    {
        builder.write(buffer, index, value);
    }

    void write(const UIntOutputBuffer& buffer, const UInt& index, unsigned value)
    {
        builder.write(buffer, index, value);
    }

    void write(const UIntOutputBuffer& buffer, unsigned index, const UInt& value)
    {
        builder.write(buffer, index, value);
    }

    void write(const UIntOutputBuffer& buffer, unsigned index, unsigned value)
    {
        builder.write(buffer, index, value);
    }

    // The record writes, for a buffer whose elements are records of N integers.
    void write(const UIntOutputBuffer& buffer, const UInt& index, const UInt2& value)
    {
        builder.write(buffer, index, value);
    }

    void write(const UIntOutputBuffer& buffer, const UInt& index, const UInt3& value)
    {
        builder.write(buffer, index, value);
    }

    void write(const UIntOutputBuffer& buffer, const UInt& index, const UInt4& value)
    {
        builder.write(buffer, index, value);
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
