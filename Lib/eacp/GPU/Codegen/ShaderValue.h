#pragma once

#include "ShaderGraph.h"
#include "ShaderTypes.h"

#include <cassert>
#include <initializer_list>

// The string-free EDSL surface: lightweight value handles into a ShaderGraph
// whose operators and constructors record IR nodes. Nothing here knows about a
// backend - the graph it builds is emitted later by ShaderEmitter.

namespace eacp::GPU
{
struct Float;
struct Float2;
struct Float3;
struct Float4;
struct Int;
struct Int2;
struct Int3;
struct Int4;
struct Bool;
struct Bool2;
struct Bool3;
struct Bool4;

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
// belongs to a Float4 and means nothing on a Float2.
constexpr bool spellableAt(int width, const char* components)
{
    for (const auto* at = components; *at != '\0'; ++at)
        if (componentIndex(*at) >= width)
            return false;

    return true;
}

// The cross product of the component set with itself, once per swizzle width;
// the action macro passed in makes one accessor out of a set of components.
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

// One vector family: the four widths a swizzle can land in and the graph types
// that go with them, so one set of accessors serves all three families.
template <typename One,
          typename Two,
          typename Three,
          typename Four,
          ValueType OneType,
          ValueType TwoType,
          ValueType ThreeType,
          ValueType FourType>
struct Family
{
    using Component = One;
    using Pair = Two;
    using Triple = Three;
    using Quad = Four;

