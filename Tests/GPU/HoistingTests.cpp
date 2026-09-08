#include "Common.h"

#include <cmath>
#include <string>

// Which of the emitter's tN names survive control flow.
//
// A repeated subexpression is bound to a name; a statement that moves what the
// value behind it read gives that name up. The question these pin is what
// happens at the two boundaries: an if, whose bodies may or may not move
// anything, and a loop header, which is re-tested after the body has run.
//
// A name kept where it should not be is a wrong number, so every shape here is
// checked twice: as emitted text on both backends, and - where the value is
// what is at stake - by running the kernel and comparing against the same loop
// written in C++.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto rowLength = 8u;
constexpr auto rowCount = 4;

int occurrences(const std::string& text, const std::string& needle)
{
    auto found = 0;

    for (auto at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + needle.size()))
        ++found;

    return found;
}

bool contains(const std::string& text, const std::string& needle)
{
    return text.find(needle) != std::string::npos;
}

// The argmax scan: the element is read in the condition and again in the body,
// and nothing in the body moves it.
void recordArgMax(ShaderBuilder& builder)
{
    auto input = builder.inputBuffer();
    auto output = builder.outputBuffer();

    auto row = builder.threadId();
    auto base = row * rowLength;
    auto best = builder.var(0u);
    auto bestValue = builder.var(input[base]);
    auto i = builder.var(1u);

    builder.loop(i < rowLength,
                 [&]
                 {
                     auto value = input[base + i];

                     builder.ifThen(value > bestValue,
                                    [&]
                                    {
                                        bestValue = value;
                                        best = i;
                                    });

                     i += 1u;
                 });

    builder.write(output, row, toFloat(best));
}

// The normalising pass: the reciprocal is computed once, reported, and then
// applied to every element of the row.
void recordNormalise(ShaderBuilder& builder)
{
    auto input = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto scales = builder.outputBuffer();

    auto row = builder.threadId();
    auto base = row * rowLength;
    auto total = builder.var(0.0f);
    auto i = builder.var(0u);

    builder.loop(i < rowLength,
                 [&]
                 {
                     total += input[base + i];
                     i += 1u;
                 });

    auto inverse = 1.0f / total;

    builder.write(scales, row, inverse);

    auto j = builder.var(0u);

    builder.loop(j < rowLength,
                 [&]
                 {
                     auto at = base + j;
                     builder.write(output, at, input[at] * inverse);
                     j += 1u;
                 });
}

// The same product before a loop and inside it, over a factor the body raises.
void recordRaisedScale(ShaderBuilder& builder)
{
    auto input = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto tail = builder.outputBuffer();

    auto row = builder.threadId();
    auto scale = builder.var(2.0f);
    auto weighted = input[row] * scale;

    builder.write(output, row, weighted);

    auto i = builder.var(0u);

    builder.loop(i < rowLength,
                 [&]
                 {
                     scale += 1.0f;
                     builder.write(tail, row * rowLength + i, weighted);
                     i += 1u;
                 });
}

// The same, over an element of an output the body stores into.
void recordReadBack(ShaderBuilder& builder)
{
    auto output = builder.outputBuffer();
    auto tally = builder.outputBuffer();

    auto row = builder.threadId();
    auto seen = output[row] * 2.0f;

    builder.write(tally, row, seen);

    auto i = builder.var(0u);

    builder.loop(i < rowLength,
                 [&]
                 {
                     auto at = row * rowLength + i;
                     builder.write(output, at, builder.constant(1.0f));
                     builder.write(tally, at, seen);
                     i += 1u;
                 });
}

// The same, over threadgroup memory the body's barrier may republish.
void recordSharedTile(ShaderBuilder& builder)
{
    auto output = builder.outputBuffer();
    auto tile = builder.shared<Float>((int) rowLength);

    auto row = builder.threadId();
    auto lane = builder.localId();

    builder.write(tile, lane, toFloat(row));
    builder.barrier();

    auto head = tile[0u] * 2.0f;

    builder.write(output, row, head);

    auto i = builder.var(0u);

    builder.loop(i < rowLength,
                 [&]
                 {
                     builder.barrier();
                     builder.write(output, row * rowLength + i, head);
                     i += 1u;
                 });
}

