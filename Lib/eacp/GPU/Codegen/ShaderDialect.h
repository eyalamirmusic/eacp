#pragma once

#include "ShaderGraph.h"
#include "ShaderText.h"

#include <string>

// The spellings the three dialects disagree about, one function per question.
// MSL and HLSL spell most of an expression identically; GLSL differs in a
// countable list, each with one arm here.

namespace eacp::GPU::ShaderEmission
{
enum class Backend
{
    Metal,
    DirectX,
    Vulkan
};

const char* typeName(Backend backend, ValueType type);
std::string floatLiteral(float value);

// GLSL rejects a non-flat integer stage input outright, HLSL wants
// nointerpolation and MSL [[flat]]; a float varying takes none of the three.
constexpr bool isFlatVarying(ValueType type)
{
    return isSignedInteger(type) || isUnsignedInteger(type);
}

constexpr std::string attributeSemantic(Backend backend, int index)
{
    if (backend == Backend::Vulkan)
        return {};

    if (backend == Backend::Metal)
        return " [[attribute(" + formatDecimal(index) + ")]]";

    return " : TEXCOORD" + formatDecimal(index);
}

// Metal hangs the flat qualifier off the member rather than putting it in front
// of the type, so it rides here beside the semantic.
constexpr std::string varyingSemantic(Backend backend, int index, ValueType type)
{
    if (backend == Backend::Metal)
        return isFlatVarying(type) ? " [[flat]]" : std::string {};

    if (backend != Backend::DirectX)
        return {};

    return " : TEXCOORD" + formatDecimal(index);
}

constexpr std::string positionSemantic(Backend backend)
{
    if (backend == Backend::Vulkan)
        return {};

    if (backend == Backend::Metal)
        return " [[position]]";

    return " : SV_Position";
}

constexpr std::string flatQualifier(Backend backend, ValueType type)
{
    if (backend == Backend::Metal || !isFlatVarying(type))
        return {};

    return backend == Backend::Vulkan ? "flat " : "nointerpolation ";
}

// Attribute i and varying i take location i, which is what the Vulkan
// vertex-input state and the vertex/fragment interface match on.
constexpr std::string locationLayout(int index)
{
    return "layout(location = " + formatDecimal(index) + ") ";
}

// GLSL reads a stage's I/O out of globals, spelled attrN and varyN because aN
// and vN are taken there by a constant array and a mutable local.
constexpr std::string attributeName(Backend backend, int slot)
{
    if (backend == Backend::Vulkan)
        return "attr" + formatDecimal(slot);

    return "input.a" + formatDecimal(slot);
}

constexpr std::string varyingName(Backend backend, int index)
{
    if (backend == Backend::Vulkan)
        return "vary" + formatDecimal(index);

    return "input.v" + formatDecimal(index);
}

std::string callName(Backend backend, const std::string& name);
bool isGenTypeCall(const std::string& name);
const char* glslComparison(const std::string& op);

std::string hlslSamplerName(const TextureSampling& sampling);
// The component a 2D or 3D kernel's thread index node asked for, and the
// uniform its matching grid extent is declared under.
constexpr const char* componentSuffix(int component)
{
    if (component == 0)
        return ".x";

    return component == 1 ? ".y" : ".z";
}

const char* vectorComponentSuffix(int component);
const char* metalPackedVectorType(ValueType type);

// A thread index under the name both kernel scaffoldings bind it to: the whole
// value where the node took the position as one, one lane otherwise.
constexpr std::string
    indexReference(const char* name, DispatchRank rank, int component)
{
    if (rank == DispatchRank::OneD || component == allComponents)
        return name;

    return name + std::string(componentSuffix(component));
}

constexpr const char* gridExtentName(int component)
{
    if (component == 0)
        return "width";

    return component == 1 ? "height" : "depth";
}

constexpr int gridExtentCount(DispatchRank rank)
{
    if (rank == DispatchRank::OneD)
        return 1;

    return rank == DispatchRank::TwoD ? 2 : 3;
}

// The type the entry point declares its indices as, and the swizzle HLSL and
// GLSL take them out of their three-component builtins with.
constexpr const char* indexTypeName(DispatchRank rank)
{
    if (rank == DispatchRank::OneD)
        return "uint";

    return rank == DispatchRank::TwoD ? "uint2" : "uint3";
}

constexpr const char* glslIndexTypeName(DispatchRank rank)
{
    if (rank == DispatchRank::OneD)
        return "uint";

    return rank == DispatchRank::TwoD ? "uvec2" : "uvec3";
}

constexpr const char* indexSwizzle(DispatchRank rank)
{
    if (rank == DispatchRank::OneD)
        return ".x";

    return rank == DispatchRank::TwoD ? ".xy" : ".xyz";
}

// A whole expression parenthesised, since an offset or a stride is printed
// into the middle of an index computation.
constexpr std::string bracketed(const std::string& expression)
{
    return "(" + expression + ")";
}

} // namespace eacp::GPU::ShaderEmission
