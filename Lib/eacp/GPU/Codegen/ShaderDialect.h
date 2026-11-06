#pragma once

#include "ShaderGraph.h"

#include <eacp/Core/Utils/Format.h>

#include <string>
#include <string_view>

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
    if (backend == Backend::Vulkan)
        return glslTypeName(type);

    return backend == Backend::DirectX ? hlslTypeName(type) : typeName(type);
}
std::string formatShaderFloatLiteral(float value);

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

// Metal writes the flat qualifier after the member name, as part of the semantic.
inline std::string getVaryingSemantic(Backend backend, int index, ValueType type)
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

inline std::string getLocationLayout(int index)
{
    return Strings::format("layout(location = {}) ", index);
}

// GLSL names stage I/O attrN and varyN: aN and vN are taken by other locals.
inline std::string getAttributeName(Backend backend, int slot)
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
bool isMetalGenTypeCall(const std::string& name);
std::string_view getGlslComparisonFunction(const std::string& op);

std::string getHlslSamplerName(const TextureSampling& sampling);
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

constexpr std::string_view getIndexSwizzle(DispatchRank rank)
{
    if (rank == DispatchRank::OneD)
        return ".x";

    return rank == DispatchRank::TwoD ? ".xy" : ".xyz";
}

std::string wrapInParentheses(const std::string& expression);
} // namespace eacp::GPU::ShaderEmission
