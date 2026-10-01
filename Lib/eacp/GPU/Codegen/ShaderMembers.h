#pragma once

#include <eacp/Core/Utils/Containers.h>

#include <array>
#include <bit>
#include <cstring>

#include "../Buffer/Buffer.h"
#include "ShaderBuilder.h"
#include "ShaderTypes.h"
#include "ShaderValue.h"
#include "UniformLayout.h"

// The member half of a struct-authored shader or kernel, which needs no device:
// the typed Uniform<T> members, the visitor their EACP_SHADER list is walked
// with, and the build and upload walks every program runs. ShaderProgram and
// ComputeKernel are both written on top of it.

namespace eacp::GPU
{
// The CPU-side storage type mirroring each shader value type. A scalar is a
// float; a vector is a packed Array, so a uniform reads like the data it is.
// Array wraps std::array with no added state, so the packed layout the upload
// walk memcpys is the same either way.
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

// The integer vectors cross from the CPU exactly as the float ones do, and read
// as the packed data they are. There is deliberately no CpuValueOf for a Bool
// or a boolean vector: ShaderBuilder refuses those as uniforms.
template <>
struct CpuValueOf<UInt2>
{
    using type = Array<std::uint32_t, 2>;
};

template <>
struct CpuValueOf<UInt3>
{
    using type = Array<std::uint32_t, 3>;
};

template <>
struct CpuValueOf<UInt4>
{
    using type = Array<std::uint32_t, 4>;
};

template <>
struct CpuValueOf<Int2>
{
    using type = Array<std::int32_t, 2>;
};

template <>
struct CpuValueOf<Int3>
{
    using type = Array<std::int32_t, 3>;
};

template <>
struct CpuValueOf<Int4>
{
    using type = Array<std::int32_t, 4>;
};

// The shader value a CPU type maps to. Built in for float / float[N] / array; a
// user type opts in either intrusively (a `using ShaderValue = Float3;` member,
// like MIRO_REFLECT) or non-intrusively via EACP_SHADER_VALUE (like
// MIRO_REFLECT_EXTERNAL). This is what lets a `Color` struct stand in for a
// float3 vertex field or uniform. The unmapped primary stays empty so the
// sub-type assignment constraint below fails softly for plain types (e.g. an
// int assigned to a Uniform<UInt> picks the CPU-value overload) instead of
// erroring inside `typename T::ShaderValue`.
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

// The shader EDSL reads a caller's own vertex-struct members and uniform
// assignments, so it keeps recognising std::array alongside EA::Array -- the
// container migration moved eacp's interfaces, not what a consumer may hand in.
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

// Byte offset of a data member within its struct, computed from a real object so
// it stays within defined behaviour (unlike the classic null-pointer offsetof).
template <typename C, typename M>
int memberOffset(M C::* member)
{
    auto object = C {};
    return (int) (reinterpret_cast<const std::byte*>(&(object.*member))
                  - reinterpret_cast<const std::byte*>(&object));
}

// A per-frame constant, declared as a member. It is both the graph value used in
// define() and a typed CPU slot: `shader.angle = 0.5f` writes the value the upload
// walk packs into the uniform block. It also accepts any registered sub-type of
// the matching shape, e.g. `shader.tint = Color {1, 0, 0}` for a Uniform<Float3>.
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
        value = std::bit_cast<Cpu>(subValue);
        return *this;
    }

    Cpu value {};
};

// A texture member: the Texture2D handle used by define()'s sample() calls and
// the slot the bound GPU::Texture is set on. Assign the texture to draw with
// (`shader.image = checkerboard`); the program binds it (with its baked
// sampler) when drawn. The program stores a pointer, so the texture must
// outlive the draw.
//
// Which is why a temporary is refused outright, here and on every resource
// member below. `shader.image = device.makeTexture(...)` would store a pointer
// into a texture destroyed at the semicolon, and there is nothing later that
// could report it: the draw reads freed memory or a live resource that happens
// to have taken the same address. Deleting the rvalue overload moves that from
// a wrong picture to a compile error, and the fix is to name the resource.
template <>
struct Uniform<Texture2D> : Texture2D
{
    Uniform& operator=(const Texture& newTexture)
    {
        value = &newTexture;
        return *this;
    }

    Uniform& operator=(Texture&&) = delete;

    const Texture* value = nullptr;

    // How this texture is sampled. Set it before compile() runs - the build walk
    // reads it to place the sampler - so a shader assigns it in its constructor
    // ahead of the compile() call. See TextureSampling for why the shader owns
    // this rather than the Texture.
    TextureSampling sampling {};
};

