#include "Common.h"

#include <algorithm>
#include <chrono>
#include <sstream>
#include <thread>
#include <vector>

using namespace nano;
using eacp::Threads::AsyncPromise;
using eacp::Threads::callAsync;

namespace
{
using Clock = std::chrono::steady_clock;
using Millis = std::chrono::duration<double, std::milli>;

double measureWaitForLagAfterResolveMs()
{
    auto promise = AsyncPromise<int>();
    auto async = promise.get();
    auto resolvedAt = Clock::time_point {};

    auto stampResolve = [&resolvedAt](const int&) { resolvedAt = Clock::now(); };

    async.then(stampResolve);

    auto resolveLater = [promise]
    {
        eacp::Time::sleepMS(3);
        callAsync([promise] { promise.resolve(7); });
    };

    auto worker = std::thread(resolveLater);

    auto value = async.waitFor(eacp::Time::MS {1000});
    auto returnedAt = Clock::now();
    worker.join();

    check(value == 7);
    return Millis(returnedAt - resolvedAt).count();
}

double medianOf(std::vector<double> values)
{
    auto middle = values.begin() + (std::ptrdiff_t) (values.size() / 2);
    std::nth_element(values.begin(), middle, values.end());
    return *middle;
}

std::string describeLags(const std::vector<double>& lags)
{
    auto text = std::ostringstream();
    text << "median lag " << medianOf(lags) << " ms of";

    for (auto lag: lags)
        text << ' ' << lag;

    return text.str();
}
} // namespace

// Before the fix nearly every attempt came back a refresh late, a median of
// 4.47-4.51 ms; after it the median is 9-16 us. The bound is on the median, so
// the odd attempt a shared CI runner preempts past 2 ms does not fail it.
auto tWaitForReturnsOnResolve =
    test("EventLoop/waitFor/returnsOnResolveNotAFrameLater") = []
{
    constexpr auto attempts = 11;
    constexpr auto maxMedianLagMs = 2.0;

    auto lags = std::vector<double>();

    for (auto attempt = 0; attempt < attempts; ++attempt)
        lags.push_back(measureWaitForLagAfterResolveMs());

    check(medianOf(lags) < maxMedianLagMs, describeLags(lags));
};
