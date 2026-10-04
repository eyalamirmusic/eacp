#include "ShaderValue.h"

#include "SwizzleGrid.h"

namespace eacp::GPU
{
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

template struct Swizzles<Floats, 2>;
template struct Swizzles<Floats, 3>;
template struct Swizzles<Floats, 4>;
template struct Swizzles<UInts, 2>;
template struct Swizzles<UInts, 3>;
template struct Swizzles<UInts, 4>;
template struct Swizzles<Ints, 2>;
template struct Swizzles<Ints, 3>;
template struct Swizzles<Ints, 4>;
template struct Swizzles<Bools, 2>;
template struct Swizzles<Bools, 3>;
template struct Swizzles<Bools, 4>;
} // namespace detail

Float4 sample(const Texture2D& texture, const Float2& coordinates)
{
    auto result = Float4 {};
    result.graph = texture.graph;
    result.node = texture.graph->addSample(texture.slot, coordinates.node);
    return result;
}

Float4 sample(const TextureCube& texture, const Float3& direction)
{
    auto result = Float4 {};
    result.graph = texture.graph;
    result.node = texture.graph->addSample(texture.slot, direction.node);
    return result;
}

Float sample(const TextureDepth2D& texture, const Float2& coordinates)
{
    auto result = Float {};
    result.graph = texture.graph;
    result.node = texture.graph->addDepthSample(texture.slot, coordinates.node);
    return result;
}

Float4
    sample(const Texture2D& texture, const Float2& coordinates, const Float& level)
{
    auto result = Float4 {};
    result.graph = texture.graph;
    result.node =
        texture.graph->addSample(texture.slot, coordinates.node, level.node);
    return result;
}

Float4 sample(const Texture2D& texture, const Float2& coordinates, float level)
{
    auto result = Float4 {};
    result.graph = texture.graph;
    result.node = texture.graph->addSample(
        texture.slot, coordinates.node, texture.graph->addConstant(level));
    return result;
}

Float4 fetch(const Texture2D& texture, const Int2& coordinates)
{
    auto result = Float4 {};
    result.graph = texture.graph;
    result.node = texture.graph->addFetch(texture.slot, coordinates.node);
    return result;
}

Float4 fetch(const Texture2D& texture, const Float2& coordinates)
{
    auto result = Float4 {};
    result.graph = texture.graph;
    result.node = texture.graph->addFetch(texture.slot, coordinates.node);
    return result;
}

