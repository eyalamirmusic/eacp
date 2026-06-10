#pragma once

namespace eacp::GPU
{
// The value types the shader EDSL understands. Inputs, varyings and expression
// results are all described with these, and they spell identically in MSL and
// HLSL ("float2" etc.), so the emitters share one type vocabulary. UInt exists
// for the compute thread id and the element count it is checked against; it is
// never a vertex attribute.
enum class ValueType
{
    Float,
    Float2,
    Float3,
    Float4,
    Float4x4,
    UInt
};

constexpr int componentCount(ValueType type)
{
    switch (type)
    {
        case ValueType::Float:
        case ValueType::UInt:
            return 1;
        case ValueType::Float2:
            return 2;
        case ValueType::Float3:
            return 3;
        case ValueType::Float4:
            return 4;
        case ValueType::Float4x4:
            return 16;
    }

    return 1;
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
        case ValueType::Float4x4:
            return "float4x4";
        case ValueType::UInt:
            return "uint";
    }

    return "float";
}
} // namespace eacp::GPU
