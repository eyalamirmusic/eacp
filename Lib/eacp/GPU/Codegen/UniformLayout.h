#pragma once

#include "../Common.h"

#include "ShaderTypes.h"

namespace eacp::GPU
{
// Uniform-block layout follows the native MSL struct rules: a vec2 aligns to 8,
// a vec3/vec4/matrix to 16, and a vec3 still occupies a full 16-byte slot. The
// CPU upload walk and both shader emitters derive their offsets from these
// helpers, so the packed bytes and the generated source cannot disagree. The
// block's total size follows the same rules - sizeof(Uniforms) is padded up to
// the widest member's alignment, which ShaderUploadVisitor::finish applies to
// the packed block, Metal's validation layer holding the bound length to it.
//
// A matrix of any shape lays each column out at a 16-byte stride, 16-aligned:
// what std140 and an HLSL cbuffer (column-major, a register per column) both
// do, and what MSL does for columns of three or four. MSL packs a column of two
// at 8 bytes, so a two-row matrix is declared there as float4 columns and
// rebuilt from their .xy where it is read; the CPU upload walk spreads each
// column out to the stride (ShaderUploadVisitor). The block size is columns x
// 16 everywhere but HLSL, whose last column holds only its rows, which is what
// the pad scalars after one account for (uniformPackedSize). The boolean
// vectors appear here for completeness only and are refused for the reason the
// scalar Bool is: the languages disagree on what a bool occupies. The integer
// vectors, signed and unsigned alike, are not in that position - every backend
// packs an int2 or a uint2 exactly where it packs a float2 - so those cross like
// their scalars do.
constexpr int uniformAlignment(ValueType type)
{
    if (isMatrix(type))
        return 16;

    switch (type)
    {
        case ValueType::Float:
        case ValueType::UInt:
        case ValueType::Int:
        case ValueType::Bool:
            return 4;
        case ValueType::Float2:
        case ValueType::UInt2:
        case ValueType::Int2:
        case ValueType::Bool2:
            return 8;
        default:
            return 16;
    }
}

// The distance between two columns of a uniform matrix.
constexpr int uniformColumnStride = 16;

constexpr int uniformSlotStride(ValueType type)
{
    if (type == ValueType::Float3 || type == ValueType::UInt3
        || type == ValueType::Int3 || type == ValueType::Bool3)
        return 16;

    if (isMatrix(type))
        return matrixColumns(type) * uniformColumnStride;

    return byteSize(type);
}

// Whether MSL holds a uniform matrix as float4 columns rather than as itself:
// its own two-row matrices pack a column to 8 bytes, not to the stride.
constexpr bool isMetalWidenedUniform(ValueType type)
{
    return isMatrix(type) && matrixRows(type) == 2;
}

// How many bytes a field occupies in an HLSL cbuffer or an std140 block, which
// is where the next one may start: a matrix's last HLSL register holds only its
// rows, and std140 rounds every column up to the stride.
constexpr int uniformPackedSize(ValueType type, bool std140)
{
    if (!isMatrix(type))
        return byteSize(type);

    auto columns = matrixColumns(type);

    if (std140)
        return columns * uniformColumnStride;

    return (columns - 1) * uniformColumnStride + matrixRows(type) * 4;
}

constexpr int alignUp(int value, int alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

// The byte offset of every uniform field in the packed block.
Vector<int> uniformOffsets(const Vector<ValueType>& types);

// Where HLSL cbuffer packing would place a field on its own: it only forbids a
// value straddling a 16-byte register, it does not align a vector to its size
// the way MSL does. Wherever this lands below the MSL offset the HLSL emitter
// inserts explicit pad scalars so both backends read the same packed bytes.
constexpr int hlslPackedOffset(int cursor, ValueType type)
{
    if (isMatrix(type))
        return alignUp(cursor, 16);

    auto size = byteSize(type);
    auto crossesRegister = cursor / 16 != (cursor + size - 1) / 16;
    return crossesRegister ? alignUp(cursor, 16) : cursor;
}

// std140 sizes a vec3 at 12 bytes where MSL gives 16, so a scalar after one pads.
constexpr int std140PackedOffset(int cursor, ValueType type)
{
    return alignUp(cursor, uniformAlignment(type));
}

int uniformBlockSize(const Vector<ValueType>& types);

// A dynamic uniform range shorter than the std140 block fails validation.
constexpr int std140BlockAlignment = 16;

int std140BlockSize(const Vector<ValueType>& types);
} // namespace eacp::GPU
