#pragma once

#include "MemberList.h"
#include "ShaderValue.h"

#include <cassert>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>

// WGSL's structs. A C++ struct whose members are value handles - scalars,
// vectors and matrices of every family, and other such structs - becomes a
// struct type of the shader by listing its members once:
//
//     struct Particle
//     {
//         Float3 position;
//         Float mass;
//         Half2 uv;
//         UInt flags;
//
//         EACP_SHADER_STRUCT(Particle, position, mass, uv, flags)
//     };
//
// The struct is still the authoring-time value it always was - an aggregate of
// handles, built with Particle {p, m, uv, f} - and is emitted as a named type
// in each dialect the first time a shader needs one: a variable, a private
// array, or a buffer of them. Its members are read as fields and written as
// fields, and a whole value is built from its members where it is stored.
//
// A struct in a buffer has WGSL's storage layout, which StructLayout spells
// out at compile time: each member at the next offset its alignment allows, a
// three-vector aligned to sixteen bytes (eight for f16) with a size of twelve
// (six), a matrix as its columns at that alignment, a struct aligned to its
// widest member and sized to a multiple of that. A Bool has no layout, so a
// struct holding one is a value only and never a buffer's element.

namespace eacp::GPU
{
// A struct with EACP_SHADER_STRUCT's member list.
template <typename T>
concept ShaderStructLike = requires {
    { T::shaderStructName } -> std::convertible_to<std::string_view>;
};

// A member that is a value handle rather than a struct.
template <typename T>
concept ShaderValueMember = requires { ValueTypeOf<T>::value; };

// One member as WGSL lays it out in storage.
struct StructMemberLayout
{
    std::string_view name;
    ValueType type = ValueType::Float;
    int offset = 0;
    int size = 0;
    int alignment = 1;
};

template <ShaderStructLike T>
struct StructLayout;

namespace detail
{
template <typename Pointer>
struct MemberPointerTraits;

template <typename Owner, typename Member>
struct MemberPointerTraits<Member Owner::*>
{
    using OwnerType = Owner;
    using MemberType = Member;
};

template <auto Pointer>
using MemberTypeOf = typename MemberPointerTraits<decltype(Pointer)>::MemberType;

template <typename M>
constexpr ValueType structMemberType()
{
    if constexpr (ShaderStructLike<M>)
        return ValueType::Struct;
    else
        return ValueTypeOf<M>::value;
}

template <typename M>
constexpr int structMemberAlignment()
{
    if constexpr (ShaderStructLike<M>)
        return StructLayout<M>::alignment;
    else
        return storageAlignment(ValueTypeOf<M>::value);
}

template <typename M>
constexpr int structMemberSize()
{
    if constexpr (ShaderStructLike<M>)
        return StructLayout<M>::size;
    else
        return storageSize(ValueTypeOf<M>::value);
}

template <typename M>
constexpr bool isStructMemberShareable()
{
    if constexpr (ShaderStructLike<M>)
        return StructLayout<M>::hostShareable;
    else
        return !isBoolean(ValueTypeOf<M>::value);
}

template <typename T>
struct StructMemberCounter
{
    template <typename M>
    constexpr void operator()(const char*, M T::*)
    {
        static_assert(ShaderStructLike<M> || ShaderValueMember<M>,
                      "eacp: a shader struct member is a value handle (a "
                      "scalar, vector or matrix) or another shader struct");
        ++count;
    }

    int count = 0;
};

template <typename T>
constexpr int structMemberCount()
{
    auto counter = StructMemberCounter<T> {};
    T::visitShaderStructMembers(counter);
    return counter.count;
}

template <typename T, int Count>
struct StructLayoutBuilder
{
    template <typename M>
    constexpr void operator()(const char* name, M T::*)
    {
        auto alignment = structMemberAlignment<M>();
        auto offset = storageRoundUp(cursor, alignment);
        auto size = structMemberSize<M>();

        members[index++] = StructMemberLayout {
            name, structMemberType<M>(), offset, size, alignment};

        cursor = offset + size;
        structAlignment = alignment > structAlignment ? alignment : structAlignment;
        shareable = shareable && isStructMemberShareable<M>();
    }

