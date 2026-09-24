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

// byLabel() folds the regions of one kernel into one line, largest first, with
// how many dispatches it covers.
auto tByLabelSumsAKernel = test("DispatchTiming/byLabelSumsAKernel") = []
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
    auto totals = timings.byLabel();

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
