#include "Common.h"

#include <memory>
#include <string>

// Device::lastFrameTimings() - what the GPU spent on each labelled pass.
//
// Two halves, tested differently. The bookkeeping half is exact and is what
// most of this file is about: which labels come back, in what order, how many,
// and how far behind the frame being drawn they are. A wrong sample index or a
// slot recycled a frame early shows up there as labels and numbers that do not
// line up, and it shows up the same way on both backends.
//
// The durations themselves are a measurement, so the assertions about them are
// bounds - non-negative, not absurd, the frame at least as long as the passes
// inside it - and one ordering, between passes a thousand times apart in cost
// that the GPU has to run in turn. That one is a fact about this code rather
// than about the scheduler: it is what fails when a pass is charged for the
// passes still running before it.
//
// Every frame here is an off-screen one through View::renderToImage, so there
// is no window and this runs in CI. Off-screen frames block until the GPU has
// finished, which is also what makes the timing deterministic to read: the
// numbers for a frame are always available by the time the next one begins.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
// A view whose only job is to open the passes it is told to, with the labels it
// is given. Nothing is drawn: a pass with no draws still has a start and an end
// and is still exactly one entry in the timings, which is what is being counted.
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

    // Twice: the first frame is the one being measured, and the second is what
    // collects it. Nothing waits for the GPU to make that happen - a frame's
    // numbers are picked up at the start of a later frame, which is the whole
    // design and is why this needs two.
    view.renderToImage(1.f);
    view.renderToImage(1.f);

    return Device::shared().lastFrameTimings();
}

// Per-pass timing needs hardware counters. The frame total does not, so a
// device without them still reports everything except the breakdown - and the
// cases about the breakdown have nothing to say there.
bool canTimePasses()
{
    return Device::shared().isValid() && Device::shared().supportsPassTimings();
}
} // namespace

// The wiring, stated as directly as it can be: the labels a frame gave its
// passes come back, all of them, in the order they were encoded. A sample index
// off by one or a slot read a frame early puts the wrong number against a name
// here, and nothing else in the suite would notice.
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

// The other half of the contract, and the reason the label is the switch: a
// pass that does not ask to be timed is not timed, and does not appear.
auto tUnlabelledPassesAreNotTimed =
    test("FrameTiming/unlabelledPassesAreNotTimed") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = TimedView {};
    view.labels = {"", "", ""};

    const auto& timings = renderAndCollect(view);

    check(timings.passes.size() == 0);

    // The frame itself is still measured. That number needs no counters and no
    // labels, so it is the one that is always there.
    check(timings.milliseconds >= 0.0);
};

// Mixed, because the ordinals are handed out per timed pass rather than per
// pass: an unlabelled one in the middle must not leave a gap or shift the
// labels that follow it onto the wrong samples.
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

// The cap is a fixed pool, so a frame past it has to lose the extra passes
// rather than write off the end of the sample buffer. maxTimedPasses back out
// of four more than that is the documented behaviour; a crash or all of them
// is not.
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

    // The ones that survived are the first ones, in order - the cap drops the
    // tail rather than an arbitrary handful.
    check(timings.passes[0].label == "pass0");
    check(timings.passes[GpuTimestamps::maxTimedPasses - 1].label
          == "pass" + std::to_string(GpuTimestamps::maxTimedPasses - 1));
};

// Timings describe a frame that has already been drawn, and saying which one is
// what makes them usable: a profiler that cannot tell how far behind it is
// cannot tell a stale reading from a fresh one.
auto tTimingsNameAnEarlierFrame = test("FrameTiming/timingsNameAnEarlierFrame") = []
{
    if (!Device::shared().isValid())
        return;

    auto view = TimedView {};
    view.labels = {"only"};

    const auto& timings = renderAndCollect(view);

    // Whether anything comes back at all on a device with no counters is the
    // one place the backends genuinely differ: Metal measures the frame from
    // the command buffer, so it reports one either way, while D3D12 and Vulkan
    // measure it with the same queries as the passes and have nothing without
    // them.
    //
    // Worth stating rather than skipping everywhere, because the Metal half is
    // what caught the bug this case exists for - a slot that could never
    // complete was left pending forever, and four of those stopped the timer
    // for good on exactly the machines that cannot sample counters.
    if constexpr (!Platform::isApple())
        if (!Device::shared().supportsPassTimings())
            return;

    check(timings.frameIndex > 0);
    check(timings.frameIndex < Device::shared().frameIndex());
};

// The durations, held to what can be asserted about a measurement without
// asserting how a GPU schedules work.
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
        // A pass that ran took *some* time. Zero is what a sample the GPU never
        // wrote reads as, so this is the case that fails if the sample indices
        // are swapped, point at one slot, or never reach the pass descriptor -
        // each of which leaves every other case here passing happily against a
        // column of zeroes.
        check(pass.spanMilliseconds > 0.0);
        check(pass.spanMilliseconds < 1000.0);

        check(pass.milliseconds >= 0.0);
        check(pass.milliseconds <= pass.spanMilliseconds + 1e-6);

        total += pass.milliseconds;
    }

    // The passes run inside the command buffer, so together they cannot outlast
    // it. This is the case that pins the tick-to-milliseconds conversion, and
    // it is worth saying why the slack is this small: the two numbers come from
    // different mechanisms - counter samples against the command buffer's own
    // GPU times - so a scale that is out by a factor puts the passes outside
    // the frame containing them and nothing else here notices.
    //
    // Written loose the first time, it missed exactly that: at 41.667x out, two
    // passes reading 0.19ms each sat inside a frame reading 0.02ms and a
    // one-millisecond tolerance waved it through.
    check(total <= timings.milliseconds + 0.05);
};

