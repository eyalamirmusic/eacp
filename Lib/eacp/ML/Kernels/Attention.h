#pragma once

#include "../../GPU/Codegen/ComputeProgram.h"
#include "../Tensor/Tensor.h"

namespace eacp::ML
{
class AttentionScoresKernel final : public GPU::ComputeProgram
{
public:
    AttentionScoresKernel();

    void dispatch(GPU::ComputePass& pass, int rows, int heads, int cols);

    GPU::Uniform<GPU::InputBuffer> query;
    GPU::Uniform<GPU::InputBuffer> key;
    GPU::Uniform<GPU::InputBuffer> additiveMask;
    GPU::Uniform<GPU::OutputBuffer> scores;
    GPU::Uniform<GPU::UInt> headCount;
    GPU::Uniform<GPU::UInt> headDimension;
    GPU::Uniform<GPU::UInt> columnCount;
    GPU::Uniform<GPU::Float> scale;

    EACP_SHADER(query,
                key,
                additiveMask,
                scores,
                headCount,
                headDimension,
                columnCount,
                scale)

private:
    void define() override;
};

// AttentionScoresKernel with no mask to add, for attention that sees every
// key: the same scores, with no rows x cols buffer of zeros built on the host
// and read back by every thread.
class UnmaskedAttentionScoresKernel final : public GPU::ComputeProgram
{
public:
    UnmaskedAttentionScoresKernel();

    void dispatch(GPU::ComputePass& pass, int rows, int heads, int cols);

    GPU::Uniform<GPU::InputBuffer> query;
    GPU::Uniform<GPU::InputBuffer> key;
    GPU::Uniform<GPU::OutputBuffer> scores;
    GPU::Uniform<GPU::UInt> headCount;
    GPU::Uniform<GPU::UInt> headDimension;
    GPU::Uniform<GPU::UInt> columnCount;
    GPU::Uniform<GPU::Float> scale;

    EACP_SHADER(query, key, scores, headCount, headDimension, columnCount, scale)

private:
    void define() override;
};

// Softmax over the scores of each (row, head), in two kernels. The row stats
// turn a row of scores into its probabilities in place - exp(score - peak),
// each evaluated once - and write their sum; the weighted sum then reads those
// probabilities back in ascending column order and divides by the sum.
class AttentionRowStatsKernel final : public GPU::ComputeProgram
{
public:
    AttentionRowStatsKernel();

    void dispatch(GPU::ComputePass& pass, int rowGroups, int cols);

    GPU::Uniform<GPU::OutputBuffer> scores;
    GPU::Uniform<GPU::OutputBuffer> rowSum;
    GPU::Uniform<GPU::UInt> columnCount;

    EACP_SHADER(scores, rowSum, columnCount)

private:
    void define() override;
};

class AttentionWeightedSumKernel final : public GPU::ComputeProgram
{
public:
    AttentionWeightedSumKernel();

    void dispatch(GPU::ComputePass& pass, int rows, int heads, int headDim);

    GPU::Uniform<GPU::InputBuffer> value;
    GPU::Uniform<GPU::InputBuffer> probabilities;
    GPU::Uniform<GPU::InputBuffer> rowSum;
    GPU::Uniform<GPU::OutputBuffer> output;
    GPU::Uniform<GPU::UInt> headCount;
    GPU::Uniform<GPU::UInt> headDimension;
    GPU::Uniform<GPU::UInt> columnCount;

    EACP_SHADER(
        value, probabilities, rowSum, output, headCount, headDimension, columnCount)

private:
    void define() override;
};

Tensor
    buildCausalMask(int rows, int cols, GPU::Device& device = GPU::Device::shared());
Tensor
    buildZeroMask(int rows, int cols, GPU::Device& device = GPU::Device::shared());

// The softmax and weighted sum of attention() over scores computed elsewhere,
// for a score kernel of its own (a soft cap, a bias, a different scale).
// scores is rows x heads x cols and is overwritten with the unnormalised
// probabilities; the result is rows x heads x headDim.
Tensor attendWithScores(GPU::ComputePass& pass,
                        Tensor& scores,
                        const Tensor& value,
                        int heads,
                        int headDim,
                        GPU::Device& device = GPU::Device::shared());

Tensor attention(GPU::ComputePass& pass,
                 const Tensor& query,
                 const Tensor& key,
                 const Tensor& value,
                 int heads,
                 int headDim,
                 const Tensor* additiveMask = nullptr,
                 const Tensor* queryNormGamma = nullptr,
                 const Tensor* keyNormGamma = nullptr,
                 float qkNormEpsilon = 1e-6f,
                 GPU::Device& device = GPU::Device::shared());
} // namespace eacp::ML
