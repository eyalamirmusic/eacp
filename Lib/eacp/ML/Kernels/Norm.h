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

    GPU::Uniform<GPU::InputBuffer> input;
    GPU::Uniform<GPU::InputBuffer> gamma;
    GPU::Uniform<GPU::OutputBuffer> output;
    GPU::Uniform<GPU::UInt> dimension;
    GPU::Uniform<GPU::Float> epsilon;

    EACP_SHADER(input, gamma, output, dimension, epsilon)

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

    GPU::Uniform<GPU::InputBuffer> input;
    GPU::Uniform<GPU::InputBuffer> gamma;
    GPU::Uniform<GPU::InputBuffer> beta;
    GPU::Uniform<GPU::OutputBuffer> output;
    GPU::Uniform<GPU::UInt> dimension;
    GPU::Uniform<GPU::Float> alpha;

    EACP_SHADER(input, gamma, beta, output, dimension, alpha)

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
}
