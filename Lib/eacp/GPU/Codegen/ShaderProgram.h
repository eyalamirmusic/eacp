#pragma once

#include <eacp/Core/Utils/Containers.h>

#include <algorithm>

#include "../Buffer/StreamingBuffers.h"
#include "../Device/Device.h"
#include "../Frame/RenderPass.h"
#include "GeneratedShader.h"
#include "PackedVertex.h"
#include "ShaderBuilder.h"
#include "ShaderTypes.h"
#include "ShaderValue.h"
#include "UniformLayout.h"

// A shader authored as a struct. Uniforms are named, typed members; vertex
// inputs are pulled out of the CPU vertex struct inside define(), which is the
// single source of the vertex layout. The program owns its GPU resources.

namespace eacp::GPU
{
// The CPU-side storage type mirroring each shader value type. Array wraps
// std::array with no added state, so the packed layout the upload walk memcpys
// is the same either way.
template <typename T>
struct CpuValueOf;

template <>
struct CpuValueOf<Float>
{
    using type = float;
};

template <>
struct CpuValueOf<Float2>
{
    using type = Array<float, 2>;
};

template <>
struct CpuValueOf<Float3>
{
    using type = Array<float, 3>;
};

template <>
struct CpuValueOf<Float4>
{
    using type = Array<float, 4>;
};

template <>
struct CpuValueOf<Float4x4>
{
    using type = Array<float, 16>;
};

template <>
struct CpuValueOf<UInt>
{
    using type = std::uint32_t;
};

template <>
struct CpuValueOf<Int>
{
    using type = std::int32_t;
};

// There is deliberately no CpuValueOf for a Bool or a boolean vector:
// ShaderBuilder refuses those as uniforms.
template <>
struct CpuValueOf<Int2>
{
    using type = std::array<std::int32_t, 2>;
};

template <>
struct CpuValueOf<Int3>
{
    using type = std::array<std::int32_t, 3>;
};

template <>
struct CpuValueOf<Int4>
{
    using type = std::array<std::int32_t, 4>;
};

// The shader value a CPU type maps to. A user type opts in with a
// `using ShaderValue = ...` member or via EACP_SHADER_VALUE. The unmapped
// primary stays empty so the sub-type assignment constraint fails softly.
template <typename T>
struct ShaderValueOf
{
};

template <typename T>
    requires requires { typename T::ShaderValue; }
struct ShaderValueOf<T>
{
    using type = typename T::ShaderValue;
};

template <>
struct ShaderValueOf<float>
{
    using type = Float;
};

template <>
struct ShaderValueOf<float[2]>
{
    using type = Float2;
};

template <>
struct ShaderValueOf<float[3]>
{
    using type = Float3;
};

template <>
struct ShaderValueOf<float[4]>
{
    using type = Float4;
};

template <>
struct ShaderValueOf<std::array<float, 2>>
{
    using type = Float2;
};

template <>
struct ShaderValueOf<std::array<float, 3>>
{
    using type = Float3;
};

template <>
struct ShaderValueOf<std::array<float, 4>>
{
    using type = Float4;
};

template <>
struct ShaderValueOf<std::array<float, 16>>
{
    using type = Float4x4;
};

template <>
struct ShaderValueOf<Array<float, 2>>
{
    using type = Float2;
};

template <>
struct ShaderValueOf<Array<float, 3>>
{
    using type = Float3;
};

template <>
struct ShaderValueOf<Array<float, 4>>
{
    using type = Float4;
};

template <>
struct ShaderValueOf<Array<float, 16>>
{
    using type = Float4x4;
};

// True when V is a CPU type registered as the shader value type T.
template <typename V, typename T>
concept ShaderValueIs = requires { typename ShaderValueOf<V>::type; }
                        && std::same_as<typename ShaderValueOf<V>::type, T>;

// Computed from a real object so it stays within defined behaviour, unlike the
// classic null-pointer offsetof.
template <typename C, typename M>
int memberOffset(M C::* member)
{
    auto object = C {};
    return (int) (reinterpret_cast<const std::byte*>(&(object.*member))
                  - reinterpret_cast<const std::byte*>(&object));
}

// Both the graph value define() uses and a typed CPU slot the upload walk packs
// into the uniform block. Also accepts any registered sub-type of the same
// shape, e.g. `shader.tint = Color {1, 0, 0}` for a Uniform<Float3>.
template <typename T>
struct Uniform : T
{
    using Cpu = typename CpuValueOf<T>::type;

