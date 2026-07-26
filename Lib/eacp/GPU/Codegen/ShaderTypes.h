#pragma once

#include "../Texture/Texture.h"

namespace eacp::GPU
{
// TextureSampling, samplingConfigurations and samplingIndex live in Texture.h,
// alongside the filter and address-mode enums they are built from: both
// backends and the shader layer need them, and Texture.h is what they all
// already include.

// The value types the shader EDSL understands. Inputs, varyings and expression
// results are all described with these, and they spell identically in MSL and
// HLSL ("float2" etc.), so the emitters share one type vocabulary. UInt exists
// for the compute thread id and the element count it is checked against; it is
// never a vertex attribute. Int is what indexes an array and what the operators
// no float has - %, the bitwise set and the shifts - are defined on; unlike UInt
// it is signed, which is what a coordinate truncated towards zero needs. Bool is
// what a comparison yields and what a branch or a select tests; like UInt it
// never crosses from the CPU.
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
    Bool
};

// Whether a type is one of the square matrices. They share every rule that
// separates a matrix from a vector: built from columns, multiplied with mul()
// on HLSL, and transposed at construction there.
constexpr bool isMatrix(ValueType type)
{
    return type == ValueType::Float2x2 || type == ValueType::Float3x3
           || type == ValueType::Float4x4;
}

// The width of a square matrix's columns, and the number of them.
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
            return 2;
        case ValueType::Float3:
            return 3;
        case ValueType::Float4:
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
        case ValueType::Bool:
            return "bool";
    }

    return "float";
}
} // namespace eacp::GPU
