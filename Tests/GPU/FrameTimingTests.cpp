#include "Common.h"

#include <string>

// Durations are a measurement, so only bounds are asserted. Every frame here is
// off-screen through renderToImage, which blocks until the GPU has finished.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
// Nothing is drawn: a pass with no draws is still exactly one timing entry.
struct TimedView final : GPUView
{
    TimedView() { setSampleCount(1); }

    void render(Frame& frame) override
    {
        for (const auto& label: labels)
            auto pass = frame.beginPass({.clearColor = {0.f, 0.4f, 0.f, 1.f},
                                         .clear = true,
                                         .label = label});
    }

    Vector<std::string> labels;
};

const FrameTimings& renderAndCollect(TimedView& view)
{
    view.setBounds({0.f, 0.f, 64.f, 64.f});

    // Twice: a frame's numbers are only picked up at the start of a later frame.
    view.renderToImage(1.f);
    view.renderToImage(1.f);

    return Device::shared().lastFrameTimings();
}

// Per-pass timing needs hardware counters; the frame total does not.
bool canTimePasses()
{
    return Device::shared().isValid() && Device::shared().supportsPassTimings();
}
} // namespace

auto tLabelsComeBackInOrder = test("FrameTiming/labelsComeBackInOrder") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = TimedView {};
    view.labels = {"first", "second", "third"};

    const auto& timings = renderAndCollect(view);

    if (!canTimePasses())
        return;

    check(timings.passes.size() == 3);
    check(timings.passes[0].label == "first");
    check(timings.passes[1].label == "second");
    check(timings.passes[2].label == "third");
};

auto tUnlabelledPassesAreNotTimed =
    test("FrameTiming/unlabelledPassesAreNotTimed") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = TimedView {};
    view.labels = {"", "", ""};

    const auto& timings = renderAndCollect(view);

    check(timings.passes.size() == 0);

    // The frame total needs no counters and no labels, so it is always there.
    check(timings.milliseconds >= 0.0);
};

// Ordinals are handed out per timed pass, so an unlabelled one in the middle
// must not shift the labels after it onto the wrong samples.
auto tOnlyLabelledPassesTakeSlots =
    test("FrameTiming/onlyLabelledPassesTakeSlots") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = TimedView {};
    view.labels = {"before", "", "after"};

    const auto& timings = renderAndCollect(view);

    if (!canTimePasses())
        return;

    check(timings.passes.size() == 2);
    check(timings.passes[0].label == "before");
    check(timings.passes[1].label == "after");
};

// The cap is a fixed pool, so the extras must be dropped rather than written
// off the end of the sample buffer.
auto tPassesPastTheCapAreDropped =
    test("FrameTiming/passesPastTheCapAreDropped") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = TimedView {};

    for (auto pass = 0; pass < GpuTimestamps::maxTimedPasses + 4; ++pass)
        view.labels.add("pass" + std::to_string(pass));

    const auto& timings = renderAndCollect(view);

    if (!canTimePasses())
        return;

    check(timings.passes.size() == GpuTimestamps::maxTimedPasses);

    // The cap drops the tail rather than an arbitrary subset.
    check(timings.passes[0].label == "pass0");
    check(timings.passes[GpuTimestamps::maxTimedPasses - 1].label
          == "pass" + std::to_string(GpuTimestamps::maxTimedPasses - 1));
};

auto tTimingsNameAnEarlierFrame = test("FrameTiming/timingsNameAnEarlierFrame") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = TimedView {};
    view.labels = {"only"};

    const auto& timings = renderAndCollect(view);

    // Metal reports a frame total without counters, D3D12 does not. Regression:
    // pending slots that could never complete stopped the timer for good on
    // machines that cannot sample counters.
    if (Platform::isWindows() && !Device::shared().supportsPassTimings())
        return;

    check(timings.frameIndex > 0);
    check(timings.frameIndex < Device::shared().frameIndex());
};

auto tDurationsAreBounded = test("FrameTiming/durationsAreBounded") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = TimedView {};
    view.labels = {"one", "two"};

    const auto& timings = renderAndCollect(view);

    check(timings.milliseconds >= 0.0);
    check(timings.milliseconds < 1000.0);

    if (!canTimePasses())
        return;

    auto total = 0.0;

    for (const auto& pass: timings.passes)
    {
        // Zero is what a sample the GPU never wrote reads as, so this is what
        // fails if the sample indices are swapped or never reach the pass.
        check(pass.milliseconds > 0.0);
        check(pass.milliseconds < 1000.0);

        total += pass.milliseconds;
    }

    // The slack is deliberately tiny: this pins the tick-to-milliseconds
    // conversion, and a one-millisecond tolerance once hid a 41.667x error.
    check(total <= timings.milliseconds + 0.05);
};
