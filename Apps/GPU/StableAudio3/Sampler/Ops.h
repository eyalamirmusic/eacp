#pragma once

#include <eacp/GPU/Codegen/ComputeProgram.h>
#include <eacp/ML/Tensor/Tensor.h>

namespace eacp::SA3Sampler
{
class ScaleAndAddKernel final : public GPU::ComputeProgram
{
public:
    ScaleAndAddKernel();

    void dispatch(GPU::ComputePass& pass, int count, float scaleAValue, float scaleBValue);

    GPU::Uniform<GPU::InputBuffer> a;
    GPU::Uniform<GPU::InputBuffer> b;
    GPU::Uniform<GPU::OutputBuffer> output;
    GPU::Uniform<GPU::Float> scaleA;
    GPU::Uniform<GPU::Float> scaleB;

    EACP_SHADER(a, b, output, scaleA, scaleB)

private:
    void define() override;
};

ML::Tensor scaleAndAdd(GPU::ComputePass& pass,
                       const ML::Tensor& a,
                       float scaleA,
                       const ML::Tensor& b,
                       float scaleB,
                       GPU::Device& device = GPU::Device::shared());
}
