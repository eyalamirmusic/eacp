#pragma once

#include "../../GPU/Codegen/ComputeProgram.h"
#include "../Tensor/Tensor.h"

namespace eacp::ML
{
class SwiGLUGateKernel final : public GPU::ComputeProgram
{
public:
    SwiGLUGateKernel();

    void dispatch(GPU::ComputePass& pass, int rows, int inner);

    GPU::Uniform<GPU::InputBuffer> hidden;
    GPU::Uniform<GPU::OutputBuffer> output;
    GPU::Uniform<GPU::UInt> innerDimension;

    EACP_SHADER(hidden, output, innerDimension)

private:
    void define() override;
};

Tensor swiGLU(GPU::ComputePass& pass,
             const Tensor& input,
             const Tensor& proj0Weight,
             const Tensor& proj0Bias,
             const Tensor& proj2Weight,
             const Tensor& proj2Bias,
             GPU::Device& device = GPU::Device::shared());
}
