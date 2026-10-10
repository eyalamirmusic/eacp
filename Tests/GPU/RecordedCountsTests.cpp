#include "Common.h"

// What a ComputePass says it recorded: pipeline binds, dispatches, indirect
// dispatches and barriers, counted as asked for. The counting sits in the
// portable header, so these numbers are the same on every backend - which is
// what lets a test pin the structure of a chain without reaching into an
// encoder. The outputs are read back as well, so the counts are of work that
// really ran.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto floatBytes = (int) sizeof(float);
constexpr auto lineCount = 100;
constexpr auto gridColumns = 16;
constexpr auto gridRows = 8;
constexpr auto indirectCapacity = 256;
constexpr auto indirectGroups = 2u;
constexpr auto untouched = -1.f;

struct LineKernel final : ComputeProgram
{
    LineKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, base + toFloat(i));
    }

    Uniform<OutputBuffer> output;
    Uniform<Float> base;

    EACP_SHADER(output, base)
};

struct GridKernel final : ComputeProgram
{
    GridKernel() { compile(); }

    void define() override
    {
        auto p = threadPosition();
        auto index = p.y * gridWidth() + p.x;
        write(output, index, toFloat(index));
    }

    Uniform<OutputBuffer> output;

    EACP_SHADER(output)
};

Buffer makeBlank(int elements)
{
    auto values = Vector<float> {};
    values.assign(elements, untouched);

    return Buffer {Device::shared(),
                   values.data(),
                   floatBytes * elements,
                   BufferUsage::Storage};
}

Buffer makeArguments()
{
    std::uint32_t groups[] = {indirectGroups, 1u, 1u};
    return Buffer {Device::shared(), groups, sizeof(groups), BufferUsage::Storage};
}

Vector<float> readFloats(const Buffer& buffer, int elements)
{
    auto values = Vector<float> {};
    values.resize(elements);
    buffer.read(values.data(), floatBytes * elements);
    return values;
}

struct Stages
{
    Stages()
        : line(makeBlank(lineCount))
        , grid(makeBlank(gridColumns * gridRows))
        , indirect(makeBlank(indirectCapacity))
        , arguments(makeArguments())
    {
        lineKernel.output = line;
        lineKernel.base = 10.f;
        lineKernel.prepare();

        gridKernel.output = grid;
        gridKernel.prepare();

        indirectKernel.output = indirect;
        indirectKernel.base = 1000.f;
        indirectKernel.prepare();
    }

    void checkOutputs() const
    {
        auto lineValues = readFloats(line, lineCount);

        for (auto i = 0; i < lineCount; ++i)
            check(lineValues[i] == 10.f + (float) i);

        auto gridValues = readFloats(grid, gridColumns * gridRows);

        for (auto i = 0; i < gridColumns * gridRows; ++i)
            check(gridValues[i] == (float) i);

        auto ran = (int) indirectGroups * ComputePass::threadGroupWidth;
        auto indirectValues = readFloats(indirect, indirectCapacity);

        for (auto i = 0; i < indirectCapacity; ++i)
            check(indirectValues[i] == (i < ran ? 1000.f + (float) i : untouched));
    }

    Buffer line;
    Buffer grid;
    Buffer indirect;
    Buffer arguments;

    LineKernel lineKernel;
    GridKernel gridKernel;
    LineKernel indirectKernel;
};
} // namespace

// Three program dispatches - 1D, 2D and indirect - with a barrier between the
// independent pair and the indirect one, and one more at the end.
auto tConcurrentPassCounts =
    test("RecordedCounts/aConcurrentPassCountsWhatItRecorded") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto stages = Stages {};
    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute({}, DispatchOrder::Concurrent);
        check(pass.recorded() == ComputePass::Recorded {});

        pass.dispatch(stages.lineKernel, lineCount);
        pass.dispatch(stages.gridKernel, gridColumns, gridRows);
        pass.barrier();
        pass.dispatchIndirect(
            stages.indirectKernel, stages.arguments, indirectCapacity);
        pass.barrier();

        const auto& counts = pass.recorded();
        check(counts.pipelineSets == 3);
        check(counts.dispatches == 2);
        check(counts.indirectDispatches == 1);
        check(counts.barriers == 2);
    }

    commands.commit();
    stages.checkOutputs();
};

// A Serial pass records nothing for a barrier(), and still counts every one
// asked for, so a chain switched between the two orders counts the same.
auto tSerialPassCountsBarriers =
    test("RecordedCounts/aSerialPassCountsItsNoOpBarriers") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto stages = Stages {};
    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();

        pass.dispatch(stages.lineKernel, lineCount);
        pass.barrier();
        pass.dispatch(stages.gridKernel, gridColumns, gridRows);
        pass.barrier();
        pass.dispatchIndirect(
            stages.indirectKernel, stages.arguments, indirectCapacity);
        pass.barrier();
        pass.barrier();

        check(pass.recorded()
              == ComputePass::Recorded {.pipelineSets = 3,
                                        .dispatches = 2,
                                        .indirectDispatches = 1,
                                        .barriers = 4});
    }

    commands.commit();
    stages.checkOutputs();
};

// A raw dispatch under a pipeline bound by hand counts one bind and one
// dispatch, and a dispatch with nothing bound - which the pass drops - still
// counts as asked for.
auto tRawCallsCount = test("RecordedCounts/rawCallsCountAsAskedFor") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto commands = device.makeCommandBuffer();
    auto stages = Stages {};

    {
        auto pass = commands.beginCompute();

        pass.dispatch(lineCount);
        check(pass.recorded()
              == ComputePass::Recorded {.pipelineSets = 0, .dispatches = 1});

        pass.setPipeline(stages.gridKernel.pipeline());
        stages.gridKernel.bindResources(pass);
        pass.setBytes(stages.gridKernel.packedUniforms(gridColumns, gridRows),
                      stages.gridKernel.uniformByteSize());
        pass.dispatch(gridColumns, gridRows);

        check(pass.recorded()
              == ComputePass::Recorded {.pipelineSets = 1, .dispatches = 2});
    }

    commands.commit();

    auto gridValues = readFloats(stages.grid, gridColumns * gridRows);

    for (auto i = 0; i < gridColumns * gridRows; ++i)
        check(gridValues[i] == (float) i);
};
