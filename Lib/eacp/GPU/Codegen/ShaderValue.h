#pragma once

#include "ShaderGraph.h"
#include "ShaderTypes.h"

#include <concepts>
#include <initializer_list>
#include <utility>

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

namespace detail
{
// The plain handle type a possibly-derived handle maps back to: a
// Uniform<Float3> member is a Float3 to every operator and intrinsic below.
// Declared, never defined - overload resolution does the mapping.
Float baseOf(const Float&);
Float2 baseOf(const Float2&);
Float3 baseOf(const Float3&);
Float4 baseOf(const Float4&);
} // namespace detail

// Any value handle, or a type derived from one (e.g. a Uniform<> member), so
// uniforms and vertex inputs work directly in expressions.
template <typename T>
concept ShaderValueLike = requires(const T& value) { detail::baseOf(value); };

// The handle type T stands in for.
template <typename T>
using ShaderBase = decltype(detail::baseOf(std::declval<const T&>()));

template <typename T>
concept ShaderVectorLike = ShaderValueLike<T> && !std::same_as<ShaderBase<T>, Float>;

template <typename T>
concept ShaderScalarLike = ShaderValueLike<T> && std::same_as<ShaderBase<T>, Float>;

// Operands of the same shape, e.g. a Float3 and a Uniform<Float3>.
template <typename L, typename R>
concept SameShaderShape = ShaderValueLike<L> && ShaderValueLike<R>
                          && std::same_as<ShaderBase<L>, ShaderBase<R>>;

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
T scalarOpLeft(char op, float lhs, const ValueHandle& rhs)
{
    auto result = T {};
    result.graph = rhs.graph;
    result.node = rhs.graph->addBinary(
        ValueTypeOf<T>::value, op, rhs.graph->addConstant(lhs), rhs.node);
    return result;
}

template <typename T>
T unaryOp(char op, const ValueHandle& value)
{
    auto result = T {};
    result.graph = value.graph;
    result.node = value.graph->addUnary(ValueTypeOf<T>::value, op, value.node);
    return result;
}

// A float literal as a handle on the same graph as an existing value, for
// intrinsics taking scalar-literal arguments.
inline ValueHandle constantOn(const ValueHandle& value, float literal)
{
    return {value.graph, value.graph->addConstant(literal)};
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

template <typename T>
T call3(const ValueHandle& a,
        const ValueHandle& b,
        const ValueHandle& c,
        ValueType type,
        const char* name)
{
    auto result = T {};
    result.graph = a.graph;
    auto args = Vector<int> {};
    args.add(a.node);
    args.add(b.node);
    args.add(c.node);
    result.node = a.graph->addCall(type, name, args);
    return result;
}

// Componentwise builtins shaped like their argument: the result is the
// argument's base handle type.
template <typename T>
ShaderBase<T> componentCall(const T& value, const char* name)
{
    return call<ShaderBase<T>>(value, ValueTypeOf<ShaderBase<T>>::value, name);
}

template <typename L, typename R>
ShaderBase<L> componentCall2(const L& a, const R& b, const char* name)
{
    return call2<ShaderBase<L>>(a, b, ValueTypeOf<ShaderBase<L>>::value, name);
}

template <typename T>
ShaderBase<T> componentCall2(const T& a, float b, const char* name)
{
    return call2<ShaderBase<T>>(
        a, constantOn(a, b), ValueTypeOf<ShaderBase<T>>::value, name);
}
} // namespace detail

// Componentwise builtins. Call nodes carry the MSL spelling; the emitter
// translates the few HLSL spells differently (fract -> frac, mix -> lerp).
template <ShaderValueLike T>
ShaderBase<T> sin(const T& value)
{
    return detail::componentCall(value, "sin");
}

template <ShaderValueLike T>
ShaderBase<T> cos(const T& value)
{
    return detail::componentCall(value, "cos");
}

template <ShaderValueLike T>
ShaderBase<T> abs(const T& value)
{
    return detail::componentCall(value, "abs");
}

template <ShaderValueLike T>
ShaderBase<T> floor(const T& value)
{
    return detail::componentCall(value, "floor");
}

template <ShaderValueLike T>
ShaderBase<T> fract(const T& value)
{
    return detail::componentCall(value, "fract");
}