namespace FrameTimingTests
{
namespace
{
struct TimingVertex
{
    float position[2];
};
} // namespace
} // namespace FrameTimingTests

EACP_SHADER_VALUE(FrameTimingTests::TimingVertex, Float2)

namespace FrameTimingTests
{
namespace
{
constexpr TimingVertex fullQuad[] = {
    {{-1.f, -1.f}},
    {{1.f, -1.f}},
    {{1.f, 1.f}},
    {{-1.f, -1.f}},
    {{1.f, 1.f}},
    {{-1.f, 1.f}},
};

constexpr auto lightIterations = 1.f;
constexpr auto heavyIterations = 1000.f;

// A fragment shader whose cost is a loop count, so a pass's weight is set by a
// number rather than by how much it draws.
struct LoopShader final : ShaderProgram
{
    LoopShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&TimingVertex::position);
        auto uv = varying(position);

        setPosition(float4(position, 0.f, 1.f));

        auto total = var(0.f);
        auto i = var(0.f);

        loop(i < iterations,
             [&]
             {
                 total += sin(uv.x() * 13.f + i) * 0.001f;
                 i += 1.f;
             });

        setFragment(float4(total, uv.y(), 0.f, 1.f)
                    + sample(previous, uv * 0.5f + 0.5f) * 0.001f);
    }

    Uniform<Float> iterations;
    Uniform<Texture2D> previous;

    EACP_SHADER(iterations, previous)
};

Texture makeWorkTarget()
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = 256;
    descriptor.height = 256;
    descriptor.format = TextureFormat::BGRA8Unorm;
    descriptor.renderTarget = true;
    return Device::shared().makeTexture(descriptor);
}

// Each pass samples what the one before it drew, so each has to wait for it:
// the shape of a frame whose passes feed each other, which is where a pass used
// to be charged for the ones it waited behind. Writing one target is not enough
// - a GPU may shade two clears of the same texture side by side.
struct WorkView final : GPUView
{
    explicit WorkView(std::initializer_list<float> costs)
    {
        setSampleCount(1);

        for (auto cost: costs)
        {
            auto& shader = *shaders.add(std::make_unique<LoopShader>());
            shader.iterations = cost;
            shader.setVertices(fullQuad, 6);
            shader.prepare(1);
        }
    }

    void render(Frame& frame) override
    {
        for (auto index = 0; index < shaders.size(); ++index)
        {
            auto& shader = *shaders[index];
            shader.previous = targets[(index + 1) % 2];

            auto pass = frame.beginPass(targets[index % 2],
                                        {.label = "pass" + std::to_string(index)});
            pass.draw(shader);
        }
    }

    Texture targets[2] = {makeWorkTarget(), makeWorkTarget()};
    Vector<std::unique_ptr<LoopShader>> shaders;
};

// Whether a frame's shares account for it: together nearly the whole frame, and
// the one heavy pass charged far more than any of the light ones. Every pass is
// labelled, so what the shares leave out is only what the command buffer does
// around them - the read-back and the gaps at either end.
bool sharesAccountForTheFrame(const FrameTimings& timings, int heavyPass)
{
    auto total = 0.0;

    for (const auto& pass: timings.passes)
        total += pass.milliseconds;

    if (total < timings.milliseconds * 0.75 - 0.05)
        return false;

    const auto heavy = timings.passes[heavyPass].milliseconds;

    for (auto index = 0; index < timings.passes.size(); ++index)
        if (index != heavyPass && timings.passes[index].milliseconds >= heavy * 0.5)
            return false;

    return true;
}
} // namespace

// The figure that adds up. A heavy pass between light ones, and light ones after
// it, which is the case the shares exist for: on a tile-based GPU each later
// pass's span reaches back to the heavy one, so spans added up to many frames.
//
// The bounds hold on every frame. The accounting is asked of the best of a few,
// because the GPU is shared: another process's work landing inside one light
// pass makes that pass genuinely slow, and that is not this code being wrong.
auto tSharesSumToTheFrame = test("FrameTiming/sharesSumToTheFrame") = []
{
    if (!Device::shared().isValid())
        return;

    constexpr auto heavyPass = 2;
    constexpr auto attempts = 4;

    auto view = WorkView {{lightIterations,
                           lightIterations,
                           heavyIterations,
                           lightIterations,
                           lightIterations,
                           lightIterations}};

    view.setBounds({0.f, 0.f, 64.f, 64.f});
    view.renderToImage(1.f);

    auto accounted = false;

    for (auto attempt = 0; attempt < attempts && !accounted; ++attempt)
    {
        view.renderToImage(1.f);

        const auto& timings = Device::shared().lastFrameTimings();

        if (!canTimePasses())
            return;

        check(timings.passes.size() == 6);

        if (timings.passes.size() != 6)
            return;

        auto total = 0.0;

        for (const auto& pass: timings.passes)
        {
            check(pass.milliseconds >= 0.0);
            check(pass.milliseconds <= timings.milliseconds + 0.05);
            total += pass.milliseconds;
        }

        check(total <= timings.milliseconds + 0.05);

        accounted = sharesAccountForTheFrame(timings, heavyPass);
    }

    check(accounted);
};
} // namespace FrameTimingTests