    Array<StructMemberLayout, Count> members {};
    int index = 0;
    int cursor = 0;
    int structAlignment = 1;
    bool shareable = true;
};

template <typename T>
constexpr auto buildStructLayout()
{
    auto builder = StructLayoutBuilder<T, structMemberCount<T>()> {};
    T::visitShaderStructMembers(builder);
    return builder;
}

template <typename T, auto Member>
struct StructMemberFinder
{
    template <typename M>
    constexpr void operator()(const char*, M T::* pointer)
    {
        if constexpr (std::is_same_v<M T::*, decltype(Member)>)
            if (pointer == Member)
                found = index;

        ++index;
    }

    int index = 0;
    int found = -1;
};

template <typename T>
struct StructFieldCounter
{
    template <typename M>
    constexpr void operator()(const char*, M T::*);

    int count = 0;
};

template <typename T>
constexpr int structFieldCount()
{
    auto counter = StructFieldCounter<T> {};
    T::visitShaderStructMembers(counter);
    return counter.count;
}

template <typename T>
template <typename M>
constexpr void StructFieldCounter<T>::operator()(const char*, M T::*)
{
    ++count;

    if constexpr (ShaderStructLike<M>)
        count += structFieldCount<M>();
}
} // namespace detail

// WGSL's storage layout of a shader struct, at compile time: each member's
// offset, size and alignment, the struct's alignment and size, and the stride
// of an array of it - which is its size, already a multiple of its alignment.
// What host code fills a buffer of them by (StructBufferData does), and what
// every dialect's declaration is made to agree with.
template <ShaderStructLike T>
struct StructLayout
{
    static constexpr int memberCount = detail::structMemberCount<T>();
    static_assert(memberCount > 0, "eacp: a shader struct has at least one member");

    static constexpr auto built = detail::buildStructLayout<T>();
    static constexpr Array<StructMemberLayout, memberCount> members = built.members;
    static constexpr int alignment = built.structAlignment;
    static constexpr int size = storageRoundUp(built.cursor, alignment);
    static constexpr int stride = size;
    static constexpr bool hostShareable = built.shareable;

    // Every member at every depth, a nested struct counted beside its own.
    static constexpr int fieldCount = detail::structFieldCount<T>();

    template <auto Member>
    static constexpr int indexOf()
    {
        auto finder = detail::StructMemberFinder<T, Member> {};
        T::visitShaderStructMembers(finder);
        return finder.found;
    }

    template <auto Member>
    static constexpr int offsetOf()
    {
        constexpr auto index = indexOf<Member>();
        static_assert(index >= 0, "eacp: that member is not in the struct's list");
        return members[index].offset;
    }
};

namespace detail
{
template <ShaderStructLike T>
int declareStruct(ShaderGraph& graph);

template <typename T>
struct StructDeclarer
{
    template <typename M>
    void operator()(const char* name, M T::*)
    {
        auto member = StructMember {};
        member.name = name;
        member.type = structMemberType<M>();
        member.offset = StructLayout<T>::members[index++].offset;

        if constexpr (ShaderStructLike<M>)
            member.structType = declareStruct<M>(graph);

        type.members.add(member);
    }

