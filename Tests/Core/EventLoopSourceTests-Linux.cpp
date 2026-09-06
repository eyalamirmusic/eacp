#include "Common.h"

#include <eacp/Core/Threads/EventLoop-Linux.h>

#include <poll.h>
#include <unistd.h>

// Threads::addLoopSource — a descriptor joining the poll set the message loop
// already waits on.
//
// This is what lets a Wayland or xcb connection be pumped by eacp's own loop
// rather than by a toolkit's, so it is worth pinning the three properties the
// windowing code will rely on: the pump wakes for the fd (not only for its own
// waker), the loop thread is where the callback lands, and removing the source
// really unhooks it. The nesting case is the fourth: a source callback that
// pumps the loop again (a modal drag loop, a resize) must still work, which is
// the whole reason eacp does not hand its loop to SDL or GTK.

using namespace nano;
using eacp::Threads::addLoopSource;
using eacp::Threads::isMainThread;
using eacp::Threads::removeLoopSource;
using eacp::Threads::runEventLoopUntil;

namespace
{
// A self-pipe standing in for a display connection: a readable fd somebody
// else writes to.
struct SourcePipe
{
    SourcePipe() { ::pipe(fds); }

    ~SourcePipe()
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
} // namespace

auto tWriteFromAnotherThreadWakesThePump =
    test("EventLoopSource/writeFromAnotherThreadWakesThePump") = []
{
    auto pipe = SourcePipe {};
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

    auto woke = runEventLoopUntil([&] { return calls > 0; }, eacp::Time::MS {2000});

    writer.join();
    removeLoopSource(pipe.readFd());

    check(woke);
    check(calls == 1);
    check(onLoopThread);
};

auto tRemovedSourceStopsFiring =
    test("EventLoopSource/removedSourceStopsFiring") = []
{
    auto pipe = SourcePipe {};
    auto calls = 0;

    addLoopSource(pipe.readFd(),
                  POLLIN,
                  [&]
                  {
                      ++calls;
                      pipe.drain();
                  });

    pipe.poke();
    runEventLoopUntil([&] { return calls > 0; }, eacp::Time::MS {2000});
    check(calls == 1);

    removeLoopSource(pipe.readFd());

    // The same poke that fired the callback a moment ago, with nothing left
    // watching the descriptor.
    pipe.poke();
    runEventLoopUntil([&] { return calls > 1; }, eacp::Time::MS {100});

    check(calls == 1);

    pipe.drain();
};

auto tSourceCallbackCanPumpTheLoop =
    test("EventLoopSource/callbackCanPumpTheLoop") = []
{
    auto pipe = SourcePipe {};
    auto nestedRan = false;
    auto calls = 0;

    addLoopSource(pipe.readFd(),
                  POLLIN,
                  [&]
                  {
                      ++calls;
                      pipe.drain();

                      eacp::Threads::callAsync([&] { nestedRan = true; });
                      runEventLoopUntil([&] { return nestedRan; },
                                        eacp::Time::MS {2000});
                  });

    pipe.poke();
    runEventLoopUntil([&] { return calls > 0; }, eacp::Time::MS {2000});

    removeLoopSource(pipe.readFd());

    check(calls == 1);
    check(nestedRan);
};

// The prepare callback, which is the half of a source that runs before the
// pump sleeps rather than after it wakes.
//
// A Wayland connection needs it because its outgoing requests sit in
// libwayland's own buffer until something flushes them, so a loop that blocks
// in poll() without flushing is waiting for a reply to a request the
// compositor never saw. The property that pins it is exactly that ordering:
// nothing else here writes to the pipe, so the pump waking at all proves the
// prepare ran on the near side of poll().
auto tPrepareRunsBeforeThePollThatWakes =
    test("EventLoopSource/prepareRunsBeforeThePollThatWakes") = []
{
    auto pipe = SourcePipe {};
    auto prepares = 0;
    auto calls = 0;

    addLoopSource(
        pipe.readFd(),
        POLLIN,
        [&]
        {
            ++calls;
            pipe.drain();
        },
        [&]
        {
            // Once only: a prepare poking on every turn would spin the loop
            // and prove nothing about ordering.
            if (prepares++ == 0)
                pipe.poke();
        });

    auto woke = runEventLoopUntil([&] { return calls > 0; }, eacp::Time::MS {2000});

    removeLoopSource(pipe.readFd());

    check(woke);
    check(calls == 1);
    check(prepares > 0);
};

// Re-registering the same descriptor replaces what was there, so a caller
// swapping its handler does not end up with the old one still installed.
auto tReAddingReplacesTheCallback =
    test("EventLoopSource/reAddingReplacesTheCallback") = []
{
    auto pipe = SourcePipe {};
    auto first = 0;
    auto second = 0;

    addLoopSource(pipe.readFd(), POLLIN, [&] { ++first; });
    addLoopSource(pipe.readFd(),
                  POLLIN,
                  [&]
                  {
                      ++second;
                      pipe.drain();
                  });

    pipe.poke();
    runEventLoopUntil([&] { return second > 0; }, eacp::Time::MS {2000});

    removeLoopSource(pipe.readFd());

    check(first == 0);
    check(second == 1);
};
