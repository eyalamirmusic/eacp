#pragma once

#include <string_view>

namespace eacp::GPU
{
// The value types the shader EDSL understands. Inputs, varyings and expression
// results are all described with these, and they spell identically in MSL and
// HLSL ("float2" etc.) but for the non-square matrices, so the emitters share
// one type vocabulary. UInt exists
// for the compute thread id and the element count it is checked against; it is
// never a vertex attribute. Int is what indexes an array and what the operators
// no float has - %, the bitwise set and the shifts - are defined on; unlike UInt
// it is signed, which is what a coordinate truncated towards zero needs. Bool is
// what a comparison yields and what a branch or a select tests; like UInt it
// never crosses from the CPU.
//
// UInt, Int and Bool have vectors of their own because a comparison of two
// vectors is componentwise in both languages - it yields a bool of the same
// width, which any() or all() then collapses - and because a shader working on a
// grid counts its cell as a pair of integers. An integer vector, signed or
// unsigned, crosses from the CPU like its scalar does; a boolean one does not,
// like the scalar does not.
enum class ValueType
{
    Float,
    Float2,
    Float3,
    Float4,
    Float2x2,
    Float3x3,
    Float4x4,
    Float2x3,
    Float2x4,
    Float3x2,
    Float3x4,
    Float4x2,
    Float4x3,
    UInt,
    UInt2,
    UInt3,
    UInt4,
    Int,
    Int2,
    Int3,
    Int4,
    Bool,
    Bool2,
    Bool3,
    Bool4
};

// The float matrices, WGSL's matCxR: C columns of R rows each, named columns
// first as MSL and GLSL name them (Float2x3 is two Float3 columns). They share
// every rule that separates a matrix from a vector: built from columns,
// multiplied with mul() on HLSL, and transposed at construction there. HLSL
// names the rows first, so its spelling of a non-square one is the other way
// round (hlslTypeName).
constexpr int matrixColumns(ValueType type)
{
    switch (type)
    {
        case ValueType::Float2x2:
        case ValueType::Float2x3:
        case ValueType::Float2x4:
            return 2;
        case ValueType::Float3x3:
        case ValueType::Float3x2:
        case ValueType::Float3x4:
            return 3;
        case ValueType::Float4x4:
        case ValueType::Float4x2:
        case ValueType::Float4x3:
            return 4;
        default:
            return 0;
    }
}

constexpr int matrixRows(ValueType type)
{
    switch (type)
    {
        case ValueType::Float2x2:
        case ValueType::Float3x2:
        case ValueType::Float4x2:
            return 2;
        case ValueType::Float3x3:
        case ValueType::Float2x3:
        case ValueType::Float4x3:
            return 3;
        case ValueType::Float4x4:
        case ValueType::Float2x4:
        case ValueType::Float3x4:
            return 4;
        default:
            return 0;
    }
}

constexpr bool isMatrix(ValueType type)
{
    return matrixColumns(type) > 0;
}

// The width of a square matrix's columns, and the number of them; 0 for a
// non-square one, which has no single order.
constexpr int matrixOrder(ValueType type)
{
    return matrixColumns(type) == matrixRows(type) ? matrixColumns(type) : 0;
}

// The float vector of a width from 1 to 4: a matrix's column, or its row.
constexpr ValueType floatVectorOf(int width)
{
    switch (width)
    {
        case 2:
            return ValueType::Float2;
        case 3:
            return ValueType::Float3;
        case 4:
            return ValueType::Float4;
        default:
            return ValueType::Float;
    }
}

// The matrix of `columns` columns of `rows` rows, both 2 to 4.
constexpr ValueType matrixOf(int columns, int rows)
{
    constexpr ValueType shapes[3][3] = {
        {ValueType::Float2x2, ValueType::Float2x3, ValueType::Float2x4},
        {ValueType::Float3x2, ValueType::Float3x3, ValueType::Float3x4},
        {ValueType::Float4x2, ValueType::Float4x3, ValueType::Float4x4}};

    return shapes[columns - 2][rows - 2];
}

constexpr ValueType transposedMatrix(ValueType type)
{
    return matrixOf(matrixRows(type), matrixColumns(type));
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
        case ValueType::UInt2:
        case ValueType::Int2:
        case ValueType::Bool2:
            return 2;
        case ValueType::Float3:
        case ValueType::UInt3:
        case ValueType::Int3:
        case ValueType::Bool3:
            return 3;
        case ValueType::Float4:
        case ValueType::UInt4:
        case ValueType::Int4:
        case ValueType::Bool4:
            return 4;
        case ValueType::Float2x2:
        case ValueType::Float3x3:
        case ValueType::Float4x4:
        case ValueType::Float2x3:
        case ValueType::Float2x4:
        case ValueType::Float3x2:
        case ValueType::Float3x4:
        case ValueType::Float4x2:
        case ValueType::Float4x3:
            return matrixColumns(type) * matrixRows(type);
    }