// A bound named before the loop, over a variable the body leaves alone.
void recordFixedBound(ShaderBuilder& builder)
{
    auto output = builder.outputBuffer();

    auto row = builder.threadId();
    auto reach = builder.var(2u);
    auto limit = reach + rowLength;

    builder.write(output, row, toFloat(limit));
    builder.write(output, row + 1u, toFloat(limit));

    auto i = builder.var(0u);

    builder.loop(i < limit, [&] { i += 2u; });
}

// The same bound, over a variable the body raises under it.
void recordRaisedBound(ShaderBuilder& builder)
{
    auto output = builder.outputBuffer();

    auto row = builder.threadId();
    auto reach = builder.var(2u);
    auto limit = reach + rowLength;

    builder.write(output, row, toFloat(limit));
    builder.write(output, row + 1u, toFloat(limit));

    auto i = builder.var(0u);

    builder.loop(i < limit,
                 [&]
                 {
                     reach += 1u;
                     i += 2u;
                 });
}

// A name the if condition needs twice, over a variable the body then raises.
void recordRaisedInBranch(ShaderBuilder& builder)
{
    auto input = builder.inputBuffer();
    auto output = builder.outputBuffer();

    auto row = builder.threadId();
    auto floor = builder.var(3.0f);
    auto scaled = input[row] * floor;

    builder.ifThen(scaled + scaled > 1.0f,
                   [&]
                   {
                       floor = 10.0f;
                       builder.write(output, row, scaled);
                   });
}

struct ArgMaxKernel final : ComputeProgram
{
    ArgMaxKernel() { compile(); }

    void define() override
    {
        auto row = threadId();
        auto base = row * rowLength;
        auto best = var(0u);
        auto bestValue = var(input[base]);
        auto i = var(1u);

        loop(i < rowLength,
             [&]
             {
                 auto value = input[base + i];

                 ifThen(value > bestValue,
                        [&]
                        {
                            bestValue = value;
                            best = i;
                        });

                 i += 1u;
             });

        write(output, row, toFloat(best));
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;

    EACP_SHADER(input, output)
};

struct NormaliseKernel final : ComputeProgram
{
    NormaliseKernel() { compile(); }

    void define() override
    {
        auto row = threadId();
        auto base = row * rowLength;
        auto total = var(0.0f);
        auto i = var(0u);

        loop(i < rowLength,
             [&]
             {
                 total += input[base + i];
                 i += 1u;
             });

        auto inverse = 1.0f / total;

        write(scales, row, inverse);

        auto j = var(0u);

        loop(j < rowLength,
             [&]
             {
                 auto at = base + j;
                 write(output, at, input[at] * inverse);
                 j += 1u;
             });
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;
    Uniform<OutputBuffer> scales;

    EACP_SHADER(input, output, scales)
};

struct RaisedScaleKernel final : ComputeProgram
{
    RaisedScaleKernel() { compile(); }

