#pragma once

#include "../../GPU/Codegen/ComputeProgram.h"
#include "../Tensor/Tensor.h"

namespace eacp::ML
{
// How LinearF32 reads its operands: four floats at a time, which needs the
// inner dimension to be a multiple of four, or one at a time, which does not.
// The two give the same bits; linearLoadsFor picks the wider one wherever the
// shape allows it, and it is what linear() dispatches.
enum class LinearLoads
{
    FourWide,
    Scalar
};

LinearLoads linearLoadsFor(int inner);

// output = activations x weightᵀ in fp32 on SIMD-group matrices.
class LinearF32 final : public GPU::ComputeProgram
{
public:
    explicit LinearF32(LinearLoads loads);

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

    LinearLoads loads;
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
