#include "PipelineConstants.h"

#include "../Codegen/PackedScalars.h"
#include "../Codegen/ShaderDialect.h"

#include <eacp/Core/Utils/Format.h>

#include <bit>
#include <climits>

namespace eacp::GPU
{
namespace
{
PipelineConstant makeConstant(int id, PipelineConstantType type, std::uint32_t bits)
{
    auto constant = PipelineConstant {};
    constant.id = id;
    constant.type = type;
    constant.bits = bits;
    return constant;
}

std::string hlslIntLiteral(std::int32_t value)
{
    if (value == INT_MIN)
        return "(-2147483647 - 1)";

    return Strings::format("{}", value);
}
} // namespace

PipelineConstant PipelineConstant::ofBool(int id, bool value)
{
    return makeConstant(id, PipelineConstantType::Bool, value ? 1u : 0u);
}

PipelineConstant PipelineConstant::ofInt(int id, std::int32_t value)
{
    return makeConstant(
        id, PipelineConstantType::Int, std::bit_cast<std::uint32_t>(value));
}

PipelineConstant PipelineConstant::ofUInt(int id, std::uint32_t value)
{
    return makeConstant(id, PipelineConstantType::UInt, value);
}

PipelineConstant PipelineConstant::ofFloat(int id, float value)
{
    return makeConstant(
        id, PipelineConstantType::Float, std::bit_cast<std::uint32_t>(value));
}

PipelineConstant PipelineConstant::ofHalf(int id, float value)
{
    const auto rounded = halfToFloat(halfFromFloat(value));
    return makeConstant(
        id, PipelineConstantType::Half, std::bit_cast<std::uint32_t>(rounded));
}

bool PipelineConstant::asBool() const
{
    return bits != 0u;
}

std::int32_t PipelineConstant::asInt() const
{
    return std::bit_cast<std::int32_t>(bits);
}

std::uint32_t PipelineConstant::asUInt() const
{
    return bits;
}

float PipelineConstant::asFloat() const
{
    return std::bit_cast<float>(bits);
}

std::uint16_t PipelineConstant::asHalfBits() const
{
    return halfFromFloat(asFloat());
}

bool PipelineConstants::isEmpty() const
{
    return values.empty();
}

PipelineConstants& PipelineConstants::set(const PipelineConstant& constant)
{
    auto position = 0;

    while (position < values.size() && values[position].id < constant.id)
        ++position;

    if (position < values.size() && values[position].id == constant.id)
        values[position] = constant;
    else
        values.insert(position, constant);

    return *this;
}

bool PipelineConstants::operator==(const PipelineConstants& other) const
{
    if (values.size() != other.values.size())
        return false;

    for (auto index = 0; index < values.size(); ++index)
        if (!(values[index] == other.values[index]))
            return false;

    return true;
}

std::string hlslOverrideMacro(int id)
{
    return Strings::format("EACP_OVERRIDE_{}", id);
}

std::string hlslLiteralOf(const PipelineConstant& constant)
{
    switch (constant.type)
    {
        case PipelineConstantType::Bool:
            return constant.asBool() ? "true" : "false";
        case PipelineConstantType::Int:
            return hlslIntLiteral(constant.asInt());
        case PipelineConstantType::UInt:
            return Strings::format("{}u", constant.asUInt());
        case PipelineConstantType::Float:
        case PipelineConstantType::Half:
            break;
    }

    return ShaderEmission::formatShaderFloatLiteral(constant.asFloat());
}

Vector<ShaderDefine> hlslDefinesFor(const PipelineConstants& constants)
{
    auto defines = Vector<ShaderDefine> {};

    for (const auto& constant: constants.values)
        defines.add({hlslOverrideMacro(constant.id), hlslLiteralOf(constant)});

    return defines;
}
} // namespace eacp::GPU
