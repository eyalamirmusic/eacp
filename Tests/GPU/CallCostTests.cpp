#include "Common.h"

#include <eacp/GPU/Timing/CallCost.h>

// CallCostCounter - what the CPU spends inside one kind of driver call, summed,
// and listed by callCosts() for as long as the counter lives.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
const CallCost* findCost(const Vector<CallCost>& costs, const std::string& label)
{
    for (const auto& cost: costs)
        if (cost.label == label)
            return &cost;

    return nullptr;
}
} // namespace

auto tCallCostSumsEveryCall = test("CallCost/sumsEveryCallAndItsBytes") = []
{
    auto counter = CallCostCounter {"test-creations"};

    counter.record(0.25, 1024);
    counter.record(0.75, 2048);

    {
        auto cost = ScopedCallCost {counter, 512};
    }

    auto total = counter.total();

    check(total.label == "test-creations");
    check(total.calls == 3);
    check(total.seconds >= 1.0);
    check(total.bytes == 1024 + 2048 + 512);
    check(total.meanMicroseconds() >= 1e6 / 3.0);
};

auto tCallCostsListsTheLiving = test("CallCost/listsOnlyTheCountersAlive") = []
{
    {
        auto counter = CallCostCounter {"test-waits"};
        counter.record(0.5);

        auto costs = callCosts();
        auto listed = findCost(costs, "test-waits");
        check(listed != nullptr && listed->calls == 1);
    }

    auto afterwards = callCosts();
    check(findCost(afterwards, "test-waits") == nullptr);
};
