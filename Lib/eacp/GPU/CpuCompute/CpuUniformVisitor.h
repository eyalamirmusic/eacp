#pragma once

#include "Plan.h"

#include <eacp/GPU/Codegen/ShaderMembers.h>

// The fourth member walk, beside the build, upload and bind ones: each uniform
// member's current value copied as it is typed - tightly packed, no MSL padding
// - into the words of the slot its handle names. A virtual call per member on a
// stack object, so it runs on every dispatch without allocating.

namespace eacp::GPU::CpuCompute
{
class CpuUniformVisitor final : public ShaderVisitor
{
public:
    CpuUniformVisitor(const ShaderGraph& graphToRead,
                      const Plan& planToFollow,
                      Word* uniformWordsToFill);

protected:
    void onUniform(const char*,
                   ValueType type,
                   detail::ValueHandle& handle,
                   const void* data) override;

private:
    const ShaderGraph& graph;
    const Plan& plan;
    Word* words;
};
} // namespace eacp::GPU::CpuCompute
