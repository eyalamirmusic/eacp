#include <eacp/Core/Testing/AsyncTest.h>
#include <eacp/Core/Threads/Async.h>
#include <eacp/Core/Threads/EventLoop.h>

#include <NanoTest/NanoTest.h>

#include <chrono>

using namespace nano;
using namespace std::chrono_literals;
using eacp::Testing::asyncTest;
using eacp::Threads::Async;
using eacp::Threads::AsyncPromise;
using eacp::Threads::callAsync;
using eacp::Threads::delay;

auto tDelayResolvesAfterDuration = test("Async/delay/resolvesAfterDuration") = []
{
    auto async = delay(40ms);
    auto start = std::chrono::steady_clock::now();
    async.waitFor(2s);
    auto elapsed = std::chrono::steady_clock::now() - start;

    check(async.isResolved());
    check(elapsed >= 40ms);
    check(elapsed < 1s);
};

auto tDelayUnblocksCoroutine = test("Async/delay/unblocksCoroutine") = []
{
    auto coro = []() -> Async<int>
    {
        co_await delay(20ms);
        co_await delay(20ms);
        co_return 7;
    };

    auto value = coro().waitFor(2s);
    check(value == 7);
};

auto tAsyncTestRunsCoroutineToCompletion =
    asyncTest("Async/asyncTest/runsCoroutineToCompletion") = []() -> Async<void>
{
    auto promise = AsyncPromise<int>();
    callAsync([promise] { promise.resolve(11); });
    auto value = co_await promise.get();
    check(value == 11);
};

// The fact that this test body has no co_return at all — yet the
// asyncTest plumbing still drives it to completion — is the whole
// point of the wrapper. Async<void>'s promise_type implicitly calls
// return_void() when control falls off the end, so the user never
// types it.
auto tAsyncTestNoExplicitReturn =
    asyncTest("Async/asyncTest/noExplicitReturnRequired") = []() -> Async<void>
{
    co_await delay(10ms);
    check(true);
};

auto tAsyncTestSurfacesCheckFailureWithoutHanging =
    asyncTest("Async/asyncTest/awaitsMultipleDelaysWithoutBlockingMainThread")
    = []() -> Async<void>
{
    // Two sequential delays: each yields the event loop. If delay()
    // accidentally blocked, the test would still pass (the outer
    // waitFor pumps), but elapsed time would balloon. Use a tight
    // upper bound to catch a regression to blocking.
    auto start = std::chrono::steady_clock::now();
    co_await delay(15ms);
    co_await delay(15ms);
    auto elapsed = std::chrono::steady_clock::now() - start;

    check(elapsed >= 30ms);
    check(elapsed < 500ms);
};
