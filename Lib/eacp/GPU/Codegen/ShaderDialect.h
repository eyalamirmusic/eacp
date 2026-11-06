#pragma once

#include "ShaderGraph.h"
#include "ShaderText.h"

#include <string>
#include <string_view>

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

constexpr std::string_view getDialectTypeName(Backend backend, ValueType type)
{
    return backend == Backend::Vulkan ? glslTypeName(type) : typeName(type);
}
std::string formatShaderFloatLiteral(float value);

// GLSL rejects a non-flat integer stage input outright, HLSL wants
// nointerpolation and MSL [[flat]]; a float varying takes none of the three.
constexpr bool isFlatVarying(ValueType type)
{
    return isSignedInteger(type) || isUnsignedInteger(type);
}

constexpr std::string getAttributeSemantic(Backend backend, int index)
{
    if (backend == Backend::Vulkan)
        return {};

    if (backend == Backend::Metal)
        return " [[attribute(" + formatDecimal(index) + ")]]";

    return " : TEXCOORD" + formatDecimal(index);
}

// Metal hangs the flat qualifier off the member rather than putting it in front
// of the type, so it rides here beside the semantic.
constexpr std::string getVaryingSemantic(Backend backend, int index, ValueType type)
{
    if (backend == Backend::Metal)
        return isFlatVarying(type) ? " [[flat]]" : std::string {};

    if (backend != Backend::DirectX)
        return {};

    return " : TEXCOORD" + formatDecimal(index);
}

constexpr std::string getPositionSemantic(Backend backend)
{
    if (backend == Backend::Vulkan)
        return {};

    if (backend == Backend::Metal)
        return " [[position]]";

    return " : SV_Position";
}

constexpr std::string getFlatQualifier(Backend backend, ValueType type)
{
    if (backend == Backend::Metal || !isFlatVarying(type))
        return {};

    return backend == Backend::Vulkan ? "flat " : "nointerpolation ";
}

// Attribute i and varying i take location i, which is what the Vulkan
// vertex-input state and the vertex/fragment interface match on.
constexpr std::string getLocationLayout(int index)
{
    return "layout(location = " + formatDecimal(index) + ") ";
}

// GLSL reads a stage's I/O out of globals, spelled attrN and varyN because aN
// and vN are taken there by a constant array and a mutable local.
constexpr std::string getAttributeName(Backend backend, int slot)
{
    if (backend == Backend::Vulkan)
        return "attr" + formatDecimal(slot);

    return "input.a" + formatDecimal(slot);
}

constexpr std::string getVaryingName(Backend backend, int index)
{
    if (backend == Backend::Vulkan)
        return "vary" + formatDecimal(index);

    return "input.v" + formatDecimal(index);
}

std::string getCallName(Backend backend, const std::string& name);
bool isGenTypeCall(const std::string& name);
std::string_view getGlslComparisonFunction(const std::string& op);

std::string getHlslSamplerName(const TextureSampling& sampling);
// The component a 2D or 3D kernel's thread index node asked for, and the
// uniform its matching grid extent is declared under.
constexpr std::string_view getComponentSuffix(int component)
{
    if (component == 0)
        return ".x";

    return component == 1 ? ".y" : ".z";
}

std::string_view getVectorComponentSuffix(int component);
std::string_view getMetalPackedVectorType(ValueType type);

// A thread index under the name both kernel scaffoldings bind it to: the whole
// value where the node took the position as one, one lane otherwise.
constexpr std::string
    getIndexReference(std::string_view name, DispatchRank rank, int component)
{
    if (rank == DispatchRank::OneD || component == allComponents)
        return std::string(name);

    return std::string(name) + std::string(getComponentSuffix(component));
}

constexpr std::string_view getGridExtentName(int component)
{
    if (component == 0)
        return "width";

    return component == 1 ? "height" : "depth";
}

constexpr int getGridExtentCount(DispatchRank rank)
{
    if (rank == DispatchRank::OneD)
        return 1;

    return rank == DispatchRank::TwoD ? 2 : 3;
}

// The type the entry point declares its indices as, and the swizzle HLSL and
// GLSL take them out of their three-component builtins with.
constexpr std::string_view getIndexTypeName(DispatchRank rank)
{
    if (rank == DispatchRank::OneD)
        return "uint";

    return rank == DispatchRank::TwoD ? "uint2" : "uint3";
}

constexpr std::string_view getGlslIndexTypeName(DispatchRank rank)
{
    if (rank == DispatchRank::OneD)
        return "uint";

    return rank == DispatchRank::TwoD ? "uvec2" : "uvec3";
}

constexpr std::string_view getIndexSwizzle(DispatchRank rank)
{
    if (rank == DispatchRank::OneD)
        return ".x";

    return rank == DispatchRank::TwoD ? ".xy" : ".xyz";
}

// A whole expression parenthesised, since an offset or a stride is printed
// into the middle of an index computation.
constexpr std::string wrapInParentheses(const std::string& expression)
{
    return "(" + expression + ")";
}

} // namespace eacp::GPU::ShaderEmission
