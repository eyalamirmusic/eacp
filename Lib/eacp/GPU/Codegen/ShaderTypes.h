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
//
// Int and Bool have vectors of their own because a comparison of two vectors is
// componentwise in both languages - it yields a bool of the same width, which
// any() or all() then collapses - and because a shader working on a grid counts
// its cell as a pair of integers. An integer vector crosses from the CPU like
// the scalar does; a boolean one does not, like the scalar does not.
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

// Whether a type belongs to the signed-integer family or the boolean one - the
// two vocabularies that sit outside float arithmetic and are crossed into
// explicitly. Asked by the uniform block, which takes the first and refuses the
// second, and by the comparison operators, which pick a mask by width.
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

// The mask a componentwise comparison of two of these yields: a boolean of the
// same width, whatever the operands were made of. Both shading languages give
// `<` on two vectors exactly this.
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
