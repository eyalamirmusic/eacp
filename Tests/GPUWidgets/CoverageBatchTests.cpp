#include "CoverageProbe.h"

#include <NanoTest/NanoTest.h>

#include <cmath>
#include <string>

// Many paths in one dispatch, against the same paths one dispatch each.
//
// Every other test of this rasterizer is a batch of one, which is exactly the
// case a batch cannot get wrong: with one path every base is zero, the search
// for which path a thread belongs to has one answer, and the record it reads is
// the first. So none of them can fail on a wrong base, a search off by one, or a
// field of the record read out of the neighbouring one - and those are the whole
// of what batching added.
//
// The reference here is therefore the unbatched rasterization of the same path,
// which PathRasterizerTests has already held to the segment-by-segment CPU
// definition. What is checked is that gathering paths together does not move
// any of them.
//
// The failures worth ruling out are all silent and all specific:
//
//  - a base off by a path: one path draws another's outline
//  - the fill rule or backdrop form read from a neighbouring record: a star
//    fills its own hole, or a region comes out inverted
//  - the origin dropped: every mask lands on top of the first
//  - the block search off by one: a strip along one path's edge belongs to the
//    path before it
//
// So the batch below is deliberately mixed: different sizes, both fill rules,
// both backdrop forms, and origins that are not multiples of the block.

using namespace nano;
using namespace eacp;
using namespace eacp::GPUWidgets;

namespace
{
using Graphics::Point;
using Graphics::Rect;

constexpr auto pi = 3.14159265358979323846f;

// The two sides sum a pixel's segments in the same order here - it is the same
// kernel - so this is looser than it needs to be for anything but the 8-bit
// texture the mask lives in.
constexpr auto tolerance = 1.5f / 255.f;

// ------------------------------------------------------------------ the paths

Path star(Rect bounds, int points)
{
    auto centre = Point {bounds.x + bounds.w * 0.5f, bounds.y + bounds.h * 0.5f};
    auto radius = std::min(bounds.w, bounds.h) * 0.5f;

    auto path = Path {};

    // Every other vertex, which is what makes the outline cross itself and the
    // two fill rules disagree about the middle.
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

// ------------------------------------------------------------------ the batch

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

// Lays the entries out in a row of shelves and rasterizes the lot in one
// dispatch. The origins are deliberately not multiples of the eight-pixel block
// a threadgroup covers: a mask whose corner happened to line up with the grid
// would hide an origin the kernel applied at block granularity rather than at
// pixel granularity.
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

    // Waited for before the texture goes anywhere. commit() returns as soon as
    // the list is on the queue on D3D12, so a test that dispatched into a
    // texture and then let it go took the device down with it - a read is what
    // both backends wait for, and it costs one texel.
    (void) probe::readRegion(target, 0, 0, 1, 1);

    return target;
}

// The same path on its own, which is what every other test in this directory
// has already checked against the definition.
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

// Every entry read back out of the shared texture and held against its own solo
// rasterization.
void checkBatchMatchesAlone(Vector<Entry>& entries)
{
    if (!GPU::Device::shared().isValid())
        return;

    auto batch = CoverageBatch {};
    auto target = rasterizeTogether(entries, batch);

    check(batch.getPathCount() == entries.size());
    check(batch.getDispatchCount() == 1);

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
} // namespace

// The mixed batch. Sizes an order of magnitude apart, so the paths do not all
// have the same block count and a base that ignored one of them would still be
// right for the others.
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

// Both fill rules in one batch, on the same geometry. Under non-zero the star is
// solid and under even-odd it has a pentagonal hole, so a kernel reading the
// rule out of the wrong record does not produce a slightly different picture -
// it produces the other one.
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

    // And the two really are different pictures, so the check above could have
    // failed. The centre is the hole under even-odd and solid under non-zero.
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

// Both backdrop forms in one batch. A full-window ellipse takes the step form
// and a small dense star takes the array, so the two arms of the branch are both
// live in one dispatch - and each path's base has to reach the buffer its own
// form lives in.
auto tBackdropFormsDoNotCross =
    test("CoverageBatch/bothBackdropFormsInOneBatch") = []
{
    auto entries = Vector<Entry> {};
    entries.add({ellipse({0.f, 0.f, 700.f, 460.f}), FillRule::NonZero, 2.f});
    entries.add({star({0.f, 0.f, 36.f, 36.f}, 9), FillRule::EvenOdd, 2.f});
    entries.add({ellipse({0.f, 0.f, 640.f, 420.f}), FillRule::EvenOdd, 2.f});

    checkBatchMatchesAlone(entries);
};

// A batch big enough that the block grid is a rectangle rather than a row, which
// is the layout the dispatch has to use at all - a dimension may have 65,535
// threadgroups and a canvas has more blocks than that.
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

// An empty batch is not a dispatch. A frame in which nothing changed geometry
// should cost nothing at all, which is the steady state of every interface that
// is not being dragged.
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

// What the batch is for: one dispatch and one set of buffer updates however many
// paths went into it. Unbatched, the same frame was one dispatch and three or
// four updates per path.
auto tOneDispatchWhateverTheCount =
    test("CoverageBatch/costIsPerFrameNotPerPath") = []
{
    if (!GPU::Device::shared().isValid())
        return;

    auto entries = Vector<Entry> {};

    for (auto i = 0; i < 40; ++i)
        entries.add(
            {ellipse({0.f, 0.f, 30.f + (float) i, 24.f}), FillRule::NonZero, 2.f});

    auto batch = CoverageBatch {};
    auto target = rasterizeTogether(entries, batch);

    check(batch.getPathCount() == 40);
    check(batch.getDispatchCount() == 1);

    // Seven buffers: the segments, the tile offsets, the two halves of the step
    // backdrop, the array backdrop, the records, and the block offsets. Not
    // seven per path, which is the whole point.
    check(batch.getBufferUpdateCount() == 7);
};
