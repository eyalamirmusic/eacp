#include <eacp/GPU/View/ContinuousRenderSwitch.h>

#include <NanoTest/NanoTest.h>

using namespace nano;
using namespace eacp::GPU;

namespace
{
struct LinkCounts
{
    int starts = 0;
    int stops = 0;
};

ContinuousRenderSwitch makeCountingSwitch(LinkCounts& counts)
{
    return {[&counts] { ++counts.starts; }, [&counts] { ++counts.stops; }};
}
} // namespace

auto tStartsWhenRequested = test("ContinuousRenderSwitch/startsWhenRequested") = []
{
    auto counts = LinkCounts {};
    auto renderSwitch = makeCountingSwitch(counts);

    renderSwitch.setRequested(true);

    check(renderSwitch.isRunning());
    check(counts.starts == 1);
    check(counts.stops == 0);
};

auto tPausesWhileHidden = test("ContinuousRenderSwitch/pausesWhileWindowHidden") = []
{
    auto counts = LinkCounts {};
    auto renderSwitch = makeCountingSwitch(counts);

    renderSwitch.setRequested(true);
    renderSwitch.setWindowVisible(false);

    check(!renderSwitch.isRunning());
    check(counts.stops == 1);

    renderSwitch.setWindowVisible(true);

    check(renderSwitch.isRunning());
    check(counts.starts == 2);
};

auto tRepeatsAreIgnored = test("ContinuousRenderSwitch/repeatedStatesDoNothing") = []
{
    auto counts = LinkCounts {};
    auto renderSwitch = makeCountingSwitch(counts);

    renderSwitch.setRequested(true);
    renderSwitch.setRequested(true);
    renderSwitch.setWindowVisible(true);
    renderSwitch.setWindowVisible(false);
    renderSwitch.setWindowVisible(false);

    check(counts.starts == 1);
    check(counts.stops == 1);
};

auto tHiddenThenRequested =
    test("ContinuousRenderSwitch/requestWhileHiddenWaitsForVisible") = []
{
    auto counts = LinkCounts {};
    auto renderSwitch = makeCountingSwitch(counts);

    renderSwitch.setWindowVisible(false);
    renderSwitch.setRequested(true);

    check(!renderSwitch.isRunning());
    check(counts.starts == 0);

    renderSwitch.setWindowVisible(true);

    check(renderSwitch.isRunning());
    check(counts.starts == 1);
};

auto tStopWhilePaused =
    test("ContinuousRenderSwitch/stopWhilePausedStaysStopped") = []
{
    auto counts = LinkCounts {};
    auto renderSwitch = makeCountingSwitch(counts);

    renderSwitch.setRequested(true);
    renderSwitch.setWindowVisible(false);
    renderSwitch.setRequested(false);
    renderSwitch.setWindowVisible(true);

    check(!renderSwitch.isRunning());
    check(counts.starts == 1);
    check(counts.stops == 1);
};
