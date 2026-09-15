#include "Common.h"

#include <cmath>
#include <vector>

// What a SIMD-group matrix has to answer with on a device: the same product a
// scalar reference computes, out of fragments a whole SIMD group holds between
// its lanes rather than out of anything one thread has.
//
// Three shapes of it. One fragment against one fragment, which is the operation
// itself with no tiling around it; the blocked product a transformer's linear
// is, over a threadgroup tile and several SIMD groups, at a shape whose every
// extent divides the tiling; and the same product at a shape whose extents
// divide none of it, which is what says the clamped loads and the guarded
// copy-out hold the edges.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto fragment = ComputeProgram::simdMatrixWidth;
constexpr auto fragmentElements = fragment * fragment;

// The fill the accumulator starts from, non-zero so that a fragment that was
// zeroed by something other than the fill still shows up.
constexpr auto accumulatorFill = 0.25f;

float scattered(int index, int salt)
{
    return (float) (((index * 37 + salt * 11) % 23) - 11) * 0.125f;
}

std::vector<float> scatteredValues(int count, int salt)
{
    auto values = std::vector<float> {};

    for (auto i = 0; i < count; ++i)
        values.push_back(scattered(i, salt));

    return values;
}

Buffer bufferOf(const std::vector<float>& values)
{
    auto buffer = Device::shared().makeBuffer((int) (values.size() * sizeof(float)),
                                              BufferUsage::Storage);

    buffer.update(values.data(), (int) (values.size() * sizeof(float)));
    return buffer;
}

Buffer outputOf(std::size_t count)
{
    return Device::shared().makeBuffer((int) (count * sizeof(float)),
                                       BufferUsage::Storage);
}

std::vector<float> readBack(Buffer& buffer, std::size_t count)
{
    auto values = std::vector<float>(count);
    buffer.read(values.data(), (int) (count * sizeof(float)));
    return values;
}

// C[m, n] = fill + sum over k of a[m, k] * b[n, k] - the second operand read
// along k, which is the layout an nn.Linear weight already has.
std::vector<float> referenceProduct(const std::vector<float>& a,
                                    const std::vector<float>& b,
                                    int rows,
                                    int columns,
                                    int inner,
                                    float fill)
{
    auto result = std::vector<float>((std::size_t) rows * columns);

    for (auto m = 0; m < rows; ++m)
        for (auto n = 0; n < columns; ++n)
        {
            auto total = (double) fill;

            for (auto k = 0; k < inner; ++k)
                total += (double) a[(std::size_t) m * inner + k]
                         * (double) b[(std::size_t) n * inner + k];

            result[(std::size_t) m * columns + n] = (float) total;
        }

    return result;
}

// One fragment times one fragment, by the one SIMD group the dispatch runs.
// No tile, no loop: the operation on its own.
struct FragmentProduct final : ComputeProgram
{
    FragmentProduct()
        : ComputeProgram({simdWidth, 1, 1})
    {
        compile();
    }

    void define() override
    {
        auto start = unsignedInteger(0u);
        auto rowStride = unsignedInteger((unsigned) fragment);

        auto accumulator = simdMatrix(accumulatorFill);
        auto left = simdMatrix(a, start, rowStride);
        auto right = simdMatrix(b, start, rowStride);

        multiplyAccumulate(accumulator, left, right);
        write(output, start, rowStride, accumulator);
    }

    Uniform<InputBuffer> a;
    Uniform<InputBuffer> b;
    Uniform<OutputBuffer> output;

    EACP_SHADER(a, b, output)
};

// The blocked product: a 64 x 64 tile of C per threadgroup of eight SIMD
// groups, each owning 32 x 16 of it as eight accumulator fragments, over a
// 32-deep slab of the inner dimension staged in threadgroup memory.
//
// A is read row-major and B along k, the two layouts an activation and a
// shipped weight already have. The slab is loaded clamped and zero-filled past
// the inner extent, and the tile of C is written back through the same
// threadgroup memory so that the copy-out can be guarded element by element -
// a fragment is stored whole, and a partial tile has no whole patch to store.
struct TiledProduct final : ComputeProgram
{
    static constexpr auto tileRows = 64;
    static constexpr auto tileColumns = 64;
    static constexpr auto slab = 32;
    static constexpr auto threads = 256;

    static constexpr auto rowFragments = 4;
    static constexpr auto columnFragments = 2;
    static constexpr auto tileElements = tileRows * slab + slab * tileColumns;
    static constexpr auto columnTileBase = tileRows * slab;

    TiledProduct()
        : ComputeProgram({threads, 1, 1})
    {
        compile();
    }

