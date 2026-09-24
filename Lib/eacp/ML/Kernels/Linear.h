#pragma once

#include "../../GPU/Codegen/ComputeProgram.h"
#include "../Tensor/Tensor.h"

namespace eacp::ML
{
// output = activations x weightᵀ in fp32 on SIMD-group matrices. vectorLoads
// reads A and the weight four floats at a time, which needs the inner
// dimension to be a multiple of four; linear() picks it whenever it is.
class LinearF32 final : public GPU::ComputeProgram
{
public:
    explicit LinearF32(bool vectorLoads = true);

    void dispatch(GPU::ComputePass& pass, int rows, int columns, int inner);

    GPU::Uniform<GPU::InputBuffer> activations;
    GPU::Uniform<GPU::InputBuffer> weight;
    GPU::Uniform<GPU::OutputBuffer> output;
    GPU::Uniform<GPU::UInt> rowCount;
    GPU::Uniform<GPU::UInt> columnCount;
    GPU::Uniform<GPU::UInt> innerCount;

    EACP_SHADER(activations, weight, output, rowCount, columnCount, innerCount)

private:
    void define() override;

    bool vectorLoads = true;
};

class LinearPackedHalf final : public GPU::ComputeProgram
{
public:
    LinearPackedHalf();

    void dispatch(GPU::ComputePass& pass, int rows, int columns, int inner);

    GPU::Uniform<GPU::InputBuffer> activations;
    GPU::Uniform<GPU::InputBuffer> weight;
    GPU::Uniform<GPU::OutputBuffer> output;
    GPU::Uniform<GPU::UInt> rowCount;
    GPU::Uniform<GPU::UInt> columnCount;
    GPU::Uniform<GPU::UInt> innerCount;

    EACP_SHADER(activations, weight, output, rowCount, columnCount, innerCount)

private:
    void define() override;
};

class AddBiasRows final : public GPU::ComputeProgram
{
public:
    AddBiasRows();

    void dispatch(GPU::ComputePass& pass, int rows, int columns);

    GPU::Uniform<GPU::OutputBuffer> values;
    GPU::Uniform<GPU::InputBuffer> bias;
    GPU::Uniform<GPU::UInt> columnCount;

    EACP_SHADER(values, bias, columnCount)

private:
    void define() override;
};

Tensor linear(GPU::ComputePass& pass,
             const Tensor& input,
             const Tensor& weight,
             const Tensor* bias = nullptr,
             GPU::Device& device = GPU::Device::shared());
}
