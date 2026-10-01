#include <NanoTest/NanoTest.h>

#include <eacp/GPU/CpuCompute/CpuCompute.h>

#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <span>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

// A dispatch split over the caller's own threads: prepareDispatch once, then
// dispatchGroups over disjoint ranges, each thread in a workspace of its own.
// Every race-free kernel here must give the bits one dispatch gives, whatever
// the partition, and the bits its C++ twin gives.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::GPU::CpuCompute;

namespace
{
constexpr auto sentinel = -1.f;
constexpr auto sentinelBits = 0xdeadbeefu;

using Words = Vector<std::uint32_t>;

Vector<float> makeFloats(int count, float value)
{
    auto values = Vector<float> {};
    values.resize(count, value);
    return values;
}

Words makeUInts(int count, std::uint32_t value)
{
    auto values = Words {};
    values.resize(count, value);
    return values;
}

Vector<float> wobble(int count)
{
    auto values = Vector<float> {};

    for (auto k = 0; k < count; ++k)
        values.add((float) (k % 1013) * 0.37f - (float) (k % 7) * 1.5f);

    return values;
}

Words bitsOf(const Vector<float>& values)
{
    auto bits = Words {};

    for (auto value: values)
        bits.add(std::bit_cast<std::uint32_t>(value));

    return bits;
}

bool sameWords(const Words& a, const Words& b)
{
    if (a.size() != b.size())
        return false;

    for (auto i = 0; i < a.size(); ++i)
    {
        if (a[i] != b[i])
        {
            std::printf("    word %d is 0x%08x, not 0x%08x\n", i, a[i], b[i]);
            return false;
        }
    }

    return true;
}

struct GroupRange
{
    std::int64_t first = 0;
    std::int64_t count = 0;
};

// The ranges between cuts at the given fractions of the dispatch's groups.
Vector<GroupRange> rangesAt(std::int64_t total, std::span<const double> fractions)
{
    auto ranges = Vector<GroupRange> {};
    auto first = std::int64_t {0};

    for (auto fraction: fractions)
    {
        auto cut = (std::int64_t) ((double) total * fraction);
        ranges.add({first, cut - first});
        first = cut;
    }

    ranges.add({first, total - first});
    return ranges;
}

// Each range on a thread of its own, each thread in a workspace of its own.
bool runOnThreads(const Executor& executor,
                  const PreparedDispatch& prepared,
                  const Vector<GroupRange>& ranges)
{
    auto failures = std::atomic<int> {0};
    auto threads = std::vector<std::thread> {};

    for (auto range: ranges)
        threads.emplace_back(
            [&executor, &prepared, &failures, range]
            {
                auto workspace = Workspace {executor.plan()};

                if (!executor.dispatchGroups(
                        prepared, range.first, range.count, workspace))
                    failures.fetch_add(1);
            });

    for (auto& thread: threads)
        thread.join();

    return failures.load() == 0;
}

// Each range in turn on the calling thread, all in one fresh workspace.
bool runInTurn(const Executor& executor,
               const PreparedDispatch& prepared,
               const Vector<GroupRange>& ranges)
{
    auto workspace = Workspace {executor.plan()};
    auto ran = true;

    for (auto range: ranges)
        ran = executor.dispatchGroups(prepared, range.first, range.count, workspace)
              && ran;

    return ran;
}

constexpr auto streamCount = 1 << 20;
constexpr auto streamGain = 0.8125f;

struct StreamKernel final : ComputeKernel
{
    StreamKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto x = input[i];
        write(output, i, x * gain - x * x * 0.25f + 1.5f);
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;
    Uniform<Float> gain;

    EACP_SHADER(input, output, gain)
};

Words streamTwin(const Vector<float>& input, float gain)
{
    auto output = Vector<float> {};

    for (auto x: input)
        output.add(x * gain - x * x * 0.25f + 1.5f);

    return bitsOf(output);
}

struct GridKernel final : ComputeKernel
{
    GridKernel() { compile(); }

    void define() override
    {
        auto position = threadPosition();
        auto cell = position.y * gridWidth() + position.x;
        write(output, cell, toFloat(position.x) + toFloat(position.y) * 100.f);
    }

