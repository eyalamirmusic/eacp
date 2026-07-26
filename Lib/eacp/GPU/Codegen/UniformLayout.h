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
// Float2x2 and Float3x3 appear here for completeness only: ShaderBuilder
// refuses them as uniforms, because this is the one place the two backends
// cannot be reconciled by padding. MSL packs a float2x2 as two float2 columns,
// 16 bytes in all, while an HLSL cbuffer gives every matrix row a register of
// its own and takes 32 - a disagreement inside the value, which no pad scalar
// between fields can correct. Both agree on a float4x4, which is why that one
// crosses the boundary and these two stay shader-local values.
inline int uniformAlignment(ValueType type)
{
    switch (type)
    {
        case ValueType::Float:
        case ValueType::UInt:
        case ValueType::Bool:
            return 4;
        case ValueType::Float2:
        case ValueType::Float2x2:
            return 8;
        case ValueType::Float3:
        case ValueType::Float4:
        case ValueType::Float3x3:
        case ValueType::Float4x4:
            return 16;
    }

    return 4;
}

inline int uniformSlotStride(ValueType type)
{
    if (type == ValueType::Float3)
        return 16;

    // A float3x3 is three float3 columns, and a column occupies a full 16 bytes
    // just as a standalone float3 does: 48, not the 36 its components add to.
    if (type == ValueType::Float3x3)
        return 48;

    return byteSize(type);
}

inline int alignUp(int value, int alignment)
{
    return (value + alignment - 1) / alignment * alignment;
}

// The byte offset of every uniform field in the packed block.
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

// Where HLSL cbuffer packing would place a field on its own: it only forbids a
// value straddling a 16-byte register, it does not align a vector to its size
// the way MSL does. Wherever this lands below the MSL offset the HLSL emitter
// inserts explicit pad scalars so both backends read the same packed bytes.
inline int hlslPackedOffset(int cursor, ValueType type)
{
    if (isMatrix(type))
        return alignUp(cursor, 16);

    auto size = byteSize(type);
    auto crossesRegister = cursor / 16 != (cursor + size - 1) / 16;
    return crossesRegister ? alignUp(cursor, 16) : cursor;
}
} // namespace eacp::GPU
