#include "Common.h"

#include <eacp/Graphics/Helpers/DisplayLink.h>
#include <eacp/Core/Threads/EventLoop-Linux.h>

#include <poll.h>

// The half of plan.md stage 0 that HostedLoopTests could not cover: a
// DisplayLink lives in eacp-graphics, which CoreTests must not link. Its
// pacing thread posts through callAsync exactly as a Timer's does, so under a
// host that never pumps it must never tick.
//
// A binary of its own because these must run outside Apps::run: pumpEventLoop
// refuses re-entry, and every case inside a running loop would be a no-op.

using namespace nano;
using namespace eacp;

namespace
{
// Everything a VST3/CLAP host offers: one descriptor to wait on and one call
// to make when it fires. Never touches runEventLoop.
struct FakeHost
{
    template <typename Predicate>
    bool pumpUntil(Predicate ready, Time::MS timeout)
    {
        auto deadline = Time::Deadline {timeout};

        while (!ready())
        {
            if (deadline.expired())
                return ready();

            auto fds = pollfd {Threads::getEventLoopFd(), POLLIN, 0};
            ::poll(&fds, 1, (int) deadline.remaining().count);

            Threads::pumpEventLoop();
        }

        return true;
    }
};
} // namespace

auto tDisplayLinkTicksOnlyWhenPumped = test("DisplayLink/ticksOnlyWhenPumped") = []
{
    Threads::attachCurrentThreadAsMain();

    auto ticks = 0;
    auto onLoopThread = true;

    auto link = Threads::DisplayLink {[&](Threads::FrameTime)
                                      {
                                          ++ticks;
                                          onLoopThread = onLoopThread
                                                         && Threads::isMainThread();
                                      }};

    // Several display periods, and nothing driving the loop.
    Time::sleepMS(150);
    check(ticks == 0, "a DisplayLink ticked with nothing pumping the loop");

    auto host = FakeHost {};

    check(host.pumpUntil([&] { return ticks >= 3; }, Time::MS {5000}),
          "the host's pump never delivered a DisplayLink tick");

    check(onLoopThread);
};

// Headless, so the fallback period is the one being paced against and no
// window system is opened to ask for a mode.
int main(int argc, char* argv[])
{
    eacp::Apps::getAppEnvironment().headless = true;

    return nano::run(argc, argv);
}
