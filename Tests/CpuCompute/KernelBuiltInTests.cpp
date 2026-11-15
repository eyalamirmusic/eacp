#include "CountingHeap.h"

#include <NanoTest/NanoTest.h>

#include <eacp/GPU/CpuCompute/CpuCompute.h>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <utility>

// The kernel built-ins on the CPU executor: localIndex(), groupCount(),
// simdLaneIndex() and simdGroupSize(), over every rank, over groups that do and
// do not divide the grid, batched several groups to a run and one at a time,
// and through an indirect dispatch - against the numbers worked out here from
// the thread's position alone.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::GPU::CpuCompute;

namespace KernelBuiltInTests
{
namespace
{
constexpr auto sentinel = 0xdeadbeefu;
constexpr auto wordsPerThread = 6;

using Extents = std::array<int, 3>;

// Each kernel writes six words a thread at its flat position in the grid.
struct FlatKernel final : ComputeKernel
{
    explicit FlatKernel(ThreadGroupShape shape)
        : ComputeKernel(shape)
    {
        compile();
    }

    void define() override
    {
        auto at = threadId() * (unsigned) wordsPerThread;
        auto groups = groupCount();

        write(output, at, localIndex());
        write(output, at + 1u, simdLaneIndex());
        write(output, at + 2u, simdGroupSize());
        write(output, at + 3u, groups.x());
        write(output, at + 4u, groups.y());
        write(output, at + 5u, groups.z());
    }

    Uniform<UIntOutputBuffer> output;
    EACP_SHADER(output)
};

struct GridKernel final : ComputeKernel
{
    explicit GridKernel(ThreadGroupShape shape)
        : ComputeKernel(shape)
    {
        compile();
    }

    void define() override
    {
        auto id = threadId2();
        auto at = (id.y() * gridWidth() + id.x()) * (unsigned) wordsPerThread;
        auto groups = groupCount();

        write(output, at, localIndex());
        write(output, at + 1u, simdLaneIndex());
        write(output, at + 2u, simdGroupSize());
        write(output, at + 3u, groups.x());
        write(output, at + 4u, groups.y());
        write(output, at + 5u, groups.z());
    }

    Uniform<UIntOutputBuffer> output;
    EACP_SHADER(output)
};

struct VolumeKernel final : ComputeKernel
{
    explicit VolumeKernel(ThreadGroupShape shape)
        : ComputeKernel(shape)
    {
        compile();
    }

    void define() override
    {
        auto id = threadId3();
        auto flat = (id.z() * gridHeight() + id.y()) * gridWidth() + id.x();
        auto at = flat * (unsigned) wordsPerThread;
        auto groups = groupCount();

        write(output, at, localIndex());
        write(output, at + 1u, simdLaneIndex());
        write(output, at + 2u, simdGroupSize());
        write(output, at + 3u, groups.x());
        write(output, at + 4u, groups.y());
        write(output, at + 5u, groups.z());
    }

