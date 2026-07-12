#include "Common.h"

#include <thread>

using namespace nano;
using eacp::Threads::Async;
using eacp::Threads::AsyncError;
using eacp::Threads::AsyncPromise;
using eacp::Threads::callAsync;

using namespace std::chrono_literals;

auto tWaitForReturnsResolvedValue =
    test("Async/waitFor/resolvedValueIsReturned") = []
{
    auto promise = AsyncPromise<int>();
    auto async = promise.get();

    callAsync([promise] { promise.resolve(42); });

    auto value = async.waitFor(1s);

    check(value == 42);
};

auto tWaitForThrowsOnReject = test("Async/waitFor/rejectionThrowsAsyncError") = []
{
    auto promise = AsyncPromise<int>();
    auto async = promise.get();

    callAsync([promise] { promise.reject("boom"); });

    auto threw = false;
    try
    {
        async.waitFor(1s);
    }
    catch (const AsyncError& e)
    {
        threw = true;
        check(std::string {e.what()} == "boom");
    }

    check(threw);
};

auto tWaitForThrowsOnTimeout = test("Async/waitFor/timeoutThrowsAsyncError") = []
{
    auto promise = AsyncPromise<int>();
    auto async = promise.get();

    auto threw = false;
    try
    {
        async.waitFor(50ms);
    }
    catch (const AsyncError&)
    {
        threw = true;
    }

    check(threw);
    check(!async.isReady());
};

auto tAlreadyResolvedReturnsWithoutPumping =
    test("Async/waitFor/alreadyResolvedSkipsLoop") = []
{
    auto promise = AsyncPromise<std::string>();
    auto async = promise.get();
    promise.resolve("hello");

    auto value = async.waitFor(1s);

    check(value == "hello");
    check(async.isReady());
};

auto tVoidWaitForResolves = test("Async/waitFor/voidResolves") = []
{
    auto promise = AsyncPromise<>();
    auto async = promise.get();

    callAsync([promise] { promise.resolve(); });

    async.waitFor(1s);
    check(async.isResolved());
};

auto tVoidWaitForRejects = test("Async/waitFor/voidRejects") = []
{
    auto promise = AsyncPromise<>();
    auto async = promise.get();

    callAsync([promise] { promise.reject("nope"); });

    auto threw = false;
    try
    {
        async.waitFor(1s);
    }
    catch (const AsyncError& e)
    {
        threw = true;
        check(std::string {e.what()} == "nope");
    }
    check(threw);
};

auto tThenFiresAfterResolve = test("Async/then/firesAfterResolve") = []
{
    auto promise = AsyncPromise<int>();
    auto async = promise.get();

    auto received = 0;
    async.then([&](int v) { received = v; });

    callAsync(
        [promise]
        {
            promise.resolve(7);
            callAsync([] { eacp::Threads::stopEventLoop(); });
        });
    eacp::Threads::runEventLoopFor(1s);

    check(received == 7);
};

auto tThenFiresImmediatelyIfAlreadyResolved =
    test("Async/then/firesImmediatelyIfAlreadyResolved") = []
{
    auto promise = AsyncPromise<int>();
    promise.resolve(99);
    auto async = promise.get();

    auto received = 0;
    async.then([&](int v) { received = v; });

    check(received == 99);
};

auto tThenInvokesErrorCallback = test("Async/then/invokesErrorCallback") = []
{
    auto promise = AsyncPromise<int>();
    auto async = promise.get();

    auto received = std::string();
    async.then([](int) {}, [&](const std::string& e) { received = e; });

    callAsync(
        [promise]
        {
            promise.reject("bad");
            callAsync([] { eacp::Threads::stopEventLoop(); });
        });
    eacp::Threads::runEventLoopFor(1s);

    check(received == "bad");
};

auto tWorkerThreadResolvesViaCallAsync =
    test("Async/waitFor/workerThreadResolves") = []
{
    auto promise = AsyncPromise<int>();
    auto async = promise.get();

    auto worker = std::thread(
        [promise]
        {
            std::this_thread::sleep_for(30ms);
            callAsync([promise] { promise.resolve(123); });
        });

    auto value = async.waitFor(2s);
    worker.join();

    check(value == 123);
};
