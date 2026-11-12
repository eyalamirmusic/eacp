#include "Common.h"
#include "Gate.h"
#include "ThreadNames.h"

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

using namespace nano;

namespace ThreadTests
{
using eacp::LOG;
using eacp::Threads::RealtimeOptions;
using eacp::Threads::Thread;
using eacp::Threads::ThreadOptions;
using eacp::Threads::Detail::machTicksFromMilliseconds;
using eacp::Threads::Detail::realtimeConstraintFor;
using eacp::Threads::Detail::realtimeSchedulerPriority;

void pause()
{
    std::this_thread::sleep_for(std::chrono::milliseconds {20});
}

auto tDefaultIsEmpty = test("Thread/defaultHoldsNoThread") = []
{
    auto thread = Thread {};
    check(!thread.isJoinable());
    check(!thread.isCurrentThread());
    thread.join();
    check(!thread.isJoinable());
};

auto tRunsOnAnotherThread = test("Thread/bodyRunsOnAnotherThread") = []
{
    auto onMain = true;
    auto current = false;
    auto ran = false;
    auto thread = Thread {};

    auto body = [&]
    {
        onMain = eacp::Threads::isMainThread();
        current = thread.isCurrentThread();
        ran = true;
    };

    thread.start(body);
    check(thread.isJoinable());
    check(!thread.isCurrentThread());
    thread.join();

    check(ran);
    check(!onMain);
    check(current);
    check(!thread.isJoinable());
};

auto tConstructorStarts = test("Thread/constructorStartsTheBody") = []
{
    auto ran = std::atomic<bool> {false};
    auto body = [&] { ran = true; };

    auto thread = Thread {body};
    thread.join();

    check(ran);
};

auto tDestructionJoins = test("Thread/destructionJoins") = []
{
    auto finished = std::atomic<bool> {false};

    auto body = [&]
    {
        pause();
        finished = true;
    };

    {
        auto thread = Thread {body};
    }

    check(finished);
};

auto tStartAgain = test("Thread/startAfterARunRunsAgain") = []
{
    auto runs = std::atomic<int> {0};
    auto body = [&] { ++runs; };

    auto thread = Thread {body};
    thread.join();
    thread.start(body);
    thread.start(body);
    thread.join();

    check(runs == 3);
};

auto tMoveConstruction = test("Thread/moveConstructionTakesTheThread") = []
{
    auto finished = std::atomic<bool> {false};

    auto body = [&]
    {
        pause();
        finished = true;
    };

    auto source = Thread {body};
    auto target = Thread {std::move(source)};

    check(!source.isJoinable());
    check(target.isJoinable());

    target.join();
    check(finished);
};

auto tMoveAssignment = test("Thread/moveAssignmentJoinsAndTakesTheThread") = []
{
    auto first = std::atomic<bool> {false};
    auto second = std::atomic<bool> {false};

    auto firstBody = [&]
    {
        pause();
        first = true;
    };

    auto secondBody = [&]
    {
        pause();
        second = true;
    };

    auto target = Thread {firstBody};
    auto source = Thread {secondBody};

    target = std::move(source);

    check(first);
    check(!source.isJoinable());
    check(target.isJoinable());

    target.join();
    check(second);
};

auto tJoinTwice = test("Thread/joinTwiceIsHarmless") = []
{
    auto ran = std::atomic<bool> {false};
    auto body = [&] { ran = true; };

    auto thread = Thread {body};
    thread.join();
    thread.join();

    check(ran);
    check(!thread.isJoinable());
};

struct ThreadTraits
{
    std::string name;
    int qos = -1;
};

ThreadTraits currentThreadTraits()
{
    return {currentThreadNameForTest(), currentQosForTest()};
}

auto tDefaultOptions = test("Thread/defaultOptionsAreEacpThreadAtNormal") = []
{
    auto thread = Thread {};
    check(thread.options().name == "EACP Thread");
    check(thread.options().priority == eacp::Threads::ThreadPriority::Normal);
    check(!thread.options().realtime.has_value());
    check(!thread.isRealtime());

    thread.start([] {});
    check(!thread.isRealtime());
};

auto tNamed = test("Thread/optionsNameTheThread") = []
{
    auto traits = ThreadTraits {};
    auto body = [&] { traits = currentThreadTraits(); };

    auto thread = Thread {body, {.name = "Thread Test"}};
    thread.join();
    check(traits.name == "Thread Test");
    check(thread.options().name == "Thread Test");

    thread.setOptions({.name = "Thread Again"});
    thread.start(body);
    thread.join();
    check(traits.name == "Thread Again");
};

auto tOptionsMove = test("Thread/moveCarriesTheOptions") = []
{
    auto source = Thread {ThreadOptions {.name = "Moved"}};
    auto target = Thread {std::move(source)};
    auto traits = ThreadTraits {};

    check(target.options().name == "Moved");
    target.start([&] { traits = currentThreadTraits(); });
    target.join();
    check(traits.name == "Moved");
};

auto tPriority = test("Thread/priorityIsAppliedOnTheThread") = []
{
    auto traits = ThreadTraits {};
    auto body = [&] { traits = currentThreadTraits(); };

    auto low = Thread {body, {.priority = eacp::Threads::ThreadPriority::Low}};
    low.join();

#if defined(__APPLE__)
    check(traits.qos == QOS_CLASS_UTILITY);
#endif

    auto high = Thread {body, {.priority = eacp::Threads::ThreadPriority::High}};
    high.join();

#if defined(__APPLE__)
    check(traits.qos == QOS_CLASS_USER_INITIATED);
#endif
};

auto tDefaultThread = test("Thread/defaultIsNamedAndNormalFromALowCreator") = []
{
    auto creatorTraits = ThreadTraits {};
    auto ours = ThreadTraits {};

    auto creatorBody = [&]
    {
        creatorTraits = currentThreadTraits();
        auto thread = Thread {[&] { ours = currentThreadTraits(); }};
        thread.join();
    };

    auto creator =
        Thread {creatorBody,
                {.name = "Creator", .priority = eacp::Threads::ThreadPriority::Low}};
    creator.join();

    check(creatorTraits.name == "Creator");
    check(ours.name == "EACP Thread");

#if defined(__APPLE__)
    check(creatorTraits.qos == QOS_CLASS_UTILITY);
    check(ours.qos == QOS_CLASS_DEFAULT);
#endif
};

auto tUnnamed = test("Thread/emptyNameLeavesTheThreadAsThePlatformNamesIt") = []
{
    auto plain = ThreadTraits {};
    auto ours = ThreadTraits {};

    auto creatorBody = [&]
    {
        auto plainThread = std::thread {[&] { plain = currentThreadTraits(); }};
        plainThread.join();

        auto thread = Thread {[&] { ours = currentThreadTraits(); },
                              ThreadOptions {.name = ""}};
        thread.join();
    };

    auto creator = Thread {creatorBody, {.name = "Creator"}};
    creator.join();

    check(ours.name == plain.name);
};

constexpr auto audioOptions = RealtimeOptions {
    .periodMs = 2.9, .processingTimeMs = 1.0, .maxProcessingTimeMs = 2.0};

auto tRealtime = test("Thread/realtimeIsSettledWhenStartReturns") = []
{
    auto seen = RealtimeProbe {};
    auto ran = false;
    auto release = Gate {};

    auto thread = Thread {[&]
                          {
                              seen = probeCurrentThreadRealtime();
                              ran = true;
                              release.wait();
                          },
                          ThreadOptions {.realtime = audioOptions}};

    auto granted = thread.isRealtime();
    release.open();
    thread.join();

    LOG("Thread realtime granted: ", granted);
    check(ran);

#if defined(__APPLE__)
    check(granted);
    check(seen.scheduledRealtime);
    check(seen.period == machTicksForTest(2.9));
    check(seen.computation == machTicksForTest(1.0));
    check(seen.constraint == machTicksForTest(2.0));
#elif defined(_WIN32)
    if (!granted)
        check(seen.priorityLevel == THREAD_PRIORITY_TIME_CRITICAL);
#elif defined(__ANDROID__)
    check(!granted);
#elif defined(__linux__)
    check(granted == seen.scheduledRealtime);

    if (granted)
        check(seen.schedulerPriority
              == realtimeSchedulerPriority(sched_get_priority_min(SCHED_RR),
                                           sched_get_priority_max(SCHED_RR)));
#endif
};

struct ChildSample
{
    RealtimeProbe realtime;
    int qos = -1;
};

ChildSample sampleCurrentThread()
{
    return {probeCurrentThreadRealtime(), currentQosForTest()};
}

auto tRealtimeChildren = test("Thread/threadsStartedByARealtimeThreadAreNot") = []
{
    auto parent = RealtimeProbe {};
    auto child = ChildSample {};
    auto poolChild = ChildSample {};
    auto granted = false;

    auto body = [&]
    {
        parent = probeCurrentThreadRealtime();

        auto thread = Thread {[&] { child = sampleCurrentThread(); }};
        thread.join();

        auto pool = eacp::Threads::ThreadPool {{.threadCount = 1}};
        pool.submit([&] { poolChild = sampleCurrentThread(); });
        check(pool.waitUntilIdleFor(threadTestBound));
    };

    auto realtime = Thread {body, ThreadOptions {.realtime = audioOptions}};
    granted = realtime.isRealtime();
    realtime.join();

    LOG("Realtime parent granted: ", granted);
    check(!child.realtime.scheduledRealtime);
    check(!poolChild.realtime.scheduledRealtime);

#if defined(__APPLE__)
    check(parent.scheduledRealtime);
    check(child.qos == QOS_CLASS_DEFAULT);
    check(poolChild.qos == QOS_CLASS_DEFAULT);
#elif defined(_WIN32)
    check(child.realtime.priorityLevel == THREAD_PRIORITY_NORMAL);
    check(poolChild.realtime.priorityLevel == THREAD_PRIORITY_NORMAL);
#elif defined(__linux__)
    check(granted == parent.scheduledRealtime);
#endif
};

auto tRealtimeResets =
    test("Thread/realtimeIsFalseAfterJoinAndWithoutTheOption") = []
{
    auto thread = Thread {[] {}, ThreadOptions {.realtime = audioOptions}};
    thread.join();
    check(!thread.isRealtime());

    thread.setOptions({});
    thread.start([] {});
    check(!thread.isRealtime());
    thread.join();
    check(!thread.isRealtime());
};

auto tRealtimeMath = test("Thread/realtimeConversionsFollowJuce") = []
{
    auto both = realtimeConstraintFor(audioOptions);
    check(both.periodMs == 2.9);
    check(both.computationMs == 1.0);
    check(both.constraintMs == 2.0);

    auto onlyMax = realtimeConstraintFor({.maxProcessingTimeMs = 3.0});
    check(onlyMax.periodMs == 0.0);
    check(onlyMax.computationMs == 3.0);
    check(onlyMax.constraintMs == 3.0);

    auto onlyPeriod = realtimeConstraintFor({.periodMs = 5.0});
    check(onlyPeriod.computationMs == 5.0);
    check(onlyPeriod.constraintMs == 5.0);

    auto nothing = realtimeConstraintFor({});
    check(nothing.periodMs == 0.0);
    check(nothing.computationMs == 10.0);
    check(nothing.constraintMs == 10.0);

    check(machTicksFromMilliseconds(1.0, 1, 1) == 1'000'000u);
    check(machTicksFromMilliseconds(1.0, 125, 3) == 24'000u);
    check(machTicksFromMilliseconds(0.0, 125, 3) == 0u);

    check(realtimeSchedulerPriority(1, 99) == 50);
    check(realtimeSchedulerPriority(0, 0) == 0);
    check(realtimeSchedulerPriority(-5, 10) == 5);

    auto audio = RealtimeOptions::forAudio(64, 48000.0);
    check(std::abs(audio.maxProcessingTimeMs - 64.0 / 48.0) < 1e-9);
    check(audio.periodMs == 0.0);
    check(audio.processingTimeMs == 0.0);
    check(RealtimeOptions::forAudio(0, 48000.0).maxProcessingTimeMs == 0.0);
};
} // namespace ThreadTests
