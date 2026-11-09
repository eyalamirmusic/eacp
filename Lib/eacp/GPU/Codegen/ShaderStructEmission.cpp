#include "ShaderStructEmission.h"

#include <eacp/Core/Utils/Format.h>

namespace eacp::GPU::ShaderEmission
{
namespace
{
constexpr auto structComponentNames = std::string_view {"xyzw"};

bool isPackedStructMember(const ShaderGraph& graph, int owner, ValueType type)
{
    return graph.structType(owner).hostShareable && !isMatrix(type)
           && componentCount(type) == 3;
}

std::string getStructMemberTypeName(const ShaderGraph& graph,
                                    Backend backend,
                                    const StructMember& member)
{
    if (member.type == ValueType::Struct)
        return getStructName(graph, member.structType);

    return std::string(getDialectTypeName(backend, member.type));
}

int getStructMemberBytes(const ShaderGraph& graph, const StructMember& member)
{
    if (member.type == ValueType::Struct)
        return graph.structType(member.structType).size;

    return storageSize(member.type);
}

std::string formatStructPadding(int bytes, int& padCount)
{
    return Strings::format("    uchar eacpPad{}[{}];\n", padCount++, bytes);
}

// MSL pads a shareable struct out to WGSL's offsets; every other declaration
// is the members as they are.
std::string formatStructBody(const ShaderGraph& graph, Backend backend, int structId)
{
    const auto& type = graph.structType(structId);
    auto laysOut = backend == Backend::Metal && type.hostShareable;
    auto source = Strings::format("struct {}\n{{\n", type.name);
    auto cursor = 0;
    auto pads = 0;

    for (const auto& member: type.members)
    {
        if (laysOut && member.offset > cursor)
            source += formatStructPadding(member.offset - cursor, pads);

        auto name = getStructMemberTypeName(graph, backend, member);

        if (laysOut && member.type != ValueType::Struct
            && isPackedStructMember(graph, structId, member.type))
            name = "packed_" + name;

        source += Strings::format("    {} {};\n", name, member.name);
        cursor = member.offset + getStructMemberBytes(graph, member);
    }

    if (laysOut && type.size > cursor)
        source += formatStructPadding(type.size - cursor, pads);

    return source + "};\n\n";
}

std::string
    formatStructConstructor(const ShaderGraph& graph, Backend backend, int structId)
{
    const auto& type = graph.structType(structId);
    auto parameters = std::string {};
    auto assignments = std::string {};

    for (auto which = 0; which < type.members.size(); ++which)
    {
        const auto& member = type.members[which];

        parameters +=
            Strings::format("{}{} m{}",
                            which > 0 ? ", " : "",
                            getStructMemberTypeName(graph, backend, member),
                            which);
        assignments += Strings::format("    value.{} = m{};\n", member.name, which);
    }

    return Strings::format("{0} eacpMake{0}({1})\n{{\n    {0} value{3};\n{2}    "
                           "return value;\n}}\n\n",
                           type.name,
                           parameters,
                           assignments,
                           backend == Backend::Metal ? " = {}" : "");
}

// Where one element of an HLSL struct buffer starts: the index times the
// stride. An offset into it is a constant from StructLayout.
struct HlslStructElement
{
    std::string buffer;
    std::string index;
    int stride = 0;
};

std::string printHlslStructAddress(const HlslStructElement& element, int offset)
{
    if (offset == 0)
        return Strings::format("({}) * {}u", element.index, element.stride);

    return Strings::format(
        "({}) * {}u + {}u", element.index, element.stride, offset);
}

// A half's word and the shift to its bits within it: fixed where every
// element starts on a word, computed where an element of halves alone starts
// on half of one.
struct HlslHalfPlace
{
    std::string word;
    std::string shift;
};

HlslHalfPlace getHlslHalfPlace(const HlslStructElement& element, int offset)
{
    if (element.stride % 4 == 0)
        return {printHlslStructAddress(element, offset & ~3),
                Strings::format("{}u", (offset & 2) * 8)};

    auto address = printHlslStructAddress(element, offset);
    return {Strings::format("({}) & ~3u", address),
            Strings::format("((({}) & 2u) << 3u)", address)};
}

std::string printHlslHalfLoad(const HlslStructElement& element, int offset)
{
    auto place = getHlslHalfPlace(element, offset);
    auto load = Strings::format("{}.Load({})", element.buffer, place.word);

    if (place.shift == "0u")
        return Strings::format("f16tof32({})", load);

    return Strings::format("f16tof32({} >> {})", load, place.shift);
}

std::string printHlslWordsLoad(const HlslStructElement& element,
                               int offset,
                               int width,
                               ValueType type)
{
    auto load =
        Strings::format("{}.Load{}({})",
                        element.buffer,
                        width > 1 ? Strings::format("{}", width) : std::string {},
                        printHlslStructAddress(element, offset));

    if (isUnsignedInteger(type))
        return load;

    return Strings::format(
        "{}({})", isSignedInteger(type) ? "asint" : "asfloat", load);
}

std::string
    printHlslHalfVectorLoad(const HlslStructElement& element, int offset, int width)
{
    if (width == 1)
        return printHlslHalfLoad(element, offset);

    auto components = std::string {};

    for (auto component = 0; component < width; ++component)
        components +=
            Strings::format("{}{}",
                            component > 0 ? ", " : "",
                            printHlslHalfLoad(element, offset + 2 * component));

    return Strings::format("float{}({})", width, components);
}

// A matrix is built from its columns as the transpose of the rows-first
// shape, as every HLSL matrix value here is.
std::string
    printHlslValueLoad(const HlslStructElement& element, int offset, ValueType type)
{
    if (!isMatrix(type))
    {
        if (isHalf(type))
            return printHlslHalfVectorLoad(element, offset, componentCount(type));

        return printHlslWordsLoad(element, offset, componentCount(type), type);
    }

    auto rows = matrixRows(type);
    auto columns = std::string {};

    for (auto column = 0; column < matrixColumns(type); ++column)
    {
        auto at = offset + column * storageColumnStride(type);
        auto loaded = isHalf(type) ? printHlslHalfVectorLoad(element, at, rows)
                                   : printHlslWordsLoad(element, at, rows, type);
        columns += Strings::format("{}{}", column > 0 ? ", " : "", loaded);
    }

    return Strings::format(
        "transpose({}({}))", typeName(floatEquivalent(type)), columns);
}

std::string
    printHlslComponent(ValueType type, const std::string& value, int component)
{
    if (componentCount(type) == 1)
        return value;

    if (!isMatrix(type))
        return Strings::format("({}).{}", value, structComponentNames[component]);

    auto rows = matrixRows(type);
    return Strings::format("transpose({})[{}].{}",
                           value,
                           component / rows,
                           structComponentNames[component % rows]);
}

std::string printHlslHalfStore(const HlslStructElement& element,
                               int offset,
                               const std::string& value,
                               const std::string& indent)
{
    auto place = getHlslHalfPlace(element, offset);

    return Strings::format("{0}{1}.InterlockedAnd({2}, ~(0xffffu << {3}));\n"
                           "{0}{1}.InterlockedOr({2}, f32tof16({4}) << {3});\n",
                           indent,
                           element.buffer,
                           place.word,
                           place.shift,
                           value);
}

std::string printHlslWordStore(const HlslStructElement& element,
                               int offset,
                               const std::string& value,
                               const std::string& indent)
{
    return Strings::format("{}{}.Store({}, asuint({}));\n",
                           indent,
                           element.buffer,
                           printHlslStructAddress(element, offset),
                           value);
}

// A word at a time: FXC drops some of the words a Store2 or Store3 wrote when a
// later store in the thread lands on one of them.
std::string printHlslValueStore(const HlslStructElement& element,
                                int offset,
                                ValueType type,
                                const std::string& value,
                                const std::string& indent)
{
    auto source = std::string {};

    for (auto component = 0; component < componentCount(type); ++component)
    {
        auto at = offset + storageComponentOffset(type, component);
        auto part = printHlslComponent(type, value, component);

        source += isHalf(type) ? printHlslHalfStore(element, at, part, indent)
                               : printHlslWordStore(element, at, part, indent);
    }

    return source;
}

HlslStructElement getHlslStructElement(const ShaderGraph& graph,
                                       int slot,
                                       const std::string& index)
{
    return {Strings::format("buffer{}", slot),
            index,
            graph.structType(graph.storageStructType(slot)).size};
}
} // namespace

const std::string& getStructName(const ShaderGraph& graph, int structId)
{
    return graph.structType(structId).name;
}

std::string getStructDeclarations(const ShaderGraph& graph, Backend backend)
{
    auto source = std::string {};

    for (auto structId = 0; structId < graph.structCount(); ++structId)
    {
        source += formatStructBody(graph, backend, structId);

        if (backend != Backend::Vulkan)
            source += formatStructConstructor(graph, backend, structId);
    }

    return source;
}

std::string printStructConstruct(const ShaderGraph& graph,
                                 Backend backend,
                                 int structId,
                                 const std::string& members)
{
    const auto& name = getStructName(graph, structId);

    if (backend == Backend::Vulkan)
        return Strings::format("{}({})", name, members);

    return Strings::format("eacpMake{}({})", name, members);
}

std::string printFieldRead(const ShaderGraph& graph,
                           Backend backend,
                           int structId,
                           int field,
                           const std::string& base)
{
    const auto& target = graph.structType(structId).fields[field];
    auto access = Strings::format("{}.{}", base, target.path);

    if (backend == Backend::Metal
        && isPackedStructMember(graph, target.owner, target.type))
        return Strings::format("{}({})", typeName(target.type), access);

    return access;
}

std::string printFieldTarget(const ShaderGraph& graph,
                             int structId,
                             int field,
                             const std::string& base)
{
    if (field < 0)
        return base;

    return Strings::format(
        "{}.{}", base, graph.structType(structId).fields[field].path);
}

std::string
    getMetalStructBufferType(const ShaderGraph& graph, BufferAccess access, int slot)
{
    const auto& name = getStructName(graph, graph.storageStructType(slot));

    if (access == BufferAccess::Read)
        return Strings::format("device const {}*", name);

    return Strings::format("device {}*", name);
}

std::string_view getHlslStructBufferType(BufferAccess access)
{
    return access == BufferAccess::Read ? "ByteAddressBuffer"
                                        : "RWByteAddressBuffer";
}

std::string printHlslStructLoad(const ShaderGraph& graph,
                                int slot,
                                const std::string& index,
                                int field)
{
    const auto& target =
        graph.structType(graph.storageStructType(slot)).fields[field];

    return printHlslValueLoad(
        getHlslStructElement(graph, slot, index), target.offset, target.type);
}

// A nested struct or the whole element is stored a leaf at a time, each from
// its own path under the value.
std::string printHlslStructStore(const ShaderGraph& graph,
                                 int slot,
                                 const std::string& index,
                                 int field,
                                 const std::string& value,
                                 const std::string& indent)
{
    const auto& type = graph.structType(graph.storageStructType(slot));
    auto element = getHlslStructElement(graph, slot, index);

    if (field >= 0 && type.fields[field].type != ValueType::Struct)
        return printHlslValueStore(element,
                                   type.fields[field].offset,
                                   type.fields[field].type,
                                   value,
                                   indent);

    auto prefix = field >= 0 ? type.fields[field].path + "." : std::string {};
    auto first = field >= 0 ? type.fields[field].firstComponent : 0;
    auto end = field >= 0 ? first + type.fields[field].componentCount
                          : type.components.size();
    auto source = std::string {};

    for (const auto& leaf: type.fields)
    {
        if (leaf.type == ValueType::Struct || leaf.firstComponent < first
            || leaf.firstComponent >= end)
            continue;

        source += printHlslValueStore(
            element,
            leaf.offset,
            leaf.type,
            Strings::format("{}.{}", value, leaf.path.substr(prefix.size())),
            indent);
    }

    return source;
}
} // namespace eacp::GPU::ShaderEmission
