#pragma once

#include "../../GPU/Codegen/ComputeProgram.h"
#include "../Tensor/Tensor.h"

namespace eacp::ML
{
class RoPEKernel final : public GPU::ComputeProgram
{
public:
    RoPEKernel();

    void dispatch(GPU::ComputePass& pass,
                  int rows,
                  int heads,
                  int headDim,
                  int segmentRows = 0);

    GPU::Uniform<GPU::InputBuffer> input;
    GPU::Uniform<GPU::InputBuffer> invFreq;
    GPU::Uniform<GPU::OutputBuffer> output;
    GPU::Uniform<GPU::UInt> headCount;
    GPU::Uniform<GPU::UInt> headDimension;
    GPU::Uniform<GPU::UInt> halfRotaryDimension;
    GPU::Uniform<GPU::UInt> segmentRows;

    EACP_SHADER(input,
                invFreq,
                output,
                headCount,
                headDimension,
                halfRotaryDimension,
                segmentRows)

private:
    void define() override;
};

Tensor applyRoPE(GPU::ComputePass& pass,
                 const Tensor& input,
                 const Tensor& invFreq,
                 int heads,
                 int headDim,
                 GPU::Device& device = GPU::Device::shared());

// Rows grouped into segments of segmentRows, each rotated as if it were a
// sequence of its own: row r takes position r % segmentRows. It is how
// independent sequences stacked into one tensor go through RoPE in a single
// dispatch. A segmentRows of 0, or of the whole tensor, is the overload above.
Tensor applyRoPE(GPU::ComputePass& pass,
                 const Tensor& input,
                 const Tensor& invFreq,
                 int heads,
                 int headDim,
                 int segmentRows,
                 GPU::Device& device = GPU::Device::shared());
} // namespace eacp::ML
