#include "Common.h"

#include <eacp/Core/Threads/EventLoop-Linux.h>

#include <optional>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

using namespace nano;

namespace FdWatcherTests
{
using eacp::Threads::FdWatcher;
using eacp::Threads::runEventLoopUntil;

constexpr auto watchTimeout = eacp::Time::MS {2000};

// Long enough for a pump that was going to call back to have done so.
constexpr auto quietPeriod = eacp::Time::MS {100};

// What a plugin's display connection is to a host's IRunLoop.
struct WatchedPipe
{
    WatchedPipe()
    {
        auto created = ::pipe(fds);
        (void) created;
    }

    ~WatchedPipe()
    {
        closeReadEnd();
        closeWriteEnd();
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

    void closeReadEnd()
    {
        if (fds[0] >= 0)
            ::close(fds[0]);
        fds[0] = -1;
    }

    void closeWriteEnd()
    {
        if (fds[1] >= 0)
            ::close(fds[1]);
        fds[1] = -1;
    }

    int readFd() const { return fds[0]; }

    int fds[2] {-1, -1};
};

auto tReadableCallsBackOnTheLoopThread =
    test("FdWatcher/readableCallsBackOnTheLoopThread") = []
{
    auto pipe = WatchedPipe {};
    auto calls = 0;
    auto onLoopThread = false;

    auto watcher = FdWatcher {pipe.readFd(),
                              [&]
                              {
                                  ++calls;
                                  onLoopThread = eacp::Threads::isMainThread();
                                  pipe.drain();
                              }};

    auto writer = std::thread(
        [&]
        {
            eacp::Time::sleepMS(30);
            pipe.poke();
        });

    auto woke = runEventLoopUntil([&] { return calls > 0; }, watchTimeout);
    writer.join();

    check(woke);
    check(calls == 1);
    check(onLoopThread);
    check(watcher.getFd() == pipe.readFd());
};

auto tNothingReadableCallsNothing =
    test("FdWatcher/nothingReadableCallsNothing") = []
{
    auto pipe = WatchedPipe {};
    auto calls = 0;

    auto watcher = FdWatcher {pipe.readFd(), [&] { ++calls; }};

    runEventLoopUntil([&] { return calls > 0; }, quietPeriod);

    check(calls == 0);
};

auto tDestroyedWatcherStopsFiring =
    test("FdWatcher/destroyedWatcherStopsFiring") = []
{
    auto pipe = WatchedPipe {};
    auto calls = 0;

    auto watcher = std::optional<FdWatcher> {};
    watcher.emplace(pipe.readFd(),
                    [&]
                    {
                        ++calls;
                        pipe.drain();
                    });

    pipe.poke();
    runEventLoopUntil([&] { return calls > 0; }, watchTimeout);
    check(calls == 1);

    watcher.reset();

    pipe.poke();
    runEventLoopUntil([&] { return calls > 1; }, quietPeriod);

    check(calls == 1);
};

// Two instances of one plugin sharing a display connection register the same
// descriptor twice, and closing one editor must not deafen the other.
auto tWatchersOnOneFdAreIndependent =
    test("FdWatcher/watchersOnOneFdAreIndependent") = []
{
    auto pipe = WatchedPipe {};
    auto first = 0;
    auto second = 0;

    auto firstWatcher = std::optional<FdWatcher> {};
    firstWatcher.emplace(pipe.readFd(), [&] { ++first; });

    auto secondWatcher = FdWatcher {pipe.readFd(), [&] { ++second; }};

    pipe.poke();
    runEventLoopUntil([&] { return first > 0 && second > 0; }, watchTimeout);

    check(first >= 1);
    check(second >= 1);

    pipe.drain();
    firstWatcher.reset();

    auto firstBefore = first;
    auto secondBefore = second;

    pipe.poke();
    runEventLoopUntil([&] { return second > secondBefore; }, watchTimeout);
    pipe.drain();

    check(first == firstBefore);
    check(second > secondBefore);
};

// addLoopSource keeps one entry per descriptor and replaces it; a watcher is
// neither replaced by it nor takes it away.
auto tLoopSourceAndWatcherShareAnFd =
    test("FdWatcher/loopSourceAndWatcherShareAnFd") = []
{
    auto pipe = WatchedPipe {};
    auto sourceCalls = 0;
    auto watcherCalls = 0;

    eacp::Threads::addLoopSource(pipe.readFd(), POLLIN, [&] { ++sourceCalls; });

    auto watcher = std::optional<FdWatcher> {};
    watcher.emplace(pipe.readFd(), [&] { ++watcherCalls; });

    pipe.poke();
    runEventLoopUntil([&] { return sourceCalls > 0 && watcherCalls > 0; },
                      watchTimeout);
    pipe.drain();

    check(sourceCalls >= 1);
    check(watcherCalls >= 1);

    watcher.reset();

    auto sourceBefore = sourceCalls;
    pipe.poke();
    runEventLoopUntil([&] { return sourceCalls > sourceBefore; }, watchTimeout);
    pipe.drain();

    check(sourceCalls > sourceBefore);

    watcher.emplace(pipe.readFd(), [&] { ++watcherCalls; });
    eacp::Threads::removeLoopSource(pipe.readFd());

    auto watcherBefore = watcherCalls;
    sourceBefore = sourceCalls;

    pipe.poke();
    runEventLoopUntil([&] { return watcherCalls > watcherBefore; }, watchTimeout);
    pipe.drain();

    check(watcherCalls > watcherBefore);
    check(sourceCalls == sourceBefore);
};

auto tACallbackCanDestroyItsOwnWatcher =
    test("FdWatcher/aCallbackCanDestroyItsOwnWatcher") = []
{
    auto pipe = WatchedPipe {};
    auto calls = 0;

    auto watcher = std::optional<FdWatcher> {};
    watcher.emplace(pipe.readFd(),
                    [&]
                    {
                        ++calls;
                        watcher.reset();
                    });

    pipe.poke();
    runEventLoopUntil([&] { return calls > 0; }, watchTimeout);

    // Never drained: a watcher still there would be called on every pump.
    runEventLoopUntil([&] { return calls > 1; }, quietPeriod);

    check(calls == 1);
    check(!watcher.has_value());
};

// Whichever of the two is called first destroys the other, which is then not
// called in the same pump.
auto tAWatcherDestroyedEarlierInThePumpIsNotCalled =
    test("FdWatcher/aWatcherDestroyedEarlierInThePumpIsNotCalled") = []
{
    auto pipe = WatchedPipe {};
    auto calls = 0;

    auto first = std::optional<FdWatcher> {};
    auto second = std::optional<FdWatcher> {};

    first.emplace(pipe.readFd(),
                  [&]
                  {
                      ++calls;
                      second.reset();
                      pipe.drain();
                  });

    second.emplace(pipe.readFd(),
                   [&]
                   {
                       ++calls;
                       first.reset();
                       pipe.drain();
                   });

    pipe.poke();
    runEventLoopUntil([&] { return calls > 0; }, watchTimeout);
    runEventLoopUntil([&] { return calls > 1; }, quietPeriod);

    check(calls == 1);
    check(first.has_value() != second.has_value());
};

auto tAnEventFdIsWatchedToo = test("FdWatcher/anEventFdIsWatchedToo") = []
{
    auto fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    check(fd >= 0);

    auto calls = 0;

    {
        auto watcher = FdWatcher {fd,
                                  [&]
                                  {
                                      ++calls;
                                      auto value = eventfd_t {};
                                      ::eventfd_read(fd, &value);
                                  }};

        ::eventfd_write(fd, 1);
        runEventLoopUntil([&] { return calls > 0; }, watchTimeout);
    }

    ::close(fd);

    check(calls == 1);
};

// A peer that went away is what a dead display connection looks like, and the
// handler is what has to notice.
auto tAHangUpCallsBack = test("FdWatcher/aHangUpCallsBack") = []
{
    auto pipe = WatchedPipe {};
    auto calls = 0;

    auto watcher = std::optional<FdWatcher> {};
    watcher.emplace(pipe.readFd(),
                    [&]
                    {
                        ++calls;
                        watcher.reset();
                    });

    pipe.closeWriteEnd();
    runEventLoopUntil([&] { return calls > 0; }, watchTimeout);

    check(calls == 1);
};
} // namespace FdWatcherTests
