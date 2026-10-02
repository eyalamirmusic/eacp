#include "CallCost.h"

#include <algorithm>
#include <vector>

namespace eacp::GPU
{
namespace
{
struct CallCostRegistry
{
    std::mutex mutex;
    std::vector<const CallCostCounter*> counters;

    static CallCostRegistry& get()
    {
        static auto registry = CallCostRegistry {};
        return registry;
    }
};
} // namespace

CallCostCounter::CallCostCounter(std::string label)
{
    running.label = std::move(label);

    auto& registry = CallCostRegistry::get();
    auto lock = std::scoped_lock {registry.mutex};
    registry.counters.push_back(this);
}

CallCostCounter::~CallCostCounter()
{
    auto& registry = CallCostRegistry::get();
    auto lock = std::scoped_lock {registry.mutex};
    std::erase(registry.counters, this);
}

void CallCostCounter::record(double seconds, std::int64_t bytes)
{
    auto lock = std::scoped_lock {mutex};
    ++running.calls;
    running.seconds += seconds;
    running.bytes += bytes;
}

CallCost CallCostCounter::total() const
{
    auto lock = std::scoped_lock {mutex};
    return running;
}

Vector<CallCost> callCosts()
{
    auto& registry = CallCostRegistry::get();
    auto lock = std::scoped_lock {registry.mutex};
    auto totals = Vector<CallCost> {};

    for (const auto* counter: registry.counters)
        totals.add(counter->total());

    return totals;
}
} // namespace eacp::GPU