// The cube form of the same member, and deliberately the same shape: the handle
// define()'s sample() calls read, the GPU::Texture bound on its slot, and the
// sampling baked into the shader. What differs is one line of the build walk -
// the handle comes from cubeTexture() rather than texture() - and what sample()
// will take, which is a Float3 direction.
//
// The GPU::Texture assigned here has to have been created with
// TextureDescriptor::cube. Nothing in the type system says so, and neither
// backend reports the mismatch: Texture::isCube is what a caller checks with.
template <>
struct Uniform<TextureCube> : TextureCube
{
    Uniform& operator=(const Texture& newTexture)
    {
        value = &newTexture;
        return *this;
    }

    Uniform& operator=(Texture&&) = delete;

    const Texture* value = nullptr;

    TextureSampling sampling {};
};

// The depth form, and the one place the pattern bends: what is assigned here is
// the *render target whose depth buffer this samples*, not a texture of its own.
// A depth attachment is created with its colour texture and lives exactly as
// long, so there is nothing else to name it by - and the bind is
// RenderPass::setFragmentDepthTexture, which takes the target for the same
// reason.
//
// It has to have been created with TextureDescriptor::sampleableDepth;
// Texture::hasSampleableDepth is what says whether it was, and a bind through
// one that was not draws nothing rather than reading something undefined.
template <>
struct Uniform<TextureDepth2D> : TextureDepth2D
{
    Uniform& operator=(const Texture& newRenderTarget)
    {
        value = &newRenderTarget;
        return *this;
    }

    Uniform& operator=(Texture&&) = delete;

    const Texture* value = nullptr;

    TextureSampling sampling {};
};

// Storage-buffer members of a compute program, following the texture pattern:
// the slot-indexed handle define() reads or writes, and the slot the assigned
// GPU::Buffer is bound at when dispatched. A Buffer binds whole, a BufferRange
// from its offset (see ComputePass::setInputBuffer). The member holds a
// BufferRange, so the buffer must outlive the dispatch.
template <>
struct Uniform<InputBuffer> : InputBuffer
{
    Uniform& operator=(const Buffer& newBuffer)
    {
        value = BufferRange::of(newBuffer);
        return *this;
    }

    Uniform& operator=(const BufferRange& newRange)
    {
        value = newRange;
        return *this;
    }

    Uniform& operator=(Buffer&&) = delete;

    BufferRange value {};
};

template <>
struct Uniform<OutputBuffer> : OutputBuffer
{
    Uniform& operator=(const Buffer& newBuffer)
    {
        value = BufferRange::of(newBuffer);
        return *this;
    }

    Uniform& operator=(const BufferRange& newRange)
    {
        value = newRange;
        return *this;
    }

    Uniform& operator=(Buffer&&) = delete;

    BufferRange value {};
};

// The integer siblings, bound through the same two calls. What the buffer
// holds is uint32s rather than floats, so that is what the bytes given to it
// want to be.
template <>
struct Uniform<UIntInputBuffer> : UIntInputBuffer
{
    Uniform& operator=(const Buffer& newBuffer)
    {
        value = BufferRange::of(newBuffer);
        return *this;
    }

    Uniform& operator=(const BufferRange& newRange)
    {
        value = newRange;
        return *this;
    }

    Uniform& operator=(Buffer&&) = delete;

    BufferRange value {};
};

template <>
struct Uniform<UIntOutputBuffer> : UIntOutputBuffer
{
    Uniform& operator=(const Buffer& newBuffer)
    {
        value = BufferRange::of(newBuffer);
        return *this;
    }

    Uniform& operator=(const BufferRange& newRange)
    {
        value = newRange;
        return *this;
    }

    Uniform& operator=(Buffer&&) = delete;

    BufferRange value {};
};

// The atomic sibling, bound the same way an output is. The buffer's contents
// are unsigned integers rather than floats, so the bytes handed to it want to
// start as such - a buffer of zeroed uint32s, not of zeroed floats, though the
// two happen to agree on zero.
template <>
struct Uniform<AtomicBuffer> : AtomicBuffer
{
    Uniform& operator=(const Buffer& newBuffer)
    {
        value = BufferRange::of(newBuffer);
        return *this;
    }

    Uniform& operator=(const BufferRange& newRange)
    {
        value = newRange;
        return *this;
    }

    Uniform& operator=(Buffer&&) = delete;

    BufferRange value {};
};

