#include "CpuPathKernels.h"
#include "PathShapes.h"

#include <NanoTest/NanoTest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <string>

// The binning stages of a batch - clear, count, backdrop, sum, fill - run on the
// CPU executor over the batches CoverageBatchTests draws, and held to what the
// GPU left in the same buffers after each stage.
//
// Both sides run the whole chain from the same gathered arrays, each stage
// reading what its own side's stage before it wrote, and every buffer a stage
// writes is compared as soon as it has run - so a disagreement names the first
// stage it appears in rather than the picture it ends up in.
//
// Everything is an exact comparison. The cells and the counts are integer
// atomic adds, whose sum no order changes. The one thing an order does change
// is where the fill puts each segment inside its own tile's run - the slot is
// whatever the tile's atomic cursor handed out - so the entries are compared as
// the same set per tile, each one bit for bit, and the space past the last run
// as untouched on both sides.

using namespace nano;
using namespace eacp;
using namespace eacp::GPUWidgets;

namespace
{
using cpu::UInts;
using Graphics::Rect;

constexpr auto untouched = 0xdeadbeefu;
constexpr auto untouchedFloat = -12345.f;

// Past the end of every array a stage writes, so a guard that let a thread
// through shows as a changed element rather than a write nobody reads.
constexpr auto pad = 5;

struct Entry
{
    Path path;
    FillRule rule = FillRule::NonZero;
    float scale = 2.f;
};

// CoverageBatch::add and upload, gathered by hand so both sides get the arrays
// and the sizes rather than the batch's private buffers.
struct Scene
{
    Vector<float> segments;
    Vector<float> records;
    Vector<float> segmentStarts;
    Vector<float> scanStarts;

    int paths = 0;
    int cells = 0;
    int tiles = 0;
    int entries = 0;
    int scanRows = 0;

