#pragma once

#include "GeneratedShader.h"
#include "ShaderBuilder.h"
#include "ShaderTypes.h"
#include "ShaderValue.h"

#include <eacp/Core/Utils/Containers.h>

#include <array>
#include <cstddef>
#include <cstring>

// A shader authored as a struct of named, typed members. Each vertex input and
// uniform is a real C++ member you set by name; the same member doubles as the
// graph value used while writing the shader body. A single member list drives two
// walks - one that builds the IR + layouts, one that packs the uniform block - so
// the CPU value and its GPU slot cannot drift, and nothing is positional.
//
//   struct MyShader final : ShaderProgram
//   {
//       VertexInput<Float2> position;
//       VertexInput<Float3> color;
//       Uniform<Float>      angle;
//
//       EACP_SHADER(position, color, angle)
//
//       MyShader() { compile(); }
//
//       void define() override
//       {
//           auto vColor = varying(color);
//           setPosition(float4(position, 0.0f, 1.0f));
//           setFragment(float4(vColor, 1.0f));
//       }
//   };
//
//   MyShader shader;          // builds the graph, derives the layouts
//   shader.angle = 0.5f;      // typed write into the member's storage
//   pass.setVertexUniforms(shader);

namespace eacp::GPU
{
// The CPU-side storage type mirroring each shader value type. A scalar is a
// float; a vector is a packed std::array, so a uniform reads like the data it is.
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
    using type = std::array<float, 2>;
};

template <>
struct CpuValueOf<Float3>
{
    using type = std::array<float, 3>;
};

template <>
struct CpuValueOf<Float4>
{
    using type = std::array<float, 4>;
};

// Byte offset of a data member within its struct, computed from a real object so
// it stays within defined behaviour (unlike the classic null-pointer offsetof).
template <typename C, typename M>
int memberOffset(M C::* member)
{
    auto object = C {};
    return (int) (reinterpret_cast<const std::byte*>(&(object.*member))
                  - reinterpret_cast<const std::byte*>(&object));
}

// A per-vertex attribute, declared as a member. During define() it behaves as its
// underlying value handle (position.x(), etc.); its data arrives from the bound
// vertex buffer, so it carries no CPU storage of its own.
//
// Bind it to the CPU vertex struct's field with a pointer-to-member so the layout
// reads real offsets, not assumed packing, and a wrong field/type is a compile
// error:  VertexInput<Float2> position { &Vertex::position };
template <typename T>
struct VertexInput : T
{
    VertexInput() = default;

    template <typename C, typename M>
    VertexInput(M C::* member)
    {
        static_assert(sizeof(M) == sizeof(typename CpuValueOf<T>::type),
                      "vertex field size does not match the shader input type");
        offset = memberOffset(member);
        stride = (int) sizeof(C);
    }

    int offset = 0;
    int stride = 0; // 0 = unbound; the program then falls back to tight packing
};

// A per-frame constant, declared as a member. It is both the graph value used in
// define() and a typed CPU slot: `shader.angle = 0.5f` writes the value that the
// upload walk packs into the uniform block.
template <typename T>
struct Uniform : T
{
    using Cpu = typename CpuValueOf<T>::type;

    Uniform& operator=(const Cpu& newValue)
    {
        value = newValue;
        return *this;
    }

    Cpu value {};
};

// Uniform-block layout follows the native shader struct rules (MSL/HLSL): a vec2
// aligns to 8, a vec3/vec4 to 16. The walk pads to these so the packed bytes land
// on the same offsets the generated source reads.
inline int uniformAlignment(ValueType type)
{
    switch (type)
    {
        case ValueType::Float:
            return 4;
        case ValueType::Float2:
            return 8;
        case ValueType::Float3:
        case ValueType::Float4:
            return 16;
    }

    return 4;
}

inline int uniformSlotStride(ValueType type)
{
    return type == ValueType::Float3 ? 16 : byteSize(type);
}

inline int alignUp(int value, int alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

inline VertexFormat toVertexFormat(ValueType type)
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
            return VertexFormat::Float4;
    }

    return VertexFormat::Float;
}

// The fixed, non-templated surface a member walk bottoms out in. The templated
// operator() adapts any Uniform<T>/VertexInput<T> onto these two calls, the same
// way Miro's Property adapts arbitrary fields onto its Reflector - but shaped for
// the GPU job (assign a slot / pack bytes) instead of serialization.
class ShaderVisitor
{
public:
    virtual ~ShaderVisitor() = default;

    template <typename T>
    void operator()(const char* name, VertexInput<T>& member)
    {
        onVertexInput(
            name, ValueTypeOf<T>::value, member, member.offset, member.stride);
    }

