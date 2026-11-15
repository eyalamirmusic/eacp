#include "Common.h"

#include <eacp/Core/Threads/ThreadPool.h>

using namespace nano;

// Platform::hasThreads() is false: a Thread refuses honestly and a ThreadPool
// has no threads, running its work on the main thread on later loop turns.
namespace ThreadlessTests
{
using namespace eacp;
using namespace eacp::Threads;

constexpr auto bound = Time::MS {5000};

auto tThreadDoesNotRun = test("Threadless/aThreadDoesNotRunItsBody") = []
{
    check(!Platform::hasThreads());

    auto ran = false;
    auto thread = Thread {[&ran] { ran = true; }};

    check(!ran);
    check(!thread.isJoinable());
    check(!thread.isRealtime());

    thread.start([&ran] { ran = true; });
    thread.join();
    check(!ran);
};

auto tPoolHasNoThreads = test("Threadless/aPoolHasNoThreads") = []
{
    auto pool = ThreadPool {{.threadCount = 4}};
    pool.submit([] {});

    check(ThreadPool::defaultThreadCount() == 0);
    check(pool.threadCount() == 0);
    check(pool.startedThreadCount() == 0);
    check(pool.realtimeThreadCount() == 0);
    check(!pool.isPoolThread());
};

auto tOnePerTurn = test("Threadless/poolWorkRunsOneTaskPerLoopTurn") = []
{
    auto pool = ThreadPool {};
    auto order = std::vector<std::string> {};

    pool.submit([&order] { order.push_back("a"); });
    pool.submit([&order] { order.push_back("b"); });
    callAsync([&order] { order.push_back("turn"); });

    check(order.empty());
    check(runEventLoopUntil([&] { return order.size() == 3; }, bound));
    check(order == std::vector<std::string> {"a", "turn", "b"});
    check(pool.isIdle());
};

auto tMainThreadCallbacks = test("Threadless/onMainThreadAndOnCancelled") = []
{
    auto pool = ThreadPool {};
    auto events = std::vector<std::string> {};

    pool.submit(PoolTask {.work = [&events] { events.push_back("work"); },
                          .onMainThread = [&events] { events.push_back("done"); }});

    pool.submit(
        PoolTask {.work = [&events] { events.push_back("never"); },
                  .onMainThread = [&events] { events.push_back("never"); },
                  .onCancelled = [&events] { events.push_back("cancelled"); }});

    check(pool.cancelPending() == 2);
    check(runEventLoopUntil([&] { return pool.isIdle(); }, bound));
    check(events == std::vector<std::string> {"cancelled"});

    events.clear();
    pool.submit(PoolTask {.work = [&events] { events.push_back("work"); },
                          .onMainThread = [&events] { events.push_back("done"); }});

    check(runEventLoopUntil([&] { return pool.isIdle(); }, bound));
    check(events == std::vector<std::string> {"work", "done"});
};

auto tWaitRunsInline = test("Threadless/waitUntilIdleRunsTheQueueInline") = []
{
    auto pool = ThreadPool {};
    auto ran = 0;
    auto delivered = 0;

    for (auto i = 0; i < 3; ++i)
        pool.submit(PoolTask {.work = [&ran] { ++ran; },
                              .onMainThread = [&delivered] { ++delivered; }});

    pool.waitUntilIdle();

    check(ran == 3);
    check(delivered == 3);
    check(pool.isIdle());
};

auto tGroupWaitRunsInline = test("Threadless/aGroupWaitRunsOnlyItsOwnTasks") = []
{
    auto pool = ThreadPool {};
    auto outside = false;
    auto inside = 0;

    pool.submit([&outside] { outside = true; });

    {
        auto group = TaskGroup {pool};
        group.submit([&inside] { ++inside; });
        group.submit([&inside] { ++inside; });
        group.wait();

        check(inside == 2);
        check(group.isIdle());
        check(!outside);
    }

    pool.waitUntilIdle();
    check(outside);
};

auto tGroupDestroyedByItsTask =
    test("Threadless/aGroupDestroyedByItsOwnTaskDeliversNothing") = []
{
    auto pool = ThreadPool {};
    auto group = std::make_unique<TaskGroup>(pool);
    auto delivered = false;

    group->submit(PoolTask {.work =
                                [&]
                            {
                                group.reset();
                                group = std::make_unique<TaskGroup>(pool);
                            },
                            .onMainThread = [&delivered] { delivered = true; }});

    check(runEventLoopUntil([&] { return pool.isIdle(); }, bound));
    check(!delivered);
    check(group->isIdle());
};

auto tGroupDestroyedByItsCallback =
    test("Threadless/aGroupDestroyedByItsOwnCallbackStaysConsistent") = []
{
    auto pool = ThreadPool {};
    auto group = std::make_unique<TaskGroup>(pool);
    auto delivered = false;

    group->submit(PoolTask {.onMainThread = [&]
                            {
                                delivered = true;
                                group.reset();
                                group = std::make_unique<TaskGroup>(pool);
                            }});

    check(runEventLoopUntil([&] { return pool.isIdle(); }, bound));
    check(delivered);
    check(group->isIdle());
};

auto tTaskIsOnAPoolThread = test("Threadless/aRunningTaskCountsAsAPoolThread") = []
{
    auto pool = ThreadPool {};
    auto inWork = false;
    auto inCallback = true;

    pool.submit(
        PoolTask {.work = [&] { inWork = pool.isPoolThread(); },
                  .onMainThread = [&] { inCallback = pool.isPoolThread(); }});

    check(!pool.isPoolThread());
    pool.waitUntilIdle();

    check(inWork);
    check(!inCallback);
    check(!pool.isPoolThread());
};

auto tGroupWaitInsideATask =
    test("Threadless/aGroupWaitInsideATaskRunsTheGroupsTasks") = []
{
    auto pool = ThreadPool {};
    auto group = TaskGroup {pool};
    auto inner = 0;
    auto innerAtWait = -1;

    pool.submit(
        [&]
        {
            group.submit([&inner] { ++inner; });
            group.wait();
            innerAtWait = inner;
        });

    pool.waitUntilIdle();
    check(innerAtWait == 1);
    check(group.isIdle());
};

auto tRunAsyncResolves = test("Threadless/runAsyncResolvesWithoutAThread") = []
{
    auto options = Processes::ProcessOptions {};
    options.executable = "/bin/sh";

    auto result = std::optional<Processes::ProcessResult> {};
    Processes::runAsync(options).then([&result](auto value) { result = value; });

    check(runEventLoopUntil([&] { return result.has_value(); }, bound));
    check(!result->launched);
};
} // namespace ThreadlessTests
