#include "CoverageProbe.h"

#include <NanoTest/NanoTest.h>

#include <cmath>
#include <string>

using namespace nano;
using namespace eacp;
using namespace eacp::GPUWidgets;

namespace
{
using Graphics::Point;
using Graphics::Rect;

constexpr auto pi = 3.14159265358979323846f;

// A step and a half of the 8-bit channel the mask lives in.
constexpr auto tolerance = 1.5f / 255.f;

Path star(Rect bounds, int points)
{
    auto centre = Point {bounds.x + bounds.w * 0.5f, bounds.y + bounds.h * 0.5f};
    auto radius = std::min(bounds.w, bounds.h) * 0.5f;

    auto path = Path {};

    // Every other vertex, so the outline crosses itself and the two fill rules
    // disagree about the middle.
    for (auto i = 0; i < points; ++i)
    {
        auto angle = 2.f * pi * (float) (i * 2 % points) / (float) points;
        auto at = Point {centre.x + std::sin(angle) * radius,
                         centre.y - std::cos(angle) * radius};

        if (i == 0)
            path.moveTo(at);
        else
            path.lineTo(at);
    }

    path.close();
    return path;
}

Path ellipse(Rect bounds)
{
    auto path = Path {};
    path.addEllipse(bounds);
    return path;
}

Path roundedRect(Rect bounds, float radius)
{
    auto path = Path {};
    path.addRoundedRect(bounds, radius);
    return path;
}

struct Entry
{
    Path path;
    FillRule rule = FillRule::NonZero;
    float scale = 2.f;

    // Filled in as the batch is built.
    int originX = 0;
    int originY = 0;
    int width = 0;
    int height = 0;
};

GPU::Texture makeTarget(int width, int height)
{
    auto descriptor = GPU::TextureDescriptor {};
    descriptor.width = width;
    descriptor.height = height;
    descriptor.format = GPU::TextureFormat::RGBA8Unorm;
    descriptor.computeWrite = true;
    return {GPU::Device::shared(), descriptor, nullptr};
}

// Deliberately not a multiple of the eight-pixel block a threadgroup covers: an
// origin lined up with the grid would hide one applied at block granularity.
constexpr auto gap = 3;

GPU::Texture rasterizeTogether(Vector<Entry>& entries, CoverageBatch& batch)
{
    auto rasterizers = Vector<PathRasterizer> {};
    rasterizers.resize(entries.size());

    auto cursorX = gap;
    auto tallest = 0;

    for (auto i = 0; i < entries.size(); ++i)
    {
        auto& entry = entries[i];

        rasterizers[i].setScale(entry.scale);
        rasterizers[i].setPath(entry.path, entry.rule);

        entry.width = rasterizers[i].getCoverageWidth();
        entry.height = rasterizers[i].getCoverageHeight();
        entry.originX = cursorX;
        entry.originY = gap;

        cursorX += entry.width + gap;
        tallest = std::max(tallest, entry.height);
    }

    auto target = makeTarget(cursorX, tallest + gap * 2);

    batch.begin(target);

    for (auto i = 0; i < entries.size(); ++i)
    {
        rasterizers[i].setTarget(target, entries[i].originX, entries[i].originY);
        batch.add(rasterizers[i]);
    }

    auto commands = GPU::Device::shared().makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        batch.dispatch(pass);
    }

    commands.commit();

    // commit() returns as soon as the list is on the queue on D3D12, and a
    // texture released before then takes the device down. A read waits on both
    // backends and costs one texel.
    (void) probe::readRegion(target, 0, 0, 1, 1);

    return target;
}

Vector<float> rasterizeAlone(const Entry& entry)
{
    auto rasterizer = PathRasterizer {};
    return probe::rasterize(rasterizer, entry.path, entry.scale, entry.rule);
}

struct Difference
{
    int past = 0;
    float worst = 0.f;
    int worstX = 0;
    int worstY = 0;
};

Difference compare(const Vector<float>& batched,
                   const Vector<float>& alone,
                   int width,
                   int height)
{
    auto result = Difference {};

    for (auto y = 0; y < height; ++y)
    {
        for (auto x = 0; x < width; ++x)
        {
            auto at = y * width + x;
            auto delta = std::abs(batched[at] - alone[at]);

            if (delta > tolerance)
                ++result.past;

            if (delta > result.worst)
            {
                result.worst = delta;
                result.worstX = x;
                result.worstY = y;
            }
        }
    }

    return result;
}

void checkEachMatchesAlone(Vector<Entry>& entries, const GPU::Texture& target)
{
    for (auto i = 0; i < entries.size(); ++i)
    {
        const auto& entry = entries[i];

        auto batched = probe::readRegion(
            target, entry.originX, entry.originY, entry.width, entry.height);
        auto alone = rasterizeAlone(entry);

        check(alone.size() == entry.width * entry.height);
        check(batched.size() == alone.size());

        if (batched.size() != alone.size())
            continue;

        auto difference = compare(batched, alone, entry.width, entry.height);

        auto where = std::to_string(difference.past) + " pixels differ in path "
                     + std::to_string(i) + ", worst "
                     + std::to_string(difference.worst) + " at ("
                     + std::to_string(difference.worstX) + ", "
                     + std::to_string(difference.worstY) + ")";

        check(difference.past == 0, where);
    }
}

void checkBatchMatchesAlone(Vector<Entry>& entries)
{
    if (!GPU::Device::shared().isValid())
        return;

    auto batch = CoverageBatch {};
    auto target = rasterizeTogether(entries, batch);

    check(batch.getPathCount() == entries.size());
    checkEachMatchesAlone(entries, target);
}
} // namespace

