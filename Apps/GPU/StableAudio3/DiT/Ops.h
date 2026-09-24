#pragma once

#include <eacp/GPU/Codegen/ComputeProgram.h>
#include <eacp/ML/Tensor/Tensor.h>

namespace eacp::SA3DiT
{
class AddBroadcastRowKernel final : public GPU::ComputeProgram
{
public:
    AddBroadcastRowKernel();

    void dispatch(GPU::ComputePass& pass, int rowStart, int rowCount, int columns);

    GPU::Uniform<GPU::OutputBuffer> values;
    GPU::Uniform<GPU::InputBuffer> addend;
    GPU::Uniform<GPU::UInt> columnCount;
    GPU::Uniform<GPU::UInt> rowStart;

    EACP_SHADER(values, addend, columnCount, rowStart)

private:
    void define() override;
};

class AdaLNModulateKernel final : public GPU::ComputeProgram
{
public:
    AdaLNModulateKernel();

    void dispatch(GPU::ComputePass& pass, int rows, int columns);

    GPU::Uniform<GPU::InputBuffer> input;
    GPU::Uniform<GPU::InputBuffer> scale;
    GPU::Uniform<GPU::InputBuffer> shift;
    GPU::Uniform<GPU::OutputBuffer> output;
    GPU::Uniform<GPU::UInt> columnCount;

    EACP_SHADER(input, scale, shift, output, columnCount)

private:
    void define() override;
};

class SigmoidGateKernel final : public GPU::ComputeProgram
{
public:
    SigmoidGateKernel();

    void dispatch(GPU::ComputePass& pass, int rows, int columns);

    GPU::Uniform<GPU::InputBuffer> input;
    GPU::Uniform<GPU::InputBuffer> gate;
    GPU::Uniform<GPU::OutputBuffer> output;
    GPU::Uniform<GPU::UInt> columnCount;

    EACP_SHADER(input, gate, output, columnCount)

private:
    void define() override;
};

class ExpoFourierFeaturesKernel final : public GPU::ComputeProgram
{
public:
    ExpoFourierFeaturesKernel();

    void dispatch(GPU::ComputePass& pass, int halfDim);

    GPU::Uniform<GPU::OutputBuffer> output;
    GPU::Uniform<GPU::Float> value;
    GPU::Uniform<GPU::Float> logMinFreq;
    GPU::Uniform<GPU::Float> logMaxFreq;
    GPU::Uniform<GPU::Float> rampDenominator;
    GPU::Uniform<GPU::UInt> halfDim;

    EACP_SHADER(output, value, logMinFreq, logMaxFreq, rampDenominator, halfDim)

private:
    void define() override;
};

ML::Tensor addBroadcastRow(GPU::ComputePass& pass,
                           const ML::Tensor& values,
                           const ML::Tensor& addend,
                           int rowStart,
                           GPU::Device& device = GPU::Device::shared());

ML::Tensor adaLNModulate(GPU::ComputePass& pass,
                         const ML::Tensor& input,
                         const ML::Tensor& scale,
                         const ML::Tensor& shift,
                         GPU::Device& device = GPU::Device::shared());

ML::Tensor sigmoidGate(GPU::ComputePass& pass,
                       const ML::Tensor& input,
                       const ML::Tensor& gate,
                       GPU::Device& device = GPU::Device::shared());

// The same two over a row of scale, shift or gate that is part of a larger
// buffer - the adaLN modulation vector is six of them side by side - read where
// it lies rather than copied out first.
ML::Tensor adaLNModulate(GPU::ComputePass& pass,
                         const ML::Tensor& input,
                         const GPU::BufferRange& scale,
                         const GPU::BufferRange& shift,
                         GPU::Device& device = GPU::Device::shared());

ML::Tensor sigmoidGate(GPU::ComputePass& pass,
                       const ML::Tensor& input,
                       const GPU::BufferRange& gate,
                       GPU::Device& device = GPU::Device::shared());

ML::Tensor squeezeTrailingUnitDim(ML::Tensor input);
ML::Tensor expoFourierFeatures(GPU::ComputePass& pass,
                               float value,
                               int dim,
                               float minFreq,
                               float maxFreq,
                               GPU::Device& device = GPU::Device::shared());
}
