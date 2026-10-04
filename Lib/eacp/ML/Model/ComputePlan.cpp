#include "ComputePlan.h"

namespace eacp::ML
{
bool ComputePlan::isEmpty() const
{
    return ops.empty();
}

bool ComputePlan::allOn(Device device) const
{
    auto isElsewhere = [device](const Op& op) { return op.device != device; };
    return !ops.empty() && ops.findIf(isElsewhere) == nullptr;
}

const ComputePlan::Op* ComputePlan::find(const std::string& type) const
{
    auto matches = [&type](const Op& op) { return op.type == type; };
    return ops.findIf(matches);
}
} // namespace eacp::ML