    Uniform<OutputBuffer> output;

    EACP_SHADER(output)
};

struct VolumeKernel final : ComputeKernel
{
    VolumeKernel() { compile(); }

    void define() override
    {
        auto position = threadPosition3();
        auto cell =
            (position.z * gridHeight() + position.y) * gridWidth() + position.x;
        write(output, cell, cell * 3u + 1u);
    }

    Uniform<UIntOutputBuffer> output;

    EACP_SHADER(output)
};

struct TicketKernel final : ComputeKernel
{
    TicketKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto ticket = atomicAdd(counter, 0u, 1u);
        write(output, ticket, i);
    }

    Uniform<AtomicBuffer> counter;
    Uniform<UIntOutputBuffer> output;

    EACP_SHADER(counter, output)
};

struct GuardedKernel final : ComputeKernel
{
    GuardedKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, i * 7u + gridCount());
    }

    Uniform<UIntOutputBuffer> output;

    EACP_SHADER(output)
};

constexpr auto twoWays = std::array {0.37};
constexpr auto threeWays = std::array {0.11, 0.62};
constexpr auto sevenWays = std::array {0.03, 0.1, 0.25, 0.4, 0.41, 0.77};
} // namespace

auto tStreamSplit =
    test("DispatchGroups/aStreamSplitOverThreadsMatchesOneDispatch") = []
{
    auto input = wobble(streamCount);
    auto kernel = StreamKernel {};
    kernel.gain = streamGain;

    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());
    check(executor.plan().groupsPerBatch() > 1);

    auto serial = makeFloats(streamCount, sentinel);
    auto bindings = Bindings {};
    bindings.set(kernel.input, input);
    bindings.set(kernel.output, serial);
    check(executor.dispatch(bindings, streamCount));

    auto twin = streamTwin(input, streamGain);
    check(sameWords(bitsOf(serial), twin));

    auto partitions = std::array {std::span<const double> {twoWays},
                                  std::span<const double> {threeWays},
                                  std::span<const double> {sevenWays}};

    for (auto fractions: partitions)
    {
        auto output = makeFloats(streamCount, sentinel);
        bindings.set(kernel.output, output);

        auto prepared = executor.prepareDispatch(bindings, streamCount);
        check(prepared.isValid());
        check(prepared.groupCount()
              == streamCount / kernel.graph().threadGroupShape().x);

        auto ranges = rangesAt(prepared.groupCount(), fractions);
        check(ranges.size() == (int) fractions.size() + 1);
        check(runOnThreads(executor, prepared, ranges));
        check(sameWords(bitsOf(output), twin));
    }
};

auto tGridSplit =
    test("DispatchGroups/aGridSplitAcrossAndWithinRowsMatchesOneDispatch") = []
{
    constexpr auto width = 1000;
    constexpr auto height = 37;

    auto kernel = GridKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto serial = makeFloats(width * height, sentinel);
    auto bindings = Bindings {};
    bindings.set(kernel.output, serial);
    check(executor.dispatch(bindings, width, height));

    for (auto y = 0; y < height; ++y)
        for (auto x = 0; x < width; ++x)
            check(serial[y * width + x] == (float) x + (float) y * 100.f);

    auto output = makeFloats(width * height, sentinel);
    bindings.set(kernel.output, output);

    auto prepared = executor.prepareDispatch(bindings, width, height);
    auto row = (std::int64_t) prepared.groups()[0];
    auto total = prepared.groupCount();
    check(prepared.groups()[1] > 2u);
    check(total == row * prepared.groups()[1]);
    check(row % executor.plan().groupsPerBatch() != 0
          || executor.plan().groupsPerBatch() == 1);

    auto cuts =
        std::array<std::int64_t, 5> {3, row, row + 5, 2 * row - 1, total - 2};
    auto ranges = Vector<GroupRange> {};
    auto first = std::int64_t {0};

    for (auto cut: cuts)
    {
        ranges.add({first, cut - first});
        first = cut;
    }

    ranges.add({first, total - first});

    check(runOnThreads(executor, prepared, ranges));
    check(sameWords(bitsOf(output), bitsOf(serial)));

    auto partial = makeFloats(width * height, sentinel);
    bindings.set(kernel.output, partial);
    prepared = executor.prepareDispatch(bindings, width, height);

    auto shape = kernel.graph().threadGroupShape();
    auto firstGroup = row + 5;
    auto groupCount = std::int64_t {3};
    auto workspace = Workspace {executor.plan()};
    check(executor.dispatchGroups(prepared, firstGroup, groupCount, workspace));

    for (auto y = 0; y < height; ++y)
    {
        for (auto x = 0; x < width; ++x)
        {
            auto group = (std::int64_t) (x / shape.x) + row * (y / shape.y);
            auto inRange = group >= firstGroup && group < firstGroup + groupCount;
            auto cell = y * width + x;
            check(partial[cell] == (inRange ? serial[cell] : sentinel));
        }
    }
};