    void define() override
    {
        auto row = threadId();
        auto scale = var(2.0f);
        auto weighted = input[row] * scale;

        write(output, row, weighted);

        auto i = var(0u);

        loop(i < rowLength,
             [&]
             {
                 scale += 1.0f;
                 write(tail, row * rowLength + i, weighted);
                 i += 1u;
             });
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;
    Uniform<OutputBuffer> tail;

    EACP_SHADER(input, output, tail)
};

Buffer makeStorage(const Vector<float>& values)
{
    return Buffer {Device::shared(),
                   values.data(),
                   (int) sizeof(float) * values.size(),
                   BufferUsage::Storage};
}

Buffer makeStorage(int elements)
{
    auto zeroed = Vector<float> {};
    zeroed.assign(elements, 0.0f);
    return makeStorage(zeroed);
}

Vector<float> readBack(const Buffer& buffer, int elements)
{
    auto values = Vector<float> {};
    values.resize(elements);
    buffer.read(values.data(), (int) sizeof(float) * elements);
    return values;
}

// A row whose largest element sits at a different place in every row, and
// whose values are not ordered around it.
Vector<float> makeRows()
{
    auto values = Vector<float> {};

    for (auto row = 0; row < rowCount; ++row)
        for (auto column = 0; column < (int) rowLength; ++column)
            values.add(column == (row * 3) % (int) rowLength
                           ? 9.0f + (float) row
                           : 1.0f + (float) ((row + column) % 5));

    return values;
}
} // namespace

// The element the scan compares is loaded once, not once per side of the
// branch: buffer0 is subscripted twice, for the seed and for the shared load.
auto tArgMaxLoadsTheElementOnce = test("Hoisting/aBranchKeepsTheLoadItWasGiven") = []
{
    auto builder = ShaderBuilder {};
    recordArgMax(builder);

    for (const auto& source: {emitMetal(builder.graph()), emitHlsl(builder.graph())})
    {
        check(occurrences(source, "buffer0[") == 2);
        check(contains(source, "if ((t1 > v1))"));
        check(contains(source, "v1 = t1;"));
    }
};

// The reciprocal is divided once, before the loop that applies it.
auto tNormaliseDividesOnce = test("Hoisting/aLoopInvariantSurvivesTheHeader") = []
{
    auto builder = ShaderBuilder {};
    recordNormalise(builder);

    for (const auto& source: {emitMetal(builder.graph()), emitHlsl(builder.graph())})
    {
        check(occurrences(source, "(1.0 / v0)") == 1);
        check(source.find("(1.0 / v0)") < source.rfind("while ("));
    }
};

// A variable the body raises: the product is recomputed inside the loop.
auto tRaisedScaleIsRecomputed = test("Hoisting/aVariableTheBodyWritesRetires") = []
{
    auto builder = ShaderBuilder {};
    recordRaisedScale(builder);

    for (const auto& source: {emitMetal(builder.graph()), emitHlsl(builder.graph())})
        check(occurrences(source, "(buffer0[gid] * v0)") == 2);
};

// A buffer element the body stores into: the read is taken again inside.
auto tReadBackIsRecomputed = test("Hoisting/aStoredSlotRetiresItsRead") = []
{
    auto builder = ShaderBuilder {};
    recordReadBack(builder);

    for (const auto& source: {emitMetal(builder.graph()), emitHlsl(builder.graph())})
        check(occurrences(source, "(buffer0[gid] * 2.0)") == 2);
};

// Threadgroup memory behind a barrier: the tile is read again inside the loop.
auto tSharedTileIsRecomputed = test("Hoisting/aBarrierRetiresASharedRead") = []
{
    auto builder = ShaderBuilder {};
    recordSharedTile(builder);

    for (const auto& source: {emitMetal(builder.graph()), emitHlsl(builder.graph())})
        check(occurrences(source, "(s0[0u] * 2.0)") == 2);
};

// The header binds nothing of its own, and reads a standing name where the
// body leaves what it was computed from alone.
auto tFixedBoundIsNamedOnce = test("Hoisting/aFixedBoundReachesTheHeader") = []
{
    auto builder = ShaderBuilder {};
    recordFixedBound(builder);

    for (const auto& source: {emitMetal(builder.graph()), emitHlsl(builder.graph())})
    {
        check(contains(source, "uint t0 = (v0 + 8u);"));
        check(contains(source, "while ((v1 < t0))"));
    }
};

// ...and prints the bound in full where the body raises it, so the header is
// re-tested against what the last iteration left.
auto tRaisedBoundIsRetested = test("Hoisting/aRaisedBoundIsRetested") = []
{
    auto builder = ShaderBuilder {};
    recordRaisedBound(builder);

    for (const auto& source: {emitMetal(builder.graph()), emitHlsl(builder.graph())})
    {
        check(contains(source, "while ((v1 < (v0 + 8u)))"));
        check(!contains(source, "while ((v1 < t"));
    }
};

// A name the condition bound is not reused by a body that moved what it read.
auto tBranchBodyRecomputesWhatItMoved =
    test("Hoisting/anIfBodyRetiresWhatItInvalidates") = []
{
    auto builder = ShaderBuilder {};
    recordRaisedInBranch(builder);

    for (const auto& source: {emitMetal(builder.graph()), emitHlsl(builder.graph())})
    {
        check(occurrences(source, "(buffer0[gid] * v0)") == 2);
        check(source.find("buffer1[gid] = t0;") == std::string::npos);
    }
};

// The scan itself, against the same argmax written in C++.
auto tArgMaxRuns = test("Hoisting/theScanFindsTheSameIndex") = []
{
    if (!Device::shared().isValid())
        return;

    auto rows = makeRows();
    auto input = makeStorage(rows);
    auto output = makeStorage(rowCount);

    auto kernel = ArgMaxKernel {};
    kernel.input = input;
    kernel.output = output;
    kernel.prepare();

    auto commands = Device::shared().makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, rowCount);
    }

