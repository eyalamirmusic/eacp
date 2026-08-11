#pragma once

#include "../Texture/Texture.h"

namespace eacp::GPU
{
// The value types the shader EDSL understands. They spell identically in MSL
// and HLSL, so the emitters share one type vocabulary. UInt and the booleans
// never cross from the CPU.
enum class ValueType
{
    Float,
    Float2,
    Float3,
    Float4,
    Float2x2,
    Float3x3,
    Float4x4,
    UInt,
    Int,
    Int2,
    Int3,
    Int4,
    Bool,
    Bool2,
    Bool3,
    Bool4
};

constexpr bool isMatrix(ValueType type)
{
    return type == ValueType::Float2x2 || type == ValueType::Float3x3
           || type == ValueType::Float4x4;
}

constexpr int matrixOrder(ValueType type)
{
    switch (type)
    {
        case ValueType::Float2x2:
            return 2;
        case ValueType::Float3x3:
            return 3;
        case ValueType::Float4x4:
            return 4;
        default:
            return 0;
    }
}

constexpr int componentCount(ValueType type)
{
    switch (type)
    {
        case ValueType::Float:
        case ValueType::UInt:
        case ValueType::Int:
        case ValueType::Bool:
            return 1;
        case ValueType::Float2:
        case ValueType::Int2:
        case ValueType::Bool2:
            return 2;
        case ValueType::Float3:
        case ValueType::Int3:
        case ValueType::Bool3:
            return 3;
        case ValueType::Float4:
        case ValueType::Int4:
        case ValueType::Bool4:
            return 4;
        case ValueType::Float2x2:
            return 4;
        case ValueType::Float3x3:
            return 9;
        case ValueType::Float4x4:
            return 16;
    }

    return 1;
}

constexpr bool isSignedInteger(ValueType type)
{
    return type == ValueType::Int || type == ValueType::Int2
           || type == ValueType::Int3 || type == ValueType::Int4;
}

constexpr bool isBoolean(ValueType type)
{
    return type == ValueType::Bool || type == ValueType::Bool2
           || type == ValueType::Bool3 || type == ValueType::Bool4;
}

// The mask a componentwise comparison yields: a boolean of the same width.
constexpr ValueType maskFor(ValueType type)
{
    switch (componentCount(type))
    {
        case 2:
            return ValueType::Bool2;
        case 3:
            return ValueType::Bool3;
        case 4:
            return ValueType::Bool4;
        default:
            return ValueType::Bool;
    }
}

constexpr int byteSize(ValueType type)
{
    return componentCount(type) * 4;
}

inline const char* typeName(ValueType type)
{
    switch (type)
    {
        case ValueType::Float:
            return "float";
        case ValueType::Float2:
            return "float2";
        case ValueType::Float3:
            return "float3";
        case ValueType::Float4:
            return "float4";
        case ValueType::Float2x2:
            return "float2x2";
        case ValueType::Float3x3:
            return "float3x3";
        case ValueType::Float4x4:
            return "float4x4";
        case ValueType::UInt:
            return "uint";
        case ValueType::Int:
            return "int";
        case ValueType::Int2:
            return "int2";
        case ValueType::Int3:
            return "int3";
        case ValueType::Int4:
            return "int4";
        case ValueType::Bool:
            return "bool";
        case ValueType::Bool2:
            return "bool2";
        case ValueType::Bool3:
            return "bool3";
        case ValueType::Bool4:
            return "bool4";
    }

    return "float";
}
} // namespace eacp::GPU
