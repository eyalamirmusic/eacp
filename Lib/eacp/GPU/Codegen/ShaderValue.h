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

template <typename T>
concept IsShaderVector =
    std::same_as<T, Float2> || std::same_as<T, Float3> || std::same_as<T, Float4>;

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
} // namespace detail

template <IsShaderVector T>
T operator+(const T& lhs, const T& rhs)
{
    return detail::binaryOp<T>('+', lhs, rhs);
}

template <IsShaderVector T>
T operator-(const T& lhs, const T& rhs)
{
    return detail::binaryOp<T>('-', lhs, rhs);
}

template <IsShaderVector T>
T operator*(const T& lhs, const T& rhs)
{
    return detail::binaryOp<T>('*', lhs, rhs);
}

template <IsShaderVector T>
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
