#include "Attention.h"

#include "../../GPU/Codegen/KernelCache.h"
#include "../../GPU/Frame/ComputePass.h"
#include "Norm.h"

#include <cassert>
#include <cmath>
#include <optional>
#include <vector>

namespace eacp::ML
{
using namespace eacp::GPU;

namespace
{
constexpr auto attentionGroupWidth = 256;
constexpr auto maskedScore = -1.0e9f;
// The side of the block of scores - and of rows in the weighted sum - one
// thread computes.
constexpr auto scoreBlock = 4u;

int blocksOf(int extent, int block)
{
    return (extent + block - 1) / block;
}
} // namespace

AttentionScoresKernel::AttentionScoresKernel()
{
    compile();
}

void AttentionScoresKernel::dispatch(ComputePass& pass,
                                     int rows,
                                     int heads,
                                     int cols)
{
    assert(headDimension.value % 4 == 0);

    headCount = (std::uint32_t) heads;
    rowCount = (std::uint32_t) rows;
    columnCount = (std::uint32_t) cols;

    // Columns, rows and heads a grid dimension each, so none of them - and no
    // product of them - meets a backend's per-dimension threadgroup ceiling.
    pass.dispatch(*this,
                  blocksOf(cols, (int) scoreBlock),
                  blocksOf(rows, (int) scoreBlock),
                  heads);
}

// A block of 4 x 4 scores of one head per thread.
void AttentionScoresKernel::define()
{
    auto position = threadPosition3();
    auto firstColumn = position.x * scoreBlock;
    auto firstRow = position.y * scoreBlock;
    auto head = position.z;

    auto rowBase = [&](unsigned a)
    {
        auto row = min(firstRow + a, rowCount - 1u);
        return (row * headCount + head) * headDimension;
    };

    auto columnBase = [&](unsigned b)
    {
        auto column = min(firstColumn + b, columnCount - 1u);
        return (column * headCount + head) * headDimension;
    };

    // One accumulator per row of the block, over its four columns. Each score
    // is the same running sum over d, in the same order, that one thread per
    // score made - a block of them only shares the reads.
    auto zero = float4(constant(0.f), 0.f, 0.f, 0.f);
    auto sum0 = var(zero), sum1 = var(zero), sum2 = var(zero), sum3 = var(zero);
    Var<Float4>* sums[] = {&sum0, &sum1, &sum2, &sum3};

    auto d = var(0u);

    loop(d.get() < headDimension,
         [&]
         {
             Float4 queries[] = {query.read4((rowBase(0u) + d.get()) / 4u),
                                 query.read4((rowBase(1u) + d.get()) / 4u),
                                 query.read4((rowBase(2u) + d.get()) / 4u),
                                 query.read4((rowBase(3u) + d.get()) / 4u)};

             Float4 keys[] = {key.read4((columnBase(0u) + d.get()) / 4u),
                              key.read4((columnBase(1u) + d.get()) / 4u),
                              key.read4((columnBase(2u) + d.get()) / 4u),
                              key.read4((columnBase(3u) + d.get()) / 4u)};

             auto component = [](const Float4& v, int c)
             {
                 return c == 0 ? v.x() : c == 1 ? v.y() : c == 2 ? v.z() : v.w();
             };

             for (auto c = 0; c < 4; ++c)
             {
                 auto keyColumn = float4(component(keys[0], c),
                                         component(keys[1], c),
                                         component(keys[2], c),
                                         component(keys[3], c));

                 for (auto a = 0; a < 4; ++a)
                     *sums[a] =
                         sums[a]->get() + component(queries[a], c) * keyColumn;
             }

             d = d.get() + 4u;
         });

    for (auto a = 0u; a < scoreBlock; ++a)
    {
        auto sum = sums[a]->get();
        Float dots[] = {sum.x(), sum.y(), sum.z(), sum.w()};

        for (auto b = 0u; b < scoreBlock; ++b)
        {
            auto row = firstRow + a;
            auto column = firstColumn + b;

            ifThen(row < rowCount && column < columnCount,
                   [&]
                   {
                       auto score = dots[b] * scale
                                    + additiveMask[row * columnCount + column];
                       write(scores,
                             (row * headCount + head) * columnCount + column,
                             score);
                   });
        }
    }
}

UnmaskedAttentionScoresKernel::UnmaskedAttentionScoresKernel()
{
    compile();
}

void UnmaskedAttentionScoresKernel::dispatch(ComputePass& pass,
                                             int rows,
                                             int heads,
                                             int cols)
{
    assert(headDimension.value % 4 == 0);

    headCount = (std::uint32_t) heads;
    rowCount = (std::uint32_t) rows;
    columnCount = (std::uint32_t) cols;

    // Columns, rows and heads a grid dimension each, so none of them - and no
    // product of them - meets a backend's per-dimension threadgroup ceiling.
    pass.dispatch(*this,
                  blocksOf(cols, (int) scoreBlock),
                  blocksOf(rows, (int) scoreBlock),
                  heads);
}

void UnmaskedAttentionScoresKernel::define()
{
    auto position = threadPosition3();
    auto firstColumn = position.x * scoreBlock;
    auto firstRow = position.y * scoreBlock;
    auto head = position.z;

    auto rowBase = [&](unsigned a)
    {
        auto row = min(firstRow + a, rowCount - 1u);
        return (row * headCount + head) * headDimension;
    };

    auto columnBase = [&](unsigned b)
    {
        auto column = min(firstColumn + b, columnCount - 1u);
        return (column * headCount + head) * headDimension;
    };

    // One accumulator per row of the block, over its four columns. Each score
    // is the same running sum over d, in the same order, that one thread per
    // score made - a block of them only shares the reads.
    auto zero = float4(constant(0.f), 0.f, 0.f, 0.f);
    auto sum0 = var(zero), sum1 = var(zero), sum2 = var(zero), sum3 = var(zero);
    Var<Float4>* sums[] = {&sum0, &sum1, &sum2, &sum3};

    auto d = var(0u);

    loop(d.get() < headDimension,
         [&]
         {
             Float4 queries[] = {query.read4((rowBase(0u) + d.get()) / 4u),
                                 query.read4((rowBase(1u) + d.get()) / 4u),
                                 query.read4((rowBase(2u) + d.get()) / 4u),
                                 query.read4((rowBase(3u) + d.get()) / 4u)};

             Float4 keys[] = {key.read4((columnBase(0u) + d.get()) / 4u),
                              key.read4((columnBase(1u) + d.get()) / 4u),
                              key.read4((columnBase(2u) + d.get()) / 4u),
                              key.read4((columnBase(3u) + d.get()) / 4u)};

             auto component = [](const Float4& v, int c)
             {
                 return c == 0 ? v.x() : c == 1 ? v.y() : c == 2 ? v.z() : v.w();
             };

             for (auto c = 0; c < 4; ++c)
             {
                 auto keyColumn = float4(component(keys[0], c),
                                         component(keys[1], c),
                                         component(keys[2], c),
                                         component(keys[3], c));

                 for (auto a = 0; a < 4; ++a)
                     *sums[a] =
                         sums[a]->get() + component(queries[a], c) * keyColumn;
             }

             d = d.get() + 4u;
         });

    for (auto a = 0u; a < scoreBlock; ++a)
    {
        auto sum = sums[a]->get();
        Float dots[] = {sum.x(), sum.y(), sum.z(), sum.w()};

        for (auto b = 0u; b < scoreBlock; ++b)
        {
            auto row = firstRow + a;
            auto column = firstColumn + b;

            ifThen(row < rowCount && column < columnCount,
                   [&]
                   {
                       auto score = dots[b] * scale;
                       write(scores,
                             (row * headCount + head) * columnCount + column,
                             score);
                   });
        }
    }
}

AttentionRowStatsKernel::AttentionRowStatsKernel()
    : ComputeProgram({attentionGroupWidth, 1, 1})
{
    compile();
}

void AttentionRowStatsKernel::dispatch(ComputePass& pass,
                                       int rows,
                                       int heads,
                                       int cols)
{
    columnCount = (std::uint32_t) cols;

    // Rows and heads take a dimension each rather than one multiplied
    // together: the product is what runs past a backend's threadgroup ceiling
    // at real sequence lengths. See ComputePass::dispatch.
    pass.dispatch(*this, attentionGroupWidth, rows, heads);
}

void AttentionRowStatsKernel::define()
{
    auto position = threadPosition3();
    auto lane = position.x;
    auto rowGroup = position.y * gridDepth() + position.z;
    auto base = rowGroup * columnCount;

    auto localMax = var(-3.0e38f);
    auto col = var(lane);

    loop(col.get() < columnCount,
         [&]
         {
             localMax = max(localMax.get(), scores[base + col.get()]);
             col = col.get() + (unsigned) attentionGroupWidth;
         });

    auto peak = groupMax(localMax.get());

    auto localSum = var(0.f);
    col = lane;

    loop(col.get() < columnCount,
         [&]
         {
             auto index = base + col.get();
             auto probability = exp(scores[index] - peak);
             write(scores, index, probability);
             localSum = localSum.get() + probability;
             col = col.get() + (unsigned) attentionGroupWidth;
         });

    auto total = groupSum(localSum.get());

    ifThen(lane == 0u, [&] { write(rowSum, rowGroup, total); });
}

AttentionWeightedSumKernel::AttentionWeightedSumKernel()
{
    compile();
}

void AttentionWeightedSumKernel::dispatch(ComputePass& pass,
                                          int rows,
                                          int heads,
                                          int headDim)
{
    assert(headDim % 4 == 0);

    headCount = (std::uint32_t) heads;
    rowCount = (std::uint32_t) rows;

    // Four of d, four rows and a head per thread, and each of the three a grid
    // dimension of its own - rows x heads x headDim as one count is what ran
    // past a backend's threadgroup ceiling on a long SAME-L decode.
    pass.dispatch(*this, headDim / 4, blocksOf(rows, (int) scoreBlock), heads);
}

// Four rows by four of d per thread, over one head. Each output is the same
// running sum over the columns, in ascending order, that one thread per output
// made; the block shares the value read across its four rows.
void AttentionWeightedSumKernel::define()
{
    auto position = threadPosition3();
    auto firstDepth = position.x * 4u;
    auto firstRow = position.y * scoreBlock;
    auto head = position.z;

    auto probabilityBase = [&](unsigned a)
    {
        auto row = min(firstRow + a, rowCount - 1u);
        return (row * headCount + head) * columnCount;
    };

    auto zero = float4(constant(0.f), 0.f, 0.f, 0.f);
    auto sum0 = var(zero), sum1 = var(zero), sum2 = var(zero), sum3 = var(zero);
    Var<Float4>* sums[] = {&sum0, &sum1, &sum2, &sum3};

    auto col = var(0u);

    loop(col.get() < columnCount,
         [&]
         {
             auto values = value.read4(
                 ((col.get() * headCount + head) * headDimension + firstDepth) / 4u);

             for (auto a = 0u; a < scoreBlock; ++a)
                 *sums[a] = sums[a]->get()
                            + probabilities[probabilityBase(a) + col.get()] * values;

             col = col.get() + 1u;
         });

    for (auto a = 0u; a < scoreBlock; ++a)
    {
        auto row = firstRow + a;

        ifThen(row < rowCount,
               [&]
               {
                   auto rowHead = row * headCount + head;
                   auto total = rowSum[rowHead];
                   auto sum = sums[a]->get();
                   auto at = rowHead * headDimension + firstDepth;

                   write(output, at, sum.x() / total);
                   write(output, at + 1u, sum.y() / total);
                   write(output, at + 2u, sum.z() / total);
                   write(output, at + 3u, sum.w() / total);
               });
    }
}

Tensor buildCausalMask(int rows, int cols, Device& device)
{
    auto values = std::vector<float>((std::size_t) rows * cols);

    for (auto row = 0; row < rows; ++row)
        for (auto col = 0; col < cols; ++col)
            values[(std::size_t) row * cols + col] = col > row ? maskedScore : 0.f;

    return Tensor::fromHostF32(values.data(), {rows, cols}, device);
}

Tensor buildZeroMask(int rows, int cols, Device& device)
{
    auto values = std::vector<float>((std::size_t) rows * cols, 0.f);
    return Tensor::fromHostF32(values.data(), {rows, cols}, device);
}

Tensor attendWithScores(ComputePass& pass,
                        Tensor& scores,
                        const Tensor& value,
                        int heads,
                        int headDim,
                        Device& device)
{
    auto rows = scores.dim(0);
    auto cols = scores.dim(2);

    auto rowSum = Tensor::uninitializedF32({rows * heads}, device);
    auto output = Tensor::uninitializedF32({rows, heads, headDim}, device);

    auto& statsKernel = sharedKernel<AttentionRowStatsKernel>(device);
    statsKernel.scores = scores.buffer();
    statsKernel.rowSum = rowSum.buffer();
    statsKernel.dispatch(pass, rows, heads, cols);

    auto& weightedSumKernel = sharedKernel<AttentionWeightedSumKernel>(device);
    weightedSumKernel.value = value.buffer();
    weightedSumKernel.probabilities = scores.buffer();
    weightedSumKernel.rowSum = rowSum.buffer();
    weightedSumKernel.output = output.buffer();
    weightedSumKernel.headDimension = (std::uint32_t) headDim;
    weightedSumKernel.columnCount = (std::uint32_t) cols;
    weightedSumKernel.dispatch(pass, rows, heads, headDim);

    return output;
}

Tensor attention(ComputePass& pass,
                 const Tensor& query,
                 const Tensor& key,
                 const Tensor& value,
                 int heads,
                 int headDim,
                 const Tensor* additiveMask,
                 const Tensor* queryNormGamma,
                 const Tensor* keyNormGamma,
                 float qkNormEpsilon,
                 Device& device)
{
    auto rows = query.rows();
    auto cols = key.rows();

    auto normalizedQueryStorage =
        queryNormGamma != nullptr
            ? std::optional<Tensor> {rmsNormPerHead(
                  pass, query, *queryNormGamma, headDim, qkNormEpsilon, device)}
            : std::nullopt;

    const auto& normalizedQuery =
        normalizedQueryStorage.has_value() ? *normalizedQueryStorage : query;

    auto normalizedKeyStorage =
        keyNormGamma != nullptr
            ? std::optional<Tensor> {rmsNormPerHead(
                  pass, key, *keyNormGamma, headDim, qkNormEpsilon, device)}
            : std::nullopt;

    const auto& normalizedKey =
        normalizedKeyStorage.has_value() ? *normalizedKeyStorage : key;

    auto scores = Tensor::uninitializedF32({rows, heads, cols}, device);
    auto scale = 1.f / std::sqrt((float) headDim);

    if (additiveMask != nullptr)
    {
        auto& scoresKernel = sharedKernel<AttentionScoresKernel>(device);
        scoresKernel.query = normalizedQuery.buffer();
        scoresKernel.key = normalizedKey.buffer();
        scoresKernel.additiveMask = additiveMask->buffer();
        scoresKernel.scores = scores.buffer();
        scoresKernel.headDimension = (std::uint32_t) headDim;
        scoresKernel.scale = scale;
        scoresKernel.dispatch(pass, rows, heads, cols);
    }
    else
    {
        auto& scoresKernel = sharedKernel<UnmaskedAttentionScoresKernel>(device);
        scoresKernel.query = normalizedQuery.buffer();
        scoresKernel.key = normalizedKey.buffer();
        scoresKernel.scores = scores.buffer();
        scoresKernel.headDimension = (std::uint32_t) headDim;
        scoresKernel.scale = scale;
        scoresKernel.dispatch(pass, rows, heads, cols);
    }

    return attendWithScores(pass, scores, value, heads, headDim, device);
}
} // namespace eacp::ML
