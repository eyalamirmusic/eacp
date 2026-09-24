#include "Linear.h"

#include "../../GPU/Codegen/KernelCache.h"
#include "../../GPU/CommandBuffer/CommandBuffer.h"
#include "../../GPU/Frame/ComputePass.h"

namespace eacp::ML
{
using namespace eacp::GPU;

namespace
{
constexpr auto tileRows = 64;
constexpr auto tileColumns = 64;
constexpr auto slab = 32;
constexpr auto threads = 256;

constexpr auto rowFragments = 4;
constexpr auto columnFragments = 2;
constexpr auto tileElements = tileRows * tileColumns;
constexpr auto side = (unsigned) ComputeProgram::simdMatrixWidth;

int tileCount(int extent, int tile)
{
    return (extent + tile - 1) / tile;
}
}

namespace
{
// LinearF32's tiling: a tile of the output per threadgroup, split into blocks
// 32 rows tall, one per SIMD group, four SIMD groups to a threadgroup. A tile
// 64 rows tall is four 32 x 32 blocks - sixteen accumulator fragments each, so
// every fragment loaded from the slab feeds four products. A tile 32 rows tall
// splits its 64 columns four ways into 32 x 16 blocks instead.
constexpr auto f32TileColumns = 64u;
constexpr auto f32BlockRows = 32u;
constexpr auto f32SimdGroups = 4u;
constexpr auto f32Threads = f32SimdGroups * 32u;
constexpr auto f32FragmentsDown = f32BlockRows / 8u;

// The slab is 16 deep in k. A shallower slab is more barriers for the same
// products, but it is also less threadgroup memory, and on an M5 Max that is
// the trade that wins: at 16 more threadgroups fit a core at once than at 32,
// and the extra ones hide the global reads better than a deeper slab
// amortises its barriers - 17% on the Stable Audio DiT's shapes, and 32 or 8
// deep are both slower.
constexpr auto f32Slab = 16u;
constexpr auto f32QuadsPerRow = f32Slab / 4u;
constexpr auto f32QuadsB = f32TileColumns * f32QuadsPerRow / f32Threads;

static_assert(f32TileColumns * f32QuadsPerRow % f32Threads == 0);
static_assert(f32QuadsB >= 1 && f32QuadsB <= 4);

// Each row of the slab and of the output tile padded by eight floats. A
// fragment load reads eight rows of eight, and at a stride that is a multiple
// of the 32 banks all eight rows land on the same eight of them; eight more
// shifts each row onto the next eight, so a load takes the two passes its 64
// floats need and no more.
constexpr auto f32SlabStride = f32Slab + 8u;
constexpr auto f32ColumnStride = f32TileColumns + 8u;

// Where a fragment that hangs off the edge of the output goes on its way out:
// a patch of 64 floats per SIMD group, after the slab.
constexpr auto f32PatchElements = 64u;

struct F32Tiling
{
    unsigned tileRows;

    unsigned blocksDown() const { return tileRows / f32BlockRows; }

    unsigned blockColumns() const
    {
        return f32TileColumns * blocksDown() / f32SimdGroups;
    }

    unsigned fragmentsAcross() const { return blockColumns() / 8u; }
    unsigned quadsA() const { return tileRows * f32QuadsPerRow / f32Threads; }
    unsigned weightBase() const { return tileRows * f32SlabStride; }

    unsigned slabElements() const
    {
        return weightBase() + f32Slab * f32ColumnStride;
    }