    int segmentCount() const { return segments.size() / 4; }
    int tileSlots() const { return tiles + 1; }
};

Scene gather(const Vector<Entry>& entries)
{
    auto scene = Scene {};

    for (const auto& entry: entries)
    {
        auto rasterizer = PathRasterizer {};
        rasterizer.setScale(entry.scale);
        rasterizer.setPath(entry.path, entry.rule);

        if (rasterizer.isEmpty())
            continue;

        scene.segmentStarts.add((float) scene.segmentCount());
        scene.scanStarts.add((float) scene.scanRows);

        scene.records.add((float) scene.cells);
        scene.records.add((float) rasterizer.getCoverageWidth());
        scene.records.add((float) rasterizer.getCoverageHeight());
        scene.records.add((float) scene.tiles);
        scene.records.add(entry.rule == FillRule::EvenOdd ? 1.f : 0.f);
        scene.records.add(0.f);
        scene.records.add(0.f);
        scene.records.add(0.f);

        for (auto value: rasterizer.getSegments())
            scene.segments.add(value);

        scene.cells += rasterizer.getCellCount();
        scene.tiles += rasterizer.getTileCount();
        scene.entries += rasterizer.getEntryBound();
        scene.scanRows += rasterizer.getCoverageHeight();
        ++scene.paths;
    }

    scene.segmentStarts.add((float) scene.segmentCount());
    scene.scanStarts.add((float) scene.scanRows);
    return scene;
}

// What every stage left, in the order they ran.
struct Stages
{
    UInts clearedCells;
    UInts clearedCounts;
    UInts countedCells;
    UInts countedCounts;
    UInts scannedCells;
    UInts offsets;
    UInts summedCounts;
    UInts filledCounts;
    Vector<float> filledEntries;
};

UInts cellsOf(const Scene& scene)
{
    return cpu::uintsOf(scene.cells + pad, untouched);
}

UInts tileArrayOf(const Scene& scene)
{
    return cpu::uintsOf(scene.tileSlots() + pad, untouched);
}

Vector<float> entriesOf(const Scene& scene)
{
    auto values = Vector<float> {};
    values.assign(4 * (scene.entries + pad), untouchedFloat);
    return values;
}

int clearCount(const Scene& scene)
{
    return std::max(scene.cells, scene.tileSlots());
}

void setBinUniforms(BinKernel& bin, const Scene& scene, unsigned mode)
{
    bin.entryCapacity = (std::uint32_t) scene.entries;
    bin.pathCount = scene.paths;
    bin.mode = mode;
}

void scanBackdropsOnCpu(const Scene& scene, UInts& cells)
{
    auto scan = BackdropScanKernel {};
    auto bindings = GPU::CpuCompute::Bindings {};
    bindings.set(scan.records, scene.records);
    bindings.set(scan.cells, cells);
    bindings.set(scan.pathStarts, scene.scanStarts);
    scan.pathCount = scene.paths;
    cpu::dispatch(scan, bindings, scene.scanRows, "BackdropScanKernel");
}

Stages runOnCpu(const Scene& scene)
{
    auto stages = Stages {};

    auto cells = cellsOf(scene);
    auto counts = tileArrayOf(scene);
    auto offsets = tileArrayOf(scene);
    auto entries = entriesOf(scene);

    auto clear = ClearKernel {};
    auto clearBindings = GPU::CpuCompute::Bindings {};
    clearBindings.set(clear.cells, cells);
    clearBindings.set(clear.tileCounts, counts);
    clear.cellCount = (std::uint32_t) scene.cells;
    clear.tileCount = (std::uint32_t) scene.tileSlots();
    cpu::dispatch(clear, clearBindings, clearCount(scene), "ClearKernel");

    stages.clearedCells = cells;
    stages.clearedCounts = counts;

    auto bin = BinKernel {};
    auto binBindings = GPU::CpuCompute::Bindings {};
    binBindings.set(bin.segments, scene.segments);
    binBindings.set(bin.records, scene.records);
    binBindings.set(bin.pathStarts, scene.segmentStarts);
    binBindings.set(bin.cells, cells);
    binBindings.set(bin.tileCounts, counts);
    binBindings.set(bin.tileOffsets, offsets);
    binBindings.set(bin.tileSegments, entries);

    setBinUniforms(bin, scene, BinKernel::countMode);
    cpu::dispatch(bin, binBindings, scene.segmentCount(), "BinKernel count");

    stages.countedCells = cells;
    stages.countedCounts = counts;

    scanBackdropsOnCpu(scene, cells);
    stages.scannedCells = cells;

    cpu::prefixSum(counts, offsets, scene.tileSlots());

    stages.offsets = offsets;
    stages.summedCounts = counts;

    setBinUniforms(bin, scene, BinKernel::fillMode);
    cpu::dispatch(bin, binBindings, scene.segmentCount(), "BinKernel fill");

    stages.filledCounts = counts;
    stages.filledEntries = entries;
    return stages;
}

// One stage per command buffer, read back before the next is recorded: the
// reads are what both backends wait for, and the buffers carry each stage's
// output into the next exactly as a batch's single pass does.
template <typename Record>
void submit(Record&& record)
{
    auto commands = GPU::Device::shared().makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        record(pass);
    }

    commands.commit();
}

Stages runOnGpu(const Scene& scene)
{
    auto stages = Stages {};

    auto cellCount = scene.cells + pad;
    auto tileCount = scene.tileSlots() + pad;

    auto segments = cpu::bufferOf(scene.segments);
    auto records = cpu::bufferOf(scene.records);
    auto segmentStarts = cpu::bufferOf(scene.segmentStarts);
    auto scanStarts = cpu::bufferOf(scene.scanStarts);
    auto cells = cpu::bufferOf(cellsOf(scene));
    auto counts = cpu::bufferOf(tileArrayOf(scene));
    auto offsets = cpu::bufferOf(tileArrayOf(scene));
    auto entries = cpu::bufferOf(entriesOf(scene));

    auto& clear = sharedKernel<ClearKernel>();
    clear.cells = cells;
    clear.tileCounts = counts;
    clear.cellCount = (std::uint32_t) scene.cells;
    clear.tileCount = (std::uint32_t) scene.tileSlots();
    submit([&](GPU::ComputePass& pass) { pass.dispatch(clear, clearCount(scene)); });

    stages.clearedCells = cpu::readBack<std::uint32_t>(cells, cellCount);
    stages.clearedCounts = cpu::readBack<std::uint32_t>(counts, tileCount);

    auto& bin = sharedKernel<BinKernel>();
    bin.segments = segments;
    bin.records = records;
    bin.pathStarts = segmentStarts;
    bin.cells = cells;
    bin.tileCounts = counts;
    bin.tileOffsets = offsets;
    bin.tileSegments = entries;

    setBinUniforms(bin, scene, BinKernel::countMode);
    submit([&](GPU::ComputePass& pass)
           { pass.dispatch(bin, scene.segmentCount()); });

    stages.countedCells = cpu::readBack<std::uint32_t>(cells, cellCount);
    stages.countedCounts = cpu::readBack<std::uint32_t>(counts, tileCount);

    auto& scan = sharedKernel<BackdropScanKernel>();
    scan.records = records;
    scan.cells = cells;
    scan.pathStarts = scanStarts;
    scan.pathCount = scene.paths;
    submit([&](GPU::ComputePass& pass) { pass.dispatch(scan, scene.scanRows); });

    stages.scannedCells = cpu::readBack<std::uint32_t>(cells, cellCount);

    auto sum = PrefixSum {};
    submit([&](GPU::ComputePass& pass)
           { sum.run(pass, counts, offsets, scene.tileSlots()); });

    stages.offsets = cpu::readBack<std::uint32_t>(offsets, tileCount);
    stages.summedCounts = cpu::readBack<std::uint32_t>(counts, tileCount);

    setBinUniforms(bin, scene, BinKernel::fillMode);
    submit([&](GPU::ComputePass& pass)
           { pass.dispatch(bin, scene.segmentCount()); });

    stages.filledCounts = cpu::readBack<std::uint32_t>(counts, tileCount);
    stages.filledEntries = cpu::readBack<float>(entries, 4 * (scene.entries + pad));
    return stages;
}