// Sizes an order of magnitude apart, so the paths do not share a block count and
// a base that ignored one of them would still be right for the others.
auto tMixedBatchMatchesAlone =
    test("CoverageBatch/everyPathMatchesItsOwnDispatch") = []
{
    auto entries = Vector<Entry> {};
    entries.add({star({0.f, 0.f, 40.f, 40.f}, 5), FillRule::NonZero, 2.f});
    entries.add({ellipse({0.f, 0.f, 220.f, 130.f}), FillRule::NonZero, 2.f});
    entries.add({roundedRect({0.f, 0.f, 64.f, 90.f}, 12.f), FillRule::NonZero, 2.f});
    entries.add({star({0.f, 0.f, 150.f, 150.f}, 7), FillRule::EvenOdd, 2.f});

    checkBatchMatchesAlone(entries);
};

auto tFillRulesDoNotCross = test("CoverageBatch/eachPathKeepsItsOwnFillRule") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto entries = Vector<Entry> {};
    entries.add({star({0.f, 0.f, 90.f, 90.f}, 5), FillRule::EvenOdd, 2.f});
    entries.add({star({0.f, 0.f, 90.f, 90.f}, 5), FillRule::NonZero, 2.f});

    checkBatchMatchesAlone(entries);

    auto batch = CoverageBatch {};
    auto target = rasterizeTogether(entries, batch);

    // The centre is the hole under even-odd and solid under non-zero, so the
    // comparison above could have failed.
    auto hollow = probe::readRegion(target,
                                    entries[0].originX,
                                    entries[0].originY,
                                    entries[0].width,
                                    entries[0].height);
    auto solid = probe::readRegion(target,
                                   entries[1].originX,
                                   entries[1].originY,
                                   entries[1].width,
                                   entries[1].height);

    auto centre = entries[0].height / 2 * entries[0].width + entries[0].width / 2;

    check(hollow[centre] < 0.01f);
    check(solid[centre] > 0.99f);
};

// The cell base is the one base that grows with a path's area, so a small path
// between two huge ones is where a base taken from the wrong record shows.
auto tCellBasesDoNotCross = test("CoverageBatch/backdropsOfVeryDifferentSizes") = []
{
    auto entries = Vector<Entry> {};
    entries.add({ellipse({0.f, 0.f, 700.f, 460.f}), FillRule::NonZero, 2.f});
    entries.add({star({0.f, 0.f, 36.f, 36.f}, 9), FillRule::EvenOdd, 2.f});
    entries.add({ellipse({0.f, 0.f, 640.f, 420.f}), FillRule::EvenOdd, 2.f});

    checkBatchMatchesAlone(entries);
};

// Backdrop cells are added into, so a second dispatch that did not clear them
// would double the backdrop -- invisible to any picture taken only once.
auto tSecondDispatchDrawsTheSame =
    test("CoverageBatch/dispatchingTwiceDrawsTheSameThing") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto entries = Vector<Entry> {};
    entries.add({star({0.f, 0.f, 44.f, 44.f}, 7), FillRule::NonZero, 2.f});
    entries.add({roundedRect({0.f, 0.f, 70.f, 52.f}, 9.f), FillRule::NonZero, 2.f});

    auto batch = CoverageBatch {};
    auto target = rasterizeTogether(entries, batch);

    auto commands = GPU::Device::shared().makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        batch.dispatch(pass);
    }

    commands.commit();

    checkEachMatchesAlone(entries, target);
};

// Big enough that the block grid is a rectangle rather than a row, since a
// dimension may have only 65,535 threadgroups.
auto tManyPathsInOneDispatch = test("CoverageBatch/manyPathsStillOneDispatch") = []
{
    auto entries = Vector<Entry> {};

    for (auto i = 0; i < 24; ++i)
    {
        auto size = 24.f + (float) (i % 6) * 11.f;
        auto rule = (i % 3) == 0 ? FillRule::EvenOdd : FillRule::NonZero;

        entries.add({star({0.f, 0.f, size, size}, 5 + i % 4), rule, 2.f});
    }

    checkBatchMatchesAlone(entries);
};

auto tEmptyBatchDispatchesNothing =
    test("CoverageBatch/nothingGatheredIsNoDispatch") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto target = makeTarget(64, 64);
    auto batch = CoverageBatch {};

    batch.begin(target);
    check(batch.isEmpty());

    auto commands = GPU::Device::shared().makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        batch.dispatch(pass);
    }

    commands.commit();

    check(batch.getDispatchCount() == 0);
    check(batch.getBufferUpdateCount() == 0);
};

// Held against a batch of one rather than a written-out number, so adding a
// stage does not need an edit that could quietly absorb a per-path dispatch.
auto tOneDispatchWhateverTheCount =
    test("CoverageBatch/costIsPerFrameNotPerPath") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto shape = ellipse({0.f, 0.f, 44.f, 24.f});

    auto gather = [&](int count, CoverageBatch& batch)
    {
        auto entries = Vector<Entry> {};

        for (auto i = 0; i < count; ++i)
            entries.add({shape, FillRule::NonZero, 2.f});

        return rasterizeTogether(entries, batch);
    };

    auto one = CoverageBatch {};
    auto many = CoverageBatch {};

    gather(1, one);
    gather(40, many);

    check(one.getPathCount() == 1);
    check(many.getPathCount() == 40);

    check(many.getDispatchCount() == one.getDispatchCount());
    check(many.getBufferUpdateCount() == one.getBufferUpdateCount());
};