    ShaderGraph& graph;
    StructType& type;
    int index = 0;
};

// Declares T in the graph, its nested structs first, and returns its id; a
// second declaration is the first.
template <ShaderStructLike T>
int declareStruct(ShaderGraph& graph)
{
    auto type = StructType {};
    type.name = std::string(T::shaderStructName);
    type.size = StructLayout<T>::size;
    type.alignment = StructLayout<T>::alignment;
    type.hostShareable = StructLayout<T>::hostShareable;

    auto declarer = StructDeclarer<T> {graph, type};
    T::visitShaderStructMembers(declarer);
    return graph.addStruct(type);
}

// Gives every value handle in `value` the node makeLeaf makes for its field,
// fields numbered as ShaderGraph numbers them: in member order, a nested
// struct's own fields right behind it.
template <ShaderStructLike T, typename MakeLeaf>
void bindStructLeaves(T& value, ShaderGraph& graph, int& field, MakeLeaf& makeLeaf)
{
    auto bindMember = [&](const char*, auto pointer)
    {
        auto& member = value.*pointer;
        using M = std::remove_cvref_t<decltype(member)>;
        auto index = field++;

        if constexpr (ShaderStructLike<M>)
        {
            bindStructLeaves(member, graph, field, makeLeaf);
        }
        else
        {
            member.graph = &graph;
            member.node = makeLeaf(index);
        }
    };

    T::visitShaderStructMembers(bindMember);
}

template <ShaderStructLike T, typename MakeLeaf>
T makeStructValue(ShaderGraph& graph, MakeLeaf& makeLeaf)
{
    auto value = T {};
    auto field = 0;
    bindStructLeaves(value, graph, field, makeLeaf);
    return value;
}

// The Construct node of a struct value, built from its members' nodes - a
// nested struct's its own Construct. Every member is a value of this graph.
template <ShaderStructLike T>
int structValueNode(ShaderGraph& graph, int structId, const T& value)
{
    auto members = Vector<int> {};
    auto which = 0;

    auto addMember = [&](const char*, auto pointer)
    {
        const auto& member = value.*pointer;
        using M = std::remove_cvref_t<decltype(member)>;

        if constexpr (ShaderStructLike<M>)
        {
            auto nested = graph.structType(structId).members[which].structType;
            members.add(structValueNode(graph, nested, member));
        }
        else
        {
            assert(member.graph == &graph && member.node >= 0
                   && "eacp: every member of a struct value is a value of this "
                      "shader - one left unset has nothing to store");
            members.add(member.node);
        }

        ++which;
    };

    T::visitShaderStructMembers(addMember);
    return graph.addStructConstruct(structId, members);
}

// The field a top-level member pointer names.
template <ShaderStructLike T, typename M>
int structFieldOf(M T::* target)
{
    auto field = 0;
    auto found = -1;

    auto visit = [&](const char*, auto pointer)
    {
        using P = decltype(pointer);
        using N = typename MemberPointerTraits<P>::MemberType;

        if constexpr (std::is_same_v<P, M T::*>)
            if (pointer == target)
                found = field;

        ++field;

        if constexpr (ShaderStructLike<N>)
            field += structFieldCount<N>();
    };

    T::visitShaderStructMembers(visit);
    assert(found >= 0 && "eacp: that member is not in the struct's list");
    return found;
}

// What a member's field takes: the handle's node, or a nested struct's
// Construct.
template <ShaderStructLike T, typename M>
int structMemberNode(ShaderGraph& graph, int structId, int field, const M& value)
{
    if constexpr (ShaderStructLike<M>)
        return structValueNode(
            graph, graph.structType(structId).fields[field].structType, value);
    else
        return value.node;
}

template <ShaderStructLike T>
T structZero(ShaderGraph& graph)
{
    auto structId = declareStruct<T>(graph);
    auto makeZero = [&](int field)
    { return graph.addZero(graph.structType(structId).fields[field].type); };

    return makeStructValue<T>(graph, makeZero);
}

struct StructBufferHandle
{
    ShaderGraph* graph = nullptr;
    int slot = -1;
};
} // namespace detail

// A runtime-sized storage buffer of structs, WGSL's array<T> of a struct T:
// element i is read whole - each field a read of its own, so a kernel that
// touches one field loads that field alone - and is readable from every stage,
// as InputBuffer is. Bound like any buffer, from bytes laid out as
// StructLayout<T> says (StructBufferData fills them); the CPU executor's
// Bindings::set takes them as bytes or as a span of host records of the
// stride's size.
//
// Each dialect holds it its own way, all at StructLayout's offsets: MSL a
// `device const T*` over a struct with packed three-vectors and padding
// members, GLSL an std430 block of T, whose rules are WGSL's, and HLSL a
// ByteAddressBuffer loaded at the computed offsets - D3D's StructuredBuffer
// packs a three-vector tightly and has no 16-bit member below SM 6.2, so the
// struct there is a value type and the buffer its bytes.
template <ShaderStructLike T>
struct StructInputBuffer : detail::StructBufferHandle
{
    using Element = T;

    T operator[](const UInt& index) const { return element(index.node); }
    T operator[](unsigned index) const
    {
        return element(graph->addUIntConstant(index));
    }

private:
    T element(int indexNode) const
    {
        auto makeLeaf = [&](int field)
        { return graph->addBufferMemberRead(slot, indexNode, field); };

        return detail::makeStructValue<T>(*graph, makeLeaf);
    }
};

// Its read-write sibling, compute-only: read as the input is - what this
// thread stored there earlier, or what the buffer was bound holding - and
// written whole or a field at a time with ShaderBuilder::write.
template <ShaderStructLike T>
struct StructOutputBuffer : detail::StructBufferHandle
{
    using Element = T;

    T operator[](const UInt& index) const { return element(index.node); }
    T operator[](unsigned index) const
    {
        return element(graph->addUIntConstant(index));
    }

