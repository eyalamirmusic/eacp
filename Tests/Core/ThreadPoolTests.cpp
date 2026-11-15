#include "Common.h"
#include "Gate.h"
#include "ThreadNames.h"

#include <eacp/Core/Threads/ThreadPool.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace nano;

namespace ThreadPoolTests
{
using namespace eacp;
using namespace eacp::Threads;

constexpr auto bound = threadTestBound;

ThreadPoolOptions withThreads(int count)
{
    return {.threadCount = count};
}

void blockOn(ThreadPool& pool, Gate& started, Gate& release)
{
    pool.submit(
        [&]
        {
            started.open();
            release.wait();
        });

    check(started.wait());
}

int submitUntilRefused(ThreadPool& pool)
{
    for (auto attempt = 0; attempt < 5000; ++attempt)
    {
        if (!pool.submit([] {}))
            return attempt;

        Time::sleep(Time::MS {1});
    }

    return -1;
}

auto tNoThreadBeforeSubmit =
    test("ThreadPool/startsNoThreadBeforeTheFirstSubmit") = []
{
    auto pool = ThreadPool {withThreads(4)};
    check(pool.startedThreadCount() == 0);
    check(pool.isIdle());

    pool.submit([] {});
    check(pool.startedThreadCount() == 1);
    check(pool.waitUntilIdleFor(bound));
};

auto tRunsOffMain = test("ThreadPool/taskRunsOffTheMainThread") = []
{
    auto pool = ThreadPool {withThreads(2)};
    auto onMain = std::atomic<bool> {true};
    auto onPoolThread = std::atomic<bool> {false};

    pool.submit(
        [&]
        {
            onMain = isMainThread();
            onPoolThread = pool.isPoolThread();
        });

    check(pool.waitUntilIdleFor(bound));
    check(!onMain);
    check(onPoolThread);
    check(!pool.isPoolThread());
};

auto tExactlyOnce = test("ThreadPool/everyTaskRunsExactlyOnce") = []
{
    auto pool = ThreadPool {withThreads(4)};
    auto runs = std::vector<std::atomic<int>>(1000);

    for (auto& count: runs)
        check(pool.submit([&count] { ++count; }));

    check(pool.waitUntilIdleFor(bound));
    check(pool.isIdle());

    for (auto& count: runs)
        check(count == 1);
};

auto tBoundedConcurrency = test("ThreadPool/neverRunsMoreThanThreadCountAtOnce") = []
{
    auto pool = ThreadPool {withThreads(2)};
    auto active = std::atomic<int> {0};
    auto maxActive = std::atomic<int> {0};

    for (auto i = 0; i < 64; ++i)
    {
        pool.submit(
            [&]
            {
                auto now = ++active;
                auto seen = maxActive.load();

                while (now > seen && !maxActive.compare_exchange_weak(seen, now))
                {
                }

                std::this_thread::yield();
                --active;
            });
    }

    check(pool.waitUntilIdleFor(bound));
    check(maxActive <= 2);
    check(pool.startedThreadCount() <= 2);
};

auto tSubmissionOrder = test("ThreadPool/oneThreadRunsInSubmissionOrder") = []
{
    auto pool = ThreadPool {withThreads(1)};
    auto order = std::vector<int> {};

    for (auto i = 0; i < 100; ++i)
        pool.submit([&order, i] { order.push_back(i); });

    check(pool.waitUntilIdleFor(bound));
    check(order.size() == 100);

    for (auto i = 0; i < (int) order.size(); ++i)
        check(order[(size_t) i] == i);
};

auto tNestedSubmit = test("ThreadPool/taskMaySubmitTasks") = []
{
    auto pool = ThreadPool {withThreads(2)};
    auto children = std::atomic<int> {0};

    pool.submit(
        [&]
        {
            for (auto i = 0; i < 10; ++i)
                pool.submit([&] { ++children; });
        });

    check(pool.waitUntilIdleFor(bound));
    check(children == 10);
};

auto tCancelPending = test("ThreadPool/cancelPendingDropsUnstartedTasks") = []
{
    auto pool = ThreadPool {withThreads(1)};
    auto started = Gate {};
    auto release = Gate {};
    auto ran = std::atomic<int> {0};
    auto cancelled = 0;
    auto cancelledOnMain = true;

    blockOn(pool, started, release);

    for (auto i = 0; i < 5; ++i)
    {
        pool.submit(PoolTask {.work = [&] { ++ran; },
                              .onCancelled =
                                  [&]
                              {
                                  ++cancelled;
                                  cancelledOnMain &= isMainThread();
                              }});
    }

    check(pool.cancelPending() == 5);
    release.open();

    check(pool.waitUntilIdleFor(bound));
    check(ran == 0);
    check(cancelled == 5);
    check(cancelledOnMain);
};

auto tWaitDelivers =
    test("ThreadPool/waitUntilIdleDeliversMainThreadCallbacksWithoutTheLoop") = []
{
    auto pool = ThreadPool {withThreads(2)};
    auto worked = std::atomic<bool> {false};
    auto delivered = false;
    auto deliveredOnMain = false;

    pool.submit(PoolTask {.work = [&] { worked = true; },
                          .onMainThread =
                              [&]
                          {
                              delivered = worked.load();
                              deliveredOnMain = isMainThread();
                          }});

    check(pool.waitUntilIdleFor(bound));
    check(delivered);
    check(deliveredOnMain);
};

auto tLoopDelivers = test("ThreadPool/mainThreadCallbackArrivesThroughTheLoop") = []
{
    auto pool = ThreadPool {withThreads(2)};
    auto delivered = false;

    pool.submit(PoolTask {.onMainThread = [&] { delivered = true; }});

    check(runEventLoopUntil([&] { return delivered; }, bound));
    check(pool.isIdle());
};

auto tThrowingTask = test("ThreadPool/throwingTaskLeavesTheThreadRunning") = []
{
    auto pool = ThreadPool {withThreads(1)};
    auto after = std::atomic<bool> {false};
    auto delivered = false;

    pool.submit([] { throw std::runtime_error {"boom"}; });
    pool.submit(PoolTask {.work = [] { throw 42; },
                          .onMainThread = [&] { delivered = true; }});
    pool.submit([&] { after = true; });

    check(pool.waitUntilIdleFor(bound));
    check(after);
    check(!delivered);
    check(pool.startedThreadCount() == 1);
};

auto tGroupWaitOwnTasks = test("ThreadPool/group/waitCoversOnlyItsOwnTasks") = []
{
    auto pool = ThreadPool {withThreads(2)};
    auto started = Gate {};
    auto release = Gate {};
    auto done = std::atomic<int> {0};

    blockOn(pool, started, release);

    {
        auto group = TaskGroup {pool};

        for (auto i = 0; i < 3; ++i)
            group.submit([&] { ++done; });

        check(group.waitFor(bound));
        check(group.isIdle());
        check(done == 3);
        check(!pool.isIdle());
    }

    release.open();
    check(pool.waitUntilIdleFor(bound));
};

auto tGroupWaitInTask =
    test("ThreadPool/group/waitInsideATaskRunsTheGroupsTasks") = []
{
    auto pool = ThreadPool {withThreads(1)};
    auto children = std::atomic<int> {0};
    auto waited = std::atomic<bool> {false};

    pool.submit(
        [&]
        {
            auto group = TaskGroup {pool};

            for (auto i = 0; i < 3; ++i)
                group.submit([&] { ++children; });

            waited = group.waitFor(bound);
        });

    check(pool.waitUntilIdleFor(bound));
    check(waited);
    check(children == 3);
};

auto tGroupWaitWakes =
    test("ThreadPool/group/waitInsideATaskRunsTasksSubmittedLater") = []
{
    auto pool = ThreadPool {withThreads(1)};
    auto group = TaskGroup {pool};
    auto waiting = Gate {};
    auto childRan = Gate {};
    auto waited = std::atomic<bool> {false};

    group.submit(PoolTask {});
    pool.submit(
        [&]
        {
            waiting.open();
            waited = group.waitFor(bound);
        });

    check(waiting.wait());
    group.submit([&] { childRan.open(); });
    check(childRan.wait());

    check(group.waitFor(bound));
    check(pool.waitUntilIdleFor(bound));
    check(waited);
};

auto tGroupDestructionCancels =
    test("ThreadPool/group/destructionCancelsWhatHasNotStarted") = []
{
    auto pool = ThreadPool {withThreads(1)};
    auto started = Gate {};
    auto release = Gate {};
    auto ran = std::atomic<int> {0};
    auto callbacks = 0;

    blockOn(pool, started, release);

    {
        auto group = TaskGroup {pool};

        for (auto i = 0; i < 3; ++i)
            group.submit(PoolTask {.work = [&] { ++ran; },
                                   .onMainThread = [&] { ++callbacks; },
                                   .onCancelled = [&] { ++callbacks; }});
    }

    release.open();
    check(pool.waitUntilIdleFor(bound));
    runEventLoopFor(Time::MS {20});

    check(ran == 0);
    check(callbacks == 0);
};

auto tGroupDestructionWaits =
    test("ThreadPool/group/destructionWaitsForRunningWork") = []
{
    auto pool = ThreadPool {withThreads(1)};
    auto started = Gate {};
    auto finished = std::atomic<bool> {false};

    {
        auto group = TaskGroup {pool};

        group.submit(
            [&]
            {
                started.open();
                Time::sleep(Time::MS {100});
                finished = true;
            });

        check(started.wait());
    }

    check(finished);
    check(pool.isIdle());
};

auto tGroupDestructionDrops =
    test("ThreadPool/group/destructionDropsItsMainThreadCallbacks") = []
{
    auto pool = ThreadPool {withThreads(1)};
    auto worked = Gate {};
    auto delivered = false;

    {
        auto group = TaskGroup {pool};
        group.submit(PoolTask {.work = [&] { worked.open(); },
                               .onMainThread = [&] { delivered = true; }});
        check(worked.wait());
    }

    runEventLoopFor(Time::MS {20});
    check(!delivered);
    check(pool.isIdle());
};

auto tTimedWait = test("ThreadPool/waitUntilIdleForTimesOutWhileATaskRuns") = []
{
    auto pool = ThreadPool {withThreads(1)};
    auto started = Gate {};
    auto release = Gate {};

    blockOn(pool, started, release);

    check(!pool.waitUntilIdleFor(Time::MS {50}));
    check(!pool.isIdle());

    release.open();
    check(pool.waitUntilIdleFor(bound));
};

auto tStopDrops =
    test("ThreadPool/stop/dropsQueuedTasksAndUndeliveredCallbacks") = []
{
    auto pool = ThreadPool {withThreads(1)};
    auto started = Gate {};
    auto ran = std::atomic<int> {0};
    auto callbacks = 0;
    auto refusedAfter = std::atomic<int> {-2};

    pool.submit(PoolTask {.onMainThread = [&] { ++callbacks; }});

    pool.submit(
        [&]
        {
            started.open();
            refusedAfter = submitUntilRefused(pool);
        });

    check(started.wait());

    for (auto i = 0; i < 3; ++i)
        pool.submit(PoolTask {.work = [&] { ++ran; },
                              .onMainThread = [&] { ++callbacks; },
                              .onCancelled = [&] { ++callbacks; }});

    pool.stop();
    runEventLoopFor(Time::MS {20});

    check(refusedAfter >= 0);
    check(ran == 0);
    check(callbacks == 0);
    check(pool.isIdle());
    check(pool.startedThreadCount() == 0);
};

// Destroyed by the pool, and calls back into it from its destructor.
struct PoolReentrantProbe
{
    ~PoolReentrantProbe()
    {
        [[maybe_unused]] auto idle = pool.isIdle();
        pool.submit([] {});
        ++destroyed;
    }