    Uniform& operator=(const Cpu& newValue)
    {
        value = newValue;
        return *this;
    }

    template <ShaderValueIs<T> V>
    Uniform& operator=(const V& subValue)
    {
        static_assert(sizeof(V) == sizeof(Cpu),
                      "uniform sub-type size does not match its shader value type");
        std::memcpy(&value, &subValue, sizeof(Cpu));
        return *this;
    }

    Cpu value {};
};

// The program stores a pointer, so the texture must outlive the draw.
template <>
struct Uniform<Texture2D> : Texture2D
{
    Uniform& operator=(const Texture& newTexture)
    {
        value = &newTexture;
        return *this;
    }

    const Texture* value = nullptr;

    // Must be set before compile() runs, the build walk reading it to place the
    // sampler.
    TextureSampling sampling {};
};

// Storage-buffer members of a compute program. The program stores a pointer, so
// the buffer must outlive the dispatch.
template <>
struct Uniform<InputBuffer> : InputBuffer
{
    Uniform& operator=(const Buffer& newBuffer)
    {
        value = &newBuffer;
        return *this;
    }

    const Buffer* value = nullptr;
};

template <>
struct Uniform<OutputBuffer> : OutputBuffer
{
    Uniform& operator=(const Buffer& newBuffer)
    {
        value = &newBuffer;
        return *this;
    }

    const Buffer* value = nullptr;
};

// The atomic sibling, bound the same way an output is. Its contents are
// unsigned integers rather than floats, so initialise it as such.
template <>
struct Uniform<AtomicBuffer> : AtomicBuffer
{
    Uniform& operator=(const Buffer& newBuffer)
    {
        value = &newBuffer;
        return *this;
    }

    const Buffer* value = nullptr;
};

// A kernel's output image. It carries no sampling, and the texture must have
// been created with TextureDescriptor::computeWrite.
template <>
struct Uniform<WritableTexture2D> : WritableTexture2D
{
    Uniform& operator=(const Texture& newTexture)
    {
        value = &newTexture;
        return *this;
    }

    const Texture* value = nullptr;
};

constexpr VertexFormat toVertexFormat(ValueType type)
{
    switch (type)
    {
        case ValueType::Float:
            return VertexFormat::Float;
        case ValueType::Float2:
            return VertexFormat::Float2;
        case ValueType::Float3:
            return VertexFormat::Float3;
        case ValueType::Float4:
        case ValueType::Float2x2:
        case ValueType::Float3x3:
        case ValueType::Float4x4:
        case ValueType::UInt:
        case ValueType::Int:
        case ValueType::Int2:
        case ValueType::Int3:
        case ValueType::Int4:
        case ValueType::Bool:
        case ValueType::Bool2:
        case ValueType::Bool3:
        case ValueType::Bool4:
            return VertexFormat::Float4; // matrix/integer/bool are never attributes
    }

    return VertexFormat::Float;
}

// A CPU vertex field's wire format: whatever its shader value implies, unless
// the type declares a vertexFormat of its own. See PackedVertex.h.
template <typename T>
struct VertexFormatOf
{
    static constexpr auto value =
        toVertexFormat(ValueTypeOf<typename ShaderValueOf<T>::type>::value);
};

template <typename T>
    requires requires { T::vertexFormat; }
struct VertexFormatOf<T>
{
    static constexpr auto value = T::vertexFormat;
};

// What a field of type M is expected to occupy: a packed field is measured
// against the format it declares, anything else against its CPU type.
template <typename M, typename Handle>
constexpr std::size_t expectedAttributeBytes()
{
    if constexpr (requires { M::vertexFormat; })
        return (std::size_t) bytesPerAttribute(M::vertexFormat);
    else
        return sizeof(typename CpuValueOf<Handle>::type);
}

// The non-templated surface the uniform member walk bottoms out in; the
// templated operator() adapts any Uniform<T> onto it.
class ShaderVisitor
{
public:
    virtual ~ShaderVisitor() = default;

