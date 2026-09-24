#pragma once

#include "Plan.h"

#include <eacp/GPU/Codegen/ShaderMembers.h>

#include <cstring>

// The fourth member walk, beside the build, upload and bind ones: each uniform
// member's current value copied as it is typed - tightly packed, no MSL padding
// - into the words of the slot its handle names. A virtual call per member on a
// stack object, so it runs on every dispatch without allocating.

namespace eacp::GPU::CpuCompute
{
class CpuUniformVisitor final : public ShaderVisitor
{
public:
    CpuUniformVisitor(const ShaderGraph& graphToRead, Word* uniformWordsToFill)
        : graph(graphToRead)
        , words(uniformWordsToFill)
    {
    }

protected:
    void onUniform(const char*,
                   ValueType type,
                   detail::ValueHandle& handle,
                   const void* data) override
    {
        if (handle.node < 0 || handle.node >= graph.nodeCount())
            return;

        auto slot = graph.expr(handle.node).index;
        auto bytes = static_cast<std::size_t>(byteSize(type));

        if (slot < 0 || slot >= graph.uniforms().size()
            || bytes > sizeof(Word) * Plan::uniformWordsPerSlot)
            return;

        std::memcpy(words + slot * Plan::uniformWordsPerSlot, data, bytes);
    }

private:
    const ShaderGraph& graph;
    Word* words;
};
} // namespace eacp::GPU::CpuCompute