template <ShaderValueLike T>
ShaderBase<T> sqrt(const T& value)
{
    return detail::componentCall(value, "sqrt");
}

template <typename L, typename R>
    requires SameShaderShape<L, R>
ShaderBase<L> min(const L& a, const R& b)
{
    return detail::componentCall2(a, b, "min");
}

template <ShaderValueLike T>
ShaderBase<T> min(const T& a, float b)
{
    return detail::componentCall2(a, b, "min");
}

template <typename L, typename R>
    requires SameShaderShape<L, R>
ShaderBase<L> max(const L& a, const R& b)
{
    return detail::componentCall2(a, b, "max");
}

template <ShaderValueLike T>
ShaderBase<T> max(const T& a, float b)
{
    return detail::componentCall2(a, b, "max");
}

template <typename L, typename R>
    requires SameShaderShape<L, R>
ShaderBase<L> pow(const L& base, const R& exponent)
{
    return detail::componentCall2(base, exponent, "pow");
}

template <ShaderScalarLike T>
Float pow(const T& base, float exponent)
{
    return detail::componentCall2(base, exponent, "pow");
}

// step(edge, x): 0 where x < edge, 1 elsewhere - the branchless building block
// until comparisons and select arrive.
template <typename L, typename R>
    requires SameShaderShape<L, R>
ShaderBase<R> step(const L& edge, const R& value)
{
    return detail::call2<ShaderBase<R>>(
        edge, value, ValueTypeOf<ShaderBase<R>>::value, "step");
}

template <ShaderValueLike T>
ShaderBase<T> step(float edge, const T& value)
{
    return detail::call2<ShaderBase<T>>(detail::constantOn(value, edge),
                                        value,
                                        ValueTypeOf<ShaderBase<T>>::value,
                                        "step");
}

template <ShaderVectorLike T>
Float length(const T& value)
{
    return detail::call<Float>(value, ValueType::Float, "length");
}

template <ShaderVectorLike T>
ShaderBase<T> normalize(const T& value)
{
    return detail::componentCall(value, "normalize");
}

template <typename L, typename R>
    requires SameShaderShape<L, R> && ShaderVectorLike<L>
Float dot(const L& a, const R& b)
{
    return detail::call2<Float>(a, b, ValueType::Float, "dot");
}

template <typename L, typename R>
    requires SameShaderShape<L, R> && std::same_as<ShaderBase<L>, Float3>
Float3 cross(const L& a, const R& b)
{
    return detail::call2<Float3>(a, b, ValueType::Float3, "cross");
}

template <typename T, typename Low, typename High>
    requires SameShaderShape<T, Low> && SameShaderShape<T, High>
ShaderBase<T> clamp(const T& value, const Low& low, const High& high)
{
    return detail::call3<ShaderBase<T>>(
        value, low, high, ValueTypeOf<ShaderBase<T>>::value, "clamp");
}

template <ShaderValueLike T>
ShaderBase<T> clamp(const T& value, float low, float high)
{
    return detail::call3<ShaderBase<T>>(value,
                                        detail::constantOn(value, low),
                                        detail::constantOn(value, high),
                                        ValueTypeOf<ShaderBase<T>>::value,
                                        "clamp");
}

// mix(from, to, amount): linear interpolation (HLSL lerp). The amount is a
// value of the same shape, a scalar broadcast across a vector, or a literal.
template <typename A, typename B, typename T>
    requires SameShaderShape<A, B> && SameShaderShape<A, T>
ShaderBase<A> mix(const A& from, const B& to, const T& amount)
{
    return detail::call3<ShaderBase<A>>(
        from, to, amount, ValueTypeOf<ShaderBase<A>>::value, "mix");
}

template <typename A, typename B, typename S>
    requires SameShaderShape<A, B> && ShaderVectorLike<A> && ShaderScalarLike<S>
ShaderBase<A> mix(const A& from, const B& to, const S& amount)
{
    return detail::call3<ShaderBase<A>>(
        from, to, amount, ValueTypeOf<ShaderBase<A>>::value, "mix");
}

template <typename A, typename B>
    requires SameShaderShape<A, B>
ShaderBase<A> mix(const A& from, const B& to, float amount)
{
    return detail::call3<ShaderBase<A>>(from,
                                        to,
                                        detail::constantOn(from, amount),
                                        ValueTypeOf<ShaderBase<A>>::value,
                                        "mix");
}

