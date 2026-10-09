#pragma once

#include "../../GPU/Codegen/ComputeProgram.h"
#include "../Tensor/Tensor.h"

namespace eacp::ML
{
constexpr auto normGroupWidth = 256;

class RMSNormKernel final : public GPU::ComputeProgram
{
public:
    RMSNormKernel();

    void dispatch(GPU::ComputePass& pass, int rows, int dim);

    // Reads each run of dim values from a view rather than from row * dim:
    // a whole tensor's rows, or each head of a column slice of one.
    void read(const TensorView& view, int dim);

    GPU::Uniform<GPU::InputBuffer> input;
    GPU::Uniform<GPU::InputBuffer> gamma;
    GPU::Uniform<GPU::OutputBuffer> output;
    GPU::Uniform<GPU::UInt> dimension;
    GPU::Uniform<GPU::Float> epsilon;
    GPU::Uniform<GPU::UInt> runsPerRow;
    GPU::Uniform<GPU::UInt> inputRowStride;
    GPU::Uniform<GPU::UInt> inputColumnOffset;

    EACP_SHADER(input,
                gamma,
                output,
                dimension,
                epsilon,
                runsPerRow,
                inputRowStride,
                inputColumnOffset)

private:
    void define() override;
};

class LayerNormKernel final : public GPU::ComputeProgram
{
public:
    LayerNormKernel();

    void dispatch(GPU::ComputePass& pass, int rows, int dim);

    GPU::Uniform<GPU::InputBuffer> input;
    GPU::Uniform<GPU::InputBuffer> gamma;
    GPU::Uniform<GPU::InputBuffer> beta;
    GPU::Uniform<GPU::OutputBuffer> output;
    GPU::Uniform<GPU::UInt> dimension;
    GPU::Uniform<GPU::Float> epsilon;

    EACP_SHADER(input, gamma, beta, output, dimension, epsilon)

private:
    void define() override;
};

class LayerNormNoBiasKernel final : public GPU::ComputeProgram
{
public:
    LayerNormNoBiasKernel();

    void dispatch(GPU::ComputePass& pass, int rows, int dim);

    GPU::Uniform<GPU::InputBuffer> input;
    GPU::Uniform<GPU::InputBuffer> gamma;
    GPU::Uniform<GPU::OutputBuffer> output;
    GPU::Uniform<GPU::UInt> dimension;
    GPU::Uniform<GPU::Float> epsilon;

    EACP_SHADER(input, gamma, output, dimension, epsilon)

private:
    void define() override;
};

class DynamicTanhKernel final : public GPU::ComputeProgram
{
public:
    DynamicTanhKernel();

    void dispatch(GPU::ComputePass& pass, int rows, int dim);

    void read(const TensorView& view, int dim);

    GPU::Uniform<GPU::InputBuffer> input;
    GPU::Uniform<GPU::InputBuffer> gamma;
    GPU::Uniform<GPU::InputBuffer> beta;
    GPU::Uniform<GPU::OutputBuffer> output;
    GPU::Uniform<GPU::UInt> dimension;
    GPU::Uniform<GPU::Float> alpha;
    GPU::Uniform<GPU::UInt> runsPerRow;
    GPU::Uniform<GPU::UInt> inputRowStride;
    GPU::Uniform<GPU::UInt> inputColumnOffset;

    EACP_SHADER(input,
                gamma,
                beta,
                output,
                dimension,
                alpha,
                runsPerRow,
                inputRowStride,
                inputColumnOffset)

private:
    void define() override;
};

Tensor rmsNorm(GPU::ComputePass& pass,
              const Tensor& input,
              const Tensor& gamma,
              float epsilon,
              GPU::Device& device = GPU::Device::shared());

Tensor layerNorm(GPU::ComputePass& pass,
                 const Tensor& input,
                 const Tensor& gamma,
                 const Tensor* beta,
                 float epsilon,
                 GPU::Device& device = GPU::Device::shared());

Tensor dynamicTanh(GPU::ComputePass& pass,
                   const Tensor& input,
                   const Tensor& gamma,
                   const Tensor& beta,
                   float alpha,
                   GPU::Device& device = GPU::Device::shared());

// The two above over each head of a rows x (heads * headDim) tensor on its own
// rather than over each row: every run of headDim values is normalised with the
// one headDim-long gamma (and beta) - the QK norm a transformer applies to its
// queries and keys before attention. The input may be a view, so the queries
// of a fused projection are normalised where they lie:
//     auto q = rmsNormPerHead(pass, qkv.columns(0, dim), gamma, headDim, eps);
// The result is a rows x cols tensor of its own.
Tensor rmsNormPerHead(GPU::ComputePass& pass,
                      const TensorView& input,
                      const Tensor& gamma,
                      int headDim,
                      float epsilon,
                      GPU::Device& device = GPU::Device::shared());

Tensor dynamicTanhPerHead(GPU::ComputePass& pass,
                          const TensorView& input,
                          const Tensor& gamma,
                          const Tensor& beta,
                          float alpha,
                          int headDim,
                          GPU::Device& device = GPU::Device::shared());
}