    template <typename T>
    void operator()(const char* name, Uniform<T>& member)
    {
        onUniform(name, ValueTypeOf<T>::value, member, &member.value);
    }

    void operator()(const char* name, Uniform<Texture2D>& member)
    {
        onTexture(name, member, member.value, member.sampling);
    }

    void operator()(const char* name, Uniform<InputBuffer>& member)
    {
        onInputBuffer(name, member, member.value);
    }

    void operator()(const char* name, Uniform<OutputBuffer>& member)
    {
        onOutputBuffer(name, member, member.value);
    }

    void operator()(const char* name, Uniform<AtomicBuffer>& member)
    {
        onAtomicBuffer(name, member, member.value);
    }

    void operator()(const char* name, Uniform<WritableTexture2D>& member)
    {
        onWritableTexture(name, member, member.value);
    }

protected:
    virtual void onUniform(const char* name,
                           ValueType type,
                           detail::ValueHandle& handle,
                           const void* data) = 0;

    // Texture and storage-buffer members are not packed into the uniform block,
    // so only the walks that care (build, resource bind) override these.
    virtual void onTexture(const char*, Texture2D&, const Texture*, TextureSampling)
    {
    }
    virtual void onInputBuffer(const char*, InputBuffer&, const Buffer*) {}
    virtual void onOutputBuffer(const char*, OutputBuffer&, const Buffer*) {}
    virtual void onAtomicBuffer(const char*, AtomicBuffer&, const Buffer*) {}
    virtual void onWritableTexture(const char*, WritableTexture2D&, const Texture*)
    {
    }
};

// Build walk: each uniform member adopts a freshly added graph slot, so define()
// can use it as a live value.
class ShaderBuildVisitor final : public ShaderVisitor
{
public:
    explicit ShaderBuildVisitor(ShaderBuilder& builderToUse)
        : builder(builderToUse)
    {
    }

    void onUniform(const char*,
                   ValueType type,
                   detail::ValueHandle& handle,
                   const void*) override
    {
        handle = builder.addUniform(type);
    }

    void onTexture(const char*,
                   Texture2D& handle,
                   const Texture*,
                   TextureSampling sampling) override
    {
        handle = builder.texture(sampling);
    }

    void onInputBuffer(const char*, InputBuffer& handle, const Buffer*) override
    {
        handle = builder.inputBuffer();
    }

    void onOutputBuffer(const char*, OutputBuffer& handle, const Buffer*) override
    {
        handle = builder.outputBuffer();
    }

    void onAtomicBuffer(const char*, AtomicBuffer& handle, const Buffer*) override
    {
        handle = builder.atomicBuffer();
    }

    void onWritableTexture(const char*,
                           WritableTexture2D& handle,
                           const Texture*) override
    {
        handle = builder.writableTexture();
    }

private:
    ShaderBuilder& builder;
};

// Texture bind walk: hand each assigned texture member to the render pass at
// the slot its handle was declared with.
class ShaderTextureBindVisitor final : public ShaderVisitor
{
public:
    explicit ShaderTextureBindVisitor(RenderPass& passToUse)
        : pass(passToUse)
    {
    }

    void
        onUniform(const char*, ValueType, detail::ValueHandle&, const void*) override
    {
    }

    void onTexture(const char*,
                   Texture2D& handle,
                   const Texture* texture,
                   TextureSampling sampling) override
    {
        if (texture != nullptr)
            pass.setFragmentTexture(*texture, handle.slot, sampling);
    }

private:
    RenderPass& pass;
};

// Bound to both stages: which stage reads the buffer is a property of define(),
// and a stage whose generated function never declares it ignores the bind.
class ShaderBufferBindVisitor final : public ShaderVisitor
{
public:
    explicit ShaderBufferBindVisitor(RenderPass& passToUse)
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
        if (buffer == nullptr)
            return;

