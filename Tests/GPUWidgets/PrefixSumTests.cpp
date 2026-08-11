#include <eacp/GPUWidgets/GPUWidgets.h>

#include <NanoTest/NanoTest.h>

#include <string>

using namespace nano;
using namespace eacp;
using namespace eacp::GPUWidgets;

namespace
{
constexpr auto perGroup = ScanBlockKernel::perGroup;

// Not all ones: a sum that dropped its input and counted instead would pass at
// every size on an array of ones.
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

    // The counting sort hands its slots out through this array, so a scan that
    // left its input would start every tile's cursor at that tile's own count.
    check(leftBehind == 0, std::to_string(leftBehind) + " elements not left zeroed");
}
} // namespace

auto tWithinOneGroup = test("PrefixSum/withinOneGroup") = []
{
    for (auto count: {1, 2, 63, 64, 65, 255, perGroup - 1, perGroup})
        expectScans(count);
};

// Where the level above has to carry what the groups below it came to.
auto tAcrossGroups = test("PrefixSum/acrossGroups") = []
{
    for (auto count: {perGroup + 1, perGroup * 2, perGroup * 7 + 3})
        expectScans(count);
};

// The first size that is three levels deep rather than two.
auto tThreeLevels = test("PrefixSum/pastTheSquareOfAGroup") = []
{ expectScans(perGroup * perGroup + 17); };
