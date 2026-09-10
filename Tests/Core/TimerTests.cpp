#include "Common.h"

using namespace nano;
using eacp::Threads::runEventLoopUntil;
using eacp::Threads::Timer;

auto tMillisecondIntervalTicks = test("Timer/millisecondIntervalTicks") = []
{
    auto ticks = 0;
    auto timer = Timer {[&] { ++ticks; }, eacp::Time::MS {20}};

    check(runEventLoopUntil([&] { return ticks >= 3; }, eacp::Time::MS {5000}));
};

// The interval a whole number of Hz cannot express - the case the Time::MS
// constructor exists for. Only the first tick is waited on; a real heartbeat
// period would outlast the suite.
//
// The lower bound sits one clock tick short of the interval: Windows' USER
// timer counts 15.6 ms ticks from a start it rounds down to one, so against a
// precise clock the first tick can land that much early. It did, once, on CI.
auto tFractionalHzInterval = test("Timer/intervalNoWholeHzCanExpress") = []
{
    auto ticks = 0;
    auto timer = Timer {[&] { ++ticks; }, eacp::Time::MS {150}};

    auto lowerBound = eacp::Time::Deadline {eacp::Time::MS {150 - 16}};

    check(runEventLoopUntil([&] { return ticks >= 1; }, eacp::Time::MS {5000}));
    check(lowerBound.expired());
};

auto tHzIntervalStillTicks = test("Timer/hzIntervalStillTicks") = []
{
    auto ticks = 0;
    auto timer = Timer {[&] { ++ticks; }, 50};

    check(runEventLoopUntil([&] { return ticks >= 3; }, eacp::Time::MS {5000}));
};

auto tDestructionStopsTicking = test("Timer/destructionStopsTicking") = []
{
    auto ticks = 0;

    {
        auto timer = Timer {[&] { ++ticks; }, eacp::Time::MS {10}};
        check(runEventLoopUntil([&] { return ticks >= 1; }, eacp::Time::MS {5000}));
    }

    auto afterStopping = ticks;
    runEventLoopUntil([] { return false; }, eacp::Time::MS {100});

    check(ticks == afterStopping);
};
