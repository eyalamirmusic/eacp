#include "Attention.h"

#include "../../GPU/Codegen/KernelCache.h"
#include "../../GPU/Frame/ComputePass.h"
#include "Norm.h"

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

Tensor rmsNormPerHead(ComputePass& pass,
                      const Tensor& input,
                      const Tensor& gamma,
                      int rowCount,
                      int heads,
                      int headDim,
                      float epsilon,
                      Device& device)
{
    auto result = Tensor::uninitializedF32(input.shape(), device);

    auto& kernel = sharedKernel<RMSNormKernel>(device);
    kernel.input = input.buffer();
    kernel.gamma = gamma.buffer();
    kernel.output = result.buffer();
    kernel.epsilon = epsilon;
    kernel.dispatch(pass, rowCount * heads, headDim);

    return result;
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
    headCount = (std::uint32_t) heads;
    columnCount = (std::uint32_t) cols;
    pass.dispatch(*this, rows * heads * cols);
}

void AttentionScoresKernel::define()
{
    auto i = threadId();
    auto col = i % columnCount;
    auto rowHead = i / columnCount;
    auto row = rowHead / headCount;

    auto queryBase = rowHead * headDimension;
    auto head = rowHead % headCount;
    auto keyBase = (col * headCount + head) * headDimension;

    auto dot = var(0.f);
    auto d = var(0u);

    loop(d.get() < headDimension,
         [&]
         {
             dot = dot.get() + query[queryBase + d.get()] * key[keyBase + d.get()];
             d = d.get() + 1u;
         });

    auto score = dot.get() * scale + additiveMask[row * columnCount + col];
    write(scores, i, score);
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
    headCount = (std::uint32_t) heads;
    columnCount = (std::uint32_t) cols;
    pass.dispatch(*this, rows * heads * cols);
}

void UnmaskedAttentionScoresKernel::define()
{
    auto i = threadId();
    auto col = i % columnCount;
    auto rowHead = i / columnCount;

    auto queryBase = rowHead * headDimension;
    auto head = rowHead % headCount;
    auto keyBase = (col * headCount + head) * headDimension;

    auto dot = var(0.f);
    auto d = var(0u);

    loop(d.get() < headDimension,
         [&]
         {
             dot = dot.get() + query[queryBase + d.get()] * key[keyBase + d.get()];
             d = d.get() + 1u;
         });

    write(scores, i, dot.get() * scale);
}

AttentionRowStatsKernel::AttentionRowStatsKernel()
    : ComputeProgram({attentionGroupWidth, 1, 1})
{
    compile();
}

void AttentionRowStatsKernel::dispatch(ComputePass& pass, int rowGroups, int cols)
{
    columnCount = (std::uint32_t) cols;
    pass.dispatch(*this, attentionGroupWidth, rowGroups);
}

void AttentionRowStatsKernel::define()
{
    auto lane = threadPosition().x;
    auto rowGroup = threadPosition().y;
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
             auto probability = var(exp(scores[index] - peak));
             write(scores, index, probability.get());
             localSum = localSum.get() + probability.get();
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
    headCount = (std::uint32_t) heads;
    pass.dispatch(*this, rows * heads * headDim);
}

void AttentionWeightedSumKernel::define()
{
    auto i = threadId();
    auto d = i % headDimension;
    auto rowHead = i / headDimension;
    auto head = rowHead % headCount;

    auto probabilityBase = rowHead * columnCount;
    auto total = rowSum[rowHead];

    auto accumulator = var(0.f);
    auto col = var(0u);

    loop(col.get() < columnCount,
         [&]
         {
             auto probability = probabilities[probabilityBase + col.get()];
             auto valueBase = (col.get() * headCount + head) * headDimension;

             accumulator = accumulator.get() + probability * value[valueBase + d];

             col = col.get() + 1u;
         });

    write(output, rowHead * headDimension + d, accumulator.get() / total);
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
    statsKernel.dispatch(pass, rows * heads, cols);

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
            ? std::optional<Tensor> {rmsNormPerHead(pass,
                                                    query,
                                                    *queryNormGamma,
                                                    rows,
                                                    heads,
                                                    headDim,
                                                    qkNormEpsilon,
                                                    device)}
            : std::nullopt;

    const auto& normalizedQuery =
        normalizedQueryStorage.has_value() ? *normalizedQueryStorage : query;

    auto normalizedKeyStorage =
        keyNormGamma != nullptr
            ? std::optional<Tensor> {rmsNormPerHead(pass,
                                                    key,
                                                    *keyNormGamma,
                                                    cols,
                                                    heads,
                                                    headDim,
                                                    qkNormEpsilon,
                                                    device)}
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