    ThreadPool& pool;
    std::atomic<int>& destroyed;
};

std::shared_ptr<PoolReentrantProbe> reentrantProbe(ThreadPool& pool,
                                                   std::atomic<int>& destroyed)
{
    return std::shared_ptr<PoolReentrantProbe>(
        new PoolReentrantProbe {pool, destroyed});
}

// On another thread, so a deadlock fails the run instead of hanging it.
void finishesWithinBound(const std::function<void()>& action)
{
    auto done = Gate {};
    auto helper = std::thread {[&]
                               {
                                   action();
                                   done.open();
                               }};

    if (!done.wait())
    {
        LOG("ThreadPool deadlocked dropping a task");
        std::abort();
    }

    helper.join();
}

auto tReentrantCancel =
    test("ThreadPool/drop/cancelPendingDestroysTasksOutsideTheLock") = []
{
    auto pool = ThreadPool {withThreads(1)};
    auto started = Gate {};
    auto release = Gate {};
    auto destroyed = std::atomic<int> {0};

    blockOn(pool, started, release);

    {
        auto workProbe = reentrantProbe(pool, destroyed);
        auto callbackProbe = reentrantProbe(pool, destroyed);
        auto plainProbe = reentrantProbe(pool, destroyed);

        pool.submit(
            PoolTask {.work = [workProbe] {}, .onMainThread = [callbackProbe] {}});
        pool.submit([plainProbe] {});
    }

    auto cancelled = 0;
    finishesWithinBound([&] { cancelled = pool.cancelPending(); });
    check(cancelled == 2);
    check(destroyed == 3);

    release.open();
    check(pool.waitUntilIdleFor(bound));
};

auto tReentrantStop = test("ThreadPool/drop/stopDestroysTasksOutsideTheLock") = []
{
    auto pool = ThreadPool {withThreads(1)};
    auto worked = Gate {};
    auto started = Gate {};
    auto destroyed = std::atomic<int> {0};

    {
        auto callbackProbe = reentrantProbe(pool, destroyed);
        pool.submit(PoolTask {.work = [&] { worked.open(); },
                              .onMainThread = [callbackProbe] {}});
    }

    check(worked.wait());

    pool.submit(
        [&]
        {
            started.open();
            submitUntilRefused(pool);
        });

    check(started.wait());

    {
        auto queuedProbe = reentrantProbe(pool, destroyed);
        pool.submit([queuedProbe] {});
    }

    finishesWithinBound([&] { pool.stop(); });
    check(destroyed == 2);
    check(pool.waitUntilIdleFor(bound));
};

auto tReentrantGroup =
    test("ThreadPool/drop/groupDestructionDestroysTasksOutsideTheLock") = []
{
    auto pool = ThreadPool {withThreads(1)};
    auto worked = Gate {};
    auto started = Gate {};
    auto release = Gate {};
    auto destroyed = std::atomic<int> {0};
    auto group = std::optional<TaskGroup> {};
    group.emplace(pool);

    {
        auto callbackProbe = reentrantProbe(pool, destroyed);
        group->submit(PoolTask {.work = [&] { worked.open(); },
                                .onMainThread = [callbackProbe] {}});
    }

    check(worked.wait());
    blockOn(pool, started, release);

    {
        auto queuedProbe = reentrantProbe(pool, destroyed);
        group->submit([queuedProbe] {});
    }

    finishesWithinBound([&] { group.reset(); });
    check(destroyed == 2);

    release.open();
    check(pool.waitUntilIdleFor(bound));
};

auto tStopRestarts = test("ThreadPool/stop/submitAfterwardsStartsThreadsAgain") = []
{
    auto pool = ThreadPool {withThreads(2)};
    auto runs = std::atomic<int> {0};

    pool.submit([&] { ++runs; });
    check(pool.waitUntilIdleFor(bound));
    check(pool.startedThreadCount() >= 1);

    pool.stop();
    check(pool.startedThreadCount() == 0);

    check(pool.submit([&] { ++runs; }));
    check(pool.startedThreadCount() == 1);
    check(pool.waitUntilIdleFor(bound));
    check(runs == 2);
};

auto tSubmitWhileStopping = test("ThreadPool/submitWhileStoppingIsRefused") = []
{
    auto pool = ThreadPool {withThreads(1)};
    auto started = Gate {};
    auto refused = std::atomic<bool> {false};
    auto callbacks = std::make_shared<std::atomic<int>>(0);

    pool.submit(
        [&]
        {
            started.open();
            submitUntilRefused(pool);

            auto task = PoolTask {.work = [callbacks] { ++*callbacks; },
                                  .onMainThread = [callbacks] { ++*callbacks; },
                                  .onCancelled = [callbacks] { ++*callbacks; }};
            refused = !pool.submit(task);
        });

    check(started.wait());
    pool.stop();
    runEventLoopFor(Time::MS {20});

    check(refused);
    check(*callbacks == 0);
};

auto tShared = test("ThreadPool/shared/isOneInstanceWithOneThreadPerCore") = []
{
    auto& pool = ThreadPool::shared();
    auto ran = std::atomic<bool> {false};

    check(&pool == &ThreadPool::shared());
    check(ThreadPool::defaultThreadCount() >= 1);
    check(pool.threadCount() == ThreadPool::defaultThreadCount());

    auto group = TaskGroup {};
    group.submit([&] { ran = true; });
    check(group.waitFor(bound));
    check(ran);
};

auto tDefaults = test("ThreadPool/options/defaultsAreOnePerCoreNormalPriority") = []
{
    auto options = ThreadPoolOptions {};
    check(options.threadCount == 0);
    check(options.thread.name == "EACP Pool");
    check(options.thread.priority == ThreadPriority::Normal);
    check(!options.thread.realtime.has_value());

    auto pool = ThreadPool {};
    check(pool.threadCount() == ThreadPool::defaultThreadCount());
    check(pool.options().thread.name == "EACP Pool");
    check(pool.realtimeThreadCount() == 0);
};

struct PoolThreadSample
{
    std::string name;
    RealtimeProbe realtime;
};

// One task per thread: each holds its thread until all have arrived.
std::vector<PoolThreadSample> sampleEachThread(ThreadPool& pool, int tasks)
{
    auto mutex = std::mutex {};
    auto samples = std::vector<PoolThreadSample> {};
    auto release = Gate {};
    auto arrived = std::atomic<int> {0};

    for (auto i = 0; i < tasks; ++i)
    {
        pool.submit(
            [&]
            {
                {
                    auto lock = std::lock_guard(mutex);
                    samples.push_back(
                        {currentThreadNameForTest(), probeCurrentThreadRealtime()});
                }

                if (++arrived == tasks)
                    release.open();

                release.wait();
            });
    }

    check(pool.waitUntilIdleFor(bound));
    return samples;
}

std::vector<std::string> threadNamesOf(ThreadPool& pool, int tasks)
{
    auto names = std::vector<std::string> {};

    for (const auto& sample: sampleEachThread(pool, tasks))
        names.push_back(sample.name);

    std::sort(names.begin(), names.end());
    return names;
}

auto tThreadNames = test("ThreadPool/options/threadsCarryTheNameAndIndex") = []
{
    auto pool = ThreadPool {{.threadCount = 2, .thread = {.name = "Pool Test"}}};
    check(threadNamesOf(pool, 2)
          == std::vector<std::string> {"Pool Test 1", "Pool Test 2"});
};

auto tDefaultThreadNames = test("ThreadPool/options/defaultNameIsEacpPool") = []
{
    auto pool = ThreadPool {withThreads(2)};
    check(threadNamesOf(pool, 2)
          == std::vector<std::string> {"EACP Pool 1", "EACP Pool 2"});
};

auto tFitName = test("ThreadPool/options/fitThreadNameKeepsTheIndex") = []
{
    check(fitThreadName("EACP Pool Thread 12", 15) == "EACP Pool Th 12");
    check(fitThreadName("EACP Pool Thread 1", 15) == "EACP Pool Thr 1");
    check(fitThreadName("Short 3", 15) == "Short 3");
    check(fitThreadName("ABCDEFGHIJKLMNOPQRS", 15) == "ABCDEFGHIJKLMNO");
    check(fitThreadName("1234567890123456789", 15) == "567890123456789");
    check(fitThreadName("anything", 0).empty());

    auto umlauts = std::string {"\xC3\x9C\xC3\x9C\xC3\x9C\xC3\x9C\xC3\x9C"
                                "\xC3\x9C\xC3\x9C\xC3\x9C 3"};
    check(fitThreadName(umlauts, 15) == umlauts.substr(0, 12) + " 3");
};

void checkPriorityRuns(ThreadPriority priority, [[maybe_unused]] int expectedQos)
{
    auto pool = ThreadPool {{.threadCount = 1, .thread = {.priority = priority}}};
    auto ran = std::atomic<bool> {false};
    auto qos = std::atomic<int> {-1};

    pool.submit(
        [&]
        {
            qos = currentQosForTest();
            ran = true;
        });

    check(pool.waitUntilIdleFor(bound));
    check(ran);

#if defined(__APPLE__)
    check(qos == expectedQos);
#endif
}

auto tPriorities = test("ThreadPool/options/lowAndHighPriorityPoolsRunTasks") = []
{
#if defined(__APPLE__)
    checkPriorityRuns(ThreadPriority::Low, QOS_CLASS_UTILITY);
    checkPriorityRuns(ThreadPriority::High, QOS_CLASS_USER_INITIATED);
#else
    checkPriorityRuns(ThreadPriority::Low, 0);
    checkPriorityRuns(ThreadPriority::High, 0);
#endif
};

auto tNormalNotInherited =
    test("ThreadPool/options/normalPoolStartedFromALowTaskIsNormal") = []
{
    auto lowPool =
        ThreadPool {{.threadCount = 1, .thread = {.priority = ThreadPriority::Low}}};
    auto normalPool = ThreadPool {withThreads(1)};
    auto lowQos = std::atomic<int> {0};
    auto normalQos = std::atomic<int> {0};

    lowPool.submit(
        [&]
        {
            lowQos = currentQosForTest();
            normalPool.submit([&] { normalQos = currentQosForTest(); });
        });

    check(lowPool.waitUntilIdleFor(bound));
    check(normalPool.waitUntilIdleFor(bound));

#if defined(__APPLE__)
    check(lowQos == QOS_CLASS_UTILITY);
    check(normalQos == QOS_CLASS_DEFAULT);
#else
    check(lowQos == normalQos);
#endif
};

ThreadPoolOptions realtimePoolOptions(int threads)
{
    return {.threadCount = threads,
            .thread = {.name = "Audio Pool",
                       .realtime = RealtimeOptions::forAudio(64, 48000.0)}};
}

auto tRealtimePool =
    test("ThreadPool/options/realtimePoolThreadsReportTheirOutcome") = []
{
    auto pool = ThreadPool {realtimePoolOptions(2)};
    auto samples = sampleEachThread(pool, 2);

    check(samples.size() == 2);
    check(pool.startedThreadCount() == 2);

    auto granted = pool.realtimeThreadCount();
    auto scheduled = std::count_if(samples.begin(),
                                   samples.end(),
                                   [](const PoolThreadSample& sample)
                                   { return sample.realtime.scheduledRealtime; });

    LOG("ThreadPool realtime threads granted: ", granted);

#if defined(__APPLE__)
    check(granted == 2);
    check(scheduled == 2);
#elif defined(_WIN32)
    check(granted >= 0 && granted <= 2);

    if (granted == 0)
        for (const auto& sample: samples)
            check(sample.realtime.priorityLevel == THREAD_PRIORITY_TIME_CRITICAL);
#elif defined(__ANDROID__)
    check(granted == 0);
#elif defined(__linux__)
    check(granted == (int) scheduled);
#endif

    pool.stop();
    check(pool.realtimeThreadCount() == 0);

    auto again = sampleEachThread(pool, 2);
    check(again.size() == 2);
    check(pool.realtimeThreadCount() == granted);
};

auto tRealtimeLazyFromTask =
    test("ThreadPool/options/realtimePoolStartedFromItsOwnTaskCompletes") = []
{
    auto pool = ThreadPool {realtimePoolOptions(2)};
    auto inner = Gate {};
    auto innerRan = std::atomic<bool> {false};

    pool.submit(
        [&]
        {
            pool.submit(
                [&]
                {
                    innerRan = true;
                    inner.open();
                });

            inner.wait();
        });

    check(pool.waitUntilIdleFor(bound));
    check(innerRan);
    check(pool.startedThreadCount() == 2);
};
} // namespace ThreadPoolTests
