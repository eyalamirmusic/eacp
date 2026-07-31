#include <eacp/GPUWidgets/GPUWidgets.h>

#include <NanoTest/NanoTest.h>

#include <string>

// The prefix sum the binner's counts go through, on its own.
//
// It is covered by every rasterization test in this directory already - a tile
// whose offset is wrong reads somebody else's segments - but only at the sizes
// those paths happen to have, and only through a picture. What is checked here
// is the thing a picture cannot say: that the levels compose. A scan that
// summed each group of 1024 correctly and never added what the groups before it
// came to draws a perfectly plausible mask for every path whose tiles fit in one
// group, which is most of them.
//
// So the sizes below straddle the group: one element, one short of a group, a
// group exactly, one past it, and past the square of it - which is the first
// size that needs three levels rather than two.
//
// And the counts are not all ones. A sum that dropped its input and counted
// instead would pass on an array of ones at every size in the list.

using namespace nano;
using namespace eacp;
using namespace eacp::GPUWidgets;

namespace
{
constexpr auto perGroup = ScanBlockKernel::perGroup;

// Something with a shape to it, so a scan that lost an element is a different
// number rather than the same one.
Vector<std::uint32_t> countsOfLength(int count)
{
    auto values = Vector<std::uint32_t> {};

    for (auto i = 0; i < count; ++i)
        values.add((std::uint32_t) ((i * 7 + (i % 13)) % 31));

    return values;
}

struct Result
{
    Vector<std::uint32_t> offsets;
    Vector<std::uint32_t> source;
};

Result scanOnGpu(const Vector<std::uint32_t>& counts)
{
    auto result = Result {};
    auto bytes = sizeof(std::uint32_t) * (std::size_t) counts.size();

    auto source = GPU::Buffer {
        GPU::Device::shared(), counts.data(), bytes, GPU::BufferUsage::Storage};
    auto destination = GPU::Buffer {
        GPU::Device::shared(), nullptr, bytes, GPU::BufferUsage::Storage};

    auto sum = PrefixSum {};
    auto commands = GPU::Device::shared().makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        sum.run(pass, source, destination, counts.size());
    }

    commands.commit();

    result.offsets.resize(counts.size());
    result.source.resize(counts.size());

    // A read is what both backends wait for; commit() returns as soon as the
    // list is on the queue on D3D12.
    destination.read(result.offsets.data(), bytes);
    source.read(result.source.data(), bytes);
    return result;
}

void expectScans(int count)
{
    if (!GPU::Device::shared().isValid())
        return;

    auto counts = countsOfLength(count);
    auto result = scanOnGpu(counts);

    auto running = std::uint32_t {0};
    auto wrong = 0;
    auto firstWrong = -1;
    auto leftBehind = 0;

    for (auto i = 0; i < count; ++i)
    {
        if (result.offsets[i] != running)
        {
            ++wrong;

            if (firstWrong < 0)
                firstWrong = i;
        }

        if (result.source[i] != 0)
            ++leftBehind;

        running += counts[i];
    }

    auto where = std::to_string(wrong) + " of " + std::to_string(count)
                 + " wrong, first at " + std::to_string(firstWrong);

    check(wrong == 0, where);

    // The counting sort that follows hands its slots out through this very
    // array, so a scan that read its input without clearing it would have every
    // tile's cursor start at that tile's own count.
    check(leftBehind == 0, std::to_string(leftBehind) + " elements not left zeroed");
}
} // namespace

// One group's worth and less, which is the whole of the sum for any path an
// interface draws.
auto tWithinOneGroup = test("PrefixSum/withinOneGroup") = []
{
    for (auto count: {1, 2, 63, 64, 65, 255, perGroup - 1, perGroup})
        expectScans(count);
};

// Past it, which is where the level above has to carry what the groups below it
// came to. A window-sized path is a couple of thousand tiles and lands here.
auto tAcrossGroups = test("PrefixSum/acrossGroups") = []
{
    for (auto count: {perGroup + 1, perGroup * 2, perGroup * 7 + 3})
        expectScans(count);
};

// And past the square of the group, which is the first size that is three levels
// deep rather than two - a canvas of a hundred and twenty-eight full-width lanes
// is four hundred thousand tiles and lands here.
auto tThreeLevels = test("PrefixSum/pastTheSquareOfAGroup") = []
{ expectScans(perGroup * perGroup + 17); };
