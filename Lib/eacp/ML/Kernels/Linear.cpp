#include "Linear.h"

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

LinearF32::LinearF32()
    : ComputeProgram({threads, 1, 1})
{
    compile();
}

void LinearF32::dispatch(ComputePass& pass, int rows, int columns, int inner)
{
    rowCount = (std::uint32_t) rows;
    columnCount = (std::uint32_t) columns;
    innerCount = (std::uint32_t) inner;

    pass.dispatch(*this,
                  tileCount(columns, tileColumns) * threads,
                  tileCount(rows, tileRows));
}

void LinearF32::define()
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
    auto columnStride = unsignedInteger((unsigned) tileColumns);

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
                     select(inside, weight[bRow + at], 0.f));
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
                    right[j] = simdMatrix(tile,
                                          (unsigned) (tileRows * slab) + kk * (unsigned) tileColumns
                                              + columnOffset + j * side,
                                          columnStride);

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
        auto kernel = LinearPackedHalf {};
        kernel.activations = input.buffer();
        kernel.weight = weight.buffer();
        kernel.output = result.buffer();
        kernel.prepare(device);
        kernel.dispatch(pass, rows, columns, inner);
    }
    else
    {
        auto kernel = LinearF32 {};
        kernel.activations = input.buffer();
        kernel.weight = weight.buffer();
        kernel.output = result.buffer();
        kernel.prepare(device);
        kernel.dispatch(pass, rows, columns, inner);
    }

    if (bias != nullptr)
    {
        auto biasKernel = AddBiasRows {};
        biasKernel.values = result.buffer();
        biasKernel.bias = bias->buffer();
        biasKernel.prepare(device);
        biasKernel.dispatch(pass, rows, columns);
    }

    return result;
}
}
