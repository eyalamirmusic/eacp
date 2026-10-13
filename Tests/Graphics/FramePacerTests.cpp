#include <eacp/Graphics/Graphics.h>

#include <NanoTest/NanoTest.h>

using namespace nano;
using namespace eacp;

namespace
{
int firesOver(Threads::FramePacer& pacer, int ticks, double seconds)
{
    auto fired = 0;

    for (auto tick = 0; tick < ticks; ++tick)
        if (pacer.advance(seconds))
            ++fired;

    return fired;
}
} // namespace

auto tUncapped = test("FramePacer/withNoCapEveryTickFires") = []
{
    auto pacer = Threads::FramePacer {};

    check(pacer.maxFps() == 0);
    check(firesOver(pacer, 50, 1.0 / 120.0) == 50);
};

auto tHalves = test("FramePacer/sixtyOnAHundredAndTwentyIsEveryOtherTick") = []
{
    auto pacer = Threads::FramePacer {};
    pacer.setMaxFps(60);

    check(pacer.maxFps() == 60);
    check(pacer.advance(1.0 / 120.0), "the first tick fires");
    check(!pacer.advance(1.0 / 120.0));
    check(pacer.advance(1.0 / 120.0));

    auto rest = firesOver(pacer, 117, 1.0 / 120.0);
    check(rest + 2 == 60, "120 ticks, 60 fires");
};

auto tUnevenAverage = test("FramePacer/anUnevenCapHoldsAsAnAverage") = []
{
    auto pacer = Threads::FramePacer {};
    pacer.setMaxFps(45);

    auto fired = firesOver(pacer, 1200, 1.0 / 120.0);

    check(fired >= 445 && fired <= 455, "ten seconds at 45 fps");
};

auto tCapAboveRate = test("FramePacer/aCapAboveTheTickRateFiresEveryTick") = []
{
    auto pacer = Threads::FramePacer {};
    pacer.setMaxFps(240);

    check(firesOver(pacer, 60, 1.0 / 60.0) == 60);
};

auto tStall = test("FramePacer/aStallBuysOneTickRatherThanABurst") = []
{
    auto pacer = Threads::FramePacer {};
    pacer.setMaxFps(30);

    firesOver(pacer, 10, 1.0 / 120.0);

    check(pacer.advance(1.0), "the tick after the stall fires");
    check(firesOver(pacer, 3, 1.0 / 120.0) <= 1, "and does not catch up");
};

auto tReset = test("FramePacer/resetFiresTheNextTick") = []
{
    auto pacer = Threads::FramePacer {};
    pacer.setMaxFps(10);

    check(pacer.advance(0.01));
    check(!pacer.advance(0.01));

    pacer.reset();
    check(pacer.advance(0.01));

    pacer.setMaxFps(0);
    check(pacer.advance(0.0), "uncapped again");
    check(pacer.advance(0.0));
};