// A kernel's output image, following the Uniform<Texture2D> pattern: the
// slot-indexed handle define() writes through, and the slot the assigned
// GPU::Texture is bound at when dispatched. It carries no sampling - nothing
// samples a written texture - and the texture must have been created with
// TextureDescriptor::computeWrite.
template <>
struct Uniform<WritableTexture2D> : WritableTexture2D
{
    Uniform& operator=(const Texture& newTexture)
    {
        value = &newTexture;
        return *this;
    }

    Uniform& operator=(Texture&&) = delete;

    const Texture* value = nullptr;
};

// The non-templated surface the uniform member walk bottoms out in. The templated
// operator() adapts any Uniform<T> onto it, the same way Miro's Property adapts
// arbitrary fields onto its Reflector - but shaped for the GPU job.
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

    void operator()(const char* name, Uniform<TextureCube>& member)
    {
        onCubeTexture(name, member, member.value, member.sampling);
    }

    void operator()(const char* name, Uniform<TextureDepth2D>& member)
    {
        onDepthTexture(name, member, member.value, member.sampling);
    }

    void operator()(const char* name, Uniform<InputBuffer>& member)
    {
        onInputBuffer(name, member, member.value);
    }

    void operator()(const char* name, Uniform<OutputBuffer>& member)
    {
        onOutputBuffer(name, member, member.value);
    }

    void operator()(const char* name, Uniform<UIntInputBuffer>& member)
    {
        onUIntInputBuffer(name, member, member.value);
    }

    void operator()(const char* name, Uniform<UIntOutputBuffer>& member)
    {
        onUIntOutputBuffer(name, member, member.value);
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
    virtual void
        onCubeTexture(const char*, TextureCube&, const Texture*, TextureSampling)
    {
    }
    virtual void
        onDepthTexture(const char*, TextureDepth2D&, const Texture*, TextureSampling)
    {
    }
    virtual void onInputBuffer(const char*, InputBuffer&, const BufferRange&) {}
    virtual void onOutputBuffer(const char*, OutputBuffer&, const BufferRange&) {}
    virtual void onUIntInputBuffer(const char*, UIntInputBuffer&, const BufferRange&)
    {
    }
    virtual void
        onUIntOutputBuffer(const char*, UIntOutputBuffer&, const BufferRange&)
    {
    }
    virtual void onAtomicBuffer(const char*, AtomicBuffer&, const BufferRange&) {}
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

    void onCubeTexture(const char*,
                       TextureCube& handle,
                       const Texture*,
                       TextureSampling sampling) override
    {
        handle = builder.cubeTexture(sampling);
    }

    void onDepthTexture(const char*,
                        TextureDepth2D& handle,
                        const Texture*,
                        TextureSampling sampling) override
    {
        handle = builder.depthTexture(sampling);
    }

    void onInputBuffer(const char*, InputBuffer& handle, const BufferRange&) override
    {
        handle = builder.inputBuffer();
    }

    void onOutputBuffer(const char*,
                        OutputBuffer& handle,
                        const BufferRange&) override
    {
        handle = builder.outputBuffer();
    }

    void onUIntInputBuffer(const char*,
                           UIntInputBuffer& handle,
                           const BufferRange&) override
    {
        handle = builder.uintInputBuffer();
    }

    void onUIntOutputBuffer(const char*,
                            UIntOutputBuffer& handle,
                            const BufferRange&) override
    {
        handle = builder.uintOutputBuffer();
    }

    void onAtomicBuffer(const char*,
                        AtomicBuffer& handle,
                        const BufferRange&) override
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

// Upload walk: copy each uniform's current value into the block at its aligned
// offset. The caller runs finish() once the walk (and any appended tail, like
// the compute count) is done: MSL pads sizeof(Uniforms) up to the widest
// member's alignment, and Metal's validation layer checks the bound length
// against that padded size - a block that stops at the last member's end binds
// short and aborts the first draw whenever the members end off that boundary.
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
} // namespace eacp::GPU

// Member-list reflection in the Miro idiom: list the declared uniform members once
// and a reflect body is generated that hands each to the visitor by name. Mirrors
// Miro's MIRO_FOR_EACH macro engine, kept GPU-local so the module needs no
// serialization dependency.
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
// Color is a Float3). Non-intrusive sibling of a `using ShaderValue = ...` member;
// place at namespace scope after the type is defined.
#define EACP_SHADER_VALUE(Type, Handle)                                             \
    template <>                                                                     \
    struct eacp::GPU::ShaderValueOf<Type>                                           \
    {                                                                               \
        using type = eacp::GPU::Handle;                                             \
    };
