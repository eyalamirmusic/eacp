#include "OverrideValues.h"

#include <eacp/Core/Utils/Format.h>

#include <climits>
#include <cmath>

namespace eacp::GPU
{
namespace
{
std::string_view typeNameOf(PipelineConstantType type)
{
    switch (type)
    {
        case PipelineConstantType::Bool:
            return "a bool";
        case PipelineConstantType::Int:
            return "an int";
        case PipelineConstantType::UInt:
            return "an unsigned";
        case PipelineConstantType::Float:
            return "a float";
        case PipelineConstantType::Half:
            return "a half";
    }

    return "a value";
}

std::string_view declaredTypeName(ValueType type)
{
    switch (type)
    {
        case ValueType::Bool:
            return "Bool";
        case ValueType::Int:
            return "Int";
        case ValueType::UInt:
            return "UInt";
        case ValueType::Half:
            return "Half";
        default:
            return "Float";
    }
}

struct Conversion
{
    PipelineConstant value;
    std::string error;
};

Conversion refused(const OverrideDeclaration& declaration, std::string_view why)
{
    return {{},
            Strings::format("override \"{}\" ({}): {}",
                            declaration.name,
                            declaredTypeName(declaration.type),
                            why)};
}

Conversion wrongType(const OverrideDeclaration& declaration,
                     const PipelineConstant& given)
{
    return refused(
        declaration,
        Strings::format("given {}, which is not its type", typeNameOf(given.type)));
}

Conversion toFloating(const OverrideDeclaration& declaration,
                      int id,
                      const PipelineConstant& given)
{
    if (given.type != PipelineConstantType::Float)
        return wrongType(declaration, given);

    if (!std::isfinite(given.asFloat()))
        return refused(declaration, "given a value that is not finite");

    if (declaration.type == ValueType::Float)
        return {PipelineConstant::ofFloat(id, given.asFloat()), {}};

    auto half = PipelineConstant::ofHalf(id, given.asFloat());

    if (!std::isfinite(half.asFloat()))
        return refused(declaration, "given a value past the f16 range");

    return {half, {}};
}

Conversion toInteger(const OverrideDeclaration& declaration,
                     int id,
                     const PipelineConstant& given)
{
    const auto isSigned = declaration.type == ValueType::Int;

    if (given.type == PipelineConstantType::Int)
    {
        if (isSigned)
            return {PipelineConstant::ofInt(id, given.asInt()), {}};

        if (given.asInt() < 0)
            return refused(declaration, "given a negative int");

        return {PipelineConstant::ofUInt(id, given.asUInt()), {}};
    }

    if (given.type == PipelineConstantType::UInt)
    {
        if (!isSigned)
            return {PipelineConstant::ofUInt(id, given.asUInt()), {}};

        if (given.asUInt() > (std::uint32_t) INT_MAX)
            return refused(declaration, "given an unsigned past INT_MAX");

        return {PipelineConstant::ofInt(id, given.asInt()), {}};
    }

    return wrongType(declaration, given);
}

Conversion convert(const OverrideDeclaration& declaration,
                   int id,
                   const PipelineConstant& given)
{
    switch (declaration.type)
    {
        case ValueType::Bool:
            if (given.type != PipelineConstantType::Bool)
                return wrongType(declaration, given);

            return {PipelineConstant::ofBool(id, given.asBool()), {}};
        case ValueType::Int:
        case ValueType::UInt:
            return toInteger(declaration, id, given);
        default:
            return toFloating(declaration, id, given);
    }
}

PipelineConstant defaultOf(const OverrideDeclaration& declaration, int id)
{
    auto constant = PipelineConstant {};
    constant.id = id;
    constant.type = pipelineConstantTypeOf(declaration.type);
    constant.bits = declaration.defaultBits;
    return constant;
}

const OverrideValues::Entry* findEntry(const OverrideValues& values,
                                       const std::string& name)
{
    for (const auto& entry: values.entries())
        if (entry.name == name)
            return &entry;

    return nullptr;
}
} // namespace

OverrideValues& OverrideValues::set(std::string_view name, float value)
{
    return setValue(name, PipelineConstant::ofFloat(0, value));
}

OverrideValues& OverrideValues::set(std::string_view name, double value)
{
    return set(name, (float) value);
}

OverrideValues& OverrideValues::set(std::string_view name, int value)
{
    return setValue(name, PipelineConstant::ofInt(0, value));
}

OverrideValues& OverrideValues::set(std::string_view name, unsigned value)
{
    return setValue(name, PipelineConstant::ofUInt(0, value));
}

OverrideValues& OverrideValues::set(std::string_view name, bool value)
{
    return setValue(name, PipelineConstant::ofBool(0, value));
}

OverrideValues& OverrideValues::setValue(std::string_view name,
                                         const PipelineConstant& value)
{
    for (auto& entry: values)
    {
        if (entry.name == name)
        {
            entry.value = value;
            return *this;
        }
    }

    values.add({std::string(name), value});
    return *this;
}

PipelineConstantType pipelineConstantTypeOf(ValueType type)
{
    switch (type)
    {
        case ValueType::Bool:
            return PipelineConstantType::Bool;
        case ValueType::Int:
            return PipelineConstantType::Int;
        case ValueType::UInt:
            return PipelineConstantType::UInt;
        case ValueType::Half:
            return PipelineConstantType::Half;
        default:
            return PipelineConstantType::Float;
    }
}

ResolvedOverrides resolveOverrides(const ShaderGraph& graph,
                                   const OverrideValues& values)
{
    auto resolved = ResolvedOverrides {};

    for (const auto& entry: values.entries())
    {
        if (graph.findOverride(entry.name) < 0)
        {
            resolved.error = Strings::format(
                "no override named \"{}\" in this shader", entry.name);
            return resolved;
        }
    }

    const auto& declarations = graph.overrides();

    for (auto id = 0; id < declarations.size(); ++id)
    {
        const auto& declaration = declarations[id];
        const auto* given = findEntry(values, declaration.name);

        if (given == nullptr && !declaration.hasDefault)
        {
            resolved.error =
                refused(declaration, "has no default and was given no value").error;
            return resolved;
        }

        if (given == nullptr)
        {
            resolved.values.add(defaultOf(declaration, id));
            continue;
        }

        auto converted = convert(declaration, id, given->value);

        if (!converted.error.empty())
        {
            resolved.error = converted.error;
            return resolved;
        }

        resolved.values.add(converted.value);

        if (!declaration.hasDefault
            || converted.value.bits != declaration.defaultBits)
            resolved.specialized.set(converted.value);
    }

    return resolved;
}
} // namespace eacp::GPU