    // An index anchored on this buffer's own graph.
    UInt index(unsigned value) const
    {
        auto result = UInt {};
        result.graph = graph;
        result.node = graph->addUIntConstant(value);
        return result;
    }

private:
    T element(int indexNode) const
    {
        auto makeLeaf = [&](int field)
        { return graph->addBufferMemberRead(slot, indexNode, field); };

        return detail::makeStructValue<T>(*graph, makeLeaf);
    }
};

// A struct variable, WGSL's var s: T. get() reads every field as the
// statements before it left them; assigning a value writes the whole struct,
// and set() one member - a nested struct member whole. Non-copyable, as a Var
// of a value is.
template <ShaderStructLike T>
struct Var<T>
{
    Var(ShaderGraph& graphToUse, int structIdToUse, int initialValue)
        : graph(&graphToUse)
        , structId(structIdToUse)
        , slot(graphToUse.addStructVariable(structIdToUse, initialValue))
    {
    }

    Var(const Var&) = delete;
    Var(Var&&) = delete;

    T get() const
    {
        auto makeLeaf = [&](int field)
        { return graph->addVarMemberRead(slot, field); };
        return detail::makeStructValue<T>(*graph, makeLeaf);
    }

    operator T() const { return get(); }
    T operator()() const { return get(); }

    Var& operator=(const T& value)
    {
        graph->assign(slot, detail::structValueNode(*graph, structId, value));
        return *this;
    }

    Var& operator=(const Var& other) { return *this = other.get(); }

    template <typename M>
    void set(M T::* member, const M& value) const
    {
        auto field = detail::structFieldOf(member);
        graph->assignMember(
            slot,
            field,
            detail::structMemberNode<T>(*graph, structId, field, value));
    }

    ShaderGraph* graph = nullptr;
    int structId = -1;
    int slot = -1;
};

// A private array of structs, WGSL's var a: array<T, N>: an element is read
// whole, written whole with set(i, value) or a member at a time with set(i,
// &T::member, value). The index is clamped once per access, as a private
// array's always is.
template <ShaderStructLike T, int Size>
struct PrivateArray<T, Size>
{
    static_assert(Size > 0, "eacp: a private array has at least one element");

    PrivateArray(ShaderGraph& graphToUse, int structIdToUse, int slotToUse)
        : graph(&graphToUse)
        , structId(structIdToUse)
        , slot(slotToUse)
    {
    }

    PrivateArray(const PrivateArray&) = delete;
    PrivateArray(PrivateArray&&) = delete;

    template <PrivateArrayIndex Index>
    T operator[](const Index& index) const
    {
        auto at = graph->clampVariableIndex(slot, indexNode(index));
        auto makeLeaf = [&](int field)
        { return graph->addVarElementMemberRead(slot, at, field); };

        return detail::makeStructValue<T>(*graph, makeLeaf);
    }

    template <PrivateArrayIndex Index>
    void set(const Index& index, const T& value) const
    {
        graph->assignElement(slot,
                             indexNode(index),
                             detail::structValueNode(*graph, structId, value));
    }

    template <PrivateArrayIndex Index, typename M>
    void set(const Index& index, M T::* member, const M& value) const
    {
        auto field = detail::structFieldOf(member);
        graph->assignElementMember(
            slot,
            graph->clampVariableIndex(slot, indexNode(index)),
            field,
            detail::structMemberNode<T>(*graph, structId, field, value));
    }

    static constexpr int size() { return Size; }

    ShaderGraph* graph = nullptr;
    int structId = -1;
    int slot = -1;

private:
    int indexNode(int index) const
    {
        assert(index >= 0 && index < Size
               && "eacp: private-array index out of range");
        return graph->addIntConstant(index);
    }

    int indexNode(unsigned index) const
    {
        assert(index < (unsigned) Size && "eacp: private-array index out of range");
        return graph->addIntConstant((int) index);
    }