        pass.setVertexStorageBuffer(*buffer, handle.slot);
        pass.setFragmentStorageBuffer(*buffer, handle.slot);
    }

    void onOutputBuffer(const char*, OutputBuffer&, const Buffer*) override
    {
        assert(false
               && "eacp: a render program cannot write a buffer - "
                  "Uniform<OutputBuffer> belongs to a ComputeProgram");
    }

    void onAtomicBuffer(const char*, AtomicBuffer&, const Buffer*) override
    {
        assert(false
               && "eacp: a render program cannot write a buffer - "
                  "Uniform<AtomicBuffer> belongs to a ComputeProgram");
    }

    void onWritableTexture(const char*, WritableTexture2D&, const Texture*) override
    {
        assert(false
               && "eacp: a render program cannot write a texture - "
                  "Uniform<WritableTexture2D> belongs to a ComputeProgram");
    }

private:
    RenderPass& pass;
};

// Copies each uniform into the block at its aligned offset. The caller must run
// finish() once the walk and any appended tail are done: Metal's validation
// layer checks the bound length against MSL's padded sizeof(Uniforms).
class ShaderUploadVisitor final : public ShaderVisitor
{
public:
    explicit ShaderUploadVisitor(Vector<std::byte>& bytesToFill)
        : bytes(bytesToFill)
    {
    }

    void onUniform(const char*,
                   ValueType type,
                   detail::ValueHandle&,
                   const void* data) override
    {
        auto alignment = uniformAlignment(type);
        auto offset = alignUp(cursor, alignment);
        auto next = offset + uniformSlotStride(type);

        if (bytes.size() < next)
            bytes.resize(next);

        std::memcpy(bytes.data() + offset, data, (std::size_t) byteSize(type));
        cursor = next;

        if (alignment > blockAlignment)
            blockAlignment = alignment;
    }

    void finish() { bytes.resize(alignUp(bytes.size(), blockAlignment)); }

private:
    Vector<std::byte>& bytes;
    int cursor = 0;
    int blockAlignment = 1;
};

// Base for struct-authored shaders. Derive, declare uniform members, list them
// with EACP_SHADER, write define() (pulling vertex inputs from the CPU vertex
// struct), and call compile() from the constructor.
class ShaderProgram
{
public:
    ShaderProgram() = default;
    virtual ~ShaderProgram() = default;

    // Members point into the owned builder's graph, so a program is pinned.
    ShaderProgram(const ShaderProgram&) = delete;
    ShaderProgram& operator=(const ShaderProgram&) = delete;

    const ShaderSource& source() const { return generated.source; }
    const VertexLayout& vertexLayout() const { return generated.vertexLayout; }

    // Owns the resulting buffer. The element type's size must match the layout
    // pulled from it in define().
    template <typename V, std::size_t N>
    void setVertices(const V (&data)[N])
    {
        setVertices(data, (int) N);
    }

    template <typename V>
    void setVertices(const V* data, int count)
    {
        assert(sizeof(V) == (std::size_t) vertexLayout().stride
               && "vertex element size does not match the shader's vertex layout");

        vertexBufferData.emplace(
            Device::shared(), data, sizeof(V) * (std::size_t) count);
        vertexCountValue = count;
    }

    // Owns the resulting buffer; draw(program) then draws indexed.
    template <std::size_t N>
    void setIndices(const std::uint32_t (&data)[N])
    {
        setIndices(data, (int) N);
    }

    template <std::size_t N>
    void setIndices(const std::uint16_t (&data)[N])
    {
        setIndices(data, (int) N);
    }

    void setIndices(const std::uint32_t* data, int count)
    {
        uploadIndices(data, sizeof(std::uint32_t), count, IndexFormat::UInt32);
    }

    void setIndices(const std::uint16_t* data, int count)
    {
        uploadIndices(data, sizeof(std::uint16_t), count, IndexFormat::UInt16);
    }

    // bufferIndex must match the slot an instanceInput() pulled into, and the
    // element size that slot's per-instance stride. Each call gets storage no
    // earlier call's draw is still reading, so a program can be flushed often.
    template <typename I, std::size_t N>
    void setInstances(int bufferIndex, const I (&data)[N])
    {
        setInstances(bufferIndex, data, (int) N);
    }