auto tVolumeSplit =
    test("DispatchGroups/aVolumeSplitAcrossSlicesMatchesOneDispatch") = []
{
    constexpr auto width = 50;
    constexpr auto height = 9;
    constexpr auto depth = 7;
    constexpr auto cells = width * height * depth;

    auto kernel = VolumeKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto serial = makeUInts(cells, sentinelBits);
    auto bindings = Bindings {};
    bindings.set(kernel.output, serial);
    check(executor.dispatch(bindings, width, height, depth));

    for (auto cell = 0; cell < cells; ++cell)
        check(serial[cell] == (std::uint32_t) cell * 3u + 1u);

    auto output = makeUInts(cells, sentinelBits);
    bindings.set(kernel.output, output);

    auto prepared = executor.prepareDispatch(bindings, width, height, depth);
    auto total = prepared.groupCount();
    check(total
          == (std::int64_t) prepared.groups()[0] * prepared.groups()[1]
                 * prepared.groups()[2]);
    check(prepared.groups()[2] > 1u);

    auto slice = (std::int64_t) prepared.groups()[0] * prepared.groups()[1];
    auto ranges = Vector<GroupRange> {{0, 1},
                                      {1, slice - 2},
                                      {slice - 1, 2},
                                      {slice + 1, total - slice - 2},
                                      {total - 1, 1}};

    check(runOnThreads(executor, prepared, ranges));
    check(sameWords(output, serial));

    auto again = makeUInts(cells, sentinelBits);
    bindings.set(kernel.output, again);
    prepared = executor.prepareDispatch(bindings, width, height, depth);

    auto single = Vector<GroupRange> {};

    for (auto group = total - 1; group >= 0; --group)
        single.add({group, 1});

    check(runInTurn(executor, prepared, single));
    check(sameWords(again, serial));
};

auto tTicketsAcrossThreads =
    test("DispatchGroups/atomicTicketsAcrossThreadsAreAPermutation") = []
{
    constexpr auto count = 100000;

    auto kernel = TicketKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto counter = makeUInts(1, 0u);
    auto output = makeUInts(count + 8, sentinelBits);

    auto bindings = Bindings {};
    bindings.set(kernel.counter, counter);
    bindings.set(kernel.output, output);

    auto prepared = executor.prepareDispatch(bindings, count);
    check(runOnThreads(
        executor, prepared, rangesAt(prepared.groupCount(), sevenWays)));

    check(counter[0] == (std::uint32_t) count);

    auto seen = makeUInts(count, 0u);

    for (auto ticket = 0; ticket < count; ++ticket)
        if (output[ticket] < (std::uint32_t) count)
            seen[(int) output[ticket]] += 1u;

    auto permutation = true;

    for (auto k = 0; k < count; ++k)
        permutation = permutation && seen[k] == 1u;

    check(permutation);

    for (auto k = count; k < output.size(); ++k)
        check(output[k] == sentinelBits);
};

