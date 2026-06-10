#pragma once

#include "ShaderGraph.h"
#include "ShaderTypes.h"

#include <concepts>
#include <initializer_list>

// The string-free EDSL surface. Float/Float2/Float3/Float4 are lightweight value
// handles into a ShaderGraph: operators and the free float2/float3/float4()
// constructors record IR nodes instead of touching strings. Nothing here knows
// about a backend - the graph it builds is emitted later by ShaderEmitter.

namespace eacp::GPU
{
namespace detail
{
struct ValueHandle
{
    ShaderGraph* graph = nullptr;
    int node = -1;

    template <typename Result>
    Result swizzle(ValueType type, const char* components) const
    {
        auto result = Result {};
        result.graph = graph;
        result.node = graph->addSwizzle(type, node, components);
        return result;
    }
};
} // namespace detail

struct Float : detail::ValueHandle
{
};

struct Float2 : detail::ValueHandle
{
    Float x() const { return swizzle<Float>(ValueType::Float, "x"); }
    Float y() const { return swizzle<Float>(ValueType::Float, "y"); }
};

struct Float3 : detail::ValueHandle
{
    Float x() const { return swizzle<Float>(ValueType::Float, "x"); }
    Float y() const { return swizzle<Float>(ValueType::Float, "y"); }
    Float z() const { return swizzle<Float>(ValueType::Float, "z"); }
    Float2 xy() const { return swizzle<Float2>(ValueType::Float2, "xy"); }
};

struct Float4 : detail::ValueHandle
{
    Float x() const { return swizzle<Float>(ValueType::Float, "x"); }
    Float y() const { return swizzle<Float>(ValueType::Float, "y"); }
    Float z() const { return swizzle<Float>(ValueType::Float, "z"); }
    Float w() const { return swizzle<Float>(ValueType::Float, "w"); }
    Float2 xy() const { return swizzle<Float2>(ValueType::Float2, "xy"); }
    Float3 xyz() const { return swizzle<Float3>(ValueType::Float3, "xyz"); }
};

// A 4x4 matrix value. No swizzles; its one operation is matrix * vector, which
// records a Mul node so the emitter can spell it per-backend.
struct Float4x4 : detail::ValueHandle
{
};

// A 2D texture declared by the shader, identified by its slot rather than an
// expression node: it is not a value, its one operation is sample(). Sampling
// is a fragment-stage operation, so it must only feed the fragment expression,
// never the position. Bind the matching GPU::Texture with
// RenderPass::setFragmentTexture at the same slot.
struct Texture2D
{
    ShaderGraph* graph = nullptr;
    int slot = -1;
};

inline Float4 sample(const Texture2D& texture, const Float2& coordinates)
{
    auto result = Float4 {};
    result.graph = texture.graph;
    result.node = texture.graph->addSample(texture.slot, coordinates.node);
    return result;
}

template <typename T>
struct ValueTypeOf;

template <>
struct ValueTypeOf<Float>
{
    static constexpr ValueType value = ValueType::Float;
};

template <>
struct ValueTypeOf<Float2>
{
    static constexpr ValueType value = ValueType::Float2;
};

template <>
struct ValueTypeOf<Float3>
{
    static constexpr ValueType value = ValueType::Float3;
};

template <>
struct ValueTypeOf<Float4>
{
    static constexpr ValueType value = ValueType::Float4;
};

template <>
struct ValueTypeOf<Float4x4>
{
    static constexpr ValueType value = ValueType::Float4x4;
};

template <typename T>
concept IsShaderVector =
    std::same_as<T, Float2> || std::same_as<T, Float3> || std::same_as<T, Float4>;

template <typename T>
concept IsShaderValue = std::same_as<T, Float> || IsShaderVector<T>;

namespace detail
{
template <typename T>
T binaryOp(char op, const ValueHandle& lhs, const ValueHandle& rhs)
{
    auto result = T {};
    result.graph = lhs.graph;
    result.node =
        lhs.graph->addBinary(ValueTypeOf<T>::value, op, lhs.node, rhs.node);
    return result;
}

template <typename T>
T scalarOp(char op, const ValueHandle& lhs, float rhs)
{
    auto result = T {};
    result.graph = lhs.graph;
    result.node = lhs.graph->addBinary(
        ValueTypeOf<T>::value, op, lhs.node, lhs.graph->addConstant(rhs));
    return result;
}

template <typename T>
T construct(ShaderGraph& graph, ValueType type, std::initializer_list<int> nodes)
{
    auto result = T {};
    result.graph = &graph;
    result.node = graph.addConstruct(type, Vector<int>(nodes));
    return result;
}

template <typename T>
T call(const ValueHandle& argument, ValueType type, const char* name)
{
    auto result = T {};
    result.graph = argument.graph;
    result.node = argument.graph->addCall(type, name, argument.node);
    return result;
}

template <typename T>
T call2(const ValueHandle& a, const ValueHandle& b, ValueType type, const char* name)
{
    auto result = T {};
    result.graph = a.graph;
    auto args = Vector<int> {};
    args.add(a.node);
    args.add(b.node);
    result.node = a.graph->addCall(type, name, args);
    return result;
}
} // namespace detail

inline Float sin(const Float& value)
{
    return detail::call<Float>(value, ValueType::Float, "sin");
}

inline Float cos(const Float& value)
{
    return detail::call<Float>(value, ValueType::Float, "cos");
}

inline Float3 normalize(const Float3& value)
{
    return detail::call<Float3>(value, ValueType::Float3, "normalize");
}

inline Float dot(const Float3& a, const Float3& b)
{
    return detail::call2<Float>(a, b, ValueType::Float, "dot");
}

inline Float max(const Float& a, const Float& b)
{
    return detail::call2<Float>(a, b, ValueType::Float, "max");
}

// vector * scalar (broadcast), e.g. a colour scaled by a lighting term.
inline Float3 operator*(const Float3& vector, const Float& scalar)
{
    return detail::binaryOp<Float3>('*', vector, scalar);
}

inline Float3 operator*(const Float& scalar, const Float3& vector)
{
    return detail::binaryOp<Float3>('*', vector, scalar);
}

template <IsShaderValue T>
T operator+(const T& lhs, const T& rhs)
{
    return detail::binaryOp<T>('+', lhs, rhs);
}

template <IsShaderValue T>
T operator-(const T& lhs, const T& rhs)
{
    return detail::binaryOp<T>('-', lhs, rhs);
}

template <IsShaderValue T>
T operator*(const T& lhs, const T& rhs)
{
    return detail::binaryOp<T>('*', lhs, rhs);
}

template <IsShaderValue T>
T operator/(const T& lhs, const T& rhs)
{
    return detail::binaryOp<T>('/', lhs, rhs);
}

template <IsShaderVector T>
T operator*(const T& lhs, float rhs)
{
    return detail::scalarOp<T>('*', lhs, rhs);
}

template <IsShaderVector T>
T operator*(float lhs, const T& rhs)
{
    return detail::scalarOp<T>('*', rhs, lhs);
}

// Matrix * vector, e.g. an MVP transform applied to a clip-space position.
inline Float4 operator*(const Float4x4& matrix, const Float4& vector)
{
    auto result = Float4 {};
    result.graph = matrix.graph;
    result.node = matrix.graph->addMul(ValueType::Float4, matrix.node, vector.node);
    return result;
}

// Matrix * matrix, e.g. composing model/view/projection in the shader.
inline Float4x4 operator*(const Float4x4& a, const Float4x4& b)
{
    auto result = Float4x4 {};
    result.graph = a.graph;
    result.node = a.graph->addMul(ValueType::Float4x4, a.node, b.node);
    return result;
}

// Builds a matrix from its four columns. Column-major, matching Metal's
// float4x4(c0, c1, c2, c3); the HLSL emitter transposes this construction, since
// HLSL fills a matrix from rows rather than columns.
inline Float4x4
    float4x4(const Float4& c0, const Float4& c1, const Float4& c2, const Float4& c3)
{
    auto& graph = *c0.graph;
    return detail::construct<Float4x4>(
        graph, ValueType::Float4x4, {c0.node, c1.node, c2.node, c3.node});
}

inline Float2 float2(const Float& x, const Float& y)
{
    auto& graph = *x.graph;
    return detail::construct<Float2>(graph, ValueType::Float2, {x.node, y.node});
}

inline Float3 float3(const Float& x, const Float& y, const Float& z)
{
    auto& graph = *x.graph;
    return detail::construct<Float3>(
        graph, ValueType::Float3, {x.node, y.node, z.node});
}

inline Float3 float3(const Float2& xy, float z)
{
    auto& graph = *xy.graph;
    return detail::construct<Float3>(
        graph, ValueType::Float3, {xy.node, graph.addConstant(z)});
}

inline Float4 float4(const Float& x, const Float& y, const Float& z, const Float& w)
{
    auto& graph = *x.graph;
    return detail::construct<Float4>(
        graph, ValueType::Float4, {x.node, y.node, z.node, w.node});
}

inline Float4 float4(const Float2& xy, float z, float w)
{
    auto& graph = *xy.graph;
    return detail::construct<Float4>(
        graph,
        ValueType::Float4,
        {xy.node, graph.addConstant(z), graph.addConstant(w)});
}

inline Float4 float4(const Float2& xy, const Float2& zw)
{
    auto& graph = *xy.graph;
    return detail::construct<Float4>(graph, ValueType::Float4, {xy.node, zw.node});
}

inline Float4 float4(const Float3& xyz, float w)
{
    auto& graph = *xyz.graph;
    return detail::construct<Float4>(
        graph, ValueType::Float4, {xyz.node, graph.addConstant(w)});
}
} // namespace eacp::GPU
