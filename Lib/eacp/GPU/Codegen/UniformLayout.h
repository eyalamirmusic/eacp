#pragma once

#include "../Common.h"

#include "ShaderTypes.h"

namespace eacp::GPU
{
// Uniform-block layout follows the native MSL struct rules: vec2 aligns to 8,
// vec3/vec4/matrix to 16, a vec3 still occupies a full 16-byte slot. Float2x2,
// Float3x3 and bool uniforms are refused: MSL and HLSL size them differently.
inline int uniformAlignment(ValueType type)
{
    switch (type)
    {
        case ValueType::Float:
        case ValueType::UInt:
        case ValueType::Int:
        case ValueType::Bool:
            return 4;
        case ValueType::Float2:
        case ValueType::Int2:
        case ValueType::Bool2:
        case ValueType::Float2x2:
            return 8;
        case ValueType::Float3:
        case ValueType::Float4:
        case ValueType::Int3:
        case ValueType::Int4:
        case ValueType::Bool3:
        case ValueType::Bool4:
        case ValueType::Float3x3:
        case ValueType::Float4x4:
            return 16;
    }

    return 4;
}

inline int uniformSlotStride(ValueType type)
{
    if (type == ValueType::Float3 || type == ValueType::Int3
        || type == ValueType::Bool3)
        return 16;

    // Three float3 columns, each occupying a full 16 bytes: 48, not 36.
    if (type == ValueType::Float3x3)
        return 48;

    return byteSize(type);
}

inline int alignUp(int value, int alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

inline Vector<int> uniformOffsets(const Vector<ValueType>& types)
{
    auto offsets = Vector<int> {};
    auto cursor = 0;

    for (auto type: types)
    {
        auto offset = alignUp(cursor, uniformAlignment(type));
        offsets.add(offset);
        cursor = offset + uniformSlotStride(type);
    }

    return offsets;
}

// HLSL cbuffer packing only forbids a value straddling a 16-byte register; it
// does not align a vector to its size the way MSL does. Where this lands below
// the MSL offset the HLSL emitter inserts pad scalars to reconcile the two.
inline int hlslPackedOffset(int cursor, ValueType type)
{
    if (isMatrix(type))
        return alignUp(cursor, 16);

    auto size = byteSize(type);
    auto crossesRegister = cursor / 16 != (cursor + size - 1) / 16;
    return crossesRegister ? alignUp(cursor, 16) : cursor;
}
} // namespace eacp::GPU
