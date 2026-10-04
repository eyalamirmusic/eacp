#include "UniformLayout.h"

namespace eacp::GPU
{
Vector<int> uniformOffsets(const Vector<ValueType>& types)
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

int uniformBlockSize(const Vector<ValueType>& types)
{
    auto cursor = 0;
    auto blockAlignment = 1;

    for (auto type: types)
    {
        cursor = alignUp(cursor, uniformAlignment(type)) + uniformSlotStride(type);

        if (uniformAlignment(type) > blockAlignment)
            blockAlignment = uniformAlignment(type);
    }

    return alignUp(cursor, blockAlignment);
}

int std140BlockSize(const Vector<ValueType>& types)
{
    return alignUp(uniformBlockSize(types), std140BlockAlignment);
}
} // namespace eacp::GPU