// ------------------------------------------------------------------ the scenes

// The batches CoverageBatchTests draws: mixed sizes and rules, backdrops of very
// different areas, and enough paths that the search has somewhere to go wrong.
Vector<Entry> mixedBatch()
{
    using namespace shapes;

    auto entries = Vector<Entry> {};
    entries.add({star({0.f, 0.f, 40.f, 40.f}, 5), FillRule::NonZero, 2.f});
    entries.add({ellipse({0.f, 0.f, 220.f, 130.f}), FillRule::NonZero, 2.f});
    entries.add({roundedRect({0.f, 0.f, 64.f, 90.f}, 12.f), FillRule::NonZero, 2.f});
    entries.add({star({0.f, 0.f, 150.f, 150.f}, 7), FillRule::EvenOdd, 2.f});
    return entries;
}

Vector<Entry> veryDifferentBackdrops()
{
    using namespace shapes;

    auto entries = Vector<Entry> {};
    entries.add({ellipse({0.f, 0.f, 700.f, 460.f}), FillRule::NonZero, 2.f});
    entries.add({star({0.f, 0.f, 36.f, 36.f}, 9), FillRule::EvenOdd, 2.f});
    entries.add({ellipse({0.f, 0.f, 640.f, 420.f}), FillRule::EvenOdd, 2.f});
    return entries;
}

Vector<Entry> manyPaths()
{
    auto entries = Vector<Entry> {};

    for (auto i = 0; i < 24; ++i)
    {
        auto size = 24.f + (float) (i % 6) * 11.f;
        auto rule = (i % 3) == 0 ? FillRule::EvenOdd : FillRule::NonZero;

        entries.add({shapes::star({0.f, 0.f, size, size}, 5 + i % 4), rule, 2.f});
    }

    return entries;
}

struct Named
{
    const char* name;
    Vector<Entry> (*entries)();
};

const auto scenes = std::array {Named {"mixed", mixedBatch},
                                Named {"backdrops", veryDifferentBackdrops},
                                Named {"many", manyPaths}};

// Both sides over every scene, with the stage the case is about checked by
// `compare`. The CPU always runs and `checkCpu` sees it whether or not a device
// came up; only the comparison needs the GPU.
template <typename CheckCpu, typename Compare>
void forEachScene(CheckCpu&& checkCpu, Compare&& compare)
{
    for (const auto& named: scenes)
    {
        auto scene = gather(named.entries());
        auto onCpu = runOnCpu(scene);
        auto where = std::string {named.name} + ": ";

        checkCpu(onCpu, scene, where);

        if (!GPU::Device::shared().isValid())
            continue;

        auto onGpu = runOnGpu(scene);
        compare(onCpu, onGpu, scene, where);
    }
}

template <typename Compare>
void forEachScene(Compare&& compare)
{
    forEachScene([](const Stages&, const Scene&, const std::string&) {}, compare);
}

// ------------------------------------------------------------ the crossings

// The one product whose rounding decides anything - where the segment enters
// and leaves a band, and so which column its crossing lands in - kept out of a
// fused multiply-add the kernel never asked for.
float strictProduct(float a, float b)
{
    volatile auto product = a * b;
    return product;
}

