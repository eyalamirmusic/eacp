#pragma once

#include "../Device/Device.h"
#include "GeneratedShader.h"
#include "ShaderBuilder.h"
#include "ShaderTypes.h"
#include "ShaderValue.h"

#include <eacp/Core/Utils/Containers.h>

#include <array>
#include <cassert>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstring>
#include <optional>

// A shader authored as a struct. Uniforms are named, typed members you set by
// name; vertex inputs are pulled straight out of the CPU vertex struct inside
// define(), so that struct is the single source of the vertex layout. The program
// also owns its realized GPU resources (vertex buffer, library, pipeline), so a
// view feeds data and draws without juggling loose handles.
//
//   struct Vertex { float position[2]; float color[3]; };
//
//   struct MyShader final : ShaderProgram
//   {
//       Uniform<Float> angle;
//       EACP_SHADER(angle)
//
//       MyShader() { compile(); }
//
//       void define() override
//       {
//           auto position = vertexInput(&Vertex::position);   // -> Float2
//           auto color    = vertexInput(&Vertex::color);      // -> Float3
//           setPosition(float4(position, 0.0f, 1.0f));
//           setFragment(float4(varying(color), 1.0f));
//       }
//   };
//
//   MyShader shader;
//   shader.setVertices(triangleVertices);   // typed; owns the buffer
//   shader.prepare(view.sampleCount());     // builds library + pipeline
//   ...
//   shader.angle = 0.5f;
//   pass.draw(shader);                       // pipeline + vertices + uniforms

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

template <>
struct CpuValueOf<Float4x4>
{
    using type = std::array<float, 16>;
};

// The shader value a CPU type maps to. Built in for float / float[N] / array; a
// user type opts in either intrusively (a `using ShaderValue = Float3;` member,
// like MIRO_REFLECT) or non-intrusively via EACP_SHADER_VALUE (like
// MIRO_REFLECT_EXTERNAL). This is what lets a `Color` struct stand in for a
// float3 vertex field or uniform.
template <typename T>
struct ShaderValueOf
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

// True when V is a CPU type registered as the shader value type T.
template <typename V, typename T>
concept ShaderValueIs = std::same_as<typename ShaderValueOf<V>::type, T>;

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

    template <typename V>
        requires ShaderValueIs<V, T>
    Uniform& operator=(const V& subValue)
    {
        static_assert(sizeof(V) == sizeof(Cpu),
                      "uniform sub-type size does not match its shader value type");
        std::memcpy(&value, &subValue, sizeof(Cpu));
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
        case ValueType::Float4x4:
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
        case ValueType::Float4x4:
            return VertexFormat::Float4; // matrices are never vertex attributes
    }

    return VertexFormat::Float;
}

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

protected:
    virtual void onUniform(const char* name,
                           ValueType type,
                           detail::ValueHandle& handle,
                           const void* data) = 0;
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

private:
    ShaderBuilder& builder;
};

// Upload walk: copy each uniform's current value into the block at its aligned
// offset.
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

// Base for struct-authored shaders. Derive, declare uniform members, list them
// with EACP_SHADER, write define() (pulling vertex inputs from the CPU vertex
// struct), and call compile() from the constructor.
class ShaderProgram
{
public:
    ShaderProgram() = default;
    virtual ~ShaderProgram() = default;

    // Uniform members and pulled vertex handles point into the owned builder's
    // graph, and the owned GPU resources are non-copyable, so a program is pinned
    // in place.
    ShaderProgram(const ShaderProgram&) = delete;
    ShaderProgram& operator=(const ShaderProgram&) = delete;

    const ShaderSource& source() const { return generated.source; }
    const VertexLayout& vertexLayout() const { return generated.vertexLayout; }

    // Uploads the typed vertex data and owns the resulting buffer. The element
    // type's size must match the layout pulled from it in define().
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

    // Builds the shader library and render pipeline. sampleCount must match the
    // render target (GPUView::sampleCount()); set depth when the view has a depth
    // buffer (GPUView::setDepth(true)).
    void prepare(int sampleCount, bool depth = false)
    {
        shaderLibrary.emplace(Device::shared(), generated.source);

        auto descriptor = RenderPipelineDescriptor {};
        descriptor.library = &*shaderLibrary;
        descriptor.sampleCount = sampleCount;
        descriptor.vertexLayout = generated.vertexLayout;
        descriptor.depth = depth;

        pipelineState.emplace(Device::shared(), descriptor);
    }

    const RenderPipeline& pipeline() const { return *pipelineState; }
    const Buffer& vertices() const { return *vertexBufferData; }
    int vertexCount() const { return vertexCountValue; }

    // Re-packs the current uniform values and returns the block, ready for
    // RenderPass::setVertexBytes. Cheap - the block is a handful of floats.
    const void* packedUniforms()
    {
        packUniforms();
        return uniformBytes.data();
    }

    int uniformByteSize() const { return uniformBytes.size(); }
    bool hasUniforms() const { return !uniformBytes.empty(); }

protected:
    // Runs the uniform build walk, the user's define() (which pulls vertex inputs),
    // then emits source + layouts. Called from the most-derived constructor.
    void compile()
    {
        auto buildVisitor = ShaderBuildVisitor {builder};
        reflectMembers(buildVisitor);
        define();
        generated = builder.build();

        // define() assembled the vertex layout from the pulled fields' real
        // offsets; use it when any input was pulled.
        if (vertexLayoutData.attributes.size() > 0)
            generated.vertexLayout = vertexLayoutData;

        packUniforms();
    }

    // Pulls a vertex attribute out of the CPU vertex struct. The field's type maps
    // to a shader value via ShaderValueOf, the attribute takes the field's real
    // offset, and the returned handle is what you write the shader with.
    template <typename C, typename M>
    typename ShaderValueOf<M>::type vertexInput(M C::* member)
    {
        using Handle = typename ShaderValueOf<M>::type;
        static_assert(sizeof(M) == sizeof(typename CpuValueOf<Handle>::type),
                      "vertex field size does not match its shader value type");

        constexpr auto type = ValueTypeOf<Handle>::value;
        vertexLayoutData.attribute(toVertexFormat(type), memberOffset(member));
        vertexLayoutData.stride = (int) sizeof(C);

        auto added = builder.addVertexInput(type);

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

    // In-shader transform builders, matching column-major / right-handed [0,1]
    // depth conventions. Build the model/view/projection inside define() from
    // scalar uniforms instead of uploading prebuilt matrices.
    Float4x4 translate(float x, float y, float z)
    {
        auto o = constant(1.0f);
        auto z0 = constant(0.0f);
        return float4x4(float4(o, z0, z0, z0),
                        float4(z0, o, z0, z0),
                        float4(z0, z0, o, z0),
                        float4(constant(x), constant(y), constant(z), o));
    }

    Float4x4 rotateZ(const Float& angle)
    {
        auto c = cos(angle);
        auto s = sin(angle);
        auto z0 = constant(0.0f);
        auto o = constant(1.0f);
        return float4x4(float4(c, s, z0, z0),
                        float4(z0 - s, c, z0, z0),
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
                        float4(z0, z0 - s, c, z0),
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

    // Generated by EACP_SHADER: visits each declared uniform member in order.
    virtual void reflectMembers(ShaderVisitor& visitor) = 0;

    // Written by the user: the shader body, pulling vertex inputs as needed.
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
    VertexLayout vertexLayoutData;
    Vector<std::byte> uniformBytes;

    std::optional<Buffer> vertexBufferData;
    std::optional<ShaderLibrary> shaderLibrary;
    std::optional<RenderPipeline> pipelineState;
    int vertexCountValue = 0;
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
