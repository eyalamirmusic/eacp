#include "BandedAttention.h"

#include "Attention.h"

#include "../../GPU/Codegen/KernelCache.h"
#include "../../GPU/Frame/ComputePass.h"

#include <algorithm>
#include <cmath>

namespace eacp::ML
{
using namespace eacp::GPU;

namespace
{
struct BandBounds
{
    UInt segmentStart;
    UInt first;
    UInt end;
};

BandBounds bandBounds(const UInt& row,
                      const UInt& segmentRows,
                      const UInt& leftRadius,
                      const UInt& rightRadius)
{
    auto segmentStart = (row / segmentRows) * segmentRows;
    auto first = max(row, segmentStart + leftRadius) - leftRadius;
    auto end = min(segmentStart + segmentRows, row + rightRadius + 1u);
    return {segmentStart, first, end};
}

int windowWidthFor(const AttentionBand& band)
{
    return std::min(band.leftRadius + band.rightRadius + 1, band.segmentRows);
}
} // namespace

BandedAttentionScoresKernel::BandedAttentionScoresKernel()
{
    compile();
}

void BandedAttentionScoresKernel::dispatch(ComputePass& pass,
                                           int rows,
                                           int heads,
                                           int window)
{
    headCount = (std::uint32_t) heads;
    windowWidth = (std::uint32_t) window;
    pass.dispatch(*this, rows * heads * window);
}

void BandedAttentionScoresKernel::define()
{
    auto i = threadId();
    auto slot = i % windowWidth;
    auto rowHead = i / windowWidth;
    auto row = rowHead / headCount;
    auto head = rowHead % headCount;

    auto bounds = bandBounds(row, segmentRows, leftRadius, rightRadius);
    auto col = bounds.first + slot;

    ifThen(col < bounds.end,
           [&]
           {
               auto queryBase = rowHead * headDimension;
               auto keyBase = (col * headCount + head) * headDimension;

               auto dot = var(0.f);
               auto d = var(0u);

               loop(d.get() < headDimension,
                    [&]
                    {
                        dot = dot.get()
                              + query[queryBase + d.get()] * key[keyBase + d.get()];
                        d = d.get() + 1u;
                    });

               write(scores, i, dot.get() * scale);
           });
}

BandedAttentionRowStatsKernel::BandedAttentionRowStatsKernel()
    : ComputeProgram({attentionGroupWidth, 1, 1})
{
    compile();
}

void BandedAttentionRowStatsKernel::dispatch(ComputePass& pass,
                                             int rows,
                                             int heads,
                                             int window)
{
    headCount = (std::uint32_t) heads;
    windowWidth = (std::uint32_t) window;

    // Rows and heads take a dimension each rather than one multiplied
    // together: the product is what runs past a backend's threadgroup ceiling
    // at real clip lengths. See ComputePass::dispatch.
    pass.dispatch(*this, attentionGroupWidth, rows, heads);
}

void BandedAttentionRowStatsKernel::define()
{
    auto position = threadPosition3();
    auto lane = position.x;
    auto row = position.y;
    auto rowHead = row * headCount + position.z;

    auto bounds = bandBounds(row, segmentRows, leftRadius, rightRadius);
    auto base = rowHead * windowWidth - bounds.first;

    auto width = (unsigned) attentionGroupWidth;
    auto offset = bounds.first - bounds.segmentStart;
    auto skippedTurns =
        select(offset > lane, (offset - lane + (width - 1u)) / width, 0u);
    auto firstColumn = bounds.segmentStart + lane + skippedTurns * width;

    auto localMax = var(-3.0e38f);
    auto col = var(firstColumn);

    loop(col.get() < bounds.end,
         [&]
         {
             localMax = max(localMax.get(), scores[base + col.get()]);
             col = col.get() + width;
         });

    auto peak = groupMax(localMax.get());

    auto localSum = var(0.f);
    col = firstColumn;

    loop(col.get() < bounds.end,
         [&]
         {
             auto index = base + col.get();
             auto probability = exp(scores[index] - peak);
             write(scores, index, probability);
             localSum = localSum.get() + probability;
             col = col.get() + width;
         });

    auto total = groupSum(localSum.get());

    ifThen(lane == 0u, [&] { write(rowSum, rowHead, total); });
}

BandedAttentionWeightedSumKernel::BandedAttentionWeightedSumKernel()
{
    compile();
}

void BandedAttentionWeightedSumKernel::dispatch(
    ComputePass& pass, int rows, int heads, int headDim, int window)
{
    headCount = (std::uint32_t) heads;
    headDimension = (std::uint32_t) headDim;
    windowWidth = (std::uint32_t) window;
    pass.dispatch(*this, rows * heads * headDim);
}

void BandedAttentionWeightedSumKernel::define()
{
    auto i = threadId();
    auto d = i % headDimension;
    auto rowHead = i / headDimension;
    auto head = rowHead % headCount;
    auto row = rowHead / headCount;

    auto bounds = bandBounds(row, segmentRows, leftRadius, rightRadius);
    auto probabilityBase = rowHead * windowWidth - bounds.first;
    auto total = rowSum[rowHead];

    auto accumulator = var(0.f);
    auto col = var(bounds.first);

    loop(col.get() < bounds.end,
         [&]
         {
             auto probability = probabilities[probabilityBase + col.get()];
             auto valueBase = (col.get() * headCount + head) * headDimension;

             accumulator = accumulator.get() + probability * value[valueBase + d];

             col = col.get() + 1u;
         });

    write(output, rowHead * headDimension + d, accumulator.get() / total);
}

Tensor bandedAttention(ComputePass& pass,
                       const Tensor& query,
                       const Tensor& key,
                       const Tensor& value,
                       int heads,
                       int headDim,
                       const AttentionBand& band,
                       Device& device)
{
    auto rows = query.rows();
    auto window = windowWidthFor(band);

    auto scores = Tensor::uninitializedF32({rows, heads, window}, device);
    auto rowSum = Tensor::uninitializedF32({rows * heads}, device);
    auto output = Tensor::uninitializedF32({rows, heads, headDim}, device);

    auto segmentRows = (std::uint32_t) band.segmentRows;
    auto leftRadius = (std::uint32_t) band.leftRadius;
    auto rightRadius = (std::uint32_t) band.rightRadius;

    auto& scoresKernel = sharedKernel<BandedAttentionScoresKernel>(device);
    scoresKernel.query = query.buffer();
    scoresKernel.key = key.buffer();
    scoresKernel.scores = scores.buffer();
    scoresKernel.headDimension = (std::uint32_t) headDim;
    scoresKernel.segmentRows = segmentRows;
    scoresKernel.leftRadius = leftRadius;
    scoresKernel.rightRadius = rightRadius;
    scoresKernel.scale = 1.f / std::sqrt((float) headDim);
    scoresKernel.dispatch(pass, rows, heads, window);

    auto& statsKernel = sharedKernel<BandedAttentionRowStatsKernel>(device);
    statsKernel.scores = scores.buffer();
    statsKernel.rowSum = rowSum.buffer();
    statsKernel.segmentRows = segmentRows;
    statsKernel.leftRadius = leftRadius;
    statsKernel.rightRadius = rightRadius;
    statsKernel.dispatch(pass, rows, heads, window);

    auto& weightedSumKernel = sharedKernel<BandedAttentionWeightedSumKernel>(device);
    weightedSumKernel.value = value.buffer();
    weightedSumKernel.probabilities = scores.buffer();
    weightedSumKernel.rowSum = rowSum.buffer();
    weightedSumKernel.output = output.buffer();
    weightedSumKernel.segmentRows = segmentRows;
    weightedSumKernel.leftRadius = leftRadius;
    weightedSumKernel.rightRadius = rightRadius;
    weightedSumKernel.dispatch(pass, rows, heads, headDim, window);

    return output;
}
} // namespace eacp::ML
