#include "RoPE.h"

#include "../../GPU/Codegen/KernelCache.h"
#include "../../GPU/Frame/ComputePass.h"

namespace eacp::ML
{
using namespace eacp::GPU;

RoPEKernel::RoPEKernel()
{
    compile();
}

void RoPEKernel::dispatch(
    ComputePass& pass, int rows, int heads, int headDim, int segmentRowCount)
{
    headCount = (std::uint32_t) heads;
    headDimension = (std::uint32_t) headDim;
    segmentRows = (std::uint32_t) (segmentRowCount > 0 ? segmentRowCount : rows);
    pass.dispatch(*this, rows * heads * headDim);
}

void RoPEKernel::define()
{
    auto i = threadId();
    auto d = i % headDimension;
    auto rowHead = i / headDimension;
    auto row = (rowHead / headCount) % segmentRows;

    auto rotaryDimension = halfRotaryDimension * 2u;
    auto base = rowHead * headDimension;

    ifThen(
        d < rotaryDimension,
        [&]
        {
            auto isFirstHalf = d < halfRotaryDimension;
            auto freqIndex = select(isFirstHalf, d, d - halfRotaryDimension);

            auto angle = toFloat(row) * invFreq[freqIndex];
            auto cosine = cos(angle);
            auto sine = sin(angle);

            auto x1 = input[base + freqIndex];
            auto x2 = input[base + halfRotaryDimension + freqIndex];

            auto rotated = select(
                isFirstHalf, x1 * cosine - x2 * sine, x2 * cosine + x1 * sine);

            write(output, base + d, rotated);
        },
        [&] { write(output, base + d, input[base + d]); });
}

Tensor applyRoPE(ComputePass& pass,
                 const Tensor& input,
                 const Tensor& invFreq,
                 int heads,
                 int headDim,
                 Device& device)
{
    return applyRoPE(pass, input, invFreq, heads, headDim, 0, device);
}

Tensor applyRoPE(ComputePass& pass,
                 const Tensor& input,
                 const Tensor& invFreq,
                 int heads,
                 int headDim,
                 int segmentRows,
                 Device& device)
{
    auto rows = input.rows();
    auto result = Tensor::uninitializedF32(input.shape(), device);

    auto& kernel = sharedKernel<RoPEKernel>(device);
    kernel.input = input.buffer();
    kernel.invFreq = invFreq.buffer();
    kernel.output = result.buffer();
    kernel.halfRotaryDimension = (std::uint32_t) invFreq.count();
    kernel.dispatch(pass, rows, heads, headDim, segmentRows);

    return result;
}
} // namespace eacp::ML