    template <typename T>
    void operator()(const char* name, Uniform<T>& member)
    {
        onUniform(name, ValueTypeOf<T>::value, member, &member.value);
    }

protected:
    virtual void onVertexInput(const char* name,
                               ValueType type,
                               detail::ValueHandle& handle,
                               int offset,
                               int stride) = 0;
    virtual void onUniform(const char* name,
                           ValueType type,
                           detail::ValueHandle& handle,
                           const void* data) = 0;
};

// Build walk: each member adopts a freshly added graph slot, so define() can then
// use the members as live values.
class ShaderBuildVisitor final : public ShaderVisitor
{
public:
    explicit ShaderBuildVisitor(ShaderBuilder& builderToUse)
        : builder(builderToUse)
    {
    }

    void onVertexInput(const char*,
                       ValueType type,
                       detail::ValueHandle& handle,
                       int offset,
                       int stride) override
    {
        handle = builder.addVertexInput(type);

        layout.attribute(toVertexFormat(type), offset);

        if (stride > 0)
            layout.stride = stride;
        else
            allBound = false;
    }

    void onUniform(const char*,
                   ValueType type,
                   detail::ValueHandle& handle,
                   const void*) override
    {
        handle = builder.addUniform(type);
    }

    // The vertex layout assembled from each input's bound field. Valid only when
    // allBound is true; otherwise the program keeps the tight-packed layout the
    // ShaderBuilder derives.
    VertexLayout layout;
    bool allBound = true;

private:
    ShaderBuilder& builder;
};

// Upload walk: copy each uniform's current value into the block at its aligned
// offset. Vertex inputs are skipped - their data comes from the vertex buffer.
class ShaderUploadVisitor final : public ShaderVisitor
{
public:
    explicit ShaderUploadVisitor(Vector<std::byte>& bytesToFill)
        : bytes(bytesToFill)
    {
    }

protected:
    void onVertexInput(
        const char*, ValueType, detail::ValueHandle&, int, int) override
    {
    }

    void onUniform(const char*,
                   ValueType type,
                   detail::ValueHandle&,
                   const void* data) override
    {
        auto offset = alignUp(cursor, uniformAlignment(type));
        auto next = offset + uniformSlotStride(type);

        if (bytes.size() < next)
            bytes.resize(next);

        std::memcpy(bytes.data() + offset, data, (std::size_t) byteSize(type));
        cursor = next;
    }

private:
    Vector<std::byte>& bytes;
    int cursor = 0;
};

// Base for struct-authored shaders. Derive, declare typed members, list them with
// EACP_SHADER, write define(), and call compile() from the constructor.
class ShaderProgram
{
public:
    ShaderProgram() = default;
    virtual ~ShaderProgram() = default;

    // Member handles point into the owned builder's graph, so a program is pinned
    // in place once built.
    ShaderProgram(const ShaderProgram&) = delete;
    ShaderProgram& operator=(const ShaderProgram&) = delete;

    const ShaderSource& source() const { return generated.source; }
    const VertexLayout& vertexLayout() const { return generated.vertexLayout; }

    // Re-packs the current member values and returns the uniform block, ready for
    // RenderPass::setVertexBytes. Cheap - the block is a handful of floats.
    const void* packedUniforms()
    {
        packUniforms();
        return uniformBytes.data();
    }

    int uniformByteSize() const { return uniformBytes.size(); }
    bool hasUniforms() const { return !uniformBytes.empty(); }

protected:
    // Runs the build walk, the user's define(), then emits source + layouts.
    // Called from the most-derived constructor, where the member list and define()
    // overrides are already in place.
    void compile()
    {
        auto buildVisitor = ShaderBuildVisitor {builder};
        reflectMembers(buildVisitor);
        define();
        generated = builder.build();

        // Prefer the layout assembled from the inputs' bound fields (real offsets);
        // fall back to the builder's tight-packed layout if any input is unbound.
        if (buildVisitor.allBound)
            generated.vertexLayout = buildVisitor.layout;

        packUniforms();
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

    void setPosition(const Float4& clipPosition) { builder.position(clipPosition); }
    void setFragment(const Float4& color) { builder.fragment(color); }

    // Generated by EACP_SHADER: visits each declared member in order.
    virtual void reflectMembers(ShaderVisitor& visitor) = 0;

    // Written by the user: the shader body, in terms of the declared members.
    virtual void define() = 0;

private:
    void packUniforms()
    {
        uniformBytes.clear();
        auto uploadVisitor = ShaderUploadVisitor {uniformBytes};
        reflectMembers(uploadVisitor);
    }

    ShaderBuilder builder;
    GeneratedShader generated;
    Vector<std::byte> uniformBytes;
};
} // namespace eacp::GPU

// Member-list reflection in the Miro idiom: list the declared members once and a
// reflect body is generated that hands each to the visitor by name. Mirrors
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