    Uniform<UIntOutputBuffer> output;
    EACP_SHADER(output)
};

Vector<std::uint32_t> wordsOf(int count, std::uint32_t value)
{
    auto values = Vector<std::uint32_t> {};
    values.resize(count, value);
    return values;
}

int groupsFor(int extent, int size)
{
    return (extent + size - 1) / size;
}

// What thread (x, y, z) of a grid of `extents` in groups of `shape` must have
// written, with the dispatch's group counts given separately so an indirect
// one can say its own.
int countMismatches(const Vector<std::uint32_t>& output,
                    Extents extents,
                    ThreadGroupShape shape,
                    Extents groups)
{
    auto mismatches = 0;

    for (auto z = 0; z < extents[2]; ++z)
    {
        for (auto y = 0; y < extents[1]; ++y)
        {
            for (auto x = 0; x < extents[0]; ++x)
            {
                auto flat = (z * extents[1] + y) * extents[0] + x;
                const auto* words = output.data() + flat * wordsPerThread;
                auto local = (unsigned) (x % shape.x + (y % shape.y) * shape.x
                                         + (z % shape.z) * shape.x * shape.y);

                mismatches += words[0] != local;
                mismatches += words[1] != local % (unsigned) simdGroupWidth;
                mismatches += words[2] != (unsigned) simdGroupWidth;
                mismatches += words[3] != (unsigned) groups[0];
                mismatches += words[4] != (unsigned) groups[1];
                mismatches += words[5] != (unsigned) groups[2];
            }
        }
    }

    return mismatches;
}

Extents directGroups(Extents extents, ThreadGroupShape shape)
{
    return {groupsFor(extents[0], shape.x),
            groupsFor(extents[1], shape.y),
            groupsFor(extents[2], shape.z)};
}

int threadsIn(Extents extents)
{
    return extents[0] * extents[1] * extents[2];
}

void checkFlat(ThreadGroupShape shape, int count, Plan::Options options = {})
{
    auto kernel = FlatKernel {shape};
    auto executor = Executor {kernel, options};
    check(executor.isValid(), executor.reason());

    auto extents = Extents {count, 1, 1};
    auto output = wordsOf(count * wordsPerThread + 1, sentinel);

    auto bindings = Bindings {};
    bindings.set(kernel.output, output);
    check(executor.dispatch(bindings, count));

    check(countMismatches(output, extents, shape, directGroups(extents, shape))
          == 0);
    check(output[count * wordsPerThread] == sentinel);
}
} // namespace

auto tFlat = test("KernelBuiltIns/flatGridsInWholeAndPartialGroups") = []
{
    checkFlat({64}, 1000);
    checkFlat({48}, 100);
    checkFlat({1}, 5);
    checkFlat({128}, 128);
};

// Several groups to a batch and one: the per-lane values are the group's own
// either way.
auto tBatching = test("KernelBuiltIns/aBatchOfGroupsAndAGroupAtATimeAgree") = []
{
    checkFlat({64}, 1000, Plan::Options {1});
    checkFlat({16}, 999, Plan::Options {4096});
};

auto tGrid = test("KernelBuiltIns/gridsInRectangularGroups") = []
{
    for (const auto& [shape, extents]:
         {std::pair {ThreadGroupShape {8, 4}, Extents {20, 10, 1}},
          std::pair {ThreadGroupShape {5, 3}, Extents {11, 7, 1}},
          std::pair {ThreadGroupShape {16, 16}, Extents {32, 32, 1}}})
    {
        auto kernel = GridKernel {shape};
        auto executor = Executor {kernel};
        check(executor.isValid(), executor.reason());

        auto output = wordsOf(threadsIn(extents) * wordsPerThread, sentinel);

        auto bindings = Bindings {};
        bindings.set(kernel.output, output);
        check(executor.dispatch(bindings, extents[0], extents[1]));

        check(countMismatches(output, extents, shape, directGroups(extents, shape))
              == 0);
    }
};

auto tVolume = test("KernelBuiltIns/volumesInBoxGroups") = []
{
    for (const auto& [shape, extents]:
         {std::pair {ThreadGroupShape {4, 2, 3}, Extents {9, 5, 7}},
          std::pair {ThreadGroupShape {4, 4, 4}, Extents {8, 8, 8}}})
    {
        auto kernel = VolumeKernel {shape};
        auto executor = Executor {kernel};
        check(executor.isValid(), executor.reason());

        auto output = wordsOf(threadsIn(extents) * wordsPerThread, sentinel);

        auto bindings = Bindings {};
        bindings.set(kernel.output, output);
        check(executor.dispatch(bindings, extents[0], extents[1], extents[2]));

        check(countMismatches(output, extents, shape, directGroups(extents, shape))
              == 0);
    }
};

// The group counts are the arguments', not the guard count's: a capacity of
// 1000 over groups of 64 would be 16.
auto tIndirect = test("KernelBuiltIns/anIndirectDispatchCountsItsArguments") = []
{
    constexpr auto capacity = 1000;
    auto shape = ThreadGroupShape {64};

    for (const auto& arguments: {Extents {3, 1, 1}, Extents {2, 2, 1}})
    {
        auto kernel = FlatKernel {shape};
        auto executor = Executor {kernel};
        check(executor.isValid(), executor.reason());

        auto output = wordsOf(capacity * wordsPerThread, sentinel);
        auto words = Vector<std::uint32_t> {(std::uint32_t) arguments[0],
                                            (std::uint32_t) arguments[1],
                                            (std::uint32_t) arguments[2]};

        auto bindings = Bindings {};
        bindings.set(kernel.output, output);
        check(executor.dispatchIndirect(bindings, words, capacity, 0));

        auto threads = Extents {arguments[0] * shape.x, 1, 1};
        check(countMismatches(output, threads, shape, arguments) == 0);
        check(output[threads[0] * wordsPerThread] == sentinel);
    }
};

// Split over the caller's threads, each in a workspace of its own: the group
// counts are the dispatch's whatever range a call runs.
auto tGroupRanges = test("KernelBuiltIns/aRangeOfGroupsSeesTheWholeDispatch") = []
{
    auto shape = ThreadGroupShape {8, 4};
    auto extents = Extents {40, 12, 1};

    auto kernel = GridKernel {shape};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto output = wordsOf(threadsIn(extents) * wordsPerThread, sentinel);

    auto bindings = Bindings {};
    bindings.set(kernel.output, output);

    auto prepared = executor.prepareDispatch(bindings, extents[0], extents[1]);
    auto workspace = Workspace {executor.plan()};

    for (auto first = std::int64_t {0}; first < prepared.groupCount(); first += 4)
        check(executor.dispatchGroups(prepared, first, 4, workspace));

    check(countMismatches(output, extents, shape, directGroups(extents, shape))
          == 0);
};

auto tNoAllocation =
    test("KernelBuiltIns/aDispatchAfterTheFirstAllocatesNothing") = []
{
    auto kernel = FlatKernel {{64}};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto output = wordsOf(500 * wordsPerThread, 0u);

    auto bindings = Bindings {};
    bindings.set(kernel.output, output);

    auto warmedUp = executor.dispatch(bindings, 500);

    ExecutorTests::allocationsCounted = 0;
    ExecutorTests::countingHeap = true;
    auto ran = executor.dispatch(bindings, 500);
    ExecutorTests::countingHeap = false;

    check(warmedUp);
    check(ran);
    check(ExecutorTests::allocationsCounted.load() == 0);
};
} // namespace KernelBuiltInTests
