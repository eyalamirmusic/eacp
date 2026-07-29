#include "MeshData.h"

namespace eacp::Mesh
{
bool fitsNarrowIndices(const MeshData& data)
{
    for (const auto& primitive: data.primitives)
        for (auto i = 0; i < primitive.indexCount; ++i)
            if (data.indices[primitive.firstIndex + i] > 0xffffu)
                return false;

    return true;
}
} // namespace eacp::Mesh
