#include "Common.h"

#include <eacp/Core/Threads/Async.h>
#include <eacp/Core/Threads/EventLoop-Linux.h>

#include <poll.h>
#include <unistd.h>

using namespace nano;
using eacp::Threads::addLoopSource;
using eacp::Threads::AsyncPromise;
using eacp::Threads::attachCurrentThreadAsMain;
using eacp::Threads::callAfter;
using eacp::Threads::callAsync;
using eacp::Threads::getEventLoopFd;
using eacp::Threads::isEventLoopRunning;
using eacp::Threads::isMainThread;
using eacp::Threads::pumpEventLoop;
using eacp::Threads::removeLoopSource;
using eacp::Threads::runEventLoopUntil;
using eacp::Threads::Timer;

namespace
{
// A self-pipe standing in for a display connection.
struct HostedPipe
{
    HostedPipe()
    {
        auto created = ::pipe(fds);
        (void) created;
    }

    ~HostedPipe()
    {
        ::close(fds[0]);
        ::close(fds[1]);
    }

    void poke() const
    {
        char byte = 1;
        auto written = ::write(fds[1], &byte, 1);
        (void) written;
    }

    void drain() const
    {
        char buffer[16];
        auto got = ::read(fds[0], buffer, sizeof(buffer));
        (void) got;
    }

    int readFd() const { return fds[0]; }

    int fds[2] {-1, -1};
};

bool loopFdIsReadable()
{
    auto fds = pollfd {getEventLoopFd(), POLLIN, 0};
    return ::poll(&fds, 1, 0) > 0;
}

// Everything a VST3/CLAP host offers: one descriptor to wait on and one call
// to make when it fires. Never touches runEventLoop.
struct FakeHost
{
    template <typename Predicate>
    bool pumpUntil(Predicate ready, eacp::Time::MS timeout)
    {
        auto deadline = eacp::Time::Deadline {timeout};

        while (!ready())
        {
            if (deadline.expired())
                return ready();

            auto fds = pollfd {getEventLoopFd(), POLLIN, 0};
            ::poll(&fds, 1, (int) deadline.remaining().count);

            pumpEventLoop();
            ++pumps;
        }

        return true;
    }

    int pumps = 0;
};
} // namespace

auto tAttachMarksTheCopyAsHosted = test("HostedLoop/attachMarksTheCopyAsHosted") = []
{
    check(!isEventLoopRunning());

    attachCurrentThreadAsMain();

    check(isEventLoopRunning());
    check(isMainThread());
};

auto tLoopFdIsOneStableDescriptor =
    test("HostedLoop/loopFdIsOneStableDescriptor") = []
{
    attachCurrentThreadAsMain();

    auto fd = getEventLoopFd();
    check(fd >= 0);

    auto pipe = HostedPipe {};
    addLoopSource(pipe.readFd(), POLLIN, [&] { pipe.drain(); });

    check(getEventLoopFd() == fd);

    removeLoopSource(pipe.readFd());

    check(getEventLoopFd() == fd);
};

auto tLoopFdIsReadableExactlyWhenThereIsWork =
    test("HostedLoop/loopFdIsReadableExactlyWhenThereIsWork") = []
{
    attachCurrentThreadAsMain();

    pumpEventLoop();
    check(!loopFdIsReadable());

    callAsync([] {});
    check(loopFdIsReadable());

    pumpEventLoop();
    check(!loopFdIsReadable());
};

auto tCallAsyncWaitsForAPump = test("HostedLoop/callAsyncWaitsForAPump") = []
{
    attachCurrentThreadAsMain();

    auto fired = false;
    callAsync([&] { fired = true; });

    eacp::Time::sleepMS(50);
    check(!fired);

    pumpEventLoop();
    check(fired);
};

auto tHostPollsTheLoopFdForWorkerThreadCallbacks =
    test("HostedLoop/hostPollsTheLoopFdForWorkerThreadCallbacks") = []
{
    attachCurrentThreadAsMain();

    auto fired = false;
    auto onLoopThread = false;

    auto worker = std::thread(
        [&]
        {
            eacp::Time::sleepMS(30);
            callAsync(
                [&]
                {
                    onLoopThread = isMainThread();
                    fired = true;
                });
        });

    auto host = FakeHost {};
    auto delivered = host.pumpUntil([&] { return fired; }, eacp::Time::MS {2000});

    worker.join();

    check(delivered);
    check(onLoopThread);
};

auto tCallAfterWaitsForAPump = test("HostedLoop/callAfterWaitsForAPump") = []
{
    attachCurrentThreadAsMain();

    auto fired = false;
    callAfter(eacp::Time::MS {20}, [&] { fired = true; });

    eacp::Time::sleepMS(120);
    check(!fired);

    pumpEventLoop();
    check(fired);
};

auto tTimerTicksOnlyWhenPumped = test("HostedLoop/timerTicksOnlyWhenPumped") = []
{
    attachCurrentThreadAsMain();

    auto ticks = 0;
    auto timer = Timer {[&] { ++ticks; }, eacp::Time::MS {20}};

    eacp::Time::sleepMS(120);
    check(ticks == 0);

    auto host = FakeHost {};
    check(host.pumpUntil([&] { return ticks >= 3; }, eacp::Time::MS {5000}));
};