template <typename E0, typename E1, typename T>
    requires SameShaderShape<E0, T> && SameShaderShape<E1, T>
ShaderBase<T> smoothstep(const E0& edge0, const E1& edge1, const T& value)
{
    return detail::call3<ShaderBase<T>>(
        edge0, edge1, value, ValueTypeOf<ShaderBase<T>>::value, "smoothstep");
}

template <ShaderValueLike T>
ShaderBase<T> smoothstep(float edge0, float edge1, const T& value)
{
    return detail::call3<ShaderBase<T>>(detail::constantOn(value, edge0),
                                        detail::constantOn(value, edge1),
                                        value,
                                        ValueTypeOf<ShaderBase<T>>::value,
                                        "smoothstep");
}

// Componentwise arithmetic between two values of the same shape.
template <typename L, typename R>
    requires SameShaderShape<L, R>
ShaderBase<L> operator+(const L& lhs, const R& rhs)
{
    return detail::binaryOp<ShaderBase<L>>('+', lhs, rhs);
}

template <typename L, typename R>
    requires SameShaderShape<L, R>
ShaderBase<L> operator-(const L& lhs, const R& rhs)
{
    return detail::binaryOp<ShaderBase<L>>('-', lhs, rhs);
}

template <typename L, typename R>
    requires SameShaderShape<L, R>
ShaderBase<L> operator*(const L& lhs, const R& rhs)
{
    return detail::binaryOp<ShaderBase<L>>('*', lhs, rhs);
}

template <typename L, typename R>
    requires SameShaderShape<L, R>
ShaderBase<L> operator/(const L& lhs, const R& rhs)
{
    return detail::binaryOp<ShaderBase<L>>('/', lhs, rhs);
}

template <ShaderValueLike T>
ShaderBase<T> operator-(const T& value)
{
    return detail::unaryOp<ShaderBase<T>>('-', value);
}

// Scalar float literals on either side, broadcast across vectors.
template <ShaderValueLike T>
ShaderBase<T> operator+(const T& lhs, float rhs)
{
    return detail::scalarOp<ShaderBase<T>>('+', lhs, rhs);
}

template <ShaderValueLike T>
ShaderBase<T> operator+(float lhs, const T& rhs)
{
    return detail::scalarOpLeft<ShaderBase<T>>('+', lhs, rhs);
}

template <ShaderValueLike T>
ShaderBase<T> operator-(const T& lhs, float rhs)
{
    return detail::scalarOp<ShaderBase<T>>('-', lhs, rhs);
}

template <ShaderValueLike T>
ShaderBase<T> operator-(float lhs, const T& rhs)
{
    return detail::scalarOpLeft<ShaderBase<T>>('-', lhs, rhs);
}

template <ShaderValueLike T>
ShaderBase<T> operator*(const T& lhs, float rhs)
{
    return detail::scalarOp<ShaderBase<T>>('*', lhs, rhs);
}

template <ShaderValueLike T>
ShaderBase<T> operator*(float lhs, const T& rhs)
{
    return detail::scalarOpLeft<ShaderBase<T>>('*', lhs, rhs);
}

template <ShaderValueLike T>
ShaderBase<T> operator/(const T& lhs, float rhs)
{
    return detail::scalarOp<ShaderBase<T>>('/', lhs, rhs);
}

template <ShaderValueLike T>
ShaderBase<T> operator/(float lhs, const T& rhs)
{
    return detail::scalarOpLeft<ShaderBase<T>>('/', lhs, rhs);
}

// vector op scalar handle broadcasts, e.g. a colour scaled by a lighting term.
template <ShaderVectorLike T, ShaderScalarLike S>
ShaderBase<T> operator*(const T& vector, const S& scalar)
{
    return detail::binaryOp<ShaderBase<T>>('*', vector, scalar);
}

template <ShaderScalarLike S, ShaderVectorLike T>
ShaderBase<T> operator*(const S& scalar, const T& vector)
{
    return detail::binaryOp<ShaderBase<T>>('*', vector, scalar);
}

template <ShaderVectorLike T, ShaderScalarLike S>
ShaderBase<T> operator/(const T& vector, const S& scalar)
{
    return detail::binaryOp<ShaderBase<T>>('/', vector, scalar);
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
