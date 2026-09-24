#pragma once

#include <eacp/GPU/Codegen/ComputeProgram.h>
#include <eacp/ML/Tensor/Tensor.h>

namespace eacp::SA3TextEncoder
{
class GemmaAttentionScoresKernel final : public GPU::ComputeProgram
{
public:
    GemmaAttentionScoresKernel();

    void dispatch(GPU::ComputePass& pass, int rows, int heads, int cols);

    GPU::Uniform<GPU::InputBuffer> query;
    GPU::Uniform<GPU::InputBuffer> key;
    GPU::Uniform<GPU::InputBuffer> additiveMask;
    GPU::Uniform<GPU::OutputBuffer> scores;
    GPU::Uniform<GPU::UInt> headCount;
    GPU::Uniform<GPU::UInt> headDimension;
    GPU::Uniform<GPU::UInt> columnCount;
    GPU::Uniform<GPU::Float> scale;
    GPU::Uniform<GPU::Float> softcap;

    EACP_SHADER(query,
               key,
               additiveMask,
               scores,
               headCount,
               headDimension,
               columnCount,
               scale,
               softcap)

private:
    void define() override;
};

ML::Tensor gemmaSelfAttention(GPU::ComputePass& pass,
                              const ML::Tensor& query,
                              const ML::Tensor& key,
                              const ML::Tensor& value,
                              const ML::Tensor& additiveMask,
                              int heads,
                              int headDim,
                              float scale,
                              float softcap,
                              GPU::Device& device = GPU::Device::shared());
}
