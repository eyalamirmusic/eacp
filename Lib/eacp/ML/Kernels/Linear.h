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

// How many rows of the output one LinearF32 threadgroup covers, 64 or 32.
// Either gives the same bits. 32 wins twice over: the last tile of a batch
// computes every one of its rows whether the batch has them or not, so a batch
// of 387 pays for 448 rows in tiles of 64 and 416 in tiles of 32; and an output
// too small to fill the GPU with tiles of 64 fills it better with twice as many
// tiles. Elsewhere 64 is as fast or a little faster, its larger blocks reusing
// each loaded fragment more.
int linearTileRowsFor(int rows, int columns);

// output = activations x weightᵀ in fp32 on SIMD-group matrices.
class LinearF32 final : public GPU::ComputeProgram
{
public:
    LinearF32(LinearLoads loads, int tileRows);

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
    int tileRows;
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