    void dispatch(ComputePass& pass, int rows, int columns, int inner)
    {
        rowCount = (std::uint32_t) rows;
        columnCount = (std::uint32_t) columns;
        innerCount = (std::uint32_t) inner;

        const auto rowTiles = (rows + tileRows - 1) / tileRows;
        const auto columnTiles = (columns + tileColumns - 1) / tileColumns;

        pass.dispatch(*this, columnTiles * threads, rowTiles);
    }

    void define() override
    {
        constexpr auto rows = (unsigned) tileRows;
        constexpr auto columns = (unsigned) tileColumns;
        constexpr auto depth = (unsigned) slab;
        constexpr auto side = (unsigned) fragment;

        auto lane = localPosition().x;
        auto group = groupPosition();
        auto simd = simdGroupIndex();

        auto m0 = group.y * rows;
        auto n0 = group.x * columns;

        auto rowOffset = (simd % 2u) * 32u;
        auto columnOffset = (simd / 2u) * 16u;

        auto tile = shared<Float>(tileElements);
        auto slabStride = unsignedInteger(depth);
        auto columnStride = unsignedInteger(columns);

        // Eight consecutive elements per thread of each slab, which is 256
        // threads covering both of them exactly.
        auto loadRow = lane / 4u;
        auto loadDepth = (lane % 4u) * 8u;

        auto aRow = min(m0 + loadRow, rowCount - 1u) * aRowStride;
        auto bRow = min(n0 + loadRow, columnCount - 1u) * bStride;

        SimdMatrix accumulators[rowFragments * columnFragments];

        for (auto& accumulator: accumulators)
            accumulator = simdMatrix();

        auto k0 = var(0u);

        loop(k0.get() < innerCount,
             [&]
             {
                 for (auto i = 0u; i < 8u; ++i)
                 {
                     auto k = k0.get() + loadDepth + i;
                     auto inside = k < innerCount;
                     auto at = min(k, innerCount - 1u);

                     write(tile,
                           loadRow * depth + loadDepth + i,
                           select(inside, a[aRow + at], 0.f));

                     write(tile,
                           columnTileBase + (loadDepth + i) * columns + loadRow,
                           select(inside, b[bRow + at], 0.f));
                 }

                 barrier();

                 for (auto kk = 0u; kk < depth; kk += side)
                 {
                     SimdMatrix left[rowFragments];
                     SimdMatrix right[columnFragments];

                     for (auto i = 0u; i < (unsigned) rowFragments; ++i)
                         left[i] = simdMatrix(
                             tile, (rowOffset + i * side) * depth + kk, slabStride);

                     for (auto j = 0u; j < (unsigned) columnFragments; ++j)
                         right[j] = simdMatrix(tile,
                                               columnTileBase + kk * columns
                                                   + columnOffset + j * side,
                                               columnStride);

                     for (auto i = 0u; i < (unsigned) rowFragments; ++i)
                         for (auto j = 0u; j < (unsigned) columnFragments; ++j)
                             multiplyAccumulate(
                                 accumulators[i * columnFragments + j],
                                 left[i],
                                 right[j]);
                 }

                 barrier();
                 k0 += depth;
             });

        barrier();

        for (auto i = 0u; i < (unsigned) rowFragments; ++i)
            for (auto j = 0u; j < (unsigned) columnFragments; ++j)
                write(tile,
                      (rowOffset + i * side) * columns + columnOffset + j * side,
                      columnStride,
                      accumulators[i * columnFragments + j]);

        barrier();

        for (auto i = 0u; i < (unsigned) (tileRows * tileColumns / threads); ++i)
        {
            auto index = lane + i * (unsigned) threads;
            auto m = m0 + index / columns;
            auto n = n0 + index % columns;

            ifThen(m < rowCount && n < columnCount,
                   [&] { write(output, m * cRowStride + n, tile[index]); });
        }
    }

    Uniform<InputBuffer> a;
    Uniform<InputBuffer> b;
    Uniform<OutputBuffer> output;
    Uniform<UInt> rowCount;
    Uniform<UInt> columnCount;
    Uniform<UInt> innerCount;
    Uniform<UInt> aRowStride;
    Uniform<UInt> bStride;
    Uniform<UInt> cRowStride;

    EACP_SHADER(a,
                b,
                output,
                rowCount,
                columnCount,
                innerCount,
                aRowStride,
                bStride,
                cRowStride)
};

// The blocked product run once over one shape, and what it wrote read back.
std::vector<float> tiledProduct(const std::vector<float>& a,
                                const std::vector<float>& b,
                                int rows,
                                int columns,
                                int inner)
{
    auto& device = Device::shared();

    auto left = bufferOf(a);
    auto right = bufferOf(b);
    auto result = outputOf((std::size_t) rows * columns);

    auto kernel = TiledProduct {};
    kernel.a = left;
    kernel.b = right;
    kernel.output = result;
    kernel.aRowStride = (std::uint32_t) inner;
    kernel.bStride = (std::uint32_t) inner;
    kernel.cRowStride = (std::uint32_t) columns;
    kernel.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        kernel.dispatch(pass, rows, columns, inner);
    }

    commands.commit();

    return readBack(result, (std::size_t) rows * columns);
}