    unsigned sharedElements() const
    {
        return slabElements() + f32SimdGroups * f32PatchElements;
    }
};
} // namespace

LinearLoads linearLoadsFor(int inner)
{
    return inner % 4 == 0 ? LinearLoads::FourWide : LinearLoads::Scalar;
}

int linearTileRowsFor(int rows, int columns)
{
    auto padsLess = tileCount(rows, 32) * 32 < tileCount(rows, 64) * 64;
    auto fewGroups =
        tileCount(rows, 64) * tileCount(columns, (int) f32TileColumns) < 512;

    return padsLess || fewGroups ? 32 : 64;
}

LinearF32::LinearF32(LinearLoads loadsToUse, int tileRowsToUse)
    : ComputeProgram({(int) f32Threads, 1, 1})
    , loads(loadsToUse)
    , tileRows(tileRowsToUse)
{
    compile();
}

void LinearF32::dispatch(ComputePass& pass, int rows, int columns, int inner)
{
    rowCount = (std::uint32_t) rows;
    columnCount = (std::uint32_t) columns;
    innerCount = (std::uint32_t) inner;

    pass.dispatch(*this,
                  tileCount(columns, (int) f32TileColumns) * (int) f32Threads,
                  tileCount(rows, tileRows));
}

// Every output element is the same sequence of 8 x 8 x 8 products - k in
// steps of 8 from zero upwards, into an accumulator that starts at zero - that
// the kernel before this one made, which is what keeps the result bit for bit
// what it was. What changed is everything around that sequence: which SIMD
// group computes which block, how deep a slab is, four-wide reads of A and of
// the weight, the next slab's reads issued before this slab's products, padded
// threadgroup rows, and fragments stored straight to the output.
void LinearF32::define()
{
    auto lane = localPosition().x;
    auto group = groupPosition();
    auto simd = simdGroupIndex();

    auto tiling = F32Tiling {(unsigned) tileRows};
    auto quadsA = tiling.quadsA();
    auto weightBase = tiling.weightBase();
    auto fragmentsAcross = tiling.fragmentsAcross();

    auto m0 = group.y * tiling.tileRows;
    auto n0 = group.x * f32TileColumns;

    auto rowOffset = (simd % tiling.blocksDown()) * f32BlockRows;
    auto columnOffset = (simd / tiling.blocksDown()) * tiling.blockColumns();

    auto tile = shared<Float>(tiling.sharedElements());
    auto slabStride = unsignedInteger(f32SlabStride);
    auto columnStride = unsignedInteger(f32ColumnStride);

    // Four consecutive k of one row, zero past the end of k. innerCount is a
    // multiple of four when the loads are four wide, so four k are inside or outside
    // together and the read is clamped whole to stay inside the buffer.
    auto readFour =
        [&](const InputBuffer& source, const UInt& rowBase, const UInt& k)
    {
        if (loads == LinearLoads::FourWide)
        {
            auto inside = k < innerCount;
            auto at = min(k, innerCount - 4u);
            auto four = source.read4((rowBase + at) / 4u);

            return float4(select(inside, four.x(), 0.f),
                          select(inside, four.y(), 0.f),
                          select(inside, four.z(), 0.f),
                          select(inside, four.w(), 0.f));
        }

        auto element = [&](unsigned offset)
        {
            auto at = k + offset;
            return select(
                at < innerCount, source[rowBase + min(at, innerCount - 1u)], 0.f);
        };

        return float4(element(0u), element(1u), element(2u), element(3u));
    };

    // Quad q of this thread's share of a slab: which row of the tile, and which
    // four k of the slab.
    auto quadRow = [&](unsigned q)
    { return (lane + q * f32Threads) / f32QuadsPerRow; };
    auto quadDepth = [&](unsigned q)
    { return ((lane + q * f32Threads) % f32QuadsPerRow) * 4u; };

    // The next slab, held in registers while the products run on this one.
    auto zero = float4(constant(0.f), 0.f, 0.f, 0.f);
    auto a0 = var(zero), a1 = var(zero), a2 = var(zero), a3 = var(zero);
    auto b0 = var(zero), b1 = var(zero), b2 = var(zero), b3 = var(zero);

    Var<Float4>* stagedA[] = {&a0, &a1, &a2, &a3};
    Var<Float4>* stagedB[] = {&b0, &b1, &b2, &b3};

    auto fetchSlab = [&](const UInt& k0)
    {
        for (auto q = 0u; q < quadsA; ++q)
        {
            auto row = min(m0 + quadRow(q), rowCount - 1u) * innerCount;
            *stagedA[q] = readFour(activations, row, k0 + quadDepth(q));
        }

        for (auto q = 0u; q < f32QuadsB; ++q)
        {
            auto row = min(n0 + quadRow(q), columnCount - 1u) * innerCount;
            *stagedB[q] = readFour(weight, row, k0 + quadDepth(q));
        }
    };

    auto storeSlab = [&]
    {
        for (auto q = 0u; q < quadsA; ++q)
        {
            auto a = stagedA[q]->get();
            auto at = quadRow(q) * f32SlabStride + quadDepth(q);

            write(tile, at, a.x());
            write(tile, at + 1u, a.y());
            write(tile, at + 2u, a.z());
            write(tile, at + 3u, a.w());
        }

        // The weight goes in k-major, so a fragment of it is an 8 x 8 patch of
        // k by n like any other.
        for (auto q = 0u; q < f32QuadsB; ++q)
        {
            auto b = stagedB[q]->get();
            auto at = weightBase + quadDepth(q) * f32ColumnStride + quadRow(q);

            write(tile, at, b.x());
            write(tile, at + f32ColumnStride, b.y());
            write(tile, at + 2u * f32ColumnStride, b.z());
            write(tile, at + 3u * f32ColumnStride, b.w());
        }
    };

    SimdMatrix accumulators[f32FragmentsDown * 4u];

    for (auto i = 0u; i < f32FragmentsDown * fragmentsAcross; ++i)
        accumulators[i] = simdMatrix();

    fetchSlab(unsignedInteger(0u));

    auto k0 = var(0u);

    loop(k0.get() < innerCount,
         [&]
         {
             barrier();
             storeSlab();
             barrier();

             fetchSlab(k0.get() + f32Slab);

             for (auto kk = 0u; kk < f32Slab; kk += side)
             {
                 SimdMatrix left[f32FragmentsDown];
                 SimdMatrix right[4];

                 for (auto i = 0u; i < f32FragmentsDown; ++i)
                     left[i] =
                         simdMatrix(tile,
                                    (rowOffset + i * side) * f32SlabStride + kk,
                                    slabStride);

                 for (auto j = 0u; j < fragmentsAcross; ++j)
                     right[j] = simdMatrix(tile,
                                           weightBase + kk * f32ColumnStride
                                               + columnOffset + j * side,
                                           columnStride);

                 for (auto i = 0u; i < f32FragmentsDown; ++i)
                     for (auto j = 0u; j < fragmentsAcross; ++j)
                         multiplyAccumulate(accumulators[i * fragmentsAcross + j],
                                            left[i],
                                            right[j]);
             }

             k0 += f32Slab;
         });

    // Out through the fragments themselves wherever one lies wholly inside the
    // output, and through this SIMD group's patch where one hangs off its edge
    // - the last rows of a batch that is not a multiple of eight.
    auto patch = tiling.slabElements() + simd * f32PatchElements;
    auto patchStride = unsignedInteger(side);

    for (auto i = 0u; i < f32FragmentsDown; ++i)
        for (auto j = 0u; j < fragmentsAcross; ++j)
        {
            const auto& accumulator = accumulators[i * fragmentsAcross + j];
            auto fragmentRow = m0 + rowOffset + i * side;
            auto fragmentColumn = n0 + columnOffset + j * side;
            auto whole = fragmentRow + side <= rowCount
                         && fragmentColumn + side <= columnCount;

            ifThen(
                whole,
                [&]
                {
                    write(output,
                          fragmentRow * columnCount + fragmentColumn,
                          columnCount,
                          accumulator);
                },
                [&] { write(tile, patch, patchStride, accumulator); });

            barrier();

            ifThen(!whole,
                   [&]
                   {
                       auto laneInGroup = lane % (unsigned) simdWidth;

                       for (auto half = 0u; half < 2u; ++half)
                       {
                           auto element = laneInGroup + half * (unsigned) simdWidth;
                           auto m = fragmentRow + element / side;
                           auto n = fragmentColumn + element % side;

                           ifThen(m < rowCount && n < columnCount,
                                  [&]
                                  {
                                      write(output,
                                            m * columnCount + n,
                                            tile[patch + element]);
                                  });
                       }
                   });

            barrier();
        }
}

LinearPackedHalf::LinearPackedHalf()
    : ComputeProgram({threads, 1, 1})
{
    compile();
}

void LinearPackedHalf::dispatch(ComputePass& pass, int rows, int columns, int inner)
{
    rowCount = (std::uint32_t) rows;
    columnCount = (std::uint32_t) columns;
    innerCount = (std::uint32_t) inner;

    pass.dispatch(*this,
                  tileCount(columns, tileColumns) * threads,
                  tileCount(rows, tileRows));
}

void LinearPackedHalf::define()
{
    auto lane = localPosition().x;
    auto group = groupPosition();
    auto simd = simdGroupIndex();

    auto m0 = group.y * (unsigned) tileRows;
    auto n0 = group.x * (unsigned) tileColumns;

    auto rowOffset = (simd % 2u) * 32u;
    auto columnOffset = (simd / 2u) * 16u;

    auto tile = shared<Float>(tileElements);
    auto slabStride = unsignedInteger((unsigned) slab);
    auto columnStrideForStaging = unsignedInteger((unsigned) tileColumns);

    auto loadRow = lane / 4u;
    auto loadDepth = (lane % 4u) * 8u;

    auto aRow = min(m0 + loadRow, rowCount - 1u) * innerCount;
    auto bRow = min(n0 + loadRow, columnCount - 1u) * innerCount;

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
                     loadRow * (unsigned) slab + loadDepth + i,
                     select(inside, activations[aRow + at], 0.f));

