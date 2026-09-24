#include "Common.h"

// TimingScope::EachDispatch - a compute pass that times every kernel it
// dispatches as a region of its own, named after the kernel, so a pass that
// runs a whole network reads back as a per-kernel profile.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto elementCount = 1 << 14;

struct RampKernel final : ComputeProgram
{
    RampKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, toFloat(i) * 0.5f);
    }

    Uniform<OutputBuffer> output;

    EACP_SHADER(output)
};

struct DoubleKernel final : ComputeProgram
{
    DoubleKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, input[i] * 2.f);
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;

    EACP_SHADER(input, output)
};

struct NamedKernel final : ComputeProgram
{
    NamedKernel() { compile(); }

    std::string name() const override { return "ramp, named by hand"; }

    void define() override
    {
        auto i = threadId();
        write(output, i, toFloat(i));
    }

    Uniform<OutputBuffer> output;

    EACP_SHADER(output)
};

Buffer makeOutput()
{
    return Device::shared().makeBuffer((int) sizeof(float) * elementCount);
}
} // namespace

// One region per dispatch, in the order they were encoded, each named
// pass/Kernel - and the results the same as an untimed pass would give.
auto tEachDispatchIsARegion = test("DispatchTiming/eachDispatchIsARegion") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto ramped = makeOutput();
    auto doubled = makeOutput();

    auto ramp = RampKernel {};
    ramp.output = ramped;
    ramp.prepare();

    auto twice = DoubleKernel {};
    twice.input = ramped;
    twice.output = doubled;
    twice.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute(
            "step", DispatchOrder::Serial, TimingScope::EachDispatch);
        pass.dispatch(ramp, elementCount);
        pass.dispatch(twice, elementCount);
        pass.dispatch(ramp, elementCount);
    }

    commands.commit();

    auto values = Vector<float> {};
    values.resize(elementCount);
    doubled.read(values.data(), (std::int64_t) sizeof(float) * elementCount);
    check(values[100] == 100.f);

    if (!commands.supportsPassTimings())
        return;

    const auto& passes = commands.timings().passes;
    check(passes.size() == 3);

    if (passes.size() != 3)
        return;

    check(passes[0].label == "step/RampKernel");
    check(passes[1].label == "step/DoubleKernel");
    check(passes[2].label == "step/RampKernel");

    for (const auto& pass: passes)
        check(pass.milliseconds > 0.0);
};

// totalsByLabel() folds the regions of one kernel into one line, largest first, with
// how many dispatches it covers.
auto tTotalsByLabelSumsAKernel = test("DispatchTiming/totalsByLabelSumsAKernel") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto output = makeOutput();

    auto ramp = RampKernel {};
    ramp.output = output;
    ramp.prepare();

    auto named = NamedKernel {};
    named.output = output;
    named.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute(
            {}, DispatchOrder::Serial, TimingScope::EachDispatch);

        for (auto i = 0; i < 4; ++i)
            pass.dispatch(ramp, elementCount);

        pass.dispatch(named, elementCount);
    }

    commands.commit();

    if (!commands.supportsPassTimings())
        return;

    const auto& timings = commands.timings();
    auto totals = timings.totalsByLabel();

    check(timings.passes.size() == 5);
    check(totals.size() == 2);

    auto sum = 0.0;

    for (const auto& total: totals)
    {
        sum += total.milliseconds;

        if (total.label == "RampKernel")
            check(total.count == 4);
        else
            check(total.label == "ramp, named by hand" && total.count == 1);
    }

    check(totals[0].milliseconds >= totals[1].milliseconds);
    check(sum <= timings.milliseconds + 0.05);
};

// Without EachDispatch a labelled pass is still one region, whatever it runs.
auto tAPassIsStillOneRegion = test("DispatchTiming/aPassIsStillOneRegion") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto output = makeOutput();

    auto ramp = RampKernel {};
    ramp.output = output;
    ramp.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute("whole");
        pass.dispatch(ramp, elementCount);
        pass.dispatch(ramp, elementCount);
    }

    commands.commit();

    if (!commands.supportsPassTimings())
        return;

    check(commands.timings().passes.size() == 1);
};

// Past one set of samples a command buffer takes another: every dispatch comes
// back, however many there are.
auto tManyDispatchesAllComeBack =
    test("DispatchTiming/manyDispatchesAllComeBack") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto output = makeOutput();

    auto ramp = RampKernel {};
    ramp.output = output;
    ramp.prepare();

    constexpr auto dispatches = GpuTimestamps::maxTimedPasses + 100;

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute(
            "many", DispatchOrder::Serial, TimingScope::EachDispatch);

        for (auto i = 0; i < dispatches; ++i)
            pass.dispatch(ramp, 64);
    }

    commands.commit();

    if (!commands.supportsPassTimings())
        return;

    const auto& timings = commands.timings();
    check(timings.passes.size() == dispatches);

    if (timings.passes.size() != dispatches)
        return;

    check(timings.passes.back().label == "many/RampKernel");
    check(timings.passes.back().milliseconds > 0.0);
    check(timings.milliseconds > 0.0);
};
