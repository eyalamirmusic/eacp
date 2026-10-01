#pragma once

#include "../Common.h"

namespace eacp::ML
{
// Where Core ML placed each op of a loaded model, read back through
// MLComputePlan (macOS 14.4 / iOS 17.4, ML::hasComputePlan()). Constants are
// left out: they run nowhere.
struct ComputePlan
{
    enum class Device
    {
        cpu,
        gpu,
        neuralEngine,
        unknown
    };

    struct Op
    {
        // The name of the op's first output, which is what a graph named it.
        std::string name;

        // The MIL operator: "linear", "softmax", ...
        std::string type;

        Device device = Device::unknown;
        Vector<Device> supported;

        // Core ML's relative estimate, the ops of one plan summing to about 1.
        // Negative when the plan gave none.
        double cost = -1.0;
    };

    bool isEmpty() const { return ops.empty(); }

    bool allOn(Device device) const
    {
        auto isElsewhere = [device](const Op& op) { return op.device != device; };
        return !ops.empty() && ops.findIf(isElsewhere) == nullptr;
    }

    const Op* find(const std::string& type) const
    {
        auto matches = [&type](const Op& op) { return op.type == type; };
        return ops.findIf(matches);
    }

    Vector<Op> ops;
};

std::string toString(ComputePlan::Device device);
} // namespace eacp::ML