                write(tile,
                     (unsigned) (tileRows * slab) + (loadDepth + i) * (unsigned) tileColumns
                         + loadRow,
                     select(inside, weight.readHalf(bRow + at), 0.f));
            }

            barrier();

            for (auto kk = 0u; kk < (unsigned) slab; kk += side)
            {
                SimdMatrix left[rowFragments];
                SimdMatrix right[columnFragments];

                for (auto i = 0u; i < (unsigned) rowFragments; ++i)
                    left[i] = simdMatrix(
                        tile, (rowOffset + i * side) * (unsigned) slab + kk, slabStride);

                for (auto j = 0u; j < (unsigned) columnFragments; ++j)
                    right[j] = simdMatrix(
                        tile,
                        (unsigned) (tileRows * slab) + kk * (unsigned) tileColumns
                            + columnOffset + j * side,
                        columnStrideForStaging);

                for (auto i = 0u; i < (unsigned) rowFragments; ++i)
                    for (auto j = 0u; j < (unsigned) columnFragments; ++j)
                        multiplyAccumulate(accumulators[i * columnFragments + j],
                                          left[i],
                                          right[j]);
            }

            barrier();
            k0 += (unsigned) slab;
        });

    barrier();

    auto columnStride = unsignedInteger((unsigned) tileColumns);

    for (auto i = 0u; i < (unsigned) rowFragments; ++i)
        for (auto j = 0u; j < (unsigned) columnFragments; ++j)
            write(tile,
                 (rowOffset + i * side) * (unsigned) tileColumns + columnOffset
                     + j * side,
                 columnStride,
                 accumulators[i * columnFragments + j]);

    barrier();

    for (auto i = 0u; i < (unsigned) (tileRows * tileColumns / threads); ++i)
    {
        auto index = lane + i * (unsigned) threads;
        auto m = m0 + index / (unsigned) tileColumns;
        auto n = n0 + index % (unsigned) tileColumns;

        ifThen(m < rowCount && n < columnCount,
              [&] { write(output, m * columnCount + n, tile[index]); });
    }
}