    commands.commit();

    auto values = readBack(output, rowCount);
    auto matched = 0;

    for (auto row = 0; row < rowCount; ++row)
    {
        auto best = 0;

        for (auto column = 1; column < (int) rowLength; ++column)
            if (rows[row * (int) rowLength + column]
                > rows[row * (int) rowLength + best])
                best = column;

        if (values[row] == (float) best)
            ++matched;
    }

    check(matched == rowCount);
};

// The normalising pass, against the same division written in C++.
auto tNormaliseRuns = test("Hoisting/theNormaliserScalesByTheSameFactor") = []
{
    if (!Device::shared().isValid())
        return;

    auto rows = makeRows();
    auto input = makeStorage(rows);
    auto output = makeStorage(rowCount * (int) rowLength);
    auto scales = makeStorage(rowCount);

    auto kernel = NormaliseKernel {};
    kernel.input = input;
    kernel.output = output;
    kernel.scales = scales;
    kernel.prepare();

    auto commands = Device::shared().makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, rowCount);
    }

    commands.commit();

    auto normalised = readBack(output, rowCount * (int) rowLength);
    auto factors = readBack(scales, rowCount);
    auto matched = 0;

    for (auto row = 0; row < rowCount; ++row)
    {
        auto total = 0.0f;

        for (auto column = 0; column < (int) rowLength; ++column)
            total += rows[row * (int) rowLength + column];

        auto inverse = 1.0f / total;

        if (std::abs(factors[row] - inverse) > 1e-6f)
            continue;

        auto correct = 0;

        for (auto column = 0; column < (int) rowLength; ++column)
        {
            auto at = row * (int) rowLength + column;

            if (std::abs(normalised[at] - rows[at] * inverse) <= 1e-6f)
                ++correct;
        }

        if (correct == (int) rowLength)
            ++matched;
    }

    check(matched == rowCount);
};

// The retired name is retired in the numbers too: every iteration sees the
// factor the one before it raised, not the one the product was named with.
auto tRaisedScaleRuns = test("Hoisting/theRaisedFactorReachesEveryIteration") = []
{
    if (!Device::shared().isValid())
        return;

    auto rows = makeRows();
    auto input = makeStorage(rows);
    auto output = makeStorage(rowCount);
    auto tail = makeStorage(rowCount * (int) rowLength);

    auto kernel = RaisedScaleKernel {};
    kernel.input = input;
    kernel.output = output;
    kernel.tail = tail;
    kernel.prepare();

    auto commands = Device::shared().makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, rowCount);
    }

    commands.commit();

    auto seeds = readBack(output, rowCount);
    auto raised = readBack(tail, rowCount * (int) rowLength);
    auto correct = 0;

    for (auto row = 0; row < rowCount; ++row)
    {
        if (std::abs(seeds[row] - rows[row] * 2.0f) > 1e-6f)
            continue;

        for (auto step = 0; step < (int) rowLength; ++step)
        {
            auto expected = rows[row] * (3.0f + (float) step);

            if (std::abs(raised[row * (int) rowLength + step] - expected) <= 1e-6f)
                ++correct;
        }
    }

    check(correct == rowCount * (int) rowLength);
};
