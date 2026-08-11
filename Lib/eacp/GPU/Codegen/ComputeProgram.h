#pragma once

#include "../Device/Device.h"
#include "../Frame/ComputePass.h"
#include "../Pipeline/ComputePipeline.h"
#include "ShaderProgram.h"

// A compute kernel authored as a struct, the compute sibling of ShaderProgram.
// Uniforms and storage buffers are members taking slots in declaration order;
// define() writes the kernel body and the generated kernel bounds-guards it.

namespace eacp::GPU
{
// Hands each assigned buffer and texture member to the compute pass at the slot
// its handle was declared with.
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

    // An atomic buffer binds exactly as an output does; only the type the
    // kernel declares it through makes it atomic.
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

    // Members point into the owned builder's graph, so a program is pinned.
    ComputeProgram(const ComputeProgram&) = delete;
    ComputeProgram& operator=(const ComputeProgram&) = delete;

    const ShaderSource& source() const { return generated.source; }

    void prepare()
    {
        shaderLibrary.emplace(Device::shared(), generated.source);
        pipelineState.emplace(Device::shared(), *shaderLibrary);
    }

    const ComputePipeline& pipeline() const { return *pipelineState; }

    // Re-packs the uniforms and appends the element count the generated bounds
    // guard reads, returning a block ready for ComputePass::setBytes.
    const void* packedUniforms(int count)
    {
        assert(dispatchRank() == DispatchRank::OneD
               && "eacp: a kernel written against threadPosition() is "
                  "dispatched with dispatch(width, height)");

        const std::uint32_t extents[] = {(std::uint32_t) count};
        return packWithExtents(extents, 1);
    }

    // The 2D sibling: extents in the order the emitted block declares them.
    const void* packedUniforms(int width, int height)
    {
        assert(dispatchRank() == DispatchRank::TwoD
               && "eacp: a kernel written against threadId() is dispatched "
                  "with dispatch(count)");

        const std::uint32_t extents[] = {(std::uint32_t) width,
                                         (std::uint32_t) height};
        return packWithExtents(extents, 2);
    }

    // Decides which dispatch overload the kernel takes.
    DispatchRank dispatchRank() const { return generated.dispatchRank; }

    // The fixed group shape every dispatch uses: localId() runs to groupWidth
    // in 1D (groupSize2D per axis in 2D), and shared tiles are sized in these.
    static constexpr int groupWidth = ComputePass::threadGroupWidth;
    static constexpr int groupSize2D = ComputePass::threadGroupSize2D;

    int uniformByteSize() const { return uniformBytes.size(); }

    // Called by ComputePass::dispatch(program, ...).
    void bindResources(ComputePass& pass)
    {
        auto bindVisitor = ComputeBindVisitor {pass};
        reflectMembers(bindVisitor);
    }

protected:
    // Must be called from the most-derived constructor.
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

    // Yields what the element held before the add; see ShaderBuilder::atomicAdd
    // for what it does and does not order.
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

    // For a buffer of N-float records: the index is in records, not floats, and
    // pairs with InputBuffer::read2/3/4.
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

    // Published to the rest of the group by the next barrier().
    template <typename T>
    void write(const Shared<T>& array, const UInt& index, const T& value)
    {
        builder.write(array, index, value);
    }

    // An atomic buffer's element, set rather than added to.
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

    // One texel of a kernel's output image.
    void write(const WritableTexture2D& texture,
               const UInt& x,
               const UInt& y,
               const Float4& color)
    {
        builder.write(texture, x, y, color);
    }

    // Generated by EACP_SHADER: visits each declared member in order.
    virtual void reflectMembers(ShaderVisitor& visitor) = 0;

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

        // After the extents, so the pad lands at the struct's end as MSL puts
        // it, not between the last member and them.
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
