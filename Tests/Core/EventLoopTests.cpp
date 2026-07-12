#include "Common.h"

#include <thread>

using namespace nano;
using eacp::Threads::callAsync;
using eacp::Threads::runEventLoopUntil;

using namespace std::chrono_literals;

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

    auto start = std::chrono::steady_clock::now();
    auto ok = runEventLoopUntil([&] { return flag; }, eacp::Time::MS {100});
    auto elapsed = std::chrono::steady_clock::now() - start;

    check(!ok);
    check(!flag);
    check(elapsed >= 100ms);
    check(elapsed < 1s);
};

auto tWorkerThreadFlipsPredicate =
    test("EventLoop/runUntil/workerThreadFlipsPredicate") = []
{
    auto flag = false;
    auto worker = std::thread(
        [&]
        {
            std::this_thread::sleep_for(50ms);
            callAsync([&] { flag = true; });
        });

    auto ok = runEventLoopUntil([&] { return flag; }, eacp::Time::MS {2000});
    worker.join();

    check(ok);
    check(flag);
};
