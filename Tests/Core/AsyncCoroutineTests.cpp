#include "Common.h"

#include <thread>

using namespace nano;
using eacp::Threads::Async;
using eacp::Threads::AsyncError;
using eacp::Threads::AsyncPromise;
using eacp::Threads::callAsync;

using namespace std::chrono_literals;

namespace
{
Async<int> coroReturning(int value)
{
    co_return value;
}

Async<void> coroReturningVoid()
{
    co_return;
}

Async<int> coroAwaiting(Async<int> upstream)
{
    auto value = co_await std::move(upstream);
    co_return value + 1;
}

Async<int> coroThatThrows()
{
    throw std::runtime_error("boom");
    co_return 0;
}

Async<int> coroChain(AsyncPromise<int> a, AsyncPromise<int> b)
{
    auto x = co_await a.get();
    auto y = co_await b.get();
    co_return x + y;
}
} // namespace

auto tCoroReturnsValue = test("Async/coro/returnsValue") = []
{
    auto result = coroReturning(42).waitFor(eacp::Time::MS {1000});
    check(result == 42);
};

auto tCoroVoidResolves = test("Async/coro/voidResolves") = []
{
    auto a = coroReturningVoid();
    a.waitFor(eacp::Time::MS {1000});
    check(a.isResolved());
};

auto tCoroAwaitsAlreadyResolved = test("Async/coro/awaitsAlreadyResolved") = []
{
    auto producer = AsyncPromise<int>();
    producer.resolve(10);

    auto result = coroAwaiting(producer.get()).waitFor(eacp::Time::MS {1000});
    check(result == 11);
};

auto tCoroAwaitsPendingResolvedAsync =
    test("Async/coro/awaitsPendingResolvedViaCallAsync") = []
{
    auto producer = AsyncPromise<int>();
    auto coro = coroAwaiting(producer.get());

    callAsync([producer] { producer.resolve(5); });

    auto result = coro.waitFor(eacp::Time::MS {1000});
    check(result == 6);
};

auto tCoroExceptionBecomesRejection =
    test("Async/coro/exceptionBecomesRejection") = []
{
    auto coro = coroThatThrows();

    auto threw = false;
    try
    {
        coro.waitFor(eacp::Time::MS {1000});
    }
    catch (const AsyncError& e)
    {
        threw = true;
        check(std::string {e.what()} == "boom");
    }
    check(threw);
};

auto tCoroChainsMultipleAwaits = test("Async/coro/chainsMultipleAwaits") = []
{
    auto a = AsyncPromise<int>();
    auto b = AsyncPromise<int>();
    auto coro = coroChain(a, b);

    callAsync(
        [a, b]
        {
            a.resolve(3);
            b.resolve(4);
        });

    auto result = coro.waitFor(eacp::Time::MS {1000});
    check(result == 7);
};

auto tCoroResumesAfterWorkerThread = test("Async/coro/resumesAfterWorkerThread") = []
{
    auto producer = AsyncPromise<int>();
    auto coro = coroAwaiting(producer.get());

    auto worker = std::thread(
        [producer]
        {
            std::this_thread::sleep_for(30ms);
            callAsync([producer] { producer.resolve(100); });
        });

    auto result = coro.waitFor(eacp::Time::MS {2000});
    worker.join();

    check(result == 101);
};

auto tCoroRejectionPropagatesThroughAwait =
    test("Async/coro/rejectionPropagatesThroughAwait") = []
{
    auto producer = AsyncPromise<int>();
    auto coro = coroAwaiting(producer.get());

    callAsync([producer] { producer.reject("upstream failed"); });

    auto threw = false;
    try
    {
        coro.waitFor(eacp::Time::MS {1000});
    }
    catch (const AsyncError& e)
    {
        threw = true;
        check(std::string {e.what()} == "upstream failed");
    }
    check(threw);
};