    template <ShaderHandleLike Index>
    int indexNode(const Index& index) const
    {
        return index.node;
    }
};

namespace detail
{
// What a member is on the host: a float per component of the two float
// families - an f16 widened, and narrowed again where it is written - and an
// int32 or uint32 per component of the integer ones, a matrix its columns one
// after another.
template <typename M>
struct StructHostValue
{
    static constexpr auto type = ValueTypeOf<M>::value;
    using Scalar = std::conditional_t<
        isUnsignedInteger(type),
        std::uint32_t,
        std::conditional_t<isSignedInteger(type), std::int32_t, float>>;
    using Value = std::conditional_t<componentCount(type) == 1,
                                     Scalar,
                                     Array<Scalar, componentCount(type)>>;
};

// The components of a value of `type`, 32 bits each on the host, written to
// and read from its storage at `at`.
void writeStorageValue(std::byte* at, ValueType type, const void* components);
void readStorageValue(const std::byte* at, ValueType type, void* components);
} // namespace detail

template <typename M>
using StructHostValue = typename detail::StructHostValue<M>::Value;

// The host's side of a buffer of structs: elements * StructLayout<T>::stride
// zeroed bytes, each member set and read at the offset StructLayout gives it.
//
//     auto particles = StructBufferData<Particle> {64};
//     particles.set<&Particle::mass>(3, 2.f);
//     particles.set<&Particle::position>(3, {1.f, 2.f, 3.f});
//
// A member of a nested struct takes both pointers: set<&Outer::inner,
// &Inner::x>(i, value).
template <ShaderStructLike T>
class StructBufferData
{
public:
    static_assert(StructLayout<T>::hostShareable,
                  "eacp: a struct holding a Bool has no storage layout");

    static constexpr int stride = StructLayout<T>::stride;

    explicit StructBufferData(int elementsToHold)
        : elements(elementsToHold)
    {
        held.resize(elementsToHold * stride, std::byte {0});
    }

    int size() const { return elements; }

    template <auto Member>
    void set(int element, const StructHostValue<detail::MemberTypeOf<Member>>& value)
    {
        write(element,
              StructLayout<T>::template offsetOf<Member>(),
              ValueTypeOf<detail::MemberTypeOf<Member>>::value,
              value);
    }

    template <auto Outer, auto Inner>
    void set(int element, const StructHostValue<detail::MemberTypeOf<Inner>>& value)
    {
        write(element,
              nestedOffset<Outer, Inner>(),
              ValueTypeOf<detail::MemberTypeOf<Inner>>::value,
              value);
    }

    template <auto Member>
    StructHostValue<detail::MemberTypeOf<Member>> get(int element) const
    {
        return read<StructHostValue<detail::MemberTypeOf<Member>>>(
            element,
            StructLayout<T>::template offsetOf<Member>(),
            ValueTypeOf<detail::MemberTypeOf<Member>>::value);
    }

    template <auto Outer, auto Inner>
    StructHostValue<detail::MemberTypeOf<Inner>> get(int element) const
    {
        return read<StructHostValue<detail::MemberTypeOf<Inner>>>(
            element,
            nestedOffset<Outer, Inner>(),
            ValueTypeOf<detail::MemberTypeOf<Inner>>::value);
    }

    std::span<std::byte> bytes() { return {held.data(), (std::size_t) held.size()}; }

    std::span<const std::byte> bytes() const
    {
        return {held.data(), (std::size_t) held.size()};
    }

private:
    template <auto Outer, auto Inner>
    static constexpr int nestedOffset()
    {
        using Nested = detail::MemberTypeOf<Outer>;
        return StructLayout<T>::template offsetOf<Outer>()
               + StructLayout<Nested>::template offsetOf<Inner>();
    }

    template <typename Value>
    void write(int element, int offset, ValueType type, const Value& value)
    {
        assert(element >= 0 && element < elements && "eacp: no such element");
        detail::writeStorageValue(
            held.data() + element * stride + offset, type, &value);
    }

    template <typename Value>
    Value read(int element, int offset, ValueType type) const
    {
        assert(element >= 0 && element < elements && "eacp: no such element");
        auto value = Value {};
        detail::readStorageValue(
            held.data() + element * stride + offset, type, &value);
        return value;
    }

    int elements = 0;
    Vector<std::byte> held;
};
} // namespace eacp::GPU

// Lists a shader struct's members, in the order they are laid out, after the
// struct's own name. Place it inside the struct, after the members.
#define EACP_SHADER_STRUCT(Type, ...)                                               \
    using ShaderStructSelf = Type;                                                  \
    static constexpr std::string_view shaderStructName = #Type;                     \
    template <typename Visitor>                                                     \
    static constexpr void visitShaderStructMembers(Visitor& visitor)                \
    {                                                                               \
        EACP_GPU_MEMBERS(visitor, __VA_ARGS__)                                      \
    }
