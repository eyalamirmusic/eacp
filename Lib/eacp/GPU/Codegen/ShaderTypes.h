#pragma once

namespace eacp::GPU
{
// The value types the shader EDSL understands. Inputs, varyings and expression
// results are all described with these, and they spell identically in MSL and
// HLSL ("float2" etc.), so the emitters share one type vocabulary.
enum class ValueType
{
    Float,
    Float2,
    Float3,
    Float4
};

inline int componentCount(ValueType type)
{
    switch (type)
    {
        case ValueType::Float:
            return 1;
        case ValueType::Float2:
            return 2;
        case ValueType::Float3:
            return 3;
        case ValueType::Float4:
            return 4;
    }

    return 1;
}

inline int byteSize(ValueType type)
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
    }

    return "float";
}
} // namespace eacp::GPU
