#pragma once

#include "../../GPU/Codegen/ComputeProgram.h"
#include "../Tensor/Tensor.h"

namespace eacp::ML
{
// The width of the threadgroup that folds one (row, head) of scores: the row
// stats here and in bandedAttention.
constexpr auto attentionGroupWidth = 256;

// Whether the scores add a rows x cols mask. Without one there is no buffer of
// zeros built on the host and read back by every thread.
enum class AttentionMask
{
    Additive,
    None
};

class AttentionScoresKernel final : public GPU::ComputeProgram
{
public:
    explicit AttentionScoresKernel(
        AttentionMask maskToUse = AttentionMask::Additive);

    void dispatch(GPU::ComputePass& pass, int rows, int heads, int cols);

    std::string name() const override;

    GPU::Uniform<GPU::InputBuffer> query;
    GPU::Uniform<GPU::InputBuffer> key;
    GPU::Uniform<GPU::InputBuffer> additiveMask;
    GPU::Uniform<GPU::OutputBuffer> scores;
    GPU::Uniform<GPU::UInt> headCount;
    GPU::Uniform<GPU::UInt> headDimension;
    GPU::Uniform<GPU::UInt> rowCount;
    GPU::Uniform<GPU::UInt> columnCount;
    GPU::Uniform<GPU::Float> scale;

    // EACP_SHADER written out, so the unmasked kernel declares no mask at all.
    void reflectMembers(GPU::ShaderVisitor& visitor) override;

private:
    void define() override;

    AttentionMask mask;
};

// Softmax over the scores of each (row, head), in two kernels. The row stats
// turn a row of scores into its probabilities in place - exp(score - peak),
// each evaluated once - and write their sum; the weighted sum then reads those
// probabilities back in ascending column order and divides by the sum.
class AttentionRowStatsKernel final : public GPU::ComputeProgram
{
public:
    AttentionRowStatsKernel();

    void dispatch(GPU::ComputePass& pass, int rows, int heads, int cols);

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
    GPU::Uniform<GPU::UInt> rowCount;
    GPU::Uniform<GPU::UInt> columnCount;

    EACP_SHADER(value,
                probabilities,
                rowSum,
                output,
                headCount,
                headDimension,
                rowCount,
                columnCount)

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

// What attention() does beyond the plain softmax(q kᵀ / √d) v: an additive
// mask over rows x cols, and an RMS norm of each query and key head with its
// own gamma before the product.
struct AttentionOptions
{
    const Tensor* mask = nullptr;
    const Tensor* queryNorm = nullptr;
    const Tensor* keyNorm = nullptr;
    float normEpsilon = 1e-6f;
};

Tensor attention(GPU::ComputePass& pass,
                 const Tensor& query,
                 const Tensor& key,
                 const Tensor& value,
                 int heads,
                 int headDim,
                 const AttentionOptions& options = {},
                 GPU::Device& device = GPU::Device::shared());
} // namespace eacp::ML