    template <typename I>
    void setInstances(int bufferIndex, const I* data, int count)
    {
        assert(bufferIndex >= 0 && bufferIndex < vertexLayout().buffers.size()
               && "instance buffer slot was not declared via instanceInput");
        assert(sizeof(I) == (std::size_t) vertexLayout().buffers[bufferIndex].stride
               && "instance element size does not match the shader's "
                  "per-instance layout");

        if (instanceBuffers.size() <= bufferIndex)
        {
            instanceStreams.resize(bufferIndex + 1);
            instanceBuffers.resize(bufferIndex + 1);
        }

        auto& stream = instanceStreams[bufferIndex];

        if (!stream.has_value())
            stream.emplace(BufferUsage::Vertex);

        instanceBuffers[bufferIndex] =
            &stream->write(data, sizeof(I) * (std::size_t) count);
        instanceCountValue = count;
        setExternalInstanceBuffer(bufferIndex, nullptr);
    }

    // Points an instance slot at a buffer the program does not own, typically a
    // kernel's output. It must outlive the draw, and its elements must match
    // the per-instance stride instanceInput() declared for this slot.
    void setInstanceBuffer(int bufferIndex, const Buffer& buffer, int count)
    {
        assert(bufferIndex >= 0 && bufferIndex < vertexLayout().buffers.size()
               && "instance buffer slot was not declared via instanceInput");

        setExternalInstanceBuffer(bufferIndex, &buffer);
        instanceCountValue = count;
    }

    // sampleCount, depth and colorFormat must all match the render target -
    // neither backend takes a draw whose pipeline disagrees with its
    // attachment. blendMode defaults to None, writing fragments straight through.
    void prepare(int sampleCount,
                 bool depth = false,
                 PrimitiveTopology topology = PrimitiveTopology::Triangles,
                 BlendMode blendMode = BlendMode::None,
                 PixelFormat colorFormat = PixelFormat::BGRA8Unorm)
    {
        auto descriptor = RenderPipelineDescriptor {};
        descriptor.sampleCount = sampleCount;
        descriptor.depth = depth;
        descriptor.topology = topology;
        descriptor.blendMode = blendMode;
        descriptor.colorFormat = colorFormat;

        prepare(descriptor);
    }

    // The named form, and the only way to reach cull mode, front face and the
    // depth comparison. The descriptor's library and vertexLayout fields are
    // overwritten with the program's own.
    void prepare(RenderPipelineDescriptor descriptor)
    {
        shaderLibrary.emplace(Device::shared(), generated.source);

        descriptor.library = &*shaderLibrary;
        descriptor.vertexLayout = generated.vertexLayout;

        pipelineState.emplace(Device::shared(), descriptor);
    }

    const RenderPipeline& pipeline() const { return *pipelineState; }
    const Buffer& vertices() const { return *vertexBufferData; }
    int vertexCount() const { return vertexCountValue; }

    bool hasIndices() const { return indexBufferData.has_value(); }
    const Buffer& indices() const { return *indexBufferData; }
    int indexCount() const { return indexCountValue; }
    IndexFormat indexFormat() const { return indexFormatValue; }

    // Re-packs the current uniform values and returns the block, ready for
    // RenderPass::setVertexBytes.
    const void* packedUniforms()
    {
        packUniforms();
        return uniformBytes.data();
    }

    int uniformByteSize() const { return uniformBytes.size(); }
    bool hasUniforms() const { return !uniformBytes.empty(); }

    // Which stage define() actually read a uniform from. Ask these rather than
    // hasUniforms() when hand-rolling a draw over app-owned geometry.
    bool vertexReadsUniforms() const { return generated.vertexReadsUniforms; }
    bool fragmentReadsUniforms() const { return generated.fragmentReadsUniforms; }

    // Called by RenderPass::draw(program).
    void bindTextures(RenderPass& pass)
    {
        auto bindVisitor = ShaderTextureBindVisitor {pass};
        reflectMembers(bindVisitor);
    }