// The tolerance a sum of `inner` products of values under 1.5 deserves in
// single precision, which is what both sides compute in.
float toleranceFor(int inner)
{
    return 2.0e-4f * (float) inner;
}

void checkMatches(const std::vector<float>& values,
                  const std::vector<float>& expected,
                  float tolerance)
{
    check(values.size() == expected.size());

    auto worst = 0.f;

    for (auto i = std::size_t {}; i < expected.size(); ++i)
        worst = std::max(worst, std::abs(values[i] - expected[i]));

    check(worst <= tolerance);
}
} // namespace

// The operation itself: one 8x8 fragment multiplied into another, by the
// thirty-two lanes that hold them between them.
auto tOneFragmentProduct =
    test("SimdMatrix/oneFragmentProductMatchesTheReference") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto a = scatteredValues(fragmentElements, 1);
    auto b = scatteredValues(fragmentElements, 2);

    auto left = bufferOf(a);
    auto right = bufferOf(b);
    auto result = outputOf(fragmentElements);

    auto kernel = FragmentProduct {};
    kernel.a = left;
    kernel.b = right;
    kernel.output = result;
    kernel.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, ComputeProgram::simdWidth);
    }

    commands.commit();

    // The kernel's fragments are read row-major, so the second operand's rows
    // are the product's inner index - a plain row-by-column product, unlike the
    // blocked one below, whose second operand is read along k.
    auto expected = std::vector<float>(fragmentElements, accumulatorFill);

    for (auto m = 0; m < fragment; ++m)
        for (auto n = 0; n < fragment; ++n)
            for (auto k = 0; k < fragment; ++k)
                expected[(std::size_t) m * fragment + n] +=
                    a[(std::size_t) m * fragment + k]
                    * b[(std::size_t) k * fragment + n];

    checkMatches(
        readBack(result, fragmentElements), expected, toleranceFor(fragment));
};

// The blocked product at a shape every extent of the tiling divides: 128 rows
// is two row tiles, 128 columns two column tiles, and 64 inner two whole slabs.
auto tTiledProductOnWholeTiles =
    test("SimdMatrix/blockedProductMatchesTheReference") = []
{
    if (!Device::shared().isValid())
        return;

    constexpr auto rows = 128;
    constexpr auto columns = 128;
    constexpr auto inner = 64;

    auto a = scatteredValues(rows * inner, 3);
    auto b = scatteredValues(columns * inner, 5);

    checkMatches(tiledProduct(a, b, rows, columns, inner),
                 referenceProduct(a, b, rows, columns, inner, 0.f),
                 toleranceFor(inner));
};

// And at a shape none of them divides: 100 rows leaves 36 of the second row
// tile past the data, 76 columns leaves 52 of the second column tile, and 45
// inner leaves 19 of the second slab. A clamped load that failed to zero-fill,
// or a copy-out that failed to guard, is a wrong number or a corrupted
// neighbour here and is neither above.
auto tTiledProductOnRaggedShape = test("SimdMatrix/blockedProductHoldsTheEdges") = []
{
    if (!Device::shared().isValid())
        return;

    constexpr auto rows = 100;
    constexpr auto columns = 76;
    constexpr auto inner = 45;

    auto a = scatteredValues(rows * inner, 7);
    auto b = scatteredValues(columns * inner, 11);

    checkMatches(tiledProduct(a, b, rows, columns, inner),
                 referenceProduct(a, b, rows, columns, inner, 0.f),
                 toleranceFor(inner));
};

// The shape a Whisper encoder's feed-forward is, which is what the primitive
// was added for. Checked against the scalar reference on a strip of the rows,
// the whole product being 2.3 million dot products of 384 terms and this being
// a test rather than a benchmark.
auto tTiledProductAtEncoderShape =
    test("SimdMatrix/blockedProductAtAnEncoderShape") = []
{
    if (!Device::shared().isValid())
        return;

    constexpr auto rows = 1500;
    constexpr auto columns = 1536;
    constexpr auto inner = 384;
    constexpr auto checkedRows = 8;

    auto a = scatteredValues(rows * inner, 13);
    auto b = scatteredValues(columns * inner, 17);

    auto values = tiledProduct(a, b, rows, columns, inner);

    auto strip = std::vector<float>(a.begin(), a.begin() + checkedRows * inner);
    auto expected = referenceProduct(strip, b, checkedRows, columns, inner, 0.f);

    values.resize((std::size_t) checkedRows * columns);
    checkMatches(values, expected, toleranceFor(inner));
};