auto tAsyncSettlesOnlyWhenPumped = test("HostedLoop/asyncSettlesOnlyWhenPumped") = []
{
    attachCurrentThreadAsMain();

    auto promise = AsyncPromise<int> {};
    auto async = promise.get();

    auto value = 0;
    async.then([&](int settled) { value = settled; });

    auto worker = std::thread(
        [promise]
        {
            eacp::Time::sleepMS(20);
            callAsync([promise] { promise.resolve(7); });
        });

    eacp::Time::sleepMS(120);
    check(value == 0);
    check(!async.isReady());

    auto host = FakeHost {};
    check(host.pumpUntil([&] { return value == 7; }, eacp::Time::MS {2000}));

    worker.join();

    check(async.isResolved());
};

auto tLoopSourceFiresUnderAHost = test("HostedLoop/loopSourceFiresUnderAHost") = []
{
    attachCurrentThreadAsMain();

    auto pipe = HostedPipe {};
    auto calls = 0;
    auto onLoopThread = false;

    addLoopSource(pipe.readFd(),
                  POLLIN,
                  [&]
                  {
                      ++calls;
                      onLoopThread = isMainThread();
                      pipe.drain();
                  });

    auto writer = std::thread(
        [&]
        {
            eacp::Time::sleepMS(30);
            pipe.poke();
        });

    auto host = FakeHost {};
    auto woke = host.pumpUntil([&] { return calls > 0; }, eacp::Time::MS {2000});

    writer.join();
    removeLoopSource(pipe.readFd());

    check(woke);
    check(calls == 1);
    check(onLoopThread);
};

// The flush a Wayland or X11 connection does from `prepare` cannot wait for
// the next readiness in a host: there is no wait to run it ahead of.
auto tPrepareRunsAtTheEndOfEveryPump =
    test("HostedLoop/prepareRunsAtTheEndOfEveryPump") = []
{
    attachCurrentThreadAsMain();

    auto pipe = HostedPipe {};
    auto prepares = 0;

    addLoopSource(pipe.readFd(), POLLIN, [&] { pipe.drain(); }, [&] { ++prepares; });

    pumpEventLoop();
    check(prepares == 1);

    pumpEventLoop();
    check(prepares == 2);

    removeLoopSource(pipe.readFd());

    pumpEventLoop();
    check(prepares == 2);
};

auto tSourceAddedFromACallbackIsPolledNextRound =
    test("HostedLoop/sourceAddedFromACallbackIsPolledNextRound") = []
{
    attachCurrentThreadAsMain();

    auto pipe = HostedPipe {};
    auto calls = 0;

    callAsync(
        [&]
        {
            addLoopSource(pipe.readFd(),
                          POLLIN,
                          [&]
                          {
                              ++calls;
                              pipe.drain();
                          });
            pipe.poke();
        });

    auto host = FakeHost {};
    auto fired = host.pumpUntil([&] { return calls > 0; }, eacp::Time::MS {2000});

    removeLoopSource(pipe.readFd());

    check(fired);
    check(calls == 1);
    check(host.pumps >= 2);
};

// A host may call back into the pump from a nested loop of its own; the inner
// call returns at once and the outer round finishes its work.
auto tReEntrantPumpIsANoOp = test("HostedLoop/reEntrantPumpIsANoOp") = []
{
    attachCurrentThreadAsMain();

    auto queuedFromInside = false;
    auto ranDuringInnerPump = false;

    callAsync(
        [&]
        {
            callAsync([&] { queuedFromInside = true; });
            pumpEventLoop();
            ranDuringInnerPump = queuedFromInside;
        });

    pumpEventLoop();

    check(!ranDuringInnerPump);
    check(!queuedFromInside);

    pumpEventLoop();
    check(queuedFromInside);
};

auto tPumpWithNothingToDoReturnsAtOnce =
    test("HostedLoop/pumpWithNothingToDoReturnsAtOnce") = []
{
    attachCurrentThreadAsMain();

    auto budget = eacp::Time::Deadline {eacp::Time::MS {1000}};

    for (auto i = 0; i < 200; ++i)
        pumpEventLoop();

    check(!budget.expired());
};

auto tRunEventLoopUntilStillWorksStandalone =
    test("HostedLoop/runEventLoopUntilStillWorksStandalone") = []
{
    auto fired = false;
    callAsync([&] { fired = true; });

    check(runEventLoopUntil([&] { return fired; }, eacp::Time::MS {2000}));
};

auto tRunEventLoopUntilStillWorksWhenHosted =
    test("HostedLoop/runEventLoopUntilStillWorksWhenHosted") = []
{
    attachCurrentThreadAsMain();

    auto ticks = 0;
    auto timer = Timer {[&] { ++ticks; }, eacp::Time::MS {20}};

    check(runEventLoopUntil([&] { return ticks >= 2; }, eacp::Time::MS {5000}));

    auto fired = false;
    callAsync([&] { fired = true; });

    pumpEventLoop();
    check(fired);
};

auto tRunEventLoopUntilTimesOutWhenHosted =
    test("HostedLoop/runEventLoopUntilTimesOutWhenHosted") = []
{
    attachCurrentThreadAsMain();

    auto lowerBound = eacp::Time::Deadline {eacp::Time::MS {100}};
    auto upperBound = eacp::Time::Deadline {eacp::Time::MS {2000}};

    check(!runEventLoopUntil([] { return false; }, eacp::Time::MS {100}));

    check(lowerBound.expired());
    check(!upperBound.expired());
};
