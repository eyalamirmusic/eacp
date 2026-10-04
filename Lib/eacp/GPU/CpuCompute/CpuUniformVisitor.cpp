#include "CpuUniformVisitor.h"

#include <cstring>

namespace eacp::GPU::CpuCompute
{
CpuUniformVisitor::CpuUniformVisitor(const ShaderGraph& graphToRead,
                                     const Plan& planToFollow,
                                     Word* uniformWordsToFill)
    : graph(graphToRead)
    , plan(planToFollow)
    , words(uniformWordsToFill)
{
}

void CpuUniformVisitor::onUniform(const char*,
                                  ValueType type,
                                  detail::ValueHandle& handle,
                                  const void* data)
{
    if (handle.node < 0 || handle.node >= graph.nodeCount())
        return;

    auto slot = graph.expr(handle.node).index;

    if (slot < 0 || slot >= plan.uniformCount()
        || byteSize(type) != byteSize(plan.uniformType(slot)))
        return;

    std::memcpy(words + plan.uniformOffset(slot),
                data,
                static_cast<std::size_t>(byteSize(type)));
}
} // namespace eacp::GPU::CpuCompute
