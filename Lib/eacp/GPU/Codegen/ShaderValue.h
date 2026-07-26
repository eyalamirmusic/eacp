#pragma once

#include "ShaderGraph.h"
#include "ShaderTypes.h"

#include <cassert>
#include <initializer_list>

// The string-free EDSL surface. Float/Float2/Float3/Float4 are lightweight value
// handles into a ShaderGraph: operators and the free float2/float3/float4()
// constructors record IR nodes instead of touching strings. Nothing here knows
// about a backend - the graph it builds is emitted later by ShaderEmitter.

namespace eacp::GPU
{
struct Float;
struct Float2;
struct Float3;
struct Float4;

namespace detail
{
struct ValueHandle
{
    template <typename Result>
    Result swizzle(ValueType type, const char* components) const
    {
        auto result = Result {};
        result.graph = graph;
        result.node = graph->addSwizzle(type, node, components);
        return result;
    }

    ShaderGraph* graph = nullptr;
    int node = -1;
};

constexpr int componentIndex(char component)
{
    switch (component)
    {
        case 'x':
            return 0;
        case 'y':
            return 1;
        case 'z':
            return 2;
        default:
            return 3;
    }
}

// Whether a vector of the given width can name these components at all: .zw
// belongs to a Float4 and means nothing on a Float2. Constraining each accessor
// on this is what keeps the wrong ones off a narrow type rather than letting
// them through to the shader compiler.
constexpr bool spellableAt(int width, const char* components)
{
    for (const auto* at = components; *at != '\0'; ++at)
        if (componentIndex(*at) >= width)
            return false;

    return true;
}

// The cross product of the component set with itself, once per swizzle width.
// The action macro passed in is what makes one accessor out of a set of
// components - declaring it inside Swizzles below, or defining it once the
// vector types it returns are complete.
#define EACP_SWIZZLE_PAIR_ROW(PAIR, a) PAIR(a, x) PAIR(a, y) PAIR(a, z) PAIR(a, w)

#define EACP_SWIZZLE_TRIPLE_COLUMN(TRIPLE, a, b)                                    \
    TRIPLE(a, b, x) TRIPLE(a, b, y) TRIPLE(a, b, z) TRIPLE(a, b, w)

#define EACP_SWIZZLE_TRIPLE_ROW(TRIPLE, a)                                          \
    EACP_SWIZZLE_TRIPLE_COLUMN(TRIPLE, a, x)                                        \
    EACP_SWIZZLE_TRIPLE_COLUMN(TRIPLE, a, y)                                        \
    EACP_SWIZZLE_TRIPLE_COLUMN(TRIPLE, a, z)                                        \
    EACP_SWIZZLE_TRIPLE_COLUMN(TRIPLE, a, w)

#define EACP_SWIZZLE_QUAD_ELEMENT(QUAD, a, b, c)                                    \
    QUAD(a, b, c, x) QUAD(a, b, c, y) QUAD(a, b, c, z) QUAD(a, b, c, w)

#define EACP_SWIZZLE_QUAD_COLUMN(QUAD, a, b)                                        \
    EACP_SWIZZLE_QUAD_ELEMENT(QUAD, a, b, x)                                        \
    EACP_SWIZZLE_QUAD_ELEMENT(QUAD, a, b, y)                                        \
    EACP_SWIZZLE_QUAD_ELEMENT(QUAD, a, b, z)                                        \
    EACP_SWIZZLE_QUAD_ELEMENT(QUAD, a, b, w)

#define EACP_SWIZZLE_QUAD_ROW(QUAD, a)                                              \
    EACP_SWIZZLE_QUAD_COLUMN(QUAD, a, x)                                            \
    EACP_SWIZZLE_QUAD_COLUMN(QUAD, a, y)                                            \
    EACP_SWIZZLE_QUAD_COLUMN(QUAD, a, z)                                            \
    EACP_SWIZZLE_QUAD_COLUMN(QUAD, a, w)

// clang-format off
#define EACP_SWIZZLES(ONE, PAIR, TRIPLE, QUAD)                                      \
    ONE(x) ONE(y) ONE(z) ONE(w)                                                     \
    EACP_SWIZZLE_PAIR_ROW(PAIR, x)                                                  \
    EACP_SWIZZLE_PAIR_ROW(PAIR, y)                                                  \
    EACP_SWIZZLE_PAIR_ROW(PAIR, z)                                                  \
    EACP_SWIZZLE_PAIR_ROW(PAIR, w)                                                  \
    EACP_SWIZZLE_TRIPLE_ROW(TRIPLE, x)                                              \
    EACP_SWIZZLE_TRIPLE_ROW(TRIPLE, y)                                              \
    EACP_SWIZZLE_TRIPLE_ROW(TRIPLE, z)                                              \
    EACP_SWIZZLE_TRIPLE_ROW(TRIPLE, w)                                              \
    EACP_SWIZZLE_QUAD_ROW(QUAD, x)                                                  \
    EACP_SWIZZLE_QUAD_ROW(QUAD, y)                                                  \
    EACP_SWIZZLE_QUAD_ROW(QUAD, z)                                                  \
    EACP_SWIZZLE_QUAD_ROW(QUAD, w)
// clang-format on

#define EACP_DECLARE_SWIZZLE_1(a)                                                   \
    Float a() const                                                                 \
        requires(spellableAt(Width, #a));

#define EACP_DECLARE_SWIZZLE_2(a, b)                                                \
    Float2 a##b() const                                                             \
        requires(spellableAt(Width, #a #b));

#define EACP_DECLARE_SWIZZLE_3(a, b, c)                                             \
    Float3 a##b##c() const                                                          \
        requires(spellableAt(Width, #a #b #c));

#define EACP_DECLARE_SWIZZLE_4(a, b, c, d)                                          \
    Float4 a##b##c##d() const                                                       \
        requires(spellableAt(Width, #a #b #c #d));

// Every ordering of one to four components, constrained to the widths that can
// spell it - 340 accessors on a Float4, which is what it takes for a swizzle to
// stay one node however it is written. Rebuilding .bgra as a constructor over
// four extracted components would instead record the source subtree four times.
//
// Declared here and defined below, once the vector types are complete: a Float3
// that returns a Float2 and a Float2 that returns a Float3 cannot both be
// defined first, and a definition - unlike a declaration - needs its return
// type complete the moment the class is instantiated.
template <int Width>
struct Swizzles : ValueHandle
{
    EACP_SWIZZLES(EACP_DECLARE_SWIZZLE_1,
                  EACP_DECLARE_SWIZZLE_2,
                  EACP_DECLARE_SWIZZLE_3,
                  EACP_DECLARE_SWIZZLE_4)
};

#undef EACP_DECLARE_SWIZZLE_1
#undef EACP_DECLARE_SWIZZLE_2
#undef EACP_DECLARE_SWIZZLE_3
#undef EACP_DECLARE_SWIZZLE_4
} // namespace detail

struct Float : detail::ValueHandle
{
};

// The compute thread id and any index computed from it (+ - * / %, min/max,
// uint uniforms and integer literals). Deliberately outside the float operator
// vocabulary; it indexes storage buffers and crosses into float arithmetic via
// toFloat().
struct UInt : detail::ValueHandle
{
};

// The signed integer: what subscripts an array, and what the operators no float
// has are defined on - the remainder, the bitwise set and the two shifts. Like
// UInt it stays outside the float operator vocabulary and crosses into it
// explicitly, with toInt() and toFloat().
//
// Signed rather than unsigned because that is what an index computed from a
// coordinate needs: int(uv.x * 4.0) is negative left of the origin, and a
// negative index has to survive as one long enough to be masked or clamped
// rather than wrapping to a huge number on the way in.
struct Int : detail::ValueHandle
{
};

// What a comparison yields: the condition an if, a while or a select tests.
// Like UInt it stays outside the float operator vocabulary - it is produced by
// the comparison operators, combined with && || !, and consumed by control
// flow, and there is no arithmetic on it.
struct Bool : detail::ValueHandle
{
};

struct Float2 : detail::Swizzles<2>
{
};

struct Float3 : detail::Swizzles<3>
{
};

struct Float4 : detail::Swizzles<4>
{
};

namespace detail
{
#define EACP_DEFINE_SWIZZLE_1(a)                                                    \
    template <int Width>                                                            \
    Float Swizzles<Width>::a() const                                                \
        requires(spellableAt(Width, #a))                                            \
    {                                                                               \
        return swizzle<Float>(ValueType::Float, #a);                                \
    }

#define EACP_DEFINE_SWIZZLE_2(a, b)                                                 \
    template <int Width>                                                            \
    Float2 Swizzles<Width>::a##b() const                                            \
        requires(spellableAt(Width, #a #b))                                         \
    {                                                                               \
        return swizzle<Float2>(ValueType::Float2, #a #b);                           \
    }

#define EACP_DEFINE_SWIZZLE_3(a, b, c)                                              \
    template <int Width>                                                            \
    Float3 Swizzles<Width>::a##b##c() const                                         \
        requires(spellableAt(Width, #a #b #c))                                      \
    {                                                                               \
        return swizzle<Float3>(ValueType::Float3, #a #b #c);                        \
    }

#define EACP_DEFINE_SWIZZLE_4(a, b, c, d)                                           \
    template <int Width>                                                            \
    Float4 Swizzles<Width>::a##b##c##d() const                                      \
        requires(spellableAt(Width, #a #b #c #d))                                   \
    {                                                                               \
        return swizzle<Float4>(ValueType::Float4, #a #b #c #d);                     \
    }

EACP_SWIZZLES(EACP_DEFINE_SWIZZLE_1,
              EACP_DEFINE_SWIZZLE_2,
              EACP_DEFINE_SWIZZLE_3,
              EACP_DEFINE_SWIZZLE_4)

#undef EACP_DEFINE_SWIZZLE_1
#undef EACP_DEFINE_SWIZZLE_2
#undef EACP_DEFINE_SWIZZLE_3
#undef EACP_DEFINE_SWIZZLE_4
#undef EACP_SWIZZLES
#undef EACP_SWIZZLE_PAIR_ROW
#undef EACP_SWIZZLE_TRIPLE_COLUMN
#undef EACP_SWIZZLE_TRIPLE_ROW
#undef EACP_SWIZZLE_QUAD_ELEMENT
#undef EACP_SWIZZLE_QUAD_COLUMN
#undef EACP_SWIZZLE_QUAD_ROW
} // namespace detail

// The square matrix values. No swizzles; their operations are matrix * vector
// and matrix * matrix, which record a Mul node so the emitter can spell it
// per-backend.
//
// Float2x2 and Float3x3 are shader-local values only - the 2D rotation a
// procedural shader builds inline, the tangent basis a lighting term assembles
// - and ShaderBuilder refuses them as uniforms: MSL and HLSL pack them to
// different sizes inside the value itself, which the uniform block's padding
// cannot bridge. See UniformLayout.h. A Float4x4, which both languages agree
// on, is the matrix to send from the CPU.
struct Float2x2 : detail::ValueHandle
{
};

struct Float3x3 : detail::ValueHandle
{
};

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

// Sampling at a mip level the shader chooses rather than the one the hardware
// derives from the neighbouring fragments. Two things need this: a sample taken
// where the derivatives are meaningless - of a coordinate that jumps between
// fragments - and a deliberate blur, which walks up the pyramid on purpose.
//
// A texture with one level ignores the level, since there is nothing else to
// read; GPU::Texture has no mips yet, so that is every texture today.
inline Float4
    sample(const Texture2D& texture, const Float2& coordinates, const Float& level)
{
    auto result = Float4 {};
    result.graph = texture.graph;
    result.node =
        texture.graph->addSample(texture.slot, coordinates.node, level.node);
    return result;
}

// The level is a literal far more often than not - a shader reaching for this
// usually wants the top of the pyramid and nothing else - and a plain float has
// no graph to record itself into. The texture carries one, so this spells what
// the caller would otherwise need a ShaderBuilder in scope to anchor.
inline Float4
    sample(const Texture2D& texture, const Float2& coordinates, float level)
{
    auto result = Float4 {};
    result.graph = texture.graph;
    result.node = texture.graph->addSample(
        texture.slot, coordinates.node, texture.graph->addConstant(level));
    return result;
}

// One texel, addressed in texels rather than in the [0, 1] the sampler works
// in, and read without it: no filtering, no wrap, no interpolation. The
// coordinates arrive as a Float2 because the EDSL has no integer vector; they
// truncate towards zero, exactly as GLSL's ivec2 conversion does, and a
// coordinate outside the texture reads as zero on both backends.
inline Float4 fetch(const Texture2D& texture, const Float2& coordinates)
{
    auto result = Float4 {};
    result.graph = texture.graph;
    result.node = texture.graph->addFetch(texture.slot, coordinates.node);
    return result;
}

// Storage buffers of float elements, declared by a compute kernel. Like
// Texture2D they are slot-identified rather than expression nodes: an input's
// one operation is the indexed read, an output's is the store recorded via
// ShaderBuilder::write. Bind the matching GPU::Buffer at the same slot
// (ComputePass::setInputBuffer / setOutputBuffer).
struct InputBuffer
{
    Float operator[](const UInt& index) const
    {
        auto result = Float {};
        result.graph = graph;
        result.node = graph->addBufferRead(slot, index.node);
        return result;
    }

    ShaderGraph* graph = nullptr;
    int slot = -1;
};

struct OutputBuffer
{
    ShaderGraph* graph = nullptr;
    int slot = -1;
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

template <>
struct ValueTypeOf<Float2x2>
{
    static constexpr ValueType value = ValueType::Float2x2;
};

template <>
struct ValueTypeOf<Float3x3>
{
    static constexpr ValueType value = ValueType::Float3x3;
};

template <>
struct ValueTypeOf<Float4x4>
{
    static constexpr ValueType value = ValueType::Float4x4;
};

template <>
struct ValueTypeOf<UInt>
{
    static constexpr ValueType value = ValueType::UInt;
};

template <>
struct ValueTypeOf<Int>
{
    static constexpr ValueType value = ValueType::Int;
};

template <>
struct ValueTypeOf<Bool>
{
    static constexpr ValueType value = ValueType::Bool;
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

// T stands in for exactly the given base handle: ShaderShape<Float3> accepts a
// Float3 or a Uniform<Float3>.
template <typename T, typename Base>
concept ShaderShape = ShaderValueLike<T> && std::same_as<ShaderBase<T>, Base>;

template <typename T>
concept ShaderScalarLike = ShaderShape<T, Float>;

template <typename T>
concept ShaderVectorLike = ShaderValueLike<T> && !ShaderScalarLike<T>;

// An operand of the same shape as another, written as a type constraint with
// the reference type as its argument: template <typename L, SameShaderShape<L> R>
// reads "R shaped like L" and accepts e.g. a Float3 next to a Uniform<Float3>.
template <typename T, typename Other>
concept SameShaderShape =
    ShaderValueLike<Other> && ShaderShape<T, ShaderBase<Other>>;

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

// The same, for an operator no char holds - which is the two shifts.
template <typename T>
T binaryOp(const char* op, const ValueHandle& lhs, const ValueHandle& rhs)
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

// Its integer siblings, for uint and int index arithmetic.
inline ValueHandle uintConstantOn(const ValueHandle& value, unsigned literal)
{
    return {value.graph, value.graph->addUIntConstant(literal)};
}

inline ValueHandle intConstantOn(const ValueHandle& value, int literal)
{
    return {value.graph, value.graph->addIntConstant(literal)};
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

// The thread id as a float, e.g. a value computed from the element index. The
// constructor-style cast spells identically in MSL and HLSL.
inline Float toFloat(const UInt& value)
{
    return detail::call<Float>(value, ValueType::Float, "float");
}

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

// The reciprocal square root, spelled rsqrt in both backends (GLSL calls it
// inversesqrt).
template <ShaderValueLike T>
ShaderBase<T> rsqrt(const T& value)
{
    return detail::componentCall(value, "rsqrt");
}

template <ShaderValueLike T>
ShaderBase<T> tan(const T& value)
{
    return detail::componentCall(value, "tan");
}

template <ShaderValueLike T>
ShaderBase<T> asin(const T& value)
{
    return detail::componentCall(value, "asin");
}

template <ShaderValueLike T>
ShaderBase<T> acos(const T& value)
{
    return detail::componentCall(value, "acos");
}

template <ShaderValueLike T>
ShaderBase<T> atan(const T& value)
{
    return detail::componentCall(value, "atan");
}

// atan2(y, x): the quadrant-aware arctangent, the form polar coordinates want.
// Both backends spell it atan2; GLSL overloads atan for it, which is why a
// ported shader picks this one by argument count.
template <typename L, SameShaderShape<L> R>
ShaderBase<L> atan2(const L& y, const R& x)
{
    return detail::componentCall2(y, x, "atan2");
}

template <ShaderValueLike T>
ShaderBase<T> exp(const T& value)
{
    return detail::componentCall(value, "exp");
}

template <ShaderValueLike T>
ShaderBase<T> exp2(const T& value)
{
    return detail::componentCall(value, "exp2");
}

template <ShaderValueLike T>
ShaderBase<T> log(const T& value)
{
    return detail::componentCall(value, "log");
}

template <ShaderValueLike T>
ShaderBase<T> log2(const T& value)
{
    return detail::componentCall(value, "log2");
}

template <ShaderValueLike T>
ShaderBase<T> sign(const T& value)
{
    return detail::componentCall(value, "sign");
}

template <ShaderValueLike T>
ShaderBase<T> ceil(const T& value)
{
    return detail::componentCall(value, "ceil");
}

template <ShaderValueLike T>
ShaderBase<T> trunc(const T& value)
{
    return detail::componentCall(value, "trunc");
}

// Rounds to the nearest integer. The two backends disagree on exact halves -
// Metal rounds them away from zero, HLSL to even - so a value landing on .5 is
// the one case this is not bit-identical across them. GLSL leaves the same case
// implementation-defined; floor(x + 0.5) is the way to pin it down.
template <ShaderValueLike T>
ShaderBase<T> round(const T& value)
{
    return detail::componentCall(value, "round");
}

// Screen-space partial derivatives and their sum of magnitudes, the width of
// one pixel in whatever the argument measures - the usual way to antialias a
// procedural edge. Fragment-stage only, like sample(): the rasteriser supplies
// them from the neighbouring pixels in the quad, so they must never feed the
// position expression.
template <ShaderValueLike T>
ShaderBase<T> dfdx(const T& value)
{
    return detail::componentCall(value, "dfdx");
}

template <ShaderValueLike T>
ShaderBase<T> dfdy(const T& value)
{
    return detail::componentCall(value, "dfdy");
}

template <ShaderValueLike T>
ShaderBase<T> fwidth(const T& value)
{
    return detail::componentCall(value, "fwidth");
}

template <typename L, SameShaderShape<L> R>
ShaderBase<L> min(const L& a, const R& b)
{
    return detail::componentCall2(a, b, "min");
}

template <ShaderValueLike T>
ShaderBase<T> min(const T& a, float b)
{
    return detail::componentCall2(a, b, "min");
}

template <typename L, SameShaderShape<L> R>
ShaderBase<L> max(const L& a, const R& b)
{
    return detail::componentCall2(a, b, "max");
}

template <ShaderValueLike T>
ShaderBase<T> max(const T& a, float b)
{
    return detail::componentCall2(a, b, "max");
}

template <typename L, SameShaderShape<L> R>
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
template <typename L, SameShaderShape<L> R>
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

template <ShaderVectorLike L, SameShaderShape<L> R>
Float dot(const L& a, const R& b)
{
    return detail::call2<Float>(a, b, ValueType::Float, "dot");
}

template <ShaderShape<Float3> L, SameShaderShape<L> R>
Float3 cross(const L& a, const R& b)
{
    return detail::call2<Float3>(a, b, ValueType::Float3, "cross");
}

template <ShaderVectorLike L, SameShaderShape<L> R>
Float distance(const L& a, const R& b)
{
    return detail::call2<Float>(a, b, ValueType::Float, "distance");
}

// reflect(incident, normal): the incident direction mirrored in the surface,
// with the normal taken as already unit length.
template <ShaderVectorLike I, SameShaderShape<I> N>
ShaderBase<I> reflect(const I& incident, const N& normal)
{
    return detail::componentCall2(incident, normal, "reflect");
}

// refract(incident, normal, eta): the incident direction bent by the ratio of
// refractive indices, or zero under total internal reflection. Both arguments
// are taken as unit length.
template <ShaderVectorLike I, SameShaderShape<I> N, ShaderScalarLike E>
ShaderBase<I> refract(const I& incident, const N& normal, const E& eta)
{
    return detail::call3<ShaderBase<I>>(
        incident, normal, eta, ValueTypeOf<ShaderBase<I>>::value, "refract");
}

template <ShaderVectorLike I, SameShaderShape<I> N>
ShaderBase<I> refract(const I& incident, const N& normal, float eta)
{
    return detail::call3<ShaderBase<I>>(incident,
                                        normal,
                                        detail::constantOn(incident, eta),
                                        ValueTypeOf<ShaderBase<I>>::value,
                                        "refract");
}

// faceforward(normal, incident, reference): the normal flipped, if needed, to
// face away from the incident direction.
template <ShaderVectorLike N, SameShaderShape<N> I, SameShaderShape<N> R>
ShaderBase<N> faceforward(const N& normal, const I& incident, const R& reference)
{
    return detail::call3<ShaderBase<N>>(normal,
                                        incident,
                                        reference,
                                        ValueTypeOf<ShaderBase<N>>::value,
                                        "faceforward");
}

template <typename T, SameShaderShape<T> Low, SameShaderShape<T> High>
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
template <typename A, SameShaderShape<A> B, SameShaderShape<A> T>
ShaderBase<A> mix(const A& from, const B& to, const T& amount)
{
    return detail::call3<ShaderBase<A>>(
        from, to, amount, ValueTypeOf<ShaderBase<A>>::value, "mix");
}

template <ShaderVectorLike A, SameShaderShape<A> B, ShaderScalarLike S>
ShaderBase<A> mix(const A& from, const B& to, const S& amount)
{
    return detail::call3<ShaderBase<A>>(
        from, to, amount, ValueTypeOf<ShaderBase<A>>::value, "mix");
}

template <typename A, SameShaderShape<A> B>
ShaderBase<A> mix(const A& from, const B& to, float amount)
{
    return detail::call3<ShaderBase<A>>(from,
                                        to,
                                        detail::constantOn(from, amount),
                                        ValueTypeOf<ShaderBase<A>>::value,
                                        "mix");
}

template <typename T, SameShaderShape<T> E0, SameShaderShape<T> E1>
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
template <typename L, SameShaderShape<L> R>
ShaderBase<L> operator+(const L& lhs, const R& rhs)
{
    return detail::binaryOp<ShaderBase<L>>('+', lhs, rhs);
}

template <typename L, SameShaderShape<L> R>
ShaderBase<L> operator-(const L& lhs, const R& rhs)
{
    return detail::binaryOp<ShaderBase<L>>('-', lhs, rhs);
}

template <typename L, SameShaderShape<L> R>
ShaderBase<L> operator*(const L& lhs, const R& rhs)
{
    return detail::binaryOp<ShaderBase<L>>('*', lhs, rhs);
}

template <typename L, SameShaderShape<L> R>
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

// vector op scalar handle broadcasts, e.g. a colour scaled by a lighting term
// or a coordinate offset by a time uniform. All four operators, on both sides,
// the way the shading languages themselves broadcast: a scalar that happens to
// live in a uniform behaves like the literal it stands in for.
template <ShaderVectorLike T, ShaderScalarLike S>
ShaderBase<T> operator+(const T& vector, const S& scalar)
{
    return detail::binaryOp<ShaderBase<T>>('+', vector, scalar);
}

template <ShaderScalarLike S, ShaderVectorLike T>
ShaderBase<T> operator+(const S& scalar, const T& vector)
{
    return detail::binaryOp<ShaderBase<T>>('+', scalar, vector);
}

template <ShaderVectorLike T, ShaderScalarLike S>
ShaderBase<T> operator-(const T& vector, const S& scalar)
{
    return detail::binaryOp<ShaderBase<T>>('-', vector, scalar);
}

template <ShaderScalarLike S, ShaderVectorLike T>
ShaderBase<T> operator-(const S& scalar, const T& vector)
{
    return detail::binaryOp<ShaderBase<T>>('-', scalar, vector);
}

template <ShaderVectorLike T, ShaderScalarLike S>
ShaderBase<T> operator*(const T& vector, const S& scalar)
{
    return detail::binaryOp<ShaderBase<T>>('*', vector, scalar);
}

template <ShaderScalarLike S, ShaderVectorLike T>
ShaderBase<T> operator*(const S& scalar, const T& vector)
{
    return detail::binaryOp<ShaderBase<T>>('*', scalar, vector);
}

template <ShaderVectorLike T, ShaderScalarLike S>
ShaderBase<T> operator/(const T& vector, const S& scalar)
{
    return detail::binaryOp<ShaderBase<T>>('/', vector, scalar);
}

template <ShaderScalarLike S, ShaderVectorLike T>
ShaderBase<T> operator/(const S& scalar, const T& vector)
{
    return detail::binaryOp<ShaderBase<T>>('/', scalar, vector);
}

// mod(x, y): the floored modulus, x - y * floor(x / y), whose result takes the
// sign of the divisor - so mod(-0.25, 1.0) is 0.75 and a tiling pattern is
// continuous across the origin.
//
// Recorded as that expression rather than as a call, which is the whole point:
// the only modulus either backend offers is fmod(), and fmod() truncates
// instead, so it returns -0.25 there and every tile left of the origin comes
// out mirrored. Building it from nodes both languages already agree on is what
// makes the two backends bit-identical here.
template <typename L, SameShaderShape<L> R>
ShaderBase<L> mod(const L& x, const R& y)
{
    return x - y * floor(x / y);
}

template <ShaderVectorLike T, ShaderScalarLike S>
ShaderBase<T> mod(const T& x, const S& y)
{
    return x - y * floor(x / y);
}

template <ShaderValueLike T>
ShaderBase<T> mod(const T& x, float y)
{
    return x - y * floor(x / y);
}

namespace detail
{
inline Bool compare(const char* op, const ValueHandle& lhs, const ValueHandle& rhs)
{
    auto result = Bool {};
    result.graph = lhs.graph;
    result.node = lhs.graph->addCompare(op, lhs.node, rhs.node);
    return result;
}
} // namespace detail

// Comparisons, on scalars and against scalar literals on either side. Two
// values of the same shape or a value and a float, the way every other binary
// operator here takes them.
//
// Scalars only, deliberately: both shading languages give `<` on two vectors a
// componentwise bool vector, and there is nothing in the EDSL to do with one -
// no bool vector type, and no any()/all() to collapse it. A shader comparing
// vectors compares their components.
#define EACP_COMPARISON(name, spelling)                                             \
    template <ShaderScalarLike L, ShaderScalarLike R>                               \
    Bool name(const L& lhs, const R& rhs)                                           \
    {                                                                               \
        return detail::compare(spelling, lhs, rhs);                                 \
    }                                                                               \
                                                                                    \
    template <ShaderScalarLike L>                                                   \
    Bool name(const L& lhs, float rhs)                                              \
    {                                                                               \
        return detail::compare(spelling, lhs, detail::constantOn(lhs, rhs));        \
    }                                                                               \
                                                                                    \
    template <ShaderScalarLike R>                                                   \
    Bool name(float lhs, const R& rhs)                                              \
    {                                                                               \
        return detail::compare(spelling, detail::constantOn(rhs, lhs), rhs);        \
    }

EACP_COMPARISON(operator<, "<")
EACP_COMPARISON(operator<=, "<=")
EACP_COMPARISON(operator>, ">")
EACP_COMPARISON(operator>=, ">=")
EACP_COMPARISON(operator==, "==")
EACP_COMPARISON(operator!=, "!=")

#undef EACP_COMPARISON

// The logical connectives. Overloading && and || gives up C++'s short-circuit -
// both operands are recorded either way - but the emitted operator is the
// language's own, so the shader itself still skips the right-hand side. That
// only matters for what it costs, never for what it computes: a recorded node
// has no side effects to skip.
inline Bool operator&&(const Bool& lhs, const Bool& rhs)
{
    return detail::compare("&&", lhs, rhs);
}

inline Bool operator||(const Bool& lhs, const Bool& rhs)
{
    return detail::compare("||", lhs, rhs);
}

inline Bool operator!(const Bool& value)
{
    return detail::unaryOp<Bool>('!', value);
}

namespace detail
{
template <typename Result>
Result selectOp(const ValueHandle& condition,
                const ValueHandle& whenTrue,
                const ValueHandle& whenFalse)
{
    auto result = Result {};
    result.graph = condition.graph;
    result.node = condition.graph->addSelect(
        ValueTypeOf<Result>::value, condition.node, whenTrue.node, whenFalse.node);
    return result;
}
} // namespace detail

// select(condition, whenTrue, whenFalse): GLSL's ternary, and the branchless
// half of control flow - both branches are values already computed, so this
// picks between them rather than skipping one. Where the two sides are
// expensive, or where one of them must not run at all, an if is what to reach
// for instead.
template <typename T, SameShaderShape<T> U>
ShaderBase<T> select(const Bool& condition, const T& whenTrue, const U& whenFalse)
{
    return detail::selectOp<ShaderBase<T>>(condition, whenTrue, whenFalse);
}

template <ShaderScalarLike T>
Float select(const Bool& condition, const T& whenTrue, float whenFalse)
{
    return detail::selectOp<Float>(
        condition, whenTrue, detail::constantOn(condition, whenFalse));
}

template <ShaderScalarLike T>
Float select(const Bool& condition, float whenTrue, const T& whenFalse)
{
    return detail::selectOp<Float>(
        condition, detail::constantOn(condition, whenTrue), whenFalse);
}

inline Float select(const Bool& condition, float whenTrue, float whenFalse)
{
    return detail::selectOp<Float>(condition,
                                   detail::constantOn(condition, whenTrue),
                                   detail::constantOn(condition, whenFalse));
}

// A mutable shader local: the one handle in the EDSL that names a place rather
// than a value. Reading it records a node at the point of the read, so what it
// evaluates to is whatever the statements before it left there - which is the
// whole point of it, and why it is the only handle whose meaning is not fixed
// the moment it is built.
//
// It is declared where it is created, so one made inside a loop body is a local
// of that body and the C++ handle leaves scope at the same brace the emitted
// one does. Non-copyable, so `auto b = a` cannot quietly alias a's slot: take
// the value with a.get(), or assign it to a Var of its own.
template <typename T>
struct Var
{
    Var(ShaderGraph& graphToUse, ValueType type, int initialValue)
        : graph(&graphToUse)
        , slot(graphToUse.addVariable(type, initialValue))
    {
    }

    Var(const Var&) = delete;
    Var(Var&&) = delete;

    // The value the variable holds where the read appears. get() and the
    // implicit conversion are the same thing; the explicit one is what a
    // generated port spells, so a read is visible in the source it produces.
    T get() const
    {
        auto value = T {};
        value.graph = graph;
        value.node = graph->addVarRead(slot);
        return value;
    }

    operator T() const { return get(); }
    T operator()() const { return get(); }

    Var& operator=(const T& value)
    {
        graph->assign(slot, value.node);
        return *this;
    }

    template <ShaderShape<T> R>
    Var& operator=(const R& value)
    {
        graph->assign(slot, T(value).node);
        return *this;
    }

    Var& operator=(const Var& other) { return *this = other.get(); }

    Var& operator=(float value)
        requires std::same_as<T, Float>
    {
        graph->assign(slot, graph->addConstant(value));
        return *this;
    }

    Var& operator=(bool value)
        requires std::same_as<T, Bool>
    {
        graph->assign(slot, graph->addBoolConstant(value));
        return *this;
    }

    Var& operator=(int value)
        requires std::same_as<T, Int>
    {
        graph->assign(slot, graph->addIntConstant(value));
        return *this;
    }

    // The compound operators, over whatever the free operators above accept:
    // another value of the same shape, a scalar broadcast across a vector, or a
    // literal. A combination they reject fails here rather than silently
    // assigning something of the wrong shape.
    template <typename R>
    Var& operator+=(const R& value)
    {
        return *this = get() + value;
    }

    template <typename R>
    Var& operator-=(const R& value)
    {
        return *this = get() - value;
    }

    template <typename R>
    Var& operator*=(const R& value)
    {
        return *this = get() * value;
    }

    template <typename R>
    Var& operator/=(const R& value)
    {
        return *this = get() / value;
    }

    ShaderGraph* graph = nullptr;
    int slot = -1;
};

// Index arithmetic on uint values: against another uint (a Uniform<UInt>
// binds here too) or an integer literal, which records a uint constant node.
// Deliberately separate from the float operator vocabulary - there are no
// implicit conversions between the two; cross over with toFloat(). Subtraction
// wraps below zero like the languages it emits into, so guard a backwards
// step with max(), or wrap deliberately with %.
inline UInt operator+(const UInt& lhs, const UInt& rhs)
{
    return detail::binaryOp<UInt>('+', lhs, rhs);
}

inline UInt operator+(const UInt& lhs, unsigned rhs)
{
    return detail::binaryOp<UInt>('+', lhs, detail::uintConstantOn(lhs, rhs));
}

inline UInt operator+(unsigned lhs, const UInt& rhs)
{
    return detail::binaryOp<UInt>('+', detail::uintConstantOn(rhs, lhs), rhs);
}

inline UInt operator-(const UInt& lhs, const UInt& rhs)
{
    return detail::binaryOp<UInt>('-', lhs, rhs);
}

inline UInt operator-(const UInt& lhs, unsigned rhs)
{
    return detail::binaryOp<UInt>('-', lhs, detail::uintConstantOn(lhs, rhs));
}

inline UInt operator-(unsigned lhs, const UInt& rhs)
{
    return detail::binaryOp<UInt>('-', detail::uintConstantOn(rhs, lhs), rhs);
}

inline UInt operator*(const UInt& lhs, const UInt& rhs)
{
    return detail::binaryOp<UInt>('*', lhs, rhs);
}

inline UInt operator*(const UInt& lhs, unsigned rhs)
{
    return detail::binaryOp<UInt>('*', lhs, detail::uintConstantOn(lhs, rhs));
}

inline UInt operator*(unsigned lhs, const UInt& rhs)
{
    return detail::binaryOp<UInt>('*', detail::uintConstantOn(rhs, lhs), rhs);
}

inline UInt operator/(const UInt& lhs, const UInt& rhs)
{
    return detail::binaryOp<UInt>('/', lhs, rhs);
}

inline UInt operator/(const UInt& lhs, unsigned rhs)
{
    return detail::binaryOp<UInt>('/', lhs, detail::uintConstantOn(lhs, rhs));
}

inline UInt operator/(unsigned lhs, const UInt& rhs)
{
    return detail::binaryOp<UInt>('/', detail::uintConstantOn(rhs, lhs), rhs);
}

inline UInt operator%(const UInt& lhs, const UInt& rhs)
{
    return detail::binaryOp<UInt>('%', lhs, rhs);
}

inline UInt operator%(const UInt& lhs, unsigned rhs)
{
    return detail::binaryOp<UInt>('%', lhs, detail::uintConstantOn(lhs, rhs));
}

inline UInt operator%(unsigned lhs, const UInt& rhs)
{
    return detail::binaryOp<UInt>('%', detail::uintConstantOn(rhs, lhs), rhs);
}

// Signed integer arithmetic, and the operators only an integer has: the
// remainder, the bitwise set and the two shifts. Against another Int (a
// Uniform<Int> binds here too) or an integer literal, which records an int
// constant node. Deliberately separate from the float operator vocabulary -
// there are no implicit conversions between the two; cross over with toInt()
// and toFloat().
//
// Division and the remainder truncate towards zero on a negative operand, as
// they do in GLSL and in both languages this emits into. That is not what
// floor-based tiling wants: mod() is the floored one, and is spelled for floats.
#define EACP_INT_OPERATOR(name, spelling)                                           \
    inline Int name(const Int& lhs, const Int& rhs)                                 \
    {                                                                               \
        return detail::binaryOp<Int>(spelling, lhs, rhs);                           \
    }                                                                               \
                                                                                    \
    inline Int name(const Int& lhs, int rhs)                                        \
    {                                                                               \
        return detail::binaryOp<Int>(                                               \
            spelling, lhs, detail::intConstantOn(lhs, rhs));                        \
    }                                                                               \
                                                                                    \
    inline Int name(int lhs, const Int& rhs)                                        \
    {                                                                               \
        return detail::binaryOp<Int>(                                               \
            spelling, detail::intConstantOn(rhs, lhs), rhs);                        \
    }

EACP_INT_OPERATOR(operator+, '+')
EACP_INT_OPERATOR(operator-, '-')
EACP_INT_OPERATOR(operator*, '*')
EACP_INT_OPERATOR(operator/, '/')
EACP_INT_OPERATOR(operator%, '%')
EACP_INT_OPERATOR(operator&, '&')
EACP_INT_OPERATOR(operator|, '|')
EACP_INT_OPERATOR(operator^, '^')
EACP_INT_OPERATOR(operator<<, "<<")
EACP_INT_OPERATOR(operator>>, ">>")

#undef EACP_INT_OPERATOR

inline Int operator-(const Int& value)
{
    return detail::unaryOp<Int>('-', value);
}

// The bitwise complement, the one unary operator no float has.
inline Int operator~(const Int& value)
{
    return detail::unaryOp<Int>('~', value);
}

// Integer comparisons, which the float ones cannot cover: those are constrained
// on the float scalar shape, and an Int is deliberately not one.
#define EACP_INT_COMPARISON(name, spelling)                                         \
    inline Bool name(const Int& lhs, const Int& rhs)                                \
    {                                                                               \
        return detail::compare(spelling, lhs, rhs);                                 \
    }                                                                               \
                                                                                    \
    inline Bool name(const Int& lhs, int rhs)                                       \
    {                                                                               \
        return detail::compare(spelling, lhs, detail::intConstantOn(lhs, rhs));     \
    }                                                                               \
                                                                                    \
    inline Bool name(int lhs, const Int& rhs)                                       \
    {                                                                               \
        return detail::compare(spelling, detail::intConstantOn(rhs, lhs), rhs);     \
    }

EACP_INT_COMPARISON(operator<, "<")
EACP_INT_COMPARISON(operator<=, "<=")
EACP_INT_COMPARISON(operator>, ">")
EACP_INT_COMPARISON(operator>=, ">=")
EACP_INT_COMPARISON(operator==, "==")
EACP_INT_COMPARISON(operator!=, "!=")

#undef EACP_INT_COMPARISON

// int min/max/abs: the branchless way to hold an index inside an array, for the
// shader that would rather clamp than mask.
inline Int min(const Int& a, const Int& b)
{
    return detail::call2<Int>(a, b, ValueType::Int, "min");
}

inline Int min(const Int& a, int b)
{
    return detail::call2<Int>(a, detail::intConstantOn(a, b), ValueType::Int, "min");
}

inline Int max(const Int& a, const Int& b)
{
    return detail::call2<Int>(a, b, ValueType::Int, "max");
}

inline Int max(const Int& a, int b)
{
    return detail::call2<Int>(a, detail::intConstantOn(a, b), ValueType::Int, "max");
}

inline Int abs(const Int& value)
{
    return detail::call<Int>(value, ValueType::Int, "abs");
}

// Crossing between the integer and the float vocabularies, explicit in both
// directions. The constructor-style cast spells identically in MSL and HLSL,
// and truncates towards zero on the way to an int exactly as GLSL's int() does.
inline Float toFloat(const Int& value)
{
    return detail::call<Float>(value, ValueType::Float, "float");
}

template <ShaderScalarLike T>
Int toInt(const T& value)
{
    return detail::call<Int>(value, ValueType::Int, "int");
}

// A constant array the shader subscripts: a palette, a set of offsets, any small
// lookup table a shader would otherwise spell out as a chain of selects. Like
// Texture2D it is slot-identified rather than an expression node - it is a
// declaration and not a value, and its one operation is the subscript.
//
// Its elements are evaluated once where the array is declared, at the top of the
// shader body, so one may read a uniform or a varying but not a mutable local:
// no local exists yet at that point.
//
// The size is part of the type, so a literal index is checked here. An Int index
// is the shader's own business, exactly as it is in GLSL - reading past the end
// is undefined in both languages - so mask it (`i & 3`) or clamp it
// (`min(max(i, 0), 3)`) unless it is already in range.
template <typename T, int Size>
struct Array
{
    T operator[](const Int& index) const
    {
        auto result = T {};
        result.graph = graph;
        result.node = graph->addArrayRead(slot, index.node);
        return result;
    }

    T operator[](int index) const
    {
        assert(index >= 0 && index < Size
               && "eacp: constant-array index out of range");

        auto result = T {};
        result.graph = graph;
        result.node = graph->addArrayRead(slot, graph->addIntConstant(index));
        return result;
    }

    ShaderGraph* graph = nullptr;
    int slot = -1;
};

// uint min/max, the branchless way to clamp an index to a valid range.
inline UInt min(const UInt& a, const UInt& b)
{
    return detail::call2<UInt>(a, b, ValueType::UInt, "min");
}

inline UInt min(const UInt& a, unsigned b)
{
    return detail::call2<UInt>(
        a, detail::uintConstantOn(a, b), ValueType::UInt, "min");
}

inline UInt max(const UInt& a, const UInt& b)
{
    return detail::call2<UInt>(a, b, ValueType::UInt, "max");
}

inline UInt max(const UInt& a, unsigned b)
{
    return detail::call2<UInt>(
        a, detail::uintConstantOn(a, b), ValueType::UInt, "max");
}

namespace detail
{
template <typename Result, typename A, typename B>
Result matrixMul(const A& a, const B& b)
{
    auto result = Result {};
    result.graph = a.graph;
    result.node = a.graph->addMul(ValueTypeOf<Result>::value, a.node, b.node);
    return result;
}
} // namespace detail

// Matrix * vector, e.g. a 2D rotation applied to a texture coordinate or an MVP
// transform applied to a clip-space position.
inline Float2 operator*(const Float2x2& matrix, const Float2& vector)
{
    return detail::matrixMul<Float2>(matrix, vector);
}

inline Float3 operator*(const Float3x3& matrix, const Float3& vector)
{
    return detail::matrixMul<Float3>(matrix, vector);
}

inline Float4 operator*(const Float4x4& matrix, const Float4& vector)
{
    return detail::matrixMul<Float4>(matrix, vector);
}

// Matrix * matrix, e.g. composing two rotations, or model/view/projection.
inline Float2x2 operator*(const Float2x2& a, const Float2x2& b)
{
    return detail::matrixMul<Float2x2>(a, b);
}

inline Float3x3 operator*(const Float3x3& a, const Float3x3& b)
{
    return detail::matrixMul<Float3x3>(a, b);
}

inline Float4x4 operator*(const Float4x4& a, const Float4x4& b)
{
    return detail::matrixMul<Float4x4>(a, b);
}

// Builds a matrix from its columns. Column-major, matching Metal's
// float4x4(c0, c1, c2, c3); the HLSL emitter transposes this construction, since
// HLSL fills a matrix from rows rather than columns.
inline Float2x2 float2x2(const Float2& c0, const Float2& c1)
{
    return detail::construct<Float2x2>(
        *c0.graph, ValueType::Float2x2, {c0.node, c1.node});
}

inline Float3x3 float3x3(const Float3& c0, const Float3& c1, const Float3& c2)
{
    return detail::construct<Float3x3>(
        *c0.graph, ValueType::Float3x3, {c0.node, c1.node, c2.node});
}

inline Float4x4
    float4x4(const Float4& c0, const Float4& c1, const Float4& c2, const Float4& c3)
{
    return detail::construct<Float4x4>(
        *c0.graph, ValueType::Float4x4, {c0.node, c1.node, c2.node, c3.node});
}

// A vector-constructor argument: any value handle (or derived member), or a
// numeric literal that becomes a constant node.
template <typename T>
concept ShaderComponent = ShaderValueLike<T> || std::is_arithmetic_v<T>;

namespace detail
{
template <typename T>
constexpr int componentsOf()
{
    if constexpr (std::is_arithmetic_v<T>)
        return 1;
    else
        return componentCount(ValueTypeOf<ShaderBase<T>>::value);
}

// The graph the constructed vector records into, taken from the first handle
// argument (the constraint guarantees one exists).
inline ShaderGraph* graphOf()
{
    return nullptr;
}

template <typename First, typename... Rest>
ShaderGraph* graphOf(const First& first, const Rest&... rest)
{
    if constexpr (std::is_arithmetic_v<First>)
        return graphOf(rest...);
    else
        return first.graph;
}

// The node an argument contributes. Going through the base handle rather than
// reading .node directly is what lets a derived handle - a Uniform<Float3>, or
// a Var<Float3> whose read is a node of its own - fill a component.
template <typename T>
int nodeOf(ShaderGraph& graph, const T& value)
{
    if constexpr (std::is_arithmetic_v<T>)
        return graph.addConstant((float) value);
    else
        return ShaderBase<T>(value).node;
}

template <typename Result, typename... Args>
Result constructFrom(ValueType type, const Args&... args)
{
    auto& graph = *graphOf(args...);

    auto nodes = Vector<int> {};
    (nodes.add(nodeOf(graph, args)), ...);

    auto result = Result {};
    result.graph = &graph;
    result.node = graph.addConstruct(type, std::move(nodes));
    return result;
}
} // namespace detail

// A pack that fills a vector of the given width: handles and numeric literals
// whose components sum to it, with at least one handle to supply the graph an
// all-literal vector lacks (those still go through constant()).
template <int Width, typename... Args>
concept ComponentsFor = (ShaderComponent<Args> && ...)
                        && (detail::componentsOf<Args>() + ... + 0) == Width
                        && (ShaderValueLike<Args> || ...);

// Vector constructors from any mix of value handles and numeric literals whose
// components total the vector's width: float4(position, 0.0f, 1.0f),
// float4(color, alpha), float3(x, uv)...
template <typename... Args>
    requires ComponentsFor<2, Args...>
Float2 float2(const Args&... args)
{
    return detail::constructFrom<Float2>(ValueType::Float2, args...);
}

template <typename... Args>
    requires ComponentsFor<3, Args...>
Float3 float3(const Args&... args)
{
    return detail::constructFrom<Float3>(ValueType::Float3, args...);
}

template <typename... Args>
    requires ComponentsFor<4, Args...>
Float4 float4(const Args&... args)
{
    return detail::constructFrom<Float4>(ValueType::Float4, args...);
}
} // namespace eacp::GPU
