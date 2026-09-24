#include "GpuOps.h"

#include <eacp/GPU/Codegen/KernelCache.h>
#include <eacp/GPU/Frame/ComputePass.h>

namespace eacp::SA3Codec
{
using namespace eacp::GPU;
using namespace eacp::ML;

namespace
{
class FoldWithNewTokensKernel final : public ComputeProgram
{
public:
    FoldWithNewTokensKernel()
    {
        compile();
    }

    void dispatch(ComputePass& pass, int outputRows, int columns)
    {
        columnCount = (std::uint32_t) columns;
        pass.dispatch(*this, columns, outputRows);
    }

    Uniform<InputBuffer> input;
    Uniform<InputBuffer> newTokens;
    Uniform<OutputBuffer> output;
    Uniform<UInt> columnCount;
    Uniform<UInt> inputSegSize;
    Uniform<UInt> subChunkSize;
    Uniform<UInt> inputRowCount;

    EACP_SHADER(input, newTokens, output, columnCount, inputSegSize, subChunkSize, inputRowCount)

private:
    void define() override
    {
        auto position = threadPosition();
        auto outputRow = position.y;
        auto c = position.x;

        auto group = outputRow / subChunkSize;
        auto local = outputRow % subChunkSize;
        auto isReal = local < inputSegSize;

        auto inputRowRaw = group * inputSegSize + local;
        auto inputRowClamped = min(inputRowRaw, inputRowCount - 1u);

        auto realValue = input[inputRowClamped * columnCount + c];
        auto tokenValue = newTokens[c];

        write(output, outputRow * columnCount + c, select(isReal, realValue, tokenValue));
    }
};

class UnfoldLastSegmentKernel final : public ComputeProgram
{
public:
    UnfoldLastSegmentKernel()
    {
        compile();
    }

    void dispatch(ComputePass& pass, int outputRows, int columns)
    {
        columnCount = (std::uint32_t) columns;
        pass.dispatch(*this, columns, outputRows);
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;
    Uniform<UInt> columnCount;
    Uniform<UInt> subChunkSize;
    Uniform<UInt> outputSegSize;
    Uniform<UInt> startLocal;

    EACP_SHADER(input, output, columnCount, subChunkSize, outputSegSize, startLocal)

private:
    void define() override
    {
        auto position = threadPosition();
        auto outputRow = position.y;
        auto c = position.x;

        auto group = outputRow / outputSegSize;
        auto local = outputRow % outputSegSize;
        auto inputRow = group * subChunkSize + startLocal + local;

        write(output, outputRow * columnCount + c, input[inputRow * columnCount + c]);
    }
};

class Conv1dUnfoldKernel final : public ComputeProgram
{
public:
    Conv1dUnfoldKernel()
    {
        compile();
    }

    void dispatch(ComputePass& pass, int rows, int channels, int kernel)
    {
        rowCount = (std::uint32_t) rows;
        channelCount = (std::uint32_t) channels;
        kernelSize = (std::uint32_t) kernel;
        pass.dispatch(*this, channels * kernel, rows);
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;
    Uniform<UInt> channelCount;
    Uniform<UInt> kernelSize;
    Uniform<UInt> padding;
    Uniform<UInt> rowCount;

    EACP_SHADER(input, output, channelCount, kernelSize, padding, rowCount)

private:
    void define() override
    {
        auto position = threadPosition();
        auto row = position.y;
        auto col = position.x;

        auto channel = col / kernelSize;
        auto k = col % kernelSize;

        auto biased = row + k;
        auto inside = biased >= padding && biased < (rowCount + padding);
        auto clampedBiased = min(max(biased, padding), rowCount + padding - 1u);
        auto sourceRow = clampedBiased - padding;

        auto value = select(inside, input[sourceRow * channelCount + channel], 0.f);
        write(output, row * (channelCount * kernelSize) + col, value);
    }
};
}

Tensor foldWithNewTokensGpu(ComputePass& pass,
                            const Tensor& input,
                            int inputSegSize,
                            int outputSegSize,
                            const Tensor& newTokens,
                            Device& device)
{
    auto subChunkSize = inputSegSize + outputSegSize;
    auto numGroups = input.rows() / inputSegSize;
    auto columns = input.cols();

    auto result = Tensor::uninitializedF32({numGroups * subChunkSize, columns}, device);

    auto& kernel = GPU::sharedKernel<FoldWithNewTokensKernel>(device);
    kernel.input = input;
    kernel.newTokens = newTokens;
    kernel.output = result;
    kernel.inputSegSize = (std::uint32_t) inputSegSize;
    kernel.subChunkSize = (std::uint32_t) subChunkSize;
    kernel.inputRowCount = (std::uint32_t) input.rows();
    kernel.dispatch(pass, numGroups * subChunkSize, columns);

    return result;
}

Tensor unfoldLastSegmentGpu(ComputePass& pass,
                           const Tensor& input,
                           int subChunkSize,
                           int outputSegSize,
                           Device& device)
{
    auto numGroups = input.rows() / subChunkSize;
    auto columns = input.cols();

    auto result = Tensor::uninitializedF32({numGroups * outputSegSize, columns}, device);

    auto& kernel = GPU::sharedKernel<UnfoldLastSegmentKernel>(device);
    kernel.input = input;
    kernel.output = result;
    kernel.subChunkSize = (std::uint32_t) subChunkSize;
    kernel.outputSegSize = (std::uint32_t) outputSegSize;
    kernel.startLocal = (std::uint32_t) (subChunkSize - outputSegSize);
    kernel.dispatch(pass, numGroups * outputSegSize, columns);

    return result;
}

Tensor conv1dUnfoldGpu(ComputePass& pass, const Tensor& input, int inChannels, int kernelSize, Device& device)
{
    auto rows = input.rows();
    auto result = Tensor::uninitializedF32({rows, inChannels * kernelSize}, device);

    auto& kernel = GPU::sharedKernel<Conv1dUnfoldKernel>(device);
    kernel.input = input;
    kernel.output = result;
    kernel.padding = (std::uint32_t) ((kernelSize - 1) / 2);
    kernel.dispatch(pass, rows, inChannels, kernelSize);

    return result;
}
}
