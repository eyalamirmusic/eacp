#include "Common.h"

#include <chrono>
#include <thread>

using namespace nano;
using eacp::Threads::AsyncPromise;
using eacp::Threads::callAsync;

auto tWaitForReturnsOnResolve =
    test("EventLoop/waitFor/returnsOnResolveNotAFrameLater") = []
{
    using Clock = std::chrono::steady_clock;

    for (auto attempt = 0; attempt < 10; ++attempt)
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

        auto lag =
            std::chrono::duration<double, std::milli>(returnedAt - resolvedAt);
        check(value == 7);
        check(lag.count() < 2.0);
    }
};