auto tClipped = test("DispatchGroups/aRangeIsClippedToTheDispatchsGroups") = []
{
    constexpr auto count = 1000;

    auto kernel = GuardedKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto groupWidth = kernel.graph().threadGroupShape().x;
    auto output = makeUInts(count, sentinelBits);
    auto bindings = Bindings {};
    bindings.set(kernel.output, output);

    auto prepared = executor.prepareDispatch(bindings, count);
    auto total = prepared.groupCount();
    check(total == (count + groupWidth - 1) / groupWidth);

    auto workspace = Workspace {executor.plan()};
    auto untouched = [&]
    {
        for (auto word: output)
            if (word != sentinelBits)
                return false;

        return true;
    };

    check(executor.dispatchGroups(prepared, 0, 0, workspace));
    check(executor.dispatchGroups(prepared, 3, -4, workspace));
    check(executor.dispatchGroups(prepared, total, 1, workspace));
    check(executor.dispatchGroups(prepared, total + 5, 10, workspace));
    check(untouched());

    check(executor.dispatchGroups(prepared, total - 1, 1000, workspace));

    auto lastFirst = (int) (total - 1) * groupWidth;

    for (auto i = 0; i < count; ++i)
    {
        auto expected = i >= lastFirst
                            ? (std::uint32_t) i * 7u + (std::uint32_t) count
                            : sentinelBits;
        check(output[i] == expected);
    }

    check(executor.dispatchGroups(prepared, -5, 6, workspace));

    for (auto i = 0; i < groupWidth; ++i)
        check(output[i] == (std::uint32_t) i * 7u + (std::uint32_t) count);

    check(output[groupWidth] == sentinelBits);

    auto empty = executor.prepareDispatch(bindings, 0);
    check(empty.isValid());
    check(empty.groupCount() == 0);
    check(executor.dispatchGroups(empty, 0, 1, workspace));

    auto unbound = executor.prepareDispatch(Bindings {}, count);
    check(!unbound.isValid());
    check(unbound.groupCount() == 0);
    check(!executor.dispatchGroups(unbound, 0, 1, workspace));

    auto wrongRank = executor.prepareDispatch(bindings, 10, 10);
    check(!wrongRank.isValid());
    check(!executor.dispatchGroups(wrongRank, 0, 1, workspace));
};

auto tUniformsAtPrepare =
    test("DispatchGroups/theUniformsAreReadOnceByEachPrepare") = []
{
    constexpr auto count = 5000;

    auto input = wobble(count);
    auto kernel = StreamKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto first = makeFloats(count, sentinel);
    auto bindings = Bindings {};
    bindings.set(kernel.input, input);
    bindings.set(kernel.output, first);

    kernel.gain = 2.f;
    auto prepared = executor.prepareDispatch(bindings, count);
    kernel.gain = 3.f;

    auto second = makeFloats(count, sentinel);
    auto secondBindings = bindings;
    secondBindings.set(kernel.output, second);
    auto reread = executor.prepareDispatch(secondBindings, count);
    kernel.gain = 4.f;

    auto between = makeFloats(count, sentinel);
    auto betweenBindings = bindings;
    betweenBindings.set(kernel.output, between);
    check(executor.dispatch(betweenBindings, count));
    check(sameWords(bitsOf(between), streamTwin(input, 4.f)));

    auto ranges = rangesAt(prepared.groupCount(), threeWays);
    check(runOnThreads(executor, prepared, ranges));
    check(sameWords(bitsOf(first), streamTwin(input, 2.f)));

    check(runOnThreads(executor, reread, ranges));
    check(sameWords(bitsOf(second), streamTwin(input, 3.f)));

    auto builder = ShaderBuilder {};
    auto in = builder.inputBuffer();
    auto out = builder.outputBuffer();
    auto gain = builder.uniform<Float>();
    auto i = builder.threadId();
    builder.write(out, i, in[i] * gain - in[i] * in[i] * 0.25f + 1.5f);

    auto bare = Executor {builder.graph()};
    check(bare.isValid(), bare.reason());

    auto third = makeFloats(count, sentinel);
    auto bareBindings = Bindings {};
    bareBindings.set(in, input);
    bareBindings.set(out, third);

    auto half = 0.5f;
    check(bare.setUniform(0, &half, sizeof(half)));
    auto barePrepared = bare.prepareDispatch(bareBindings, count);

    auto five = 5.f;
    check(bare.setUniform(0, &five, sizeof(five)));
    check(runOnThreads(bare, barePrepared, ranges));
    check(sameWords(bitsOf(third), streamTwin(input, 0.5f)));

    auto fourth = makeFloats(count, sentinel);
    bareBindings.set(out, fourth);
    check(bare.dispatch(bareBindings, count));
    check(sameWords(bitsOf(fourth), streamTwin(input, 5.f)));
};