    // Its storage-buffer sibling, also called by RenderPass::draw(program).
    void bindBuffers(RenderPass& pass)
    {
        auto bindVisitor = ShaderBufferBindVisitor {pass};
        reflectMembers(bindVisitor);
    }

    // True once any instanceInput() was pulled: the program must be drawn with
    // drawInstanced(program, ...).
    bool isInstanced() const { return usesInstancing; }

    int instanceCount() const { return instanceCountValue; }

    // Called by RenderPass::drawInstanced(program, ...) after binding the
    // per-vertex buffer at slot 0.
    void bindInstances(RenderPass& pass)
    {
        // Over both lists: a program fed only by kernels has no owned uploads.
        auto slots =
            std::max(instanceBuffers.size(), externalInstanceBuffers.size());

        for (auto slot = 0; slot < slots; ++slot)
            if (const auto* buffer = instanceBufferAt(slot))
                pass.setVertexBuffer(*buffer, slot);
    }

protected:
    // Must be called from the most-derived constructor.
    void compile()
    {
        auto buildVisitor = ShaderBuildVisitor {builder};
        reflectMembers(buildVisitor);
        define();
        generated = builder.build();

        // define() assembled the vertex layout from the pulled fields' real
        // offsets; use it when any input was pulled.
        if (vertexLayoutData.attributes.size() > 0)
        {
            // Publish the per-vertex slot 0 too, so every bound buffer carries
            // a stride and step rate.
            if (usesInstancing)
                vertexLayoutData.buffer(
                    0, vertexLayoutData.stride, StepRate::PerVertex);

            generated.vertexLayout = vertexLayoutData;
        }

        packUniforms();
    }

    // Pulls a vertex attribute out of the CPU vertex struct, at the field's
    // real offset and with the shader value ShaderValueOf maps its type to.
    template <typename C, typename M>
    typename ShaderValueOf<M>::type vertexInput(M C::* member)
    {
        using Handle = typename ShaderValueOf<M>::type;
        static_assert(sizeof(M) == expectedAttributeBytes<M, Handle>(),
                      "vertex field size does not match the format it declares");

        constexpr auto type = ValueTypeOf<Handle>::value;
        vertexLayoutData.attribute(VertexFormatOf<M>::value, memberOffset(member));
        vertexLayoutData.stride = (int) sizeof(C);

        auto added = builder.addVertexInput(type);

        auto handle = Handle {};
        handle.graph = added.graph;
        handle.node = added.node;
        return handle;
    }

    // Per-instance sibling of vertexInput. bufferIndex is the slot the matching
    // setInstances() buffer binds to; the per-vertex geometry stays at slot 0,
    // so per-instance streams start at 1.
    template <typename C, typename M>
    typename ShaderValueOf<M>::type instanceInput(M C::* member, int bufferIndex)
    {
        using Handle = typename ShaderValueOf<M>::type;
        static_assert(sizeof(M) == expectedAttributeBytes<M, Handle>(),
                      "instance field size does not match the format it declares");

        constexpr auto type = ValueTypeOf<Handle>::value;
        vertexLayoutData.attribute(
            VertexFormatOf<M>::value, memberOffset(member), bufferIndex);
        vertexLayoutData.buffer(bufferIndex, (int) sizeof(C), StepRate::PerInstance);
        usesInstancing = true;

        auto added = builder.addInstanceInput(type, bufferIndex);

        auto handle = Handle {};
        handle.graph = added.graph;
        handle.node = added.node;
        return handle;
    }

    Float varying(const Float& vertexValue) { return builder.varying(vertexValue); }
    Float2 varying(const Float2& vertexValue)
    {
        return builder.varying(vertexValue);
    }
    Float3 varying(const Float3& vertexValue)
    {
        return builder.varying(vertexValue);
    }
    Float4 varying(const Float4& vertexValue)
    {
        return builder.varying(vertexValue);
    }

    Float constant(float value) { return builder.constant(value); }
    Bool boolean(bool value) { return builder.boolean(value); }
    Int integer(int value) { return builder.integer(value); }