// BinKernel's count mode, crossings only, in plain C++ single precision: what
// the executor's cells are held to on every lane, device or none.
UInts referenceCrossings(const Scene& scene, UInts cells)
{
    constexpr auto tile = (float) PathIndexedKernel::tileSize;

    for (auto path = 0; path < scene.paths; ++path)
    {
        const auto* record = scene.records.data() + path * 8;
        auto cellBase = (std::uint32_t) record[0];
        auto height = (int) record[2];
        auto tilesWide = ((int) record[1] + PathIndexedKernel::tileSize - 1)
                         / PathIndexedKernel::tileSize;
        auto tilesHigh =
            (height + PathIndexedKernel::tileSize - 1) / PathIndexedKernel::tileSize;

        auto first = (int) scene.segmentStarts[path];
        auto last = (int) scene.segmentStarts[path + 1];

        for (auto segment = first; segment < last; ++segment)
        {
            const auto* at = scene.segments.data() + segment * 4;
            auto fromX = at[0];
            auto fromY = at[1];
            auto topY = std::min(at[1], at[3]);
            auto bottomY = std::max(at[1], at[3]);
            auto slope = (at[2] - at[0]) / (at[3] - at[1]);
            auto winding = at[3] > at[1] ? backdropFixedScale : -backdropFixedScale;

            auto lastRow =
                std::min((int) std::ceil(bottomY * (1.f / tile)) - 1, tilesHigh - 1);

            for (auto row = std::max((int) std::floor(topY * (1.f / tile)), 0);
                 row <= lastRow;
                 ++row)
            {
                auto bandTop = std::max(topY, (float) row * tile);
                auto bandBottom = std::min(bottomY, (float) (row + 1) * tile);

                if (!(bandBottom > bandTop))
                    continue;

                auto enters = fromX + strictProduct(bandTop - fromY, slope);
                auto leaves = fromX + strictProduct(bandBottom - fromY, slope);
                auto column = std::max(
                    (int) std::ceil(std::max(enters, leaves) * (1.f / tile)), 0);

                if (column >= tilesWide)
                    continue;

                auto end = std::min((int) std::ceil(bandBottom), height);

                for (auto pixel = std::max((int) std::floor(bandTop), 0);
                     pixel < end;
                     ++pixel)
                {
                    auto covered = std::min(bandBottom, (float) (pixel + 1))
                                   - std::max(bandTop, (float) pixel);

                    if (covered > 0.f)
                    {
                        auto scaled = strictProduct(covered, winding) + 0.5f;
                        cells[(int) cellBase + column * height + pixel] +=
                            (std::uint32_t) (int) std::floor(scaled);
                    }
                }
            }
        }
    }

    return cells;
}

// BackdropScanKernel in plain C++: each pixel row's cells summed left to right.
UInts referenceBackdrops(const Scene& scene, UInts cells)
{
    for (auto path = 0; path < scene.paths; ++path)
    {
        const auto* record = scene.records.data() + path * 8;
        auto cellBase = (int) record[0];
        auto height = (int) record[2];
        auto tilesWide = ((int) record[1] + PathIndexedKernel::tileSize - 1)
                         / PathIndexedKernel::tileSize;

        for (auto row = 0; row < height; ++row)
        {
            auto running = std::uint32_t {0};

            for (auto column = 0; column < tilesWide; ++column)
            {
                auto& cell = cells[cellBase + column * height + row];
                running += cell;
                cell = running;
            }
        }
    }

    return cells;
}

// ------------------------------------------------------------------ the fill

using Bits = std::array<std::uint32_t, 4>;

Vector<Bits>
    runOf(const Vector<float>& entries, std::uint32_t from, std::uint32_t to)
{
    auto run = Vector<Bits> {};

    for (auto at = from; at < to; ++at)
    {
        auto entry = Bits {};

        for (auto k = 0u; k < 4u; ++k)
            entry[k] = std::bit_cast<std::uint32_t>(entries[(int) (at * 4 + k)]);

        run.add(entry);
    }

    std::sort(run.begin(), run.end());
    return run;
}

