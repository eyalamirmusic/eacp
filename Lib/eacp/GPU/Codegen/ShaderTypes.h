#pragma once

/** @file ValueType, the scalar, vector and matrix types the shader EDSL understands.
*/

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
//
// Half and its vectors and matrices are WGSL's f16, a float family of its own:
// the same operators and built-ins as Float, crossed into and out of
// explicitly, never mixed with it. They come after every other type so the
// order of the ones before them, which loops over the enum walk, is unchanged.
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
    Bool4,
    Half,
    Half2,
    Half3,
    Half4,
    Half2x2,
    Half3x3,
    Half4x4,
    Half2x3,
    Half2x4,
    Half3x2,
    Half3x4,
    Half4x2,
    Half4x3,

    // A struct the shader declares (ShaderStruct.h). Which one is the node's,
    // the variable's or the storage slot's struct id, never this alone. It is
    // in no family and sits past lastValueType, so the loops over the value
    // types never meet it.
    Struct
};

inline constexpr auto lastValueType = ValueType::Half4x3;

// The f16 family, matrices included.
constexpr bool isHalf(ValueType type)
{
    return static_cast<int>(type) >= static_cast<int>(ValueType::Half)
           && static_cast<int>(type) <= static_cast<int>(lastValueType);
}

// The float matrices, WGSL's matCxR: C columns of R rows each, named columns
// first as MSL and GLSL name them (Float2x3 is two Float3 columns). They share
// every rule that separates a matrix from a vector: built from columns,
// multiplied with mul() on HLSL, and transposed at construction there. HLSL
// names the rows first, so its spelling of a non-square one is the other way
// round (hlslTypeName). The half matrices are the same shapes over f16.
constexpr int matrixColumns(ValueType type)
{
    switch (type)
    {
        case ValueType::Float2x2:
        case ValueType::Float2x3:
        case ValueType::Float2x4:
        case ValueType::Half2x2:
        case ValueType::Half2x3:
        case ValueType::Half2x4:
            return 2;
        case ValueType::Float3x3:
        case ValueType::Float3x2:
        case ValueType::Float3x4:
        case ValueType::Half3x3:
        case ValueType::Half3x2:
        case ValueType::Half3x4:
            return 3;
        case ValueType::Float4x4:
        case ValueType::Float4x2:
        case ValueType::Float4x3:
        case ValueType::Half4x4:
        case ValueType::Half4x2:
        case ValueType::Half4x3:
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
        case ValueType::Half2x2:
        case ValueType::Half3x2:
        case ValueType::Half4x2:
            return 2;
        case ValueType::Float3x3:
        case ValueType::Float2x3:
        case ValueType::Float4x3:
        case ValueType::Half3x3:
        case ValueType::Half2x3:
        case ValueType::Half4x3:
            return 3;
        case ValueType::Float4x4:
        case ValueType::Float2x4:
        case ValueType::Float3x4:
        case ValueType::Half4x4:
        case ValueType::Half2x4:
        case ValueType::Half3x4:
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

// Its f16 sibling.
constexpr ValueType halfVectorOf(int width)
{
    switch (width)
    {
        case 2:
            return ValueType::Half2;
        case 3:
            return ValueType::Half3;
        case 4:
            return ValueType::Half4;
        default:
            return ValueType::Half;
    }
}

// The matrix of `columns` columns of `rows` rows, both 2 to 4, over f32 or f16.
constexpr ValueType matrixOf(int columns, int rows, bool half = false)
{
    constexpr ValueType shapes[3][3] = {
        {ValueType::Float2x2, ValueType::Float2x3, ValueType::Float2x4},
        {ValueType::Float3x2, ValueType::Float3x3, ValueType::Float3x4},
        {ValueType::Float4x2, ValueType::Float4x3, ValueType::Float4x4}};

    constexpr ValueType halfShapes[3][3] = {
        {ValueType::Half2x2, ValueType::Half2x3, ValueType::Half2x4},
        {ValueType::Half3x2, ValueType::Half3x3, ValueType::Half3x4},
        {ValueType::Half4x2, ValueType::Half4x3, ValueType::Half4x4}};

    return half ? halfShapes[columns - 2][rows - 2] : shapes[columns - 2][rows - 2];
}

constexpr ValueType transposedMatrix(ValueType type)
{
    return matrixOf(matrixRows(type), matrixColumns(type), isHalf(type));
}

// The vector one column of a matrix is.
constexpr ValueType matrixColumnType(ValueType type)
{
    return isHalf(type) ? halfVectorOf(matrixRows(type))
                        : floatVectorOf(matrixRows(type));
}

constexpr int componentCount(ValueType type)
{
    switch (type)
    {
        case ValueType::Float:
        case ValueType::UInt:
        case ValueType::Int:
        case ValueType::Bool:
        case ValueType::Half:
            return 1;
        case ValueType::Float2:
        case ValueType::UInt2:
        case ValueType::Int2:
        case ValueType::Bool2:
        case ValueType::Half2:
            return 2;
        case ValueType::Float3:
        case ValueType::UInt3:
        case ValueType::Int3:
        case ValueType::Bool3:
        case ValueType::Half3:
            return 3;
        case ValueType::Float4:
        case ValueType::UInt4:
        case ValueType::Int4:
        case ValueType::Bool4:
        case ValueType::Half4:
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
        case ValueType::Half2x2:
        case ValueType::Half3x3:
        case ValueType::Half4x4:
        case ValueType::Half2x3:
        case ValueType::Half2x4:
        case ValueType::Half3x2:
        case ValueType::Half3x4:
        case ValueType::Half4x2:
        case ValueType::Half4x3:
            return matrixColumns(type) * matrixRows(type);
        case ValueType::Struct:
            return 0;
    }

    return 1;
}

// The same shape in the other float family: Half3 for Float3, Float2x3 for
// Half2x3. Anything outside the two families maps to itself.
constexpr ValueType halfEquivalent(ValueType type)
{
    if (isMatrix(type))
        return matrixOf(matrixColumns(type), matrixRows(type), true);

    switch (type)
    {
        case ValueType::Float:
        case ValueType::Float2:
        case ValueType::Float3:
        case ValueType::Float4:
            return halfVectorOf(componentCount(type));
        default:
            return type;
    }
}

constexpr ValueType floatEquivalent(ValueType type)
{
    if (!isHalf(type))
        return type;

    if (isMatrix(type))
        return matrixOf(matrixColumns(type), matrixRows(type));

    return floatVectorOf(componentCount(type));
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
    return componentCount(type) * (isHalf(type) ? 2 : 4);
}

// WGSL's storage layout, which an std430 block shares: AlignOf and SizeOf of
// a value in a storage buffer or a struct held in one. A scalar is its own
// size, two bytes for f16 and four otherwise; a two- and a four-vector are
// aligned to their size and a three-vector to the four-vector's, with its
// size still three scalars; a matrix is its columns, each at a stride of the
// column vector's alignment.
constexpr int storageRoundUp(int value, int multiple)
{
    return (value + multiple - 1) / multiple * multiple;
}

constexpr int storageScalarBytes(ValueType type)
{
    return isHalf(type) ? 2 : 4;
}

constexpr int storageVectorAlignment(int width, int scalarBytes)
{
    return (width == 3 ? 4 : width) * scalarBytes;
}

constexpr int storageAlignment(ValueType type)
{
    auto width = isMatrix(type) ? matrixRows(type) : componentCount(type);
    return storageVectorAlignment(width, storageScalarBytes(type));
}

constexpr int storageColumnStride(ValueType type)
{
    return storageVectorAlignment(matrixRows(type), storageScalarBytes(type));
}

constexpr int storageSize(ValueType type)
{
    if (isMatrix(type))
        return matrixColumns(type) * storageColumnStride(type);

    return componentCount(type) * storageScalarBytes(type);
}

// Where component `component` of a value sits from the value's start: a
// matrix's run column by column, each column at its stride.
constexpr int storageComponentOffset(ValueType type, int component)
{
    auto scalar = storageScalarBytes(type);

    if (!isMatrix(type))
        return component * scalar;

    auto rows = matrixRows(type);
    return component / rows * storageColumnStride(type) + component % rows * scalar;
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
        case ValueType::Half:
            return "half";
        case ValueType::Half2:
            return "half2";
        case ValueType::Half3:
            return "half3";
        case ValueType::Half4:
            return "half4";
        case ValueType::Half2x2:
            return "half2x2";
        case ValueType::Half3x3:
            return "half3x3";
        case ValueType::Half4x4:
            return "half4x4";
        case ValueType::Half2x3:
            return "half2x3";
        case ValueType::Half2x4:
            return "half2x4";
        case ValueType::Half3x2:
            return "half3x2";
        case ValueType::Half3x4:
            return "half3x4";
        case ValueType::Half4x2:
            return "half4x2";
        case ValueType::Half4x3:
            return "half4x3";
        case ValueType::Struct:
            return "struct";
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
        case ValueType::Half:
            return "float16_t";
        case ValueType::Half2:
            return "f16vec2";
        case ValueType::Half3:
            return "f16vec3";
        case ValueType::Half4:
            return "f16vec4";
        case ValueType::Half2x2:
            return "f16mat2";
        case ValueType::Half3x3:
            return "f16mat3";
        case ValueType::Half4x4:
            return "f16mat4";
        case ValueType::Half2x3:
            return "f16mat2x3";
        case ValueType::Half2x4:
            return "f16mat2x4";
        case ValueType::Half3x2:
            return "f16mat3x2";
        case ValueType::Half3x4:
            return "f16mat3x4";
        case ValueType::Half4x2:
            return "f16mat4x2";
        case ValueType::Half4x3:
            return "f16mat4x3";
        case ValueType::Struct:
            return "struct";
    }

    return "float";
}

// HLSL names a matrix rows first: MSL's float2x3, two columns of three rows,
// is HLSL's float3x2. Every other type is spelled as typeName spells it - but
// for the f16 family, which HLSL holds in float (FXC's SM5 has no 16-bit type
// that stores), so a Half3 is a float3 there and a Half2x3 a float3x2.
constexpr const char* hlslTypeName(ValueType type)
{
    auto held = floatEquivalent(type);
    return isMatrix(held) ? typeName(transposedMatrix(held)) : typeName(held);
}

// A constructor-style cast is a call under the target's canonical type name.
constexpr const char* glslTypeNameFor(std::string_view canonicalName)
{
    for (auto raw = 0; raw <= static_cast<int>(lastValueType); ++raw)
    {
        auto type = static_cast<ValueType>(raw);

        if (canonicalName == typeName(type))
            return glslTypeName(type);
    }

    return nullptr;
}

// The HLSL spelling of the same cast: a conversion to Half3 is one to float3,
// which the emitter then rounds to f16.
constexpr const char* hlslTypeNameFor(std::string_view canonicalName)
{
    for (auto raw = static_cast<int>(ValueType::Half);
         raw <= static_cast<int>(lastValueType);
         ++raw)
    {
        auto type = static_cast<ValueType>(raw);

        if (canonicalName == typeName(type))
            return hlslTypeName(type);
    }

    return nullptr;
}
} // namespace eacp::GPU
