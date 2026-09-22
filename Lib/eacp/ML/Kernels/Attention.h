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

class AttentionRowStatsKernel final : public GPU::ComputeProgram
{
public:
    AttentionRowStatsKernel();

    void dispatch(GPU::ComputePass& pass, int rowGroups, int cols);

    GPU::Uniform<GPU::InputBuffer> scores;
    GPU::Uniform<GPU::OutputBuffer> rowMax;
    GPU::Uniform<GPU::OutputBuffer> rowSum;
    GPU::Uniform<GPU::UInt> columnCount;

    EACP_SHADER(scores, rowMax, rowSum, columnCount)

private:
    void define() override;
};

class AttentionWeightedSumKernel final : public GPU::ComputeProgram
{
public:
    AttentionWeightedSumKernel();

    void dispatch(GPU::ComputePass& pass, int rows, int heads, int headDim);

    GPU::Uniform<GPU::InputBuffer> value;
    GPU::Uniform<GPU::InputBuffer> scores;
    GPU::Uniform<GPU::InputBuffer> rowMax;
    GPU::Uniform<GPU::InputBuffer> rowSum;
    GPU::Uniform<GPU::OutputBuffer> output;
    GPU::Uniform<GPU::UInt> headCount;
    GPU::Uniform<GPU::UInt> headDimension;
    GPU::Uniform<GPU::UInt> columnCount;

    EACP_SHADER(value,
               scores,
               rowMax,
               rowSum,
               output,
               headCount,
               headDimension,
               columnCount)

private:
    void define() override;
};

Tensor buildCausalMask(int rows, int cols, GPU::Device& device = GPU::Device::shared());
Tensor buildZeroMask(int rows, int cols, GPU::Device& device = GPU::Device::shared());

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
}