    template <ShaderValueLike T, SameShaderShape<T>... Rest>
    ConstantArray<ShaderBase<T>, 1 + (int) sizeof...(Rest)>
        array(const T& first, const Rest&... rest)
    {
        return builder.array(first, rest...);
    }

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

    // In-shader transform builders, column-major and right-handed with a [0,1]
    // depth range.
    Float4x4 translate(float x, float y, float z)
    {
        auto o = constant(1.0f);
        auto z0 = constant(0.0f);
        return float4x4(float4(o, z0, z0, z0),
                        float4(z0, o, z0, z0),
                        float4(z0, z0, o, z0),
                        float4(constant(x), constant(y), constant(z), o));
    }

    Float4x4 translate(const Float& x, const Float& y, const Float& z)
    {
        auto o = constant(1.0f);
        auto z0 = constant(0.0f);
        return float4x4(float4(o, z0, z0, z0),
                        float4(z0, o, z0, z0),
                        float4(z0, z0, o, z0),
                        float4(x, y, z, o));
    }

    Float4x4 rotateY(const Float& angle)
    {
        auto c = cos(angle);
        auto s = sin(angle);
        auto z0 = constant(0.0f);
        auto o = constant(1.0f);
        return float4x4(float4(c, z0, -s, z0),
                        float4(z0, o, z0, z0),
                        float4(s, z0, c, z0),
                        float4(z0, z0, z0, o));
    }

    Float4x4 rotateZ(const Float& angle)
    {
        auto c = cos(angle);
        auto s = sin(angle);
        auto z0 = constant(0.0f);
        auto o = constant(1.0f);
        return float4x4(float4(c, s, z0, z0),
                        float4(-s, c, z0, z0),
                        float4(z0, z0, o, z0),
                        float4(z0, z0, z0, o));
    }

    Float4x4 rotateX(float radians)
    {
        auto c = constant(std::cos(radians));
        auto s = constant(std::sin(radians));
        auto z0 = constant(0.0f);
        auto o = constant(1.0f);
        return float4x4(float4(o, z0, z0, z0),
                        float4(z0, c, s, z0),
                        float4(z0, -s, c, z0),
                        float4(z0, z0, z0, o));
    }

    // aspect is a live uniform; the field of view, near and far are baked in.
    Float4x4 perspective(const Float& aspect, float fovY, float nearZ, float farZ)
    {
        auto f = constant(1.0f / std::tan(fovY * 0.5f));
        auto z0 = constant(0.0f);
        return float4x4(
            float4(f / aspect, z0, z0, z0),
            float4(z0, f, z0, z0),
            float4(z0, z0, constant(farZ / (nearZ - farZ)), constant(-1.0f)),
            float4(z0, z0, constant((farZ * nearZ) / (nearZ - farZ)), z0));
    }

    void setPosition(const Float4& clipPosition) { builder.position(clipPosition); }
    void setFragment(const Float4& color) { builder.fragment(color); }

    // Kills the fragment when value falls below threshold, before any colour or
    // depth is written.
    void setDiscardBelow(const Float& value, float threshold)
    {
        builder.discardBelow(value, threshold);
    }

    // Generated by EACP_SHADER: visits each declared uniform member in order.
    virtual void reflectMembers(ShaderVisitor& visitor) = 0;

    virtual void define() = 0;

private:
    void packUniforms()
    {
        uniformBytes.clear();
        auto uploadVisitor = ShaderUploadVisitor {uniformBytes};
        reflectMembers(uploadVisitor);
        uploadVisitor.finish();
    }

    void setExternalInstanceBuffer(int bufferIndex, const Buffer* buffer)
    {
        if (externalInstanceBuffers.size() <= bufferIndex)
            externalInstanceBuffers.resize(bufferIndex + 1);

        externalInstanceBuffers[bufferIndex] = buffer;
    }

    // A slot carries either an owned upload or a borrowed buffer; the last call
    // for that slot wins, so a program can be re-pointed between the two.
    const Buffer* instanceBufferAt(int slot) const
    {
        if (slot < externalInstanceBuffers.size()
            && externalInstanceBuffers[slot] != nullptr)
            return externalInstanceBuffers[slot];

        if (slot < instanceBuffers.size() && instanceBuffers[slot] != nullptr)
            return instanceBuffers[slot];

        return nullptr;
    }