AddBiasRows::AddBiasRows()
{
    compile();
}

void AddBiasRows::dispatch(ComputePass& pass, int rows, int columns)
{
    columnCount = (std::uint32_t) columns;
    pass.dispatch(*this, columns, rows);
}

void AddBiasRows::define()
{
    auto position = threadPosition();
    auto index = position.y * columnCount + position.x;

    write(values, index, values[index] + bias[position.x]);
}

Tensor linear(ComputePass& pass,
             const Tensor& input,
             const Tensor& weight,
             const Tensor* bias,
             Device& device)
{
    auto rows = input.rows();
    auto inner = input.cols();
    auto columns = weight.dim(0);

    auto result = Tensor::uninitializedF32({rows, columns}, device);

    if (weight.isPacked())
    {
        auto& kernel = sharedKernel<LinearPackedHalf>(device);
        kernel.activations = input.buffer();
        kernel.weight = weight.buffer();
        kernel.output = result.buffer();
        kernel.dispatch(pass, rows, columns, inner);
    }
    else
    {
        auto& kernel = sharedKernel<LinearF32>(
            device, linearLoadsFor(inner), linearTileRowsFor(rows, columns));
        kernel.activations = input.buffer();
        kernel.weight = weight.buffer();
        kernel.output = result.buffer();
        kernel.dispatch(pass, rows, columns, inner);
    }

    if (bias != nullptr)
    {
        auto& biasKernel = sharedKernel<AddBiasRows>(device);
        biasKernel.values = result.buffer();
        biasKernel.bias = bias->buffer();
        biasKernel.dispatch(pass, rows, columns);
    }

    return result;
}
}