void expectSameRuns(const Stages& onCpu,
                    const Stages& onGpu,
                    const Scene& scene,
                    const std::string& where)
{
    const auto& offsets = onCpu.offsets;
    auto total = offsets[scene.tiles];

    check(total <= (std::uint32_t) scene.entries,
          where + "the entries fit their bound");

    if (total > (std::uint32_t) scene.entries)
        return;

    auto differingTiles = 0;
    auto firstTile = -1;

    for (auto tile = 0; tile < scene.tiles; ++tile)
    {
        auto from = offsets[tile];
        auto to = offsets[tile + 1];

        if (runOf(onCpu.filledEntries, from, to)
            != runOf(onGpu.filledEntries, from, to))
        {
            ++differingTiles;

            if (firstTile < 0)
                firstTile = tile;
        }
    }

    check(
        differingTiles == 0,
        where + std::to_string(differingTiles) + " of " + std::to_string(scene.tiles)
            + " tiles hold different segments, first " + std::to_string(firstTile));

    auto untouchedCpu = 0;
    auto untouchedGpu = 0;

    for (auto at = (int) total * 4; at < onCpu.filledEntries.size(); ++at)
    {
        untouchedCpu += onCpu.filledEntries[at] == untouchedFloat ? 1 : 0;
        untouchedGpu += onGpu.filledEntries[at] == untouchedFloat ? 1 : 0;
    }

    auto past = onCpu.filledEntries.size() - (int) total * 4;
    check(untouchedCpu == past, where + "cpu wrote nothing past the last run");
    check(untouchedGpu == past, where + "gpu wrote nothing past the last run");
}
} // namespace

auto tClear = test("PathKernels/clearOnTheCpuMatchesTheGpu") = []
{
    forEachScene(
        [](const Stages& onCpu,
           const Stages& onGpu,
           const Scene&,
           const std::string& where)
        {
            cpu::expectSame(onCpu.clearedCells, onGpu.clearedCells, where + "cells");
            cpu::expectSame(
                onCpu.clearedCounts, onGpu.clearedCounts, where + "tile counts");
        });
};

// The crossings are held to the C++ reference always, and with the tile counts
// to the GPU wherever a device is.
auto tBinCount = test("PathKernels/binCountOnTheCpuMatchesTheGpu") = []
{
    forEachScene(
        [](const Stages& onCpu, const Scene& scene, const std::string& where)
        {
            cpu::expectSame(onCpu.countedCells,
                            referenceCrossings(scene, onCpu.clearedCells),
                            where + "crossings against the reference");
        },
        [](const Stages& onCpu,
           const Stages& onGpu,
           const Scene&,
           const std::string& where)
        {
            cpu::expectSame(
                onCpu.countedCounts, onGpu.countedCounts, where + "tile counts");
            cpu::expectSame(
                onCpu.countedCells, onGpu.countedCells, where + "crossings");
        });
};

// The scan on its own, over the GPU's own crossings: integer sums with no
// literal in them, so it is held to the GPU exactly whatever the crossings came
// to. The whole chain's backdrops are then compared as well.
auto tBackdrop = test("PathKernels/backdropScanOnTheCpuMatchesTheGpu") = []
{
    forEachScene(
        [](const Stages& onCpu, const Scene& scene, const std::string& where)
        {
            cpu::expectSame(onCpu.scannedCells,
                            referenceBackdrops(scene, onCpu.countedCells),
                            where + "backdrops against the reference");
        },
        [](const Stages& onCpu,
           const Stages& onGpu,
           const Scene& scene,
           const std::string& where)
        {
            auto scanned = onGpu.countedCells;
            scanBackdropsOnCpu(scene, scanned);
            cpu::expectSame(scanned,
                            onGpu.scannedCells,
                            where + "backdrops of the gpu crossings");

            cpu::expectSame(
                onCpu.scannedCells, onGpu.scannedCells, where + "backdrops");
        });
};

auto tTileSum = test("PathKernels/tileSumOnTheCpuMatchesTheGpu") = []
{
    forEachScene(
        [](const Stages& onCpu,
           const Stages& onGpu,
           const Scene&,
           const std::string& where)
        {
            cpu::expectSame(onCpu.offsets, onGpu.offsets, where + "tile offsets");
            cpu::expectSame(
                onCpu.summedCounts, onGpu.summedCounts, where + "cursors");
        });
};

auto tBinFill = test("PathKernels/binFillOnTheCpuMatchesTheGpu") = []
{
    forEachScene(
        [](const Stages& onCpu,
           const Stages& onGpu,
           const Scene& scene,
           const std::string& where)
        {
            cpu::expectSame(
                onCpu.filledCounts, onGpu.filledCounts, where + "tile counts");
            expectSameRuns(onCpu, onGpu, scene, where);
        });
};