    return 1;
}

// Which of the three vocabularies outside float arithmetic a type belongs to -
// the unsigned integers, the signed ones, the booleans - each crossed into
// explicitly. Asked by the uniform block, which takes the two integer families
// and refuses the boolean one, and by the operator sets, which are defined per
// family.
constexpr bool isUnsignedInteger(ValueType type)
{
    return type == ValueType::UInt || type == ValueType::UInt2
           || type == ValueType::UInt3 || type == ValueType::UInt4;
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

constexpr const char* typeName(ValueType type)
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
        case ValueType::Float2x3:
            return "float2x3";
        case ValueType::Float2x4:
            return "float2x4";
        case ValueType::Float3x2:
            return "float3x2";
        case ValueType::Float3x4:
            return "float3x4";
        case ValueType::Float4x2:
            return "float4x2";
        case ValueType::Float4x3:
            return "float4x3";
        case ValueType::UInt:
            return "uint";
        case ValueType::UInt2:
            return "uint2";
        case ValueType::UInt3:
            return "uint3";
        case ValueType::UInt4:
            return "uint4";
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

constexpr const char* glslTypeName(ValueType type)
{
    switch (type)
    {
        case ValueType::Float:
            return "float";
        case ValueType::Float2:
            return "vec2";
        case ValueType::Float3:
            return "vec3";
        case ValueType::Float4:
            return "vec4";
        case ValueType::Float2x2:
            return "mat2";
        case ValueType::Float3x3:
            return "mat3";
        case ValueType::Float4x4:
            return "mat4";
        case ValueType::Float2x3:
            return "mat2x3";
        case ValueType::Float2x4:
            return "mat2x4";
        case ValueType::Float3x2:
            return "mat3x2";
        case ValueType::Float3x4:
            return "mat3x4";
        case ValueType::Float4x2:
            return "mat4x2";
        case ValueType::Float4x3:
            return "mat4x3";
        case ValueType::UInt:
            return "uint";
        case ValueType::UInt2:
            return "uvec2";
        case ValueType::UInt3:
            return "uvec3";
        case ValueType::UInt4:
            return "uvec4";
        case ValueType::Int:
            return "int";
        case ValueType::Int2:
            return "ivec2";
        case ValueType::Int3:
            return "ivec3";
        case ValueType::Int4:
            return "ivec4";
        case ValueType::Bool:
            return "bool";
        case ValueType::Bool2:
            return "bvec2";
        case ValueType::Bool3:
            return "bvec3";
        case ValueType::Bool4:
            return "bvec4";
    }

    return "float";
}

// HLSL names a matrix rows first: MSL's float2x3, two columns of three rows,
// is HLSL's float3x2. Every other type is spelled as typeName spells it.
constexpr const char* hlslTypeName(ValueType type)
{
    return isMatrix(type) ? typeName(transposedMatrix(type)) : typeName(type);
}

// A constructor-style cast is a call under the target's canonical type name.
constexpr const char* glslTypeNameFor(std::string_view canonicalName)
{
    for (auto raw = 0; raw <= static_cast<int>(ValueType::Bool4); ++raw)
    {
        auto type = static_cast<ValueType>(raw);

        if (canonicalName == typeName(type))
            return glslTypeName(type);
    }

    return nullptr;
}
} // namespace eacp::GPU
