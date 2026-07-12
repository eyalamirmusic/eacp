#include "Common.h"

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
