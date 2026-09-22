#pragma once

#include "../../GPU/Codegen/ComputeProgram.h"
#include "../Tensor/Tensor.h"

namespace eacp::ML
{
enum class ActivationKind
{
    SiLU,
    GeluTanh,
    GeluExact,
    Sigmoid
};

class ActivationKernel final : public GPU::ComputeProgram
{
public:
    explicit ActivationKernel(ActivationKind kindToUse);

    void dispatch(GPU::ComputePass& pass, int count);

    GPU::Uniform<GPU::InputBuffer> input;
    GPU::Uniform<GPU::OutputBuffer> output;

    EACP_SHADER(input, output)

private:
    void define() override;

    ActivationKind kind;
};

Tensor applyActivation(GPU::ComputePass& pass,
                       const Tensor& input,
                       ActivationKind kind,
                       GPU::Device& device = GPU::Device::shared());
}
