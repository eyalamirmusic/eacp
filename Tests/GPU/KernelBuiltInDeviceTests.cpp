#include "CpuCrossCheck.h"

#include <array>
#include <cstdint>
#include <initializer_list>
#include <utility>

// The kernel built-ins on the device and on the CPU executor, compared:
// localIndex(), groupCount(), simdLaneIndex() and simdGroupSize(), at every
// rank, in groups that do and do not divide the grid, and through an indirect
// dispatch. Every number has one right answer, so the backends agree word for
// word - save the group count of an indirect dispatch on D3D, which has no
// system value for it and reports the groups the guard count covers instead.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::GPU::CrossChecks;

namespace KernelBuiltInDeviceTests
{
namespace
{
constexpr auto wordsPerThread = 6;

using Extents = std::array<int, 3>;

struct FlatBuiltInKernel final : ComputeProgram
{
    explicit FlatBuiltInKernel(ThreadGroupShape shape)
        : ComputeProgram(shape)
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

struct GridBuiltInKernel final : ComputeProgram
{
    explicit GridBuiltInKernel(ThreadGroupShape shape)
        : ComputeProgram(shape)
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

struct VolumeBuiltInKernel final : ComputeProgram
{
    explicit VolumeBuiltInKernel(ThreadGroupShape shape)
        : ComputeProgram(shape)
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

int groupsFor(int extent, int size)
{
    return (extent + size - 1) / size;
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

// Every thread's six words against its position, in a grid of `extents`
// dispatched as `groups`.
void expectBuiltIns(const Vector<std::uint32_t>& output,
                    Extents extents,
                    ThreadGroupShape shape,
                    Extents groups,
                    const char* name)
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

    check(mismatches == 0, name);
}
} // namespace

auto tFlat = test("KernelBuiltInDevice/flatGridsMatchTheCpu") = []
{
    for (const auto& entry: {std::pair {ThreadGroupShape {64}, 1000},
                             std::pair {ThreadGroupShape {48}, 100},
                             std::pair {ThreadGroupShape {128}, 256}})
    {
        auto shape = entry.first;
        auto count = entry.second;
        auto kernel = FlatBuiltInKernel {shape};
        auto extents = Extents {count, 1, 1};

        auto verify = [&](const Readback& readback)
        {
            expectBuiltIns(readback.uints(kernel.output),
                           extents,
                           shape,
                           directGroups(extents, shape),
                           readback.name());
        };

        CrossCheck {kernel}
            .output(kernel.output, count * wordsPerThread)
            .agreeing()
            .run(count, verify);
    }
};

auto tGrid = test("KernelBuiltInDevice/gridsMatchTheCpu") = []
{
    for (const auto& entry:
         {std::pair {ThreadGroupShape {8, 4}, Extents {20, 10, 1}},
          std::pair {ThreadGroupShape {16, 16}, Extents {48, 32, 1}}})
    {
        auto shape = entry.first;
        auto extents = entry.second;
        auto kernel = GridBuiltInKernel {shape};

        auto verify = [&](const Readback& readback)
        {
            expectBuiltIns(readback.uints(kernel.output),
                           extents,
                           shape,
                           directGroups(extents, shape),
                           readback.name());
        };

        CrossCheck {kernel}
            .output(kernel.output, threadsIn(extents) * wordsPerThread)
            .agreeing()
            .run(extents[0], extents[1], verify);
    }
};

auto tVolume = test("KernelBuiltInDevice/volumesMatchTheCpu") = []
{
    auto shape = ThreadGroupShape {4, 2, 4};
    auto extents = Extents {9, 5, 7};
    auto kernel = VolumeBuiltInKernel {shape};

    auto verify = [&](const Readback& readback)
    {
        expectBuiltIns(readback.uints(kernel.output),
                       extents,
                       shape,
                       directGroups(extents, shape),
                       readback.name());
    };

    CrossCheck {kernel}
        .output(kernel.output, threadsIn(extents) * wordsPerThread)
        .agreeing()
        .run(extents[0], extents[1], extents[2], verify);
};

// Metal, Vulkan and the CPU count the arguments' groups. D3D derives the count
// from the guard count, so there it is the capacity's sixteen groups.
auto tIndirect =
    test("KernelBuiltInDevice/anIndirectDispatchCountsItsArguments") = []
{
    constexpr auto capacity = 1000;
    auto shape = ThreadGroupShape {64};
    auto arguments = Extents {3, 1, 1};
    auto kernel = FlatBuiltInKernel {shape};
    auto threads = Extents {arguments[0] * shape.x, 1, 1};

    auto words = Vector<std::uint32_t> {(std::uint32_t) arguments[0],
                                        (std::uint32_t) arguments[1],
                                        (std::uint32_t) arguments[2]};

    auto verify = [&](const Readback& readback)
    {
        auto derivesFromTheGuard =
            readback.backend == Backend::Gpu && Platform::isWindows();
        auto groups = derivesFromTheGuard
                          ? Extents {groupsFor(capacity, shape.x), 1, 1}
                          : arguments;

        expectBuiltIns(
            readback.uints(kernel.output), threads, shape, groups, readback.name());
    };

    CrossCheck {kernel}
        .output(kernel.output, capacity * wordsPerThread)
        .runIndirect(words, capacity, 0, verify);
};
} // namespace KernelBuiltInDeviceTests
