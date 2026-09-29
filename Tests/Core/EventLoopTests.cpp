#include "Common.h"

#include <array>
#include <chrono>
#include <thread>

using namespace nano;
using eacp::Threads::callAsync;
using eacp::Threads::runEventLoopUntil;

auto tReadyImmediatelyReturnsTrue = test("EventLoop/runUntil/readyImmediately") = []
{
    auto called = 0;
    auto ok = runEventLoopUntil(
        [&]
        {
            ++called;
            return true;
        },
        eacp::Time::MS {1000});

    check(ok);
    check(called == 1);
};

auto tCallAsyncFlipsPredicate =
    test("EventLoop/runUntil/callAsyncFlipsPredicate") = []
{
    auto flag = false;

    callAsync([&] { flag = true; });

    auto ok = runEventLoopUntil([&] { return flag; }, eacp::Time::MS {1000});

    check(ok);
    check(flag);
};

auto tTimeoutReturnsFalse = test("EventLoop/runUntil/timeoutReturnsFalse") = []
{
    auto flag = false;

    auto lowerBound = eacp::Time::Deadline {eacp::Time::MS {100}};
    auto upperBound = eacp::Time::Deadline {eacp::Time::MS {1000}};
    auto ok = runEventLoopUntil([&] { return flag; }, eacp::Time::MS {100});

    check(!ok);
    check(!flag);
    check(lowerBound.expired());
    check(!upperBound.expired());
};

auto tWorkerThreadFlipsPredicate =
    test("EventLoop/runUntil/workerThreadFlipsPredicate") = []
{
    auto flag = false;
    auto worker = std::thread(
        [&]
        {
            eacp::Time::sleepMS(50);
            callAsync([&] { flag = true; });
        });

    auto ok = runEventLoopUntil([&] { return flag; }, eacp::Time::MS {2000});
    worker.join();

    check(ok);
    check(flag);
};

using eacp::Threads::callAfter;
using eacp::Threads::isMainThread;

auto tCallAfterWaitsForTheDelay = test("EventLoop/callAfter/waitsForTheDelay") = []
{
    auto fired = false;

    auto lowerBound = eacp::Time::Deadline {eacp::Time::MS {150}};
    callAfter(eacp::Time::MS {150}, [&] { fired = true; });

    auto ok = runEventLoopUntil([&] { return fired; }, eacp::Time::MS {2000});

    check(ok);
    check(lowerBound.expired());
};

auto tCallAfterRunsOnMessageThread =
    test("EventLoop/callAfter/runsOnTheMessageThread") = []
{
    auto onMessageThread = false;
    auto fired = false;

    callAfter(eacp::Time::MS {10},
              [&]
              {
                  onMessageThread = isMainThread();
                  fired = true;
              });

    check(runEventLoopUntil([&] { return fired; }, eacp::Time::MS {2000}));
    check(onMessageThread);
};

auto tCallAfterZeroDelayIsCallAsync =
    test("EventLoop/callAfter/zeroDelayIsCallAsync") = []
{
    auto fired = false;
    callAfter(eacp::Time::MS {0}, [&] { fired = true; });

    check(runEventLoopUntil([&] { return fired; }, eacp::Time::MS {1000}));
};

// The reason callAfter exists: a rate limiter holds one deadline per bucket,
// and none of them may cost a thread of its own.
auto tCallAfterOrdersManyDeadlines =
    test("EventLoop/callAfter/ordersManyPendingDeadlines") = []
{
    using Clock = std::chrono::steady_clock;

    constexpr auto count = 64;
    auto order = EA::Vector<int>();
    auto earliest = std::array<Clock::time_point, count + 1> {};
    auto latest = earliest;

    // The delays run down while the calls take time, so on a busy machine a
    // shorter delay can still land after a longer one. Each deadline is only
    // known to lie between the clock before its call and the clock after it.
    for (auto i = count; i > 0; --i)
    {
        auto delay = std::chrono::milliseconds {i};
        earliest[i] = Clock::now() + delay;
        callAfter(eacp::Time::MS {i}, [&, i] { order.push_back(i); });
        latest[i] = Clock::now() + delay;
    }

    auto ok = runEventLoopUntil([&] { return order.size() == count; },
                                eacp::Time::MS {5000});

    check(ok);

    auto inDeadlineOrder = true;

    for (auto first = 0; first < (int) order.size(); ++first)
        for (auto then = first + 1; then < (int) order.size(); ++then)
            if (earliest[order[first]] > latest[order[then]])
                inDeadlineOrder = false;

    check(inDeadlineOrder, "a later deadline fired first");
};

auto tCallAfterNestsFromItsOwnCallback =
    test("EventLoop/callAfter/schedulesFromItsOwnCallback") = []
{
    auto ticks = 0;

    auto scheduleNext = [&](auto& self) -> void
    {
        if (++ticks < 5)
            callAfter(eacp::Time::MS {5}, [&] { self(self); });
    };

    callAfter(eacp::Time::MS {5}, [&] { scheduleNext(scheduleNext); });

    check(runEventLoopUntil([&] { return ticks == 5; }, eacp::Time::MS {5000}));
};
