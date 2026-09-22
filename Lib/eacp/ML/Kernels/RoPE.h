#pragma once

#include "../../GPU/Codegen/ComputeProgram.h"
#include "../Tensor/Tensor.h"

namespace eacp::ML
{
class RoPEKernel final : public GPU::ComputeProgram
{
public:
    RoPEKernel();

    void dispatch(GPU::ComputePass& pass, int rows, int heads, int headDim);

    GPU::Uniform<GPU::InputBuffer> input;
    GPU::Uniform<GPU::InputBuffer> invFreq;
    GPU::Uniform<GPU::OutputBuffer> output;
    GPU::Uniform<GPU::UInt> headCount;
    GPU::Uniform<GPU::UInt> headDimension;
    GPU::Uniform<GPU::UInt> halfRotaryDimension;

    EACP_SHADER(input,
               invFreq,
               output,
               headCount,
               headDimension,
               halfRotaryDimension)

private:
    void define() override;
};

Tensor applyRoPE(GPU::ComputePass& pass,
                 const Tensor& input,
                 const Tensor& invFreq,
                 int heads,
                 int headDim,
                 GPU::Device& device = GPU::Device::shared());
}