    // A fresh buffer per call, deliberately: refilling one in place would hand
    // an already-encoded draw the next call's data, since a draw reads its
    // buffer when the frame executes rather than when it was encoded.
    void uploadIndices(const void* data,
                       std::size_t elementSize,
                       int count,
                       IndexFormat format)
    {
        indexBufferData.emplace(Device::shared(),
                                data,
                                elementSize * (std::size_t) count,
                                BufferUsage::Index);
        indexCountValue = count;
        indexFormatValue = format;
    }

    ShaderBuilder builder;
    GeneratedShader generated;
    VertexLayout vertexLayoutData;
    Vector<std::byte> uniformBytes;

    std::optional<Buffer> vertexBufferData;
    std::optional<Buffer> indexBufferData;
    std::optional<ShaderLibrary> shaderLibrary;
    std::optional<RenderPipeline> pipelineState;

    // Indexed by vertex-buffer slot; slot 0 stays empty (the per-vertex
    // buffer). The stream owns a slot's storage across frames, the pointer
    // beside it names the one buffer the last setInstances wrote.
    bool usesInstancing = false;
    Vector<std::optional<StreamingBuffers>> instanceStreams;
    Vector<const Buffer*> instanceBuffers;

    // Slots pointed at a buffer someone else owns (setInstanceBuffer), parallel
    // to instanceBuffers so a slot can be moved between the two.
    Vector<const Buffer*> externalInstanceBuffers;
    int instanceCountValue = 0;

    int vertexCountValue = 0;
    int indexCountValue = 0;
    IndexFormat indexFormatValue = IndexFormat::UInt32;
};
} // namespace eacp::GPU

// Member-list reflection: list the declared uniform members once and a reflect
// body is generated that hands each to the visitor by name.
#define EACP_GPU_PARENS ()

#define EACP_GPU_EXPAND(...)                                                        \
    EACP_GPU_EXPAND3(EACP_GPU_EXPAND3(EACP_GPU_EXPAND3(__VA_ARGS__)))
#define EACP_GPU_EXPAND3(...)                                                       \
    EACP_GPU_EXPAND2(EACP_GPU_EXPAND2(EACP_GPU_EXPAND2(__VA_ARGS__)))
#define EACP_GPU_EXPAND2(...)                                                       \
    EACP_GPU_EXPAND1(EACP_GPU_EXPAND1(EACP_GPU_EXPAND1(__VA_ARGS__)))
#define EACP_GPU_EXPAND1(...) __VA_ARGS__

#define EACP_GPU_VISIT_FIELD(visitor, field) visitor(#field, field);
#define EACP_GPU_FIELDS_HELPER(visitor, a, ...)                                     \
    EACP_GPU_VISIT_FIELD(visitor, a)                                                \
    __VA_OPT__(EACP_GPU_FIELDS_AGAIN EACP_GPU_PARENS(visitor, __VA_ARGS__))
#define EACP_GPU_FIELDS_AGAIN() EACP_GPU_FIELDS_HELPER
#define EACP_GPU_FIELDS(visitor, ...)                                               \
    __VA_OPT__(EACP_GPU_EXPAND(EACP_GPU_FIELDS_HELPER(visitor, __VA_ARGS__)))

#define EACP_SHADER(...)                                                            \
    void reflectMembers(eacp::GPU::ShaderVisitor& visitor) override                 \
    {                                                                               \
        EACP_GPU_FIELDS(visitor, __VA_ARGS__)                                       \
    }

// Teach the shader layer that a CPU type maps to a shader value (e.g. a 3-float
// Color is a Float3). Place at namespace scope after the type is defined.
#define EACP_SHADER_VALUE(Type, Handle)                                             \
    template <>                                                                     \
    struct eacp::GPU::ShaderValueOf<Type>                                           \
    {                                                                               \
        using type = eacp::GPU::Handle;                                             \
    };