static_assert(std::is_trivially_copyable_v<PreparedDispatch>);

auto tMismatch =
    test("DispatchGroups/anotherExecutorsWorkspaceOrPreparedDispatchIsRefused") = []
{
    constexpr auto count = 1000;

    auto small = GuardedKernel {};
    auto smallExecutor = Executor {small};
    check(smallExecutor.isValid(), smallExecutor.reason());

    auto input = wobble(count);
    auto large = StreamKernel {};
    auto largeExecutor = Executor {large};
    check(largeExecutor.isValid(), largeExecutor.reason());
    check(largeExecutor.plan().totalWords() > smallExecutor.plan().totalWords());

    auto output = makeUInts(count, sentinelBits);
    auto bindings = Bindings {};
    bindings.set(small.output, output);
    auto prepared = smallExecutor.prepareDispatch(bindings, count);
    check(prepared.isValid());

    auto largeOutput = makeFloats(count, sentinel);
    auto largeBindings = Bindings {};
    largeBindings.set(large.input, input);
    largeBindings.set(large.output, largeOutput);
    auto largePrepared = largeExecutor.prepareDispatch(largeBindings, count);
    check(largePrepared.isValid());

    auto twin = GuardedKernel {};
    auto twinExecutor = Executor {twin};
    check(twinExecutor.plan().totalWords() == smallExecutor.plan().totalWords());

    auto wrongWorkspaces = std::array {Workspace {largeExecutor.plan()},
                                       Workspace {twinExecutor.plan()}};

    for (auto& workspace: wrongWorkspaces)
        check(!smallExecutor.dispatchGroups(
            prepared, 0, prepared.groupCount(), workspace));

    auto workspace = Workspace {smallExecutor.plan()};
    check(!smallExecutor.dispatchGroups(
        largePrepared, 0, largePrepared.groupCount(), workspace));
    check(!largeExecutor.dispatchGroups(
        prepared, 0, prepared.groupCount(), wrongWorkspaces[0]));

    for (auto word: output)
        check(word == sentinelBits);

    for (auto value: largeOutput)
        check(value == sentinel);

    auto moved = std::move(workspace);
    check(!smallExecutor.dispatchGroups(prepared, 0, 1, workspace));
    check(smallExecutor.dispatchGroups(prepared, 0, prepared.groupCount(), moved));

    for (auto i = 0; i < count; ++i)
        check(output[i] == (std::uint32_t) i * 7u + (std::uint32_t) count);
};

auto tIndirectSplit =
    test("DispatchGroups/anIndirectDispatchSplitsAsADirectOneDoes") = []
{
    constexpr auto groups = 7u;
    constexpr auto guard = 400;

    auto kernel = GuardedKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto groupWidth = (std::uint32_t) kernel.graph().threadGroupShape().x;
    auto size = (int) (groups * groupWidth) + 8;
    auto arguments = std::array<std::uint32_t, 5> {9u, 9u, groups, 1u, 1u};

    auto serial = makeUInts(size, sentinelBits);
    auto bindings = Bindings {};
    bindings.set(kernel.output, serial);
    check(executor.dispatchIndirect(bindings, arguments, guard, 2));

    for (auto i = 0; i < size; ++i)
        check(serial[i]
              == (i < guard ? (std::uint32_t) i * 7u + (std::uint32_t) guard
                            : sentinelBits));

    auto output = makeUInts(size, sentinelBits);
    bindings.set(kernel.output, output);

    auto prepared = executor.prepareDispatchIndirect(bindings, arguments, guard, 2);
    check(prepared.isValid());
    check(prepared.groupCount() == (std::int64_t) groups);
    check(runOnThreads(executor, prepared, rangesAt(groups, threeWays)));
    check(sameWords(output, serial));

    auto noArguments =
        executor.prepareDispatchIndirect(bindings, arguments, guard, 3);
    check(noArguments.isValid());
    check(noArguments.groupCount() == 0);
};