namespace detail
{
UInt bufferIndex(ShaderGraph* graph, unsigned index)
{
    auto result = UInt {};
    result.graph = graph;
    result.node = graph->addUIntConstant(index);
    return result;
}

int recordBase(ShaderGraph* graph, const UInt& index, int count)
{
    return graph->addBinary(
        ValueType::UInt, '*', index.node, graph->addUIntConstant((unsigned) count));
}

// One element of a storage buffer, whichever way the kernel declared it. Both
// backends subscript the binding they were given, so a read is the same node
// and the same emitted text for an input and for an output.
template <typename T>
T readBufferElement(ShaderGraph* graph, int slot, const UInt& index)
{
    auto result = T {};
    result.graph = graph;
    result.node = graph->addBufferRead(slot, index.node);
    return result;
}

// count consecutive elements starting at index * count, assembled into a
// vector, as count separate subscripts. A buffer stays a run of floats on every
// backend - this is arithmetic over the binding that already works, not a
// retyped one - so what it costs is count scalar loads. See
// ShaderBuilder::write for the store that lays the same layout down.
//
// What a *read-only* buffer takes instead is readBufferVectorLoad below. This
// is what an output takes, and the difference is not a missing optimisation:
// an output may hold what this very thread stored into it a statement ago, and
// the subscript through the pointer that was written is what orders the two.
// A load through a second pointer of another type has nothing saying it may not
// be hoisted above the store.
//
// Which is also why one GPU::Buffer must not be bound to an input slot and an
// output slot of the same kernel, as InputBuffer's own comment says: the
// emitter orders a read against the stores to *its slot*, not against the
// stores to whatever resource the slot was bound, so an input's packed load is
// unordered against a write through the output slot that happens to name the
// same buffer. A kernel computing in place declares one OutputBuffer and reads
// that.
template <typename T>
T readBufferVector(
    ShaderGraph* graph, int slot, const UInt& index, ValueType type, int count)
{
    auto base = recordBase(graph, index, count);

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

// The same run of elements as one node, which Metal makes one load of and the
// other two print as exactly the construct above. Read-only buffers only.
template <typename T>
T readBufferVectorLoad(
    ShaderGraph* graph, int slot, const UInt& index, ValueType type, int count)
{
    auto result = T {};
    result.graph = graph;
    result.node =
        graph->addBufferVectorRead(slot, recordBase(graph, index, count), type);

    return result;
}

ValueHandle constantOn(const ValueHandle& value, float literal)
{
    return {value.graph, value.graph->addConstant(literal)};
}

ValueHandle uintConstantOn(const ValueHandle& value, unsigned literal)
{
    return {value.graph, value.graph->addUIntConstant(literal)};
}

ValueHandle intConstantOn(const ValueHandle& value, int literal)
{
    return {value.graph, value.graph->addIntConstant(literal)};
}

ValueHandle boolConstantOn(const ValueHandle& value, bool literal)
{
    return {value.graph, value.graph->addBoolConstant(literal)};
}

void anchorAt(const ValueHandle*&, float) {}

int argumentNode(const ValueHandle& anchor, float literal)
{
    return anchor.graph->addConstant(literal);
}
} // namespace detail

Float InputBuffer::operator[](const UInt& index) const
{
    return detail::readBufferElement<Float>(graph, slot, index);
}

Float InputBuffer::operator[](unsigned index) const
{
    return (*this)[detail::bufferIndex(graph, index)];
}

Float2 InputBuffer::read2(const UInt& index) const
{
    return detail::readBufferVectorLoad<Float2>(
        graph, slot, index, ValueType::Float2, 2);
}

Float3 InputBuffer::read3(const UInt& index) const
{
    return detail::readBufferVectorLoad<Float3>(
        graph, slot, index, ValueType::Float3, 3);
}

Float4 InputBuffer::read4(const UInt& index) const
{
    return detail::readBufferVectorLoad<Float4>(
        graph, slot, index, ValueType::Float4, 4);
}

Float2 InputBuffer::read2(unsigned index) const
{
    return read2(detail::bufferIndex(graph, index));
}

Float3 InputBuffer::read3(unsigned index) const
{
    return read3(detail::bufferIndex(graph, index));
}

Float4 InputBuffer::read4(unsigned index) const
{
    return read4(detail::bufferIndex(graph, index));
}

Float OutputBuffer::operator[](const UInt& index) const
{
    return detail::readBufferElement<Float>(graph, slot, index);
}

Float OutputBuffer::operator[](unsigned index) const
{
    return (*this)[detail::bufferIndex(graph, index)];
}

Float2 OutputBuffer::read2(const UInt& index) const
{
    return detail::readBufferVector<Float2>(
        graph, slot, index, ValueType::Float2, 2);
}

Float3 OutputBuffer::read3(const UInt& index) const
{
    return detail::readBufferVector<Float3>(
        graph, slot, index, ValueType::Float3, 3);
}

Float4 OutputBuffer::read4(const UInt& index) const
{
    return detail::readBufferVector<Float4>(
        graph, slot, index, ValueType::Float4, 4);
}

Float2 OutputBuffer::read2(unsigned index) const
{
    return read2(detail::bufferIndex(graph, index));
}

Float3 OutputBuffer::read3(unsigned index) const
{
    return read3(detail::bufferIndex(graph, index));
}

Float4 OutputBuffer::read4(unsigned index) const
{
    return read4(detail::bufferIndex(graph, index));
}

UInt UIntInputBuffer::operator[](const UInt& index) const
{
    return detail::readBufferElement<UInt>(graph, slot, index);
}

UInt UIntInputBuffer::operator[](unsigned index) const
{
    return (*this)[detail::bufferIndex(graph, index)];
}

UInt2 UIntInputBuffer::read2(const UInt& index) const
{
    return detail::readBufferVectorLoad<UInt2>(
        graph, slot, index, ValueType::UInt2, 2);
}

UInt3 UIntInputBuffer::read3(const UInt& index) const
{
    return detail::readBufferVectorLoad<UInt3>(
        graph, slot, index, ValueType::UInt3, 3);
}

UInt4 UIntInputBuffer::read4(const UInt& index) const
{
    return detail::readBufferVectorLoad<UInt4>(
        graph, slot, index, ValueType::UInt4, 4);
}

UInt2 UIntInputBuffer::read2(unsigned index) const
{
    return read2(detail::bufferIndex(graph, index));
}

UInt3 UIntInputBuffer::read3(unsigned index) const
{
    return read3(detail::bufferIndex(graph, index));
}

UInt4 UIntInputBuffer::read4(unsigned index) const
{
    return read4(detail::bufferIndex(graph, index));
}

UInt UIntOutputBuffer::operator[](const UInt& index) const
{
    return detail::readBufferElement<UInt>(graph, slot, index);
}

UInt UIntOutputBuffer::operator[](unsigned index) const
{
    return (*this)[detail::bufferIndex(graph, index)];
}

UInt2 UIntOutputBuffer::read2(const UInt& index) const
{
    return detail::readBufferVector<UInt2>(graph, slot, index, ValueType::UInt2, 2);
}

UInt3 UIntOutputBuffer::read3(const UInt& index) const
{
    return detail::readBufferVector<UInt3>(graph, slot, index, ValueType::UInt3, 3);
}

UInt4 UIntOutputBuffer::read4(const UInt& index) const
{
    return detail::readBufferVector<UInt4>(graph, slot, index, ValueType::UInt4, 4);
}

UInt2 UIntOutputBuffer::read2(unsigned index) const
{
    return read2(detail::bufferIndex(graph, index));
}

UInt3 UIntOutputBuffer::read3(unsigned index) const
{
    return read3(detail::bufferIndex(graph, index));
}

UInt4 UIntOutputBuffer::read4(unsigned index) const
{
    return read4(detail::bufferIndex(graph, index));
}

UInt UIntOutputBuffer::literal(unsigned value) const
{
    return detail::bufferIndex(graph, value);
}

UInt AtomicBuffer::load(const UInt& index) const
{
    auto result = UInt {};
    result.graph = graph;
    result.node = graph->addAtomicLoad(slot, index.node);
    return result;
}

UInt AtomicBuffer::load(unsigned index) const
{
    return load(literal(index));
}

UInt AtomicBuffer::literal(unsigned value) const
{
    auto result = UInt {};
    result.graph = graph;
    result.node = graph->addUIntConstant(value);
    return result;
}

Float toFloat(const UInt& value)
{
    return detail::call<Float>(value, ValueType::Float, "float");
}

namespace detail
{
Bool compare(const char* op, const ValueHandle& lhs, const ValueHandle& rhs)
{
    auto result = Bool {};
    result.graph = lhs.graph;
    result.node = lhs.graph->addCompare(op, lhs.node, rhs.node);
    return result;
}
} // namespace detail

Bool operator&&(const Bool& lhs, const Bool& rhs)
{
    return detail::compare("&&", lhs, rhs);
}

Bool operator||(const Bool& lhs, const Bool& rhs)
{
    return detail::compare("||", lhs, rhs);
}

Bool operator!(const Bool& value)
{
    return detail::unaryOp<Bool>('!', value);
}

Bool operator==(const Bool& lhs, const Bool& rhs)
{
    return detail::compare("==", lhs, rhs);
}

Bool operator!=(const Bool& lhs, const Bool& rhs)
{
    return detail::compare("!=", lhs, rhs);
}

Float select(const Bool& condition, float whenTrue, float whenFalse)
{
    return detail::selectOp<Float>(condition,
                                   detail::constantOn(condition, whenTrue),
                                   detail::constantOn(condition, whenFalse));
}

UInt select(const Bool& condition, unsigned whenTrue, unsigned whenFalse)
{
    return detail::selectOp<UInt>(condition,
                                  detail::uintConstantOn(condition, whenTrue),
                                  detail::uintConstantOn(condition, whenFalse));
}

Int select(const Bool& condition, int whenTrue, int whenFalse)
{
    return detail::selectOp<Int>(condition,
                                 detail::intConstantOn(condition, whenTrue),
                                 detail::intConstantOn(condition, whenFalse));
}

Bool select(const Bool& condition, bool whenTrue, bool whenFalse)
{
    return detail::selectOp<Bool>(condition,
                                  detail::boolConstantOn(condition, whenTrue),
                                  detail::boolConstantOn(condition, whenFalse));
}

UInt operator~(const UInt& value)
{
    return detail::unaryOp<UInt>('~', value);
}

Int operator-(const Int& value)
{
    return detail::unaryOp<Int>('-', value);
}

Int operator~(const Int& value)
{
    return detail::unaryOp<Int>('~', value);
}

Int min(const Int& a, const Int& b)
{
    return detail::call2<Int>(a, b, ValueType::Int, "min");
}

Int min(const Int& a, int b)
{
    return detail::call2<Int>(a, detail::intConstantOn(a, b), ValueType::Int, "min");
}

Int min(int a, const Int& b)
{
    return detail::call2<Int>(detail::intConstantOn(b, a), b, ValueType::Int, "min");
}

Int max(const Int& a, const Int& b)
{
    return detail::call2<Int>(a, b, ValueType::Int, "max");
}

Int max(const Int& a, int b)
{
    return detail::call2<Int>(a, detail::intConstantOn(a, b), ValueType::Int, "max");
}

Int max(int a, const Int& b)
{
    return detail::call2<Int>(detail::intConstantOn(b, a), b, ValueType::Int, "max");
}

Int abs(const Int& value)
{
    return detail::call<Int>(value, ValueType::Int, "abs");
}

Float toFloat(const Int& value)
{
    return detail::convertTo<Float>(value);
}

Int toInt(const Bool& value)
{
    return detail::convertTo<Int>(value);
}

Float toFloat(const Bool& value)
{
    return detail::convertTo<Float>(value);
}

Int toInt(const UInt& value)
{
    return detail::convertTo<Int>(value);
}

UInt toUInt(const Int& value)
{
    return detail::convertTo<UInt>(value);
}

UInt asUInt(const Float& value)
{
    return detail::intrinsic<UInt>("as_type<uint>", value);
}

Float asFloat(const UInt& value)
{
    return detail::call<Float>(value, ValueType::Float, "as_type<float>");
}

Float2 unpackHalf2(const UInt& bits)
{
    auto arguments = Vector<int> {};
    arguments.add(bits.node);

    auto result = Float2 {};
    result.graph = bits.graph;
    result.node = bits.graph->addCall(
        ValueType::Float2, "eacpUnpackHalf2", std::move(arguments));

    return result;
}

UInt packHalf2(const Float2& values)
{
    return detail::call<UInt>(values, ValueType::UInt, "eacpPackHalf2");
}

// One fp16 element of a buffer whose elements are halves rather than floats,
// widened to a Float. The index counts halves, so a buffer of N weights is
// walked 0..N-1 exactly as a float one is and nothing at the call site spells
// the packing: the word is index / 2 and which half of it is index % 2.
//
// The choice between the two halves is made inside a helper taking the word
// and that parity, rather than by unpacking both and selecting. Both are
// correct; the helper is one call node instead of six, and it is the shape the
// languages already have - MSL and HLSL each reach the wanted half with a
// single shift, where a select computes both and throws one away.
Float InputBuffer::readHalf(const UInt& index) const
{
    return detail::call2<Float>(
        asUInt((*this)[index / 2u]), index % 2u, ValueType::Float, "eacpReadHalf");
}

// The literal form, folded here rather than emitted as `6u / 2u`.
Float InputBuffer::readHalf(unsigned index) const
{
    return detail::call2<Float>(asUInt((*this)[index / 2u]),
                                detail::bufferIndex(graph, index % 2u),
                                ValueType::Float,
                                "eacpReadHalf");
}

// Both halves of one word, which is what a kernel walking a weight matrix two
// at a time wants. The index counts words here rather than halves - it is the
// same index the matching writeHalf2 stores at.
Float2 InputBuffer::readHalf2(const UInt& index) const
{
    return unpackHalf2(asUInt((*this)[index]));
}

Float2 InputBuffer::readHalf2(unsigned index) const
{
    return readHalf2(detail::bufferIndex(graph, index));
}

Float2 unpackBFloat16x2(const UInt& bits)
{
    auto arguments = Vector<int> {};
    arguments.add(bits.node);

    auto result = Float2 {};
    result.graph = bits.graph;
    result.node = bits.graph->addCall(
        ValueType::Float2, "eacpUnpackBFloat16x2", std::move(arguments));

    return result;
}

UInt packBFloat16x2(const Float2& values)
{
    return detail::call<UInt>(values, ValueType::UInt, "eacpPackBFloat16x2");
}

// One bf16 element of a buffer whose elements are bfloat16s rather than floats,
// widened to a Float. The index counts bfloat16s, so a buffer of N weights is
// walked 0..N-1 and the call site never spells the packing: the word is
// index / 2 and which half of it is index % 2.
//
// The parity goes into the helper rather than selecting between both halves,
// for the reason readHalf gives: shifting the wanted half up is one instruction
// in every language.
Float InputBuffer::readBFloat16(const UInt& index) const
{
    return detail::call2<Float>(asUInt((*this)[index / 2u]),
                                index % 2u,
                                ValueType::Float,
                                "eacpReadBFloat16");
}

// The literal form, folded here rather than emitted as `6u / 2u`.
Float InputBuffer::readBFloat16(unsigned index) const
{
    return detail::call2<Float>(asUInt((*this)[index / 2u]),
                                detail::bufferIndex(graph, index % 2u),
                                ValueType::Float,
                                "eacpReadBFloat16");
}

// Both bfloat16s of one word, which is what a kernel walking a weight matrix
// two at a time wants. The index counts words here rather than elements - it is
// the same index the matching writeBFloat16x2 stores at.
Float2 InputBuffer::readBFloat16x2(const UInt& index) const
{
    return unpackBFloat16x2(asUInt((*this)[index]));
}

Float2 InputBuffer::readBFloat16x2(unsigned index) const
{
    return readBFloat16x2(detail::bufferIndex(graph, index));
}

Float4 unpackInt8x4(const UInt& bits)
{
    return detail::call<Float4>(bits, ValueType::Float4, "eacpUnpackInt8x4");
}

Float4 unpackUInt8x4(const UInt& bits)
{
    return detail::call<Float4>(bits, ValueType::Float4, "eacpUnpackUInt8x4");
}

UInt packInt8x4(const Int4& values)
{
    return detail::call<UInt>(values, ValueType::UInt, "eacpPackInt8x4");
}

UInt packUInt8x4(const UInt4& values)
{
    return detail::call<UInt>(values, ValueType::UInt, "eacpPackUInt8x4");
}

Float4Pair unpackInt4x8(const UInt& bits)
{
    return {
        detail::call<Float4>(bits, ValueType::Float4, "eacpUnpackInt4x4"),
        detail::call<Float4>(bits >> 16u, ValueType::Float4, "eacpUnpackInt4x4")};
}

Float4Pair unpackUInt4x8(const UInt& bits)
{
    return {
        detail::call<Float4>(bits, ValueType::Float4, "eacpUnpackUInt4x4"),
        detail::call<Float4>(bits >> 16u, ValueType::Float4, "eacpUnpackUInt4x4")};
}

// One byte of a buffer whose elements are int8 rather than float, widened. The
// index counts bytes, so the call site never spells the packing: the word is
// index / 4 and which byte of it is index % 4.
//
// The byte position goes into the helper rather than selecting between four
// unpacked values, for the reason readHalf gives - shifting the wanted byte
// down is one instruction in every language, where a select computes four and
// throws three away.
Float InputBuffer::readInt8(const UInt& index) const
{
    return detail::call2<Float>(
        asUInt((*this)[index / 4u]), index % 4u, ValueType::Float, "eacpReadInt8");
}

// The literal form, folded here rather than emitted as `6u / 4u`.
Float InputBuffer::readInt8(unsigned index) const
{
    return detail::call2<Float>(asUInt((*this)[index / 4u]),
                                detail::bufferIndex(graph, index % 4u),
                                ValueType::Float,
                                "eacpReadInt8");
}

Float InputBuffer::readUInt8(const UInt& index) const
{
    return detail::call2<Float>(
        asUInt((*this)[index / 4u]), index % 4u, ValueType::Float, "eacpReadUInt8");
}

Float InputBuffer::readUInt8(unsigned index) const
{
    return detail::call2<Float>(asUInt((*this)[index / 4u]),
                                detail::bufferIndex(graph, index % 4u),
                                ValueType::Float,
                                "eacpReadUInt8");
}

// All four bytes of one word, which is what a kernel walking a quantized row
// wants. The index counts words here rather than bytes - it is the same index
// the matching writeInt8x4 stores at.
Float4 InputBuffer::readInt8x4(const UInt& index) const
{
    return unpackInt8x4(asUInt((*this)[index]));
}

Float4 InputBuffer::readInt8x4(unsigned index) const
{
    return readInt8x4(detail::bufferIndex(graph, index));
}

Float4 InputBuffer::readUInt8x4(const UInt& index) const
{
    return unpackUInt8x4(asUInt((*this)[index]));
}

Float4 InputBuffer::readUInt8x4(unsigned index) const
{
    return readUInt8x4(detail::bufferIndex(graph, index));
}

// Eight nibbles out of one word, on the terms readInt8x4 sets: the index counts
// words, and the whole word is fetched once.
Float4Pair InputBuffer::readInt4x8(const UInt& index) const
{
    return unpackInt4x8(asUInt((*this)[index]));
}

Float4Pair InputBuffer::readInt4x8(unsigned index) const
{
    return readInt4x8(detail::bufferIndex(graph, index));
}

Float4Pair InputBuffer::readUInt4x8(const UInt& index) const
{
    return unpackUInt4x8(asUInt((*this)[index]));
}

Float4Pair InputBuffer::readUInt4x8(unsigned index) const
{
    return readUInt4x8(detail::bufferIndex(graph, index));
}

// The wide byte reads. Each takes its words through one record read - read2 for
// eight bytes, read4 for sixteen - rather than through that many subscripts, so
// the whole record is one vector load wherever the backend has one and the four
// unpackings are register arithmetic over the value it brought back. It is the
// same shape readBFloat16x4 has, and deliberately: half the bytes for the same
// number of loads is the point of storing weights as bytes at all.
//
// The load is emitted once because the record read is one node with four uses,
// and the emitter names any node it evaluates more than once. Nothing here
// depends on a compiler noticing two subscripts are the same address, which the
// graph does not look for.
Float4Pair InputBuffer::readInt8x8(const UInt& index) const
{
    auto words = read2(index);

    return {unpackInt8x4(asUInt(words.x())), unpackInt8x4(asUInt(words.y()))};
}

Float4Pair InputBuffer::readInt8x8(unsigned index) const
{
    return readInt8x8(detail::bufferIndex(graph, index));
}

Float4Pair InputBuffer::readUInt8x8(const UInt& index) const
{
    auto words = read2(index);

    return {unpackUInt8x4(asUInt(words.x())), unpackUInt8x4(asUInt(words.y()))};
}

Float4Pair InputBuffer::readUInt8x8(unsigned index) const
{
    return readUInt8x8(detail::bufferIndex(graph, index));
}

Float4Quad InputBuffer::readInt8x16(const UInt& index) const
{
    auto words = read4(index);

    return {unpackInt8x4(asUInt(words.x())),
            unpackInt8x4(asUInt(words.y())),
            unpackInt8x4(asUInt(words.z())),
            unpackInt8x4(asUInt(words.w()))};
}

Float4Quad InputBuffer::readInt8x16(unsigned index) const
{
    return readInt8x16(detail::bufferIndex(graph, index));
}

Float4Quad InputBuffer::readUInt8x16(const UInt& index) const
{
    auto words = read4(index);

    return {unpackUInt8x4(asUInt(words.x())),
            unpackUInt8x4(asUInt(words.y())),
            unpackUInt8x4(asUInt(words.z())),
            unpackUInt8x4(asUInt(words.w()))};
}

Float4Quad InputBuffer::readUInt8x16(unsigned index) const
{
    return readUInt8x16(detail::bufferIndex(graph, index));
}

// Sixteen nibbles are the eight bytes readInt8x8 already fetches in one load,
// unpacked four ways instead of two - so the wide nibble read is the wide byte
// read's machinery with unpackInt4x8 in place of unpackInt8x4, and costs a
// helper of its own nothing.
Float4Quad InputBuffer::readInt4x16(const UInt& index) const
{
    auto words = read2(index);
    auto first = unpackInt4x8(asUInt(words.x()));
    auto second = unpackInt4x8(asUInt(words.y()));

    return {first.low, first.high, second.low, second.high};
}

Float4Quad InputBuffer::readInt4x16(unsigned index) const
{
    return readInt4x16(detail::bufferIndex(graph, index));
}

Float4Quad InputBuffer::readUInt4x16(const UInt& index) const
{
    auto words = read2(index);
    auto first = unpackUInt4x8(asUInt(words.x()));
    auto second = unpackUInt4x8(asUInt(words.y()));

    return {first.low, first.high, second.low, second.high};
}

Float4Quad InputBuffer::readUInt4x16(unsigned index) const
{
    return readUInt4x16(detail::bufferIndex(graph, index));
}

UInt min(const UInt& a, const UInt& b)
{
    return detail::call2<UInt>(a, b, ValueType::UInt, "min");
}

UInt min(const UInt& a, unsigned b)
{
    return detail::call2<UInt>(
        a, detail::uintConstantOn(a, b), ValueType::UInt, "min");
}

UInt max(const UInt& a, const UInt& b)
{
    return detail::call2<UInt>(a, b, ValueType::UInt, "max");
}

UInt max(const UInt& a, unsigned b)
{
    return detail::call2<UInt>(
        a, detail::uintConstantOn(a, b), ValueType::UInt, "max");
}

Float2 operator*(const Float2x2& matrix, const Float2& vector)
{
    return detail::matrixMul<Float2>(matrix, vector);
}

Float3 operator*(const Float3x3& matrix, const Float3& vector)
{
    return detail::matrixMul<Float3>(matrix, vector);
}

Float4 operator*(const Float4x4& matrix, const Float4& vector)
{
    return detail::matrixMul<Float4>(matrix, vector);
}

Float2 operator*(const Float2& vector, const Float2x2& matrix)
{
    return detail::matrixMul<Float2>(vector, matrix);
}

Float3 operator*(const Float3& vector, const Float3x3& matrix)
{
    return detail::matrixMul<Float3>(vector, matrix);
}

Float4 operator*(const Float4& vector, const Float4x4& matrix)
{
    return detail::matrixMul<Float4>(vector, matrix);
}

Float2x2 operator*(const Float2x2& a, const Float2x2& b)
{
    return detail::matrixMul<Float2x2>(a, b);
}

Float3x3 operator*(const Float3x3& a, const Float3x3& b)
{
    return detail::matrixMul<Float3x3>(a, b);
}

Float4x4 operator*(const Float4x4& a, const Float4x4& b)
{
    return detail::matrixMul<Float4x4>(a, b);
}

Float2x2 float2x2(const Float2& c0, const Float2& c1)
{
    return detail::construct<Float2x2>(
        *c0.graph, ValueType::Float2x2, {c0.node, c1.node});
}

Float3x3 float3x3(const Float3& c0, const Float3& c1, const Float3& c2)
{
    return detail::construct<Float3x3>(
        *c0.graph, ValueType::Float3x3, {c0.node, c1.node, c2.node});
}

Float4x4
    float4x4(const Float4& c0, const Float4& c1, const Float4& c2, const Float4& c3)
{
    return detail::construct<Float4x4>(
        *c0.graph, ValueType::Float4x4, {c0.node, c1.node, c2.node, c3.node});
}

Float2x2 transpose(const Float2x2& matrix)
{
    return detail::call<Float2x2>(matrix, ValueType::Float2x2, "transpose");
}

Float3x3 transpose(const Float3x3& matrix)
{
    return detail::call<Float3x3>(matrix, ValueType::Float3x3, "transpose");
}

Float4x4 transpose(const Float4x4& matrix)
{
    return detail::call<Float4x4>(matrix, ValueType::Float4x4, "transpose");
}

Float determinant(const Float2x2& matrix)
{
    return detail::call<Float>(matrix, ValueType::Float, "determinant");
}

Float determinant(const Float3x3& matrix)
{
    return detail::call<Float>(matrix, ValueType::Float, "determinant");
}

Float determinant(const Float4x4& matrix)
{
    return detail::call<Float>(matrix, ValueType::Float, "determinant");
}

// Four halves, which is two words - and read as one record of two floats rather
// than as two subscripts, so the eight bytes are a single load wherever the
// backend has one. The index counts records of four halves, so element k of the
// buffer is readHalf(k) and readHalf4(k / 4) component k % 4, on the layout
// readHalf2 already fixes: the low half of a word comes first.
Float4 InputBuffer::readHalf4(const UInt& index) const
{
    auto words = read2(index);

    return float4(unpackHalf2(asUInt(words.x())), unpackHalf2(asUInt(words.y())));
}

Float4 InputBuffer::readHalf4(unsigned index) const
{
    return readHalf4(detail::bufferIndex(graph, index));
}

// Four bfloat16s in two words, on exactly the terms readHalf4 sets: one record
// read of two floats, so one load, and the widening is four shifts over what it
// brought back.
Float4 InputBuffer::readBFloat16x4(const UInt& index) const
{
    auto words = read2(index);

    return float4(unpackBFloat16x2(asUInt(words.x())),
                  unpackBFloat16x2(asUInt(words.y())));
}

Float4 InputBuffer::readBFloat16x4(unsigned index) const
{
    return readBFloat16x4(detail::bufferIndex(graph, index));
}

#define EACP_DEFINE_UINT_OPERATOR(name, spelling)                                   \
    UInt name(const UInt& lhs, const UInt& rhs)                                     \
    {                                                                               \
        return detail::binaryOp<UInt>(spelling, lhs, rhs);                          \
    }                                                                               \
                                                                                    \
    UInt name(const UInt& lhs, unsigned rhs)                                        \
    {                                                                               \
        return detail::binaryOp<UInt>(                                              \
            spelling, lhs, detail::uintConstantOn(lhs, rhs));                       \
    }                                                                               \
                                                                                    \
    UInt name(unsigned lhs, const UInt& rhs)                                        \
    {                                                                               \
        return detail::binaryOp<UInt>(                                              \
            spelling, detail::uintConstantOn(rhs, lhs), rhs);                       \
    }

EACP_DEFINE_UINT_OPERATOR(operator+, '+')
EACP_DEFINE_UINT_OPERATOR(operator-, '-')
EACP_DEFINE_UINT_OPERATOR(operator*, '*')
EACP_DEFINE_UINT_OPERATOR(operator/, '/')
EACP_DEFINE_UINT_OPERATOR(operator%, '%')
EACP_DEFINE_UINT_OPERATOR(operator&, '&')
EACP_DEFINE_UINT_OPERATOR(operator|, '|')
EACP_DEFINE_UINT_OPERATOR(operator^, '^')
EACP_DEFINE_UINT_OPERATOR(operator<<, "<<")
EACP_DEFINE_UINT_OPERATOR(operator>>, ">>")

#undef EACP_DEFINE_UINT_OPERATOR

#define EACP_DEFINE_INT_OPERATOR(name, spelling)                                    \
    Int name(const Int& lhs, const Int& rhs)                                        \
    {                                                                               \
        return detail::binaryOp<Int>(spelling, lhs, rhs);                           \
    }                                                                               \
                                                                                    \
    Int name(const Int& lhs, int rhs)                                               \
    {                                                                               \
        return detail::binaryOp<Int>(                                               \
            spelling, lhs, detail::intConstantOn(lhs, rhs));                        \
    }                                                                               \
                                                                                    \
    Int name(int lhs, const Int& rhs)                                               \
    {                                                                               \
        return detail::binaryOp<Int>(                                               \
            spelling, detail::intConstantOn(rhs, lhs), rhs);                        \
    }

EACP_DEFINE_INT_OPERATOR(operator+, '+')
EACP_DEFINE_INT_OPERATOR(operator-, '-')
EACP_DEFINE_INT_OPERATOR(operator*, '*')
EACP_DEFINE_INT_OPERATOR(operator/, '/')
EACP_DEFINE_INT_OPERATOR(operator%, '%')
EACP_DEFINE_INT_OPERATOR(operator&, '&')
EACP_DEFINE_INT_OPERATOR(operator|, '|')
EACP_DEFINE_INT_OPERATOR(operator^, '^')
EACP_DEFINE_INT_OPERATOR(operator<<, "<<")
EACP_DEFINE_INT_OPERATOR(operator>>, ">>")

#undef EACP_DEFINE_INT_OPERATOR

#define EACP_DEFINE_INT_COMPARISON(name, spelling)                                  \
    Bool name(const Int& lhs, const Int& rhs)                                       \
    {                                                                               \
        return detail::compare(spelling, lhs, rhs);                                 \
    }                                                                               \
                                                                                    \
    Bool name(const Int& lhs, int rhs)                                              \
    {                                                                               \
        return detail::compare(spelling, lhs, detail::intConstantOn(lhs, rhs));     \
    }                                                                               \
                                                                                    \
    Bool name(int lhs, const Int& rhs)                                              \
    {                                                                               \
        return detail::compare(spelling, detail::intConstantOn(rhs, lhs), rhs);     \
    }

EACP_DEFINE_INT_COMPARISON(operator<, "<")
EACP_DEFINE_INT_COMPARISON(operator<=, "<=")
EACP_DEFINE_INT_COMPARISON(operator>, ">")
EACP_DEFINE_INT_COMPARISON(operator>=, ">=")
EACP_DEFINE_INT_COMPARISON(operator==, "==")
EACP_DEFINE_INT_COMPARISON(operator!=, "!=")

#undef EACP_DEFINE_INT_COMPARISON

#define EACP_DEFINE_UINT_COMPARISON(name, spelling)                                 \
    Bool name(const UInt& lhs, const UInt& rhs)                                     \
    {                                                                               \
        return detail::compare(spelling, lhs, rhs);                                 \
    }                                                                               \
                                                                                    \
    Bool name(const UInt& lhs, unsigned rhs)                                        \
    {                                                                               \
        return detail::compare(spelling, lhs, detail::uintConstantOn(lhs, rhs));    \
    }                                                                               \
                                                                                    \
    Bool name(unsigned lhs, const UInt& rhs)                                        \
    {                                                                               \
        return detail::compare(spelling, detail::uintConstantOn(rhs, lhs), rhs);    \
    }

EACP_DEFINE_UINT_COMPARISON(operator<, "<")
EACP_DEFINE_UINT_COMPARISON(operator<=, "<=")
EACP_DEFINE_UINT_COMPARISON(operator>, ">")
EACP_DEFINE_UINT_COMPARISON(operator>=, ">=")
EACP_DEFINE_UINT_COMPARISON(operator==, "==")
EACP_DEFINE_UINT_COMPARISON(operator!=, "!=")

#undef EACP_DEFINE_UINT_COMPARISON

#define EACP_DEFINE_MATRIX_SCALE(Matrix)                                            \
    Matrix operator*(const Matrix& matrix, float scalar)                            \
    {                                                                               \
        return detail::scalarOp<Matrix>('*', matrix, scalar);                       \
    }                                                                               \
                                                                                    \
    Matrix operator*(float scalar, const Matrix& matrix)                            \
    {                                                                               \
        return detail::scalarOpLeft<Matrix>('*', scalar, matrix);                   \
    }                                                                               \
                                                                                    \
    Matrix operator/(const Matrix& matrix, float scalar)                            \
    {                                                                               \
        return detail::scalarOp<Matrix>('/', matrix, scalar);                       \
    }

EACP_DEFINE_MATRIX_SCALE(Float2x2)
EACP_DEFINE_MATRIX_SCALE(Float3x3)
EACP_DEFINE_MATRIX_SCALE(Float4x4)

#undef EACP_DEFINE_MATRIX_SCALE
} // namespace eacp::GPU
