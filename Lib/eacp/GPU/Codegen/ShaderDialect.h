#pragma once

#include "ShaderGraph.h"

#include <eacp/Core/Utils/Format.h>

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

inline std::string getAttributeSemantic(Backend backend, int index)
{
    if (backend == Backend::Vulkan)
        return {};

    if (backend == Backend::Metal)
        return Strings::format(" [[attribute({})]]", index);

    return Strings::format(" : TEXCOORD{}", index);
}

inline // Metal writes the flat qualifier after the member name, so on Metal it is part
    // of the semantic.
    std::string getVaryingSemantic(Backend backend, int index, ValueType type)
{
    if (backend == Backend::Metal)
        return isFlatVarying(type) ? " [[flat]]" : std::string {};

    if (backend != Backend::DirectX)
        return {};

    return Strings::format(" : TEXCOORD{}", index);
}

constexpr std::string_view getPositionSemantic(Backend backend)
{
    if (backend == Backend::Vulkan)
        return {};

    if (backend == Backend::Metal)
        return " [[position]]";

    return " : SV_Position";
}

constexpr std::string_view getFlatQualifier(Backend backend, ValueType type)
{
    if (backend == Backend::Metal || !isFlatVarying(type))
        return {};

    return backend == Backend::Vulkan ? "flat " : "nointerpolation ";
}

inline // Attribute i and varying i take location i, which is what the Vulkan
    // vertex-input state and the vertex/fragment interface match on.
    std::string getLocationLayout(int index)
{
    return Strings::format("layout(location = {}) ", index);
}

inline // GLSL reads a stage's I/O out of globals, spelled attrN and varyN because aN
    // and vN are taken there by a constant array and a mutable local.
    std::string getAttributeName(Backend backend, int slot)
{
    if (backend == Backend::Vulkan)
        return Strings::format("attr{}", slot);

    return Strings::format("input.a{}", slot);
}

inline std::string getVaryingName(Backend backend, int index)
{
    if (backend == Backend::Vulkan)
        return Strings::format("vary{}", index);

    return Strings::format("input.v{}", index);
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

std::string
    getIndexReference(std::string_view name, DispatchRank rank, int component);

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

std::string wrapInParentheses(const std::string& expression);

} // namespace eacp::GPU::ShaderEmission