    static constexpr ValueType componentType = OneType;
    static constexpr ValueType pairType = TwoType;
    static constexpr ValueType tripleType = ThreeType;
    static constexpr ValueType quadType = FourType;
};

using Floats = Family<Float,
                      Float2,
                      Float3,
                      Float4,
                      ValueType::Float,
                      ValueType::Float2,
                      ValueType::Float3,
                      ValueType::Float4>;

using Ints = Family<Int,
                    Int2,
                    Int3,
                    Int4,
                    ValueType::Int,
                    ValueType::Int2,
                    ValueType::Int3,
                    ValueType::Int4>;

using Bools = Family<Bool,
                     Bool2,
                     Bool3,
                     Bool4,
                     ValueType::Bool,
                     ValueType::Bool2,
                     ValueType::Bool3,
                     ValueType::Bool4>;

#define EACP_DECLARE_SWIZZLE_1(a)                                                   \
    typename Group::Component a() const                                             \
        requires(spellableAt(Width, #a));

#define EACP_DECLARE_SWIZZLE_2(a, b)                                                \
    typename Group::Pair a##b() const                                               \
        requires(spellableAt(Width, #a #b));

#define EACP_DECLARE_SWIZZLE_3(a, b, c)                                             \
    typename Group::Triple a##b##c() const                                          \
        requires(spellableAt(Width, #a #b #c));

#define EACP_DECLARE_SWIZZLE_4(a, b, c, d)                                          \
    typename Group::Quad a##b##c##d() const                                         \
        requires(spellableAt(Width, #a #b #c #d));

// Every ordering of one to four components, constrained to the widths that can
// spell it, so a swizzle stays one node however it is written. Defined below
// rather than here, once the vector types they return are complete.
template <typename Group, int Width>
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

// The compute thread id and any index computed from it. Deliberately outside
// the float operator vocabulary; cross over explicitly with toFloat().
struct UInt : detail::ValueHandle
{
};

// The signed integer: what subscripts an array, and what the remainder, the
// bitwise set and the two shifts are defined on. Outside the float operator
// vocabulary; cross over explicitly with toInt() and toFloat().
struct Int : detail::ValueHandle
{
};

// What a comparison yields: the condition an if, a while or a select tests.
// Combined with && || ! and consumed by control flow; no arithmetic on it.
struct Bool : detail::ValueHandle
{
};

struct Float2 : detail::Swizzles<detail::Floats, 2>
{
};

struct Float3 : detail::Swizzles<detail::Floats, 3>
{
};

struct Float4 : detail::Swizzles<detail::Floats, 4>
{
};

// The integer vectors carry the whole integer operator set componentwise, and
// cross into float arithmetic explicitly as the scalar does.
struct Int2 : detail::Swizzles<detail::Ints, 2>
{
};

struct Int3 : detail::Swizzles<detail::Ints, 3>
{
};

struct Int4 : detail::Swizzles<detail::Ints, 4>
{
};

// What comparing two vectors yields. Nothing tests one directly - a branch and
// a select take a scalar condition - so collapse it with any() or all().
struct Bool2 : detail::Swizzles<detail::Bools, 2>
{
};

struct Bool3 : detail::Swizzles<detail::Bools, 3>
{
};

struct Bool4 : detail::Swizzles<detail::Bools, 4>
{
};

namespace detail
{
#define EACP_DEFINE_SWIZZLE_1(a)                                                    \
    template <typename Group, int Width>                                            \
    typename Group::Component Swizzles<Group, Width>::a() const                     \
        requires(spellableAt(Width, #a))                                            \
    {                                                                               \
        return swizzle<typename Group::Component>(Group::componentType, #a);        \
    }

#define EACP_DEFINE_SWIZZLE_2(a, b)                                                 \
    template <typename Group, int Width>                                            \
    typename Group::Pair Swizzles<Group, Width>::a##b() const                       \
        requires(spellableAt(Width, #a #b))                                         \
    {                                                                               \
        return swizzle<typename Group::Pair>(Group::pairType, #a #b);               \
    }

#define EACP_DEFINE_SWIZZLE_3(a, b, c)                                              \
    template <typename Group, int Width>                                            \
    typename Group::Triple Swizzles<Group, Width>::a##b##c() const                  \
        requires(spellableAt(Width, #a #b #c))                                      \
    {                                                                               \
        return swizzle<typename Group::Triple>(Group::tripleType, #a #b #c);        \
    }

#define EACP_DEFINE_SWIZZLE_4(a, b, c, d)                                           \
    template <typename Group, int Width>                                            \
    typename Group::Quad Swizzles<Group, Width>::a##b##c##d() const                 \
        requires(spellableAt(Width, #a #b #c #d))                                   \
    {                                                                               \
        return swizzle<typename Group::Quad>(Group::quadType, #a #b #c #d);         \
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

// The square matrix values, with no swizzles. Float2x2 and Float3x3 are
// shader-local only - ShaderBuilder refuses them as uniforms because MSL and
// HLSL pack them to different sizes (see UniformLayout.h).
struct Float2x2 : detail::ValueHandle
{
};

struct Float3x3 : detail::ValueHandle
{
};

struct Float4x4 : detail::ValueHandle
{
};

// Slot-identified rather than an expression node. Sampling is fragment-stage
// only, so it must never feed the position expression. Bind the matching
// GPU::Texture with RenderPass::setFragmentTexture at the same slot.
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

// Sampling at a mip level the shader chooses rather than the hardware-derived
// one. A texture with a single level ignores it.
inline Float4
    sample(const Texture2D& texture, const Float2& coordinates, const Float& level)
{
    auto result = Float4 {};
    result.graph = texture.graph;
    result.node =
        texture.graph->addSample(texture.slot, coordinates.node, level.node);
    return result;
}

// A literal level, anchored on the texture's own graph so the caller needs no
// ShaderBuilder in scope.
inline Float4
    sample(const Texture2D& texture, const Float2& coordinates, float level)
{
    auto result = Float4 {};
    result.graph = texture.graph;
    result.node = texture.graph->addSample(
        texture.slot, coordinates.node, texture.graph->addConstant(level));
    return result;
}

// One texel, addressed in texels rather than in the sampler's [0, 1] and read
// without one: no filtering, no wrap, no interpolation. A coordinate outside
// the texture reads as zero on both backends; the Float2 form truncates.
inline Float4 fetch(const Texture2D& texture, const Int2& coordinates)
{
    auto result = Float4 {};
    result.graph = texture.graph;
    result.node = texture.graph->addFetch(texture.slot, coordinates.node);
    return result;
}

inline Float4 fetch(const Texture2D& texture, const Float2& coordinates)
{
    auto result = Float4 {};
    result.graph = texture.graph;
    result.node = texture.graph->addFetch(texture.slot, coordinates.node);
    return result;
}

// Write-only on both backends; its one operation is ShaderBuilder::write. Bind
// the matching GPU::Texture, created with TextureDescriptor::computeWrite, at
// the same slot (ComputePass::setOutputTexture).
struct WritableTexture2D
{
    ShaderGraph* graph = nullptr;
    int slot = -1;
};

// Where a 2D kernel's work item sits in the grid.
struct ThreadPosition
{
    UInt x;
    UInt y;
};

namespace detail
{
// count consecutive elements starting at index * count, assembled into a
// vector; the buffer stays a run of floats, so this costs count scalar loads.
template <typename T>
T readBufferVector(
    ShaderGraph* graph, int slot, const UInt& index, ValueType type, int count)
{
    auto base = graph->addBinary(
        ValueType::UInt, '*', index.node, graph->addUIntConstant((unsigned) count));

    auto components = Vector<int> {};

    for (auto i = 0; i < count; ++i)
    {
        auto element = i == 0
                           ? base
                           : graph->addBinary(ValueType::UInt,
                                              '+',
                                              base,
                                              graph->addUIntConstant((unsigned) i));

        components.add(graph->addBufferRead(slot, element));
    }

    auto result = T {};
    result.graph = graph;
    result.node = graph->addConstruct(type, std::move(components));
    return result;
}
} // namespace detail

// Storage buffers of float elements, slot-identified rather than expression
// nodes. Bind the matching GPU::Buffer at the same slot
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

    // For a buffer of records rather than single floats: read4(i) is elements
    // 4i..4i+3. The index is in records, not floats, and addresses the same
    // record write(output, i, Float4) does.
    Float2 read2(const UInt& index) const
    {
        return detail::readBufferVector<Float2>(
            graph, slot, index, ValueType::Float2, 2);
    }

    Float3 read3(const UInt& index) const
    {
        return detail::readBufferVector<Float3>(
            graph, slot, index, ValueType::Float3, 3);
    }

    Float4 read4(const UInt& index) const
    {
        return detail::readBufferVector<Float4>(
            graph, slot, index, ValueType::Float4, 4);
    }

    ShaderGraph* graph = nullptr;
    int slot = -1;
};

struct OutputBuffer
{
    ShaderGraph* graph = nullptr;
    int slot = -1;
};

// A storage buffer of unsigned integers every thread in the dispatch may
// read-modify-write at once. Its elements are integers, so the same GPU::Buffer
// bound to an InputBuffer later reads those bits as floats and yields nonsense.
struct AtomicBuffer
{
    // Relaxed, like the add: ordered against this thread's own earlier
    // operations and nothing else.
    UInt load(const UInt& index) const
    {
        auto result = UInt {};
        result.graph = graph;
        result.node = graph->addAtomicLoad(slot, index.node);
        return result;
    }

    // A literal index, anchored on this buffer's own graph.
    UInt load(unsigned index) const { return load(literal(index)); }

    UInt literal(unsigned value) const
    {
        auto result = UInt {};
        result.graph = graph;
        result.node = graph->addUIntConstant(value);
        return result;
    }

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
struct ValueTypeOf<Int2>
{
    static constexpr ValueType value = ValueType::Int2;
};

template <>
struct ValueTypeOf<Int3>
{
    static constexpr ValueType value = ValueType::Int3;
};

template <>
struct ValueTypeOf<Int4>
{
    static constexpr ValueType value = ValueType::Int4;
};

template <>
struct ValueTypeOf<Bool>
{
    static constexpr ValueType value = ValueType::Bool;
};

template <>
struct ValueTypeOf<Bool2>
{
    static constexpr ValueType value = ValueType::Bool2;
};

template <>
struct ValueTypeOf<Bool3>
{
    static constexpr ValueType value = ValueType::Bool3;
};

template <>
struct ValueTypeOf<Bool4>
{
    static constexpr ValueType value = ValueType::Bool4;
};

namespace detail
{
// The plain handle type a possibly-derived handle maps back to, declared but
// never defined. Float handles only, which is what makes ShaderValueLike below
// mean "in the float vocabulary"; other families go through handleOf().
Float baseOf(const Float&);
Float2 baseOf(const Float2&);
Float3 baseOf(const Float3&);
Float4 baseOf(const Float4&);

// The same mapping over every family, for the places that take any of them.
// Split from baseOf so that widening the one does not widen the other.
Float handleOf(const Float&);
Float2 handleOf(const Float2&);
Float3 handleOf(const Float3&);
Float4 handleOf(const Float4&);
UInt handleOf(const UInt&);
Int handleOf(const Int&);
Int2 handleOf(const Int2&);
Int3 handleOf(const Int3&);
Int4 handleOf(const Int4&);
Bool handleOf(const Bool&);
Bool2 handleOf(const Bool2&);
Bool3 handleOf(const Bool3&);
Bool4 handleOf(const Bool4&);
} // namespace detail

// The handle type T stands in for, in whichever family it belongs to.
template <typename T>
using ShaderHandle = decltype(detail::handleOf(std::declval<const T&>()));

// T is a handle (or a Uniform<> of one) in the given family.
template <typename T>
concept ShaderHandleLike = requires(const T& value) { detail::handleOf(value); };

template <typename T>
concept IntValueLike =
    ShaderHandleLike<T> && isSignedInteger(ValueTypeOf<ShaderHandle<T>>::value);

template <typename T>
concept BoolValueLike =
    ShaderHandleLike<T> && isBoolean(ValueTypeOf<ShaderHandle<T>>::value);

// An integer or boolean *vector* specifically - what a componentwise operator
// takes and what any()/all() collapses.
template <typename T>
concept IntVectorLike =
    IntValueLike<T> && ValueTypeOf<ShaderHandle<T>>::value != ValueType::Int;

template <typename T>
concept BoolVectorLike =
    BoolValueLike<T> && ValueTypeOf<ShaderHandle<T>>::value != ValueType::Bool;

template <typename T>
concept IntScalarLike =
    IntValueLike<T> && ValueTypeOf<ShaderHandle<T>>::value == ValueType::Int;

// Two handles of exactly the same type, whichever family they are in.
template <typename T, typename Other>
concept SameShaderHandle =
    ShaderHandleLike<Other> && std::same_as<ShaderHandle<T>, ShaderHandle<Other>>;

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

// An operand of the same shape as another: template <typename L,
// SameShaderShape<L> R> reads "R shaped like L".
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

// A literal has no graph of its own, so it becomes a constant on the graph the
// handles bring: at least one argument must be a handle.
template <typename T>
concept LiteralArgument = std::same_as<T, float>;

template <typename T>
concept IntrinsicArgument = ShaderValueLike<T> || LiteralArgument<T>;

inline void anchorAt(const ValueHandle*&, float) {}

template <ShaderValueLike T>
void anchorAt(const ValueHandle*& anchor, const T& value)
{
    if (anchor == nullptr)
        anchor = &value;
}

inline int argumentNode(const ValueHandle& anchor, float literal)
{
    return anchor.graph->addConstant(literal);
}

template <ShaderValueLike T>
int argumentNode(const ValueHandle&, const T& value)
{
    return value.node;
}

// One call node out of arguments that may be handles or literals in any mix,
// the way GLSL takes them.
template <typename Result, IntrinsicArgument... Args>
Result intrinsic(const char* name, const Args&... arguments)
{
    const ValueHandle* anchor = nullptr;
    (anchorAt(anchor, arguments), ...);

    auto nodes = Vector<int> {};
    (nodes.add(argumentNode(*anchor, arguments)), ...);

    auto result = Result {};
    result.graph = anchor->graph;
    result.node =
        anchor->graph->addCall(ValueTypeOf<Result>::value, name, std::move(nodes));
    return result;
}
} // namespace detail

// An argument written beside a value of a given shape: the same shape, a scalar
// broadcast across it, or a float literal - GLSL's genType.
template <typename T, typename Shape>
concept ShapedBeside =
    detail::LiteralArgument<T> || SameShaderShape<T, Shape> || ShaderScalarLike<T>;

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

// GLSL calls this inversesqrt.
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

// The quadrant-aware arctangent, which GLSL spells as a two-argument atan.
template <ShaderValueLike L, ShapedBeside<L> R>
ShaderBase<L> atan2(const L& y, const R& x)
{
    return detail::intrinsic<ShaderBase<L>>("atan2", y, x);
}

template <ShaderValueLike R>
ShaderBase<R> atan2(float y, const R& x)
{
    return detail::intrinsic<ShaderBase<R>>("atan2", y, x);
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

// The backends disagree on exact halves - Metal rounds them away from zero,
// HLSL to even - so use floor(x + 0.5) where that matters.
template <ShaderValueLike T>
ShaderBase<T> round(const T& value)
{
    return detail::componentCall(value, "round");
}

// Screen-space partial derivatives and their sum of magnitudes: the width of
// one pixel in whatever the argument measures. Fragment-stage only, like
// sample(), so they must never feed the position expression.
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

// Every intrinsic from here down takes a float literal in any argument
// position; the second form covers the shape-deciding argument being one.
template <ShaderValueLike L, ShapedBeside<L> R>
ShaderBase<L> min(const L& a, const R& b)
{
    return detail::intrinsic<ShaderBase<L>>("min", a, b);
}

template <ShaderValueLike R>
ShaderBase<R> min(float a, const R& b)
{
    return detail::intrinsic<ShaderBase<R>>("min", a, b);
}

template <ShaderValueLike L, ShapedBeside<L> R>
ShaderBase<L> max(const L& a, const R& b)
{
    return detail::intrinsic<ShaderBase<L>>("max", a, b);
}

template <ShaderValueLike R>
ShaderBase<R> max(float a, const R& b)
{
    return detail::intrinsic<ShaderBase<R>>("max", a, b);
}

template <ShaderValueLike L, ShapedBeside<L> R>
ShaderBase<L> pow(const L& base, const R& exponent)
{
    return detail::intrinsic<ShaderBase<L>>("pow", base, exponent);
}

template <ShaderValueLike R>
ShaderBase<R> pow(float base, const R& exponent)
{
    return detail::intrinsic<ShaderBase<R>>("pow", base, exponent);
}

// 0 where x < edge, 1 elsewhere. The shape is x's.
template <typename E, ShaderValueLike T>
    requires ShapedBeside<E, T>
ShaderBase<T> step(const E& edge, const T& value)
{
    return detail::intrinsic<ShaderBase<T>>("step", edge, value);
}

template <ShaderValueLike E>
ShaderBase<E> step(const E& edge, float value)
{
    return detail::intrinsic<ShaderBase<E>>("step", edge, value);
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

// The normal is taken as already unit length.
template <ShaderVectorLike I, SameShaderShape<I> N>
ShaderBase<I> reflect(const I& incident, const N& normal)
{
    return detail::componentCall2(incident, normal, "reflect");
}

// eta is the ratio of refractive indices; the result is zero under total
// internal reflection. Incident and normal are taken as unit length.
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

// The normal flipped, if needed, to face away from the incident direction.
template <ShaderVectorLike N, SameShaderShape<N> I, SameShaderShape<N> R>
ShaderBase<N> faceforward(const N& normal, const I& incident, const R& reference)
{
    return detail::call3<ShaderBase<N>>(normal,
                                        incident,
                                        reference,
                                        ValueTypeOf<ShaderBase<N>>::value,
                                        "faceforward");
}

template <ShaderValueLike T, ShapedBeside<T> Low, ShapedBeside<T> High>
ShaderBase<T> clamp(const T& value, const Low& low, const High& high)
{
    return detail::intrinsic<ShaderBase<T>>("clamp", value, low, high);
}

// The result's shape is the first argument that is not itself a literal.
template <ShaderValueLike Low, ShapedBeside<Low> High>
ShaderBase<Low> clamp(float value, const Low& low, const High& high)
{
    return detail::intrinsic<ShaderBase<Low>>("clamp", value, low, high);
}

template <ShaderValueLike High>
ShaderBase<High> clamp(float value, float low, const High& high)
{
    return detail::intrinsic<ShaderBase<High>>("clamp", value, low, high);
}

// Linear interpolation, which HLSL spells lerp.
template <ShaderValueLike A, ShapedBeside<A> B, ShapedBeside<A> T>
ShaderBase<A> mix(const A& from, const B& to, const T& amount)
{
    return detail::intrinsic<ShaderBase<A>>("mix", from, to, amount);
}

template <ShaderValueLike B, ShapedBeside<B> T>
ShaderBase<B> mix(float from, const B& to, const T& amount)
{
    return detail::intrinsic<ShaderBase<B>>("mix", from, to, amount);
}

template <ShaderValueLike T>
ShaderBase<T> mix(float from, float to, const T& amount)
{
    return detail::intrinsic<ShaderBase<T>>("mix", from, to, amount);
}

// The shape is x's; either edge may independently be a literal or a value.
template <typename E0, typename E1, ShaderValueLike T>
    requires ShapedBeside<E0, T> && ShapedBeside<E1, T>
ShaderBase<T> smoothstep(const E0& edge0, const E1& edge1, const T& value)
{
    return detail::intrinsic<ShaderBase<T>>("smoothstep", edge0, edge1, value);
}

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

// Float literals on either side, broadcast across vectors.
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

// vector op scalar handle, broadcast the way the shading languages do.
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

// The floored modulus, whose result takes the sign of the divisor. Spelled out
// rather than called, because both backends only offer fmod(), which truncates.
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

// The componentwise form, yielding a mask of the operands' width.
template <typename Mask>
Mask compareWide(const char* op, const ValueHandle& lhs, const ValueHandle& rhs)
{
    auto result = Mask {};
    result.graph = lhs.graph;
    result.node =
        lhs.graph->addCompare(ValueTypeOf<Mask>::value, op, lhs.node, rhs.node);
    return result;
}
} // namespace detail

// Comparisons, on scalars and against scalar literals on either side.
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

// The componentwise comparisons, GLSL's lessThan() and friends. The result is a
// mask, not a condition: collapse it with any() or all() before branching.
#define EACP_VECTOR_COMPARISON_AT(name, spelling, Vector, Mask)                     \
    template <ShaderShape<Vector> L, SameShaderShape<L> R>                          \
    Mask name(const L& lhs, const R& rhs)                                           \
    {                                                                               \
        return detail::compareWide<Mask>(spelling, lhs, rhs);                       \
    }

#define EACP_INT_VECTOR_COMPARISON_AT(name, spelling, Vector, Mask)                 \
    template <SameShaderHandle<Vector> L, SameShaderHandle<L> R>                    \
    Mask name(const L& lhs, const R& rhs)                                           \
    {                                                                               \
        return detail::compareWide<Mask>(spelling, lhs, rhs);                       \
    }

#define EACP_VECTOR_COMPARISON(name, spelling)                                      \
    EACP_VECTOR_COMPARISON_AT(name, spelling, Float2, Bool2)                        \
    EACP_VECTOR_COMPARISON_AT(name, spelling, Float3, Bool3)                        \
    EACP_VECTOR_COMPARISON_AT(name, spelling, Float4, Bool4)                        \
    EACP_INT_VECTOR_COMPARISON_AT(name, spelling, Int2, Bool2)                      \
    EACP_INT_VECTOR_COMPARISON_AT(name, spelling, Int3, Bool3)                      \
    EACP_INT_VECTOR_COMPARISON_AT(name, spelling, Int4, Bool4)

EACP_VECTOR_COMPARISON(operator<, "<")
EACP_VECTOR_COMPARISON(operator<=, "<=")
EACP_VECTOR_COMPARISON(operator>, ">")
EACP_VECTOR_COMPARISON(operator>=, ">=")
EACP_VECTOR_COMPARISON(operator==, "==")
EACP_VECTOR_COMPARISON(operator!=, "!=")

#undef EACP_VECTOR_COMPARISON
#undef EACP_VECTOR_COMPARISON_AT
#undef EACP_INT_VECTOR_COMPARISON_AT

// Both operands are recorded whatever the condition; the emitted operator is
// the language's own, so the shader itself still short-circuits.
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

// Two conditions compared rather than combined: `a == b` is true when both are
// false.
inline Bool operator==(const Bool& lhs, const Bool& rhs)
{
    return detail::compare("==", lhs, rhs);
}

inline Bool operator!=(const Bool& lhs, const Bool& rhs)
{
    return detail::compare("!=", lhs, rhs);
}

// Collapses a mask into something a branch or a select can test. Both take the
// scalar Bool too, where they are the identity.
template <BoolValueLike T>
Bool all(const T& mask)
{
    return detail::call<Bool>(mask, ValueType::Bool, "all");
}

template <BoolValueLike T>
Bool any(const T& mask)
{
    return detail::call<Bool>(mask, ValueType::Bool, "any");
}

// The componentwise negation, which GLSL spells not().
template <BoolVectorLike T>
ShaderHandle<T> operator!(const T& mask)
{
    return detail::unaryOp<ShaderHandle<T>>('!', mask);
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

// GLSL's ternary. Both branches are values already computed, so this picks
// between them rather than skipping one; use ifThen() where that matters.
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

// A mutable shader local: the one handle naming a place rather than a value, so
// a read evaluates to whatever the statements before it left there. It is
// declared where it is created, and non-copyable so it cannot alias a slot.
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

    // The value the variable holds where the read appears.
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

    Var& operator=(unsigned value)
        requires std::same_as<T, UInt>
    {
        graph->assign(slot, graph->addUIntConstant(value));
        return *this;
    }

    // The compound operators, over whatever the free operators above accept.
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

// Index arithmetic on uint values, against another uint or an integer literal.
// There are no implicit conversions to the float vocabulary; cross with
// toFloat(). Subtraction wraps below zero, so guard a backwards step with max().
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
// remainder, the bitwise set and the two shifts. Division and the remainder
// truncate towards zero on a negative operand; mod() is the floored one.
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

// The same set componentwise on the integer vectors: against another vector of
// the same width, or a scalar Int or literal broadcast across it either way
// round.
#define EACP_INT_VECTOR_OPERATOR(name, spelling)                                    \
    template <IntVectorLike L, SameShaderHandle<L> R>                               \
    ShaderHandle<L> name(const L& lhs, const R& rhs)                                \
    {                                                                               \
        return detail::binaryOp<ShaderHandle<L>>(spelling, lhs, rhs);               \
    }                                                                               \
                                                                                    \
    template <IntVectorLike L, IntScalarLike R>                                     \
    ShaderHandle<L> name(const L& lhs, const R& rhs)                                \
    {                                                                               \
        return detail::binaryOp<ShaderHandle<L>>(spelling, lhs, rhs);               \
    }                                                                               \
                                                                                    \
    template <IntScalarLike L, IntVectorLike R>                                     \
    ShaderHandle<R> name(const L& lhs, const R& rhs)                                \
    {                                                                               \
        return detail::binaryOp<ShaderHandle<R>>(spelling, lhs, rhs);               \
    }                                                                               \
                                                                                    \
    template <IntVectorLike L>                                                      \
    ShaderHandle<L> name(const L& lhs, int rhs)                                     \
    {                                                                               \
        return detail::binaryOp<ShaderHandle<L>>(                                   \
            spelling, lhs, detail::intConstantOn(lhs, rhs));                        \
    }                                                                               \
                                                                                    \
    template <IntVectorLike R>                                                      \
    ShaderHandle<R> name(int lhs, const R& rhs)                                     \
    {                                                                               \
        return detail::binaryOp<ShaderHandle<R>>(                                   \
            spelling, detail::intConstantOn(rhs, lhs), rhs);                        \
    }

EACP_INT_VECTOR_OPERATOR(operator+, '+')
EACP_INT_VECTOR_OPERATOR(operator-, '-')
EACP_INT_VECTOR_OPERATOR(operator*, '*')
EACP_INT_VECTOR_OPERATOR(operator/, '/')
EACP_INT_VECTOR_OPERATOR(operator%, '%')
EACP_INT_VECTOR_OPERATOR(operator&, '&')
EACP_INT_VECTOR_OPERATOR(operator|, '|')
EACP_INT_VECTOR_OPERATOR(operator^, '^')
EACP_INT_VECTOR_OPERATOR(operator<<, "<<")
EACP_INT_VECTOR_OPERATOR(operator>>, ">>")

#undef EACP_INT_VECTOR_OPERATOR

template <IntVectorLike T>
ShaderHandle<T> operator-(const T& value)
{
    return detail::unaryOp<ShaderHandle<T>>('-', value);
}

template <IntVectorLike T>
ShaderHandle<T> operator~(const T& value)
{
    return detail::unaryOp<ShaderHandle<T>>('~', value);
}

// min/max/abs componentwise.
template <IntVectorLike L, SameShaderHandle<L> R>
ShaderHandle<L> min(const L& a, const R& b)
{
    return detail::call2<ShaderHandle<L>>(
        a, b, ValueTypeOf<ShaderHandle<L>>::value, "min");
}

template <IntVectorLike L, SameShaderHandle<L> R>
ShaderHandle<L> max(const L& a, const R& b)
{
    return detail::call2<ShaderHandle<L>>(
        a, b, ValueTypeOf<ShaderHandle<L>>::value, "max");
}

template <IntVectorLike T>
ShaderHandle<T> abs(const T& value)
{
    return detail::call<ShaderHandle<T>>(
        value, ValueTypeOf<ShaderHandle<T>>::value, "abs");
}

// Integer comparisons: the float ones are constrained on the float scalar
// shape, which an Int deliberately is not.
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

// And the uint ones. The literal overloads take unsigned and record a uint
// constant node, so a bound is spelled i < 4u.
#define EACP_UINT_COMPARISON(name, spelling)                                        \
    inline Bool name(const UInt& lhs, const UInt& rhs)                              \
    {                                                                               \
        return detail::compare(spelling, lhs, rhs);                                 \
    }                                                                               \
                                                                                    \
    inline Bool name(const UInt& lhs, unsigned rhs)                                 \
    {                                                                               \
        return detail::compare(spelling, lhs, detail::uintConstantOn(lhs, rhs));    \
    }                                                                               \
                                                                                    \
    inline Bool name(unsigned lhs, const UInt& rhs)                                 \
    {                                                                               \
        return detail::compare(spelling, detail::uintConstantOn(rhs, lhs), rhs);    \
    }

EACP_UINT_COMPARISON(operator<, "<")
EACP_UINT_COMPARISON(operator<=, "<=")
EACP_UINT_COMPARISON(operator>, ">")
EACP_UINT_COMPARISON(operator>=, ">=")
EACP_UINT_COMPARISON(operator==, "==")
EACP_UINT_COMPARISON(operator!=, "!=")

#undef EACP_UINT_COMPARISON

// int min/max/abs.
inline Int min(const Int& a, const Int& b)
{
    return detail::call2<Int>(a, b, ValueType::Int, "min");
}

inline Int min(const Int& a, int b)
{
    return detail::call2<Int>(a, detail::intConstantOn(a, b), ValueType::Int, "min");
}

// The literal on the left as well.
inline Int min(int a, const Int& b)
{
    return detail::call2<Int>(detail::intConstantOn(b, a), b, ValueType::Int, "min");
}

inline Int max(const Int& a, const Int& b)
{
    return detail::call2<Int>(a, b, ValueType::Int, "max");
}

inline Int max(const Int& a, int b)
{
    return detail::call2<Int>(a, detail::intConstantOn(a, b), ValueType::Int, "max");
}

inline Int max(int a, const Int& b)
{
    return detail::call2<Int>(detail::intConstantOn(b, a), b, ValueType::Int, "max");
}

inline Int abs(const Int& value)
{
    return detail::call<Int>(value, ValueType::Int, "abs");
}

// Crossing between the integer and the float vocabularies, explicit in both
// directions and truncating towards zero on the way to an int.
namespace detail
{
template <typename Result, typename T>
Result convertTo(const T& value)
{
    constexpr auto type = ValueTypeOf<Result>::value;
    return call<Result>(value, type, typeName(type));
}
} // namespace detail

inline Float toFloat(const Int& value)
{
    return detail::convertTo<Float>(value);
}

// Out of a condition: int(a > b) is 1 or 0.
inline Int toInt(const Bool& value)
{
    return detail::convertTo<Int>(value);
}

inline Float toFloat(const Bool& value)
{
    return detail::convertTo<Float>(value);
}

template <ShaderScalarLike T>
Int toInt(const T& value)
{
    return detail::convertTo<Int>(value);
}

// And between the two integer vocabularies. toUInt of a float scalar truncates
// towards zero on the way, exactly as toInt does.
inline Int toInt(const UInt& value)
{
    return detail::convertTo<Int>(value);
}

inline UInt toUInt(const Int& value)
{
    return detail::convertTo<UInt>(value);
}

template <ShaderScalarLike T>
UInt toUInt(const T& value)
{
    return detail::convertTo<UInt>(value);
}

// The same crossing a whole vector at a time, component by component.
template <ShaderShape<Float2> T>
Int2 toInt(const T& value)
{
    return detail::convertTo<Int2>(value);
}

template <ShaderShape<Float3> T>
Int3 toInt(const T& value)
{
    return detail::convertTo<Int3>(value);
}

template <ShaderShape<Float4> T>
Int4 toInt(const T& value)
{
    return detail::convertTo<Int4>(value);
}

template <SameShaderHandle<Int2> T>
Float2 toFloat(const T& value)
{
    return detail::convertTo<Float2>(value);
}

template <SameShaderHandle<Int3> T>
Float3 toFloat(const T& value)
{
    return detail::convertTo<Float3>(value);
}

template <SameShaderHandle<Int4> T>
Float4 toFloat(const T& value)
{
    return detail::convertTo<Float4>(value);
}

// A constant array the shader subscripts, slot-identified rather than an
// expression node. Its elements are evaluated once at the top of the body, so
// none may be a mutable local; an Int index past the end is undefined.
template <typename T, int Size>
struct ConstantArray
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

// A threadgroup-shared array, created inside a kernel body. Never CPU-visible,
// so the element type is any value type. What one thread wrote is visible to
// the rest of its group only after barrier(); no read is reused across one.
template <typename T>
struct Shared
{
    T operator[](const UInt& index) const
    {
        auto result = T {};
        result.graph = graph;
        result.node = graph->addSharedRead(slot, index.node);
        return result;
    }

    // The literal subscript, e.g. tile[0u].
    T operator[](unsigned index) const
    {
        auto result = T {};
        result.graph = graph;
        result.node = graph->addSharedRead(slot, graph->addUIntConstant(index));
        return result;
    }

    ShaderGraph* graph = nullptr;
    int slot = -1;
};

// uint min/max.
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

// Matrix * vector.
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

// Vector * matrix: the same product against the matrix's rows rather than its
// columns.
inline Float2 operator*(const Float2& vector, const Float2x2& matrix)
{
    return detail::matrixMul<Float2>(vector, matrix);
}

inline Float3 operator*(const Float3& vector, const Float3x3& matrix)
{
    return detail::matrixMul<Float3>(vector, matrix);
}

inline Float4 operator*(const Float4& vector, const Float4x4& matrix)
{
    return detail::matrixMul<Float4>(vector, matrix);
}

// Matrix * matrix.
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

// A matrix scaled by a scalar, on either side and by a handle or a literal.
// Not a Mul node: it multiplies every element, which a transpose leaves alone.
#define EACP_MATRIX_SCALE(Matrix)                                                   \
    template <ShaderScalarLike S>                                                   \
    Matrix operator*(const Matrix& matrix, const S& scalar)                         \
    {                                                                               \
        return detail::binaryOp<Matrix>('*', matrix, scalar);                       \
    }                                                                               \
                                                                                    \
    template <ShaderScalarLike S>                                                   \
    Matrix operator*(const S& scalar, const Matrix& matrix)                         \
    {                                                                               \
        return detail::binaryOp<Matrix>('*', scalar, matrix);                       \
    }                                                                               \
                                                                                    \
    inline Matrix operator*(const Matrix& matrix, float scalar)                     \
    {                                                                               \
        return detail::scalarOp<Matrix>('*', matrix, scalar);                       \
    }                                                                               \
                                                                                    \
    inline Matrix operator*(float scalar, const Matrix& matrix)                     \
    {                                                                               \
        return detail::scalarOpLeft<Matrix>('*', scalar, matrix);                   \
    }                                                                               \
                                                                                    \
    template <ShaderScalarLike S>                                                   \
    Matrix operator/(const Matrix& matrix, const S& scalar)                         \
    {                                                                               \
        return detail::binaryOp<Matrix>('/', matrix, scalar);                       \
    }                                                                               \
                                                                                    \
    inline Matrix operator/(const Matrix& matrix, float scalar)                     \
    {                                                                               \
        return detail::scalarOp<Matrix>('/', matrix, scalar);                       \
    }

EACP_MATRIX_SCALE(Float2x2)
EACP_MATRIX_SCALE(Float3x3)
EACP_MATRIX_SCALE(Float4x4)

#undef EACP_MATRIX_SCALE

// Column-major, matching Metal's float4x4(c0, c1, c2, c3); the HLSL emitter
// transposes this construction, HLSL filling a matrix from rows.
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

// Correct on both backends despite HLSL holding transposed what MSL holds:
// transposing each leaves each holding the transpose of the same logical value.
// There is no inverse() because neither shading language offers one.
inline Float2x2 transpose(const Float2x2& matrix)
{
    return detail::call<Float2x2>(matrix, ValueType::Float2x2, "transpose");
}

inline Float3x3 transpose(const Float3x3& matrix)
{
    return detail::call<Float3x3>(matrix, ValueType::Float3x3, "transpose");
}

inline Float4x4 transpose(const Float4x4& matrix)
{
    return detail::call<Float4x4>(matrix, ValueType::Float4x4, "transpose");
}

// A matrix and its transpose have the same determinant, so the backends agree
// whichever each is holding.
inline Float determinant(const Float2x2& matrix)
{
    return detail::call<Float>(matrix, ValueType::Float, "determinant");
}

inline Float determinant(const Float3x3& matrix)
{
    return detail::call<Float>(matrix, ValueType::Float, "determinant");
}

inline Float determinant(const Float4x4& matrix)
{
    return detail::call<Float>(matrix, ValueType::Float, "determinant");
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

// Taken from the first handle argument; the constraint guarantees one exists.
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

// Going through the base handle rather than reading .node directly is what lets
// a derived handle - a Uniform<Float3>, a Var<Float3> - fill a component.
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

// A pack that fills a vector of the given width, with at least one handle to
// supply the graph an all-literal vector lacks.
template <int Width, typename... Args>
concept ComponentsFor = (ShaderComponent<Args> && ...)
                        && (detail::componentsOf<Args>() + ... + 0) == Width
                        && (ShaderValueLike<Args> || ...);

// Vector constructors from any mix of value handles and numeric literals whose
// components total the vector's width, e.g. float4(position, 0.0f, 1.0f).
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

namespace detail
{
// The same machinery for the integer and boolean families, kept separate so a
// literal becomes a constant of the right kind and the families cannot mix.
template <typename T>
constexpr int handleComponentsOf()
{
    if constexpr (std::is_arithmetic_v<T>)
        return 1;
    else
        return componentCount(ValueTypeOf<ShaderHandle<T>>::value);
}

inline ShaderGraph* handleGraphOf()
{
    return nullptr;
}

template <typename First, typename... Rest>
ShaderGraph* handleGraphOf(const First& first, const Rest&... rest)
{
    if constexpr (std::is_arithmetic_v<First>)
        return handleGraphOf(rest...);
    else
        return first.graph;
}

template <typename T>
int intNodeOf(ShaderGraph& graph, const T& value)
{
    if constexpr (std::is_arithmetic_v<T>)
        return graph.addIntConstant((int) value);
    else
        return ShaderHandle<T>(value).node;
}

template <typename T>
int boolNodeOf(ShaderGraph& graph, const T& value)
{
    if constexpr (std::is_arithmetic_v<T>)
        return graph.addBoolConstant(value != 0);
    else
        return ShaderHandle<T>(value).node;
}

template <typename Result, bool Integer, typename... Args>
Result buildFrom(const Args&... args)
{
    auto& graph = *handleGraphOf(args...);

    auto nodes = Vector<int> {};

    if constexpr (Integer)
        (nodes.add(intNodeOf(graph, args)), ...);
    else
        (nodes.add(boolNodeOf(graph, args)), ...);

    auto result = Result {};
    result.graph = &graph;
    result.node = graph.addConstruct(ValueTypeOf<Result>::value, std::move(nodes));
    return result;
}
} // namespace detail

// A pack filling an integer or a boolean vector of the given width, under the
// same at-least-one-handle rule the float constructors carry.
template <int Width, typename... Args>
concept IntComponentsFor = ((IntValueLike<Args> || std::is_integral_v<Args>) && ...)
                           && (detail::handleComponentsOf<Args>() + ... + 0) == Width
                           && (IntValueLike<Args> || ...);

template <int Width, typename... Args>
concept BoolComponentsFor =
    ((BoolValueLike<Args> || std::is_same_v<Args, bool>) && ...)
    && (detail::handleComponentsOf<Args>() + ... + 0) == Width
    && (BoolValueLike<Args> || ...);

template <typename... Args>
    requires IntComponentsFor<2, Args...>
Int2 int2(const Args&... args)
{
    return detail::buildFrom<Int2, true>(args...);
}

template <typename... Args>
    requires IntComponentsFor<3, Args...>
Int3 int3(const Args&... args)
{
    return detail::buildFrom<Int3, true>(args...);
}

template <typename... Args>
    requires IntComponentsFor<4, Args...>
Int4 int4(const Args&... args)
{
    return detail::buildFrom<Int4, true>(args...);
}

template <typename... Args>
    requires BoolComponentsFor<2, Args...>
Bool2 bool2(const Args&... args)
{
    return detail::buildFrom<Bool2, false>(args...);
}

template <typename... Args>
    requires BoolComponentsFor<3, Args...>
Bool3 bool3(const Args&... args)
{
    return detail::buildFrom<Bool3, false>(args...);
}

template <typename... Args>
    requires BoolComponentsFor<4, Args...>
Bool4 bool4(const Args&... args)
{
    return detail::buildFrom<Bool4, false>(args...);
}
} // namespace eacp::GPU
