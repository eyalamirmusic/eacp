#include "GpuOps.h"

#include <eacp/GPU/Codegen/KernelCache.h>
#include <eacp/GPU/Frame/ComputePass.h>

namespace eacp::SA3Codec
{
using namespace eacp::GPU;
using namespace eacp::ML;

namespace
{
class SliceRowsKernel final : public ComputeProgram
{
public:
    SliceRowsKernel()
    {
        compile();
    }

    void dispatch(ComputePass& pass, int rows, int columns)
    {
        columnCount = (std::uint32_t) columns;
        pass.dispatch(*this, columns, rows);
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;
    Uniform<UInt> startRow;
    Uniform<UInt> columnCount;

    EACP_SHADER(input, output, startRow, columnCount)

private:
    void define() override
    {
        auto position = threadPosition();
        auto outIndex = position.y * columnCount + position.x;
        auto inIndex = (startRow + position.y) * columnCount + position.x;
        write(output, outIndex, input[inIndex]);
    }
};

class SliceColumnsKernel final : public ComputeProgram
{
public:
    SliceColumnsKernel()
    {
        compile();
    }

    void dispatch(ComputePass& pass, int rows, int outColumns)
    {
        outputColumnCount = (std::uint32_t) outColumns;
        pass.dispatch(*this, outColumns, rows);
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;
    Uniform<UInt> inputColumnCount;
    Uniform<UInt> startColumn;
    Uniform<UInt> outputColumnCount;

    EACP_SHADER(input, output, inputColumnCount, startColumn, outputColumnCount)

private:
    void define() override
    {
        auto position = threadPosition();
        auto outIndex = position.y * outputColumnCount + position.x;
        auto inIndex = position.y * inputColumnCount + startColumn + position.x;
        write(output, outIndex, input[inIndex]);
    }
};

class WriteRowsIntoKernel final : public ComputeProgram
{
public:
    WriteRowsIntoKernel()
    {
        compile();
    }

    void dispatch(ComputePass& pass, int rows, int columns)
    {
        columnCount = (std::uint32_t) columns;
        pass.dispatch(*this, columns, rows);
    }

    Uniform<InputBuffer> source;
    Uniform<OutputBuffer> destination;
    Uniform<UInt> destinationRowOffset;
    Uniform<UInt> columnCount;

    EACP_SHADER(source, destination, destinationRowOffset, columnCount)

private:
    void define() override
    {
        auto position = threadPosition();
        auto srcIndex = position.y * columnCount + position.x;
        auto dstIndex = (destinationRowOffset + position.y) * columnCount + position.x;
        write(destination, dstIndex, source[srcIndex]);
    }
};

class FillZeroKernel final : public ComputeProgram
{
public:
    FillZeroKernel()
    {
        compile();
    }

    void dispatch(ComputePass& pass, int count)
    {
        pass.dispatch(*this, count);
    }

    Uniform<OutputBuffer> output;

    EACP_SHADER(output)

private:
    void define() override
    {
        write(output, threadId(), var(0.f).get());
    }
};

class ElementwiseAddKernel final : public ComputeProgram
{
public:
    ElementwiseAddKernel()
    {
        compile();
    }

    void dispatch(ComputePass& pass, int count)
    {
        pass.dispatch(*this, count);
    }

    Uniform<InputBuffer> a;
    Uniform<InputBuffer> b;
    Uniform<OutputBuffer> output;

    EACP_SHADER(a, b, output)

private:
    void define() override
    {
        auto i = threadId();
        write(output, i, a[i] + b[i]);
    }
};

class ElementwiseSubtractKernel final : public ComputeProgram
{
public:
    ElementwiseSubtractKernel()
    {
        compile();
    }

    void dispatch(ComputePass& pass, int count)
    {
        pass.dispatch(*this, count);
    }

    Uniform<InputBuffer> a;
    Uniform<InputBuffer> b;
    Uniform<OutputBuffer> output;

    EACP_SHADER(a, b, output)

private:
    void define() override
    {
        auto i = threadId();
        write(output, i, a[i] - b[i]);
    }
};

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

Tensor reshapeFlat(Tensor tensor, std::vector<int> newShape)
{
    auto dtype = tensor.dtype();
    return Tensor {std::move(tensor.buffer()), std::move(newShape), dtype};
}

Tensor sliceRowsGpu(ComputePass& pass, const Tensor& input, int startRow, int rowCount, Device& device)
{
    auto columns = input.cols();
    auto result = Tensor::uninitializedF32({rowCount, columns}, device);

    auto& kernel = GPU::sharedKernel<SliceRowsKernel>(device);
    kernel.input = input.buffer();
    kernel.output = result.buffer();
    kernel.startRow = (std::uint32_t) startRow;
    kernel.dispatch(pass, rowCount, columns);

    return result;
}

Tensor sliceColumnsGpu(ComputePass& pass,
                      const Tensor& input,
                      int startColumn,
                      int columnCount,
                      Device& device)
{
    auto rows = input.rows();
    auto result = Tensor::uninitializedF32({rows, columnCount}, device);

    auto& kernel = GPU::sharedKernel<SliceColumnsKernel>(device);
    kernel.input = input.buffer();
    kernel.output = result.buffer();
    kernel.inputColumnCount = (std::uint32_t) input.cols();
    kernel.startColumn = (std::uint32_t) startColumn;
    kernel.dispatch(pass, rows, columnCount);

    return result;
}

void writeRowsIntoGpu(ComputePass& pass,
                      const Tensor& destination,
                      int destinationRowOffset,
                      const Tensor& source,
                      Device& device)
{
    auto& kernel = GPU::sharedKernel<WriteRowsIntoKernel>(device);
    kernel.source = source.buffer();
    kernel.destination = destination.buffer();
    kernel.destinationRowOffset = (std::uint32_t) destinationRowOffset;
    kernel.dispatch(pass, source.rows(), source.cols());
}

Tensor zerosGpu(ComputePass& pass, int rows, int columns, Device& device)
{
    auto result = Tensor::uninitializedF32({rows, columns}, device);

    auto& kernel = GPU::sharedKernel<FillZeroKernel>(device);
    kernel.output = result.buffer();
    kernel.dispatch(pass, rows * columns);

    return result;
}

Tensor zeroPadRowsGpu(ComputePass& pass, const Tensor& input, int multiple, Device& device)
{
    auto rows = input.rows();
    auto remainder = rows % multiple;
    auto padRows = remainder == 0 ? 0 : multiple - remainder;

    auto result = zerosGpu(pass, rows + padRows, input.cols(), device);
    writeRowsIntoGpu(pass, result, 0, input, device);

    return result;
}

Tensor concatRowsGpu(ComputePass& pass, const Tensor& a, const Tensor& b, const Tensor& c, Device& device)
{
    auto totalRows = a.rows() + b.rows() + c.rows();
    auto result = Tensor::uninitializedF32({totalRows, a.cols()}, device);

    writeRowsIntoGpu(pass, result, 0, a, device);
    writeRowsIntoGpu(pass, result, a.rows(), b, device);
    writeRowsIntoGpu(pass, result, a.rows() + b.rows(), c, device);

    return result;
}

Tensor addTensorsGpu(ComputePass& pass, const Tensor& a, const Tensor& b, Device& device)
{
    auto result = Tensor::uninitializedF32(a.shape(), device);

    auto& kernel = GPU::sharedKernel<ElementwiseAddKernel>(device);
    kernel.a = a.buffer();
    kernel.b = b.buffer();
    kernel.output = result.buffer();
    kernel.dispatch(pass, a.count());

    return result;
}

Tensor subtractTensorsGpu(ComputePass& pass, const Tensor& a, const Tensor& b, Device& device)
{
    auto result = Tensor::uninitializedF32(a.shape(), device);

    auto& kernel = GPU::sharedKernel<ElementwiseSubtractKernel>(device);
    kernel.a = a.buffer();
    kernel.b = b.buffer();
    kernel.output = result.buffer();
    kernel.dispatch(pass, a.count());

    return result;
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
    kernel.input = input.buffer();
    kernel.newTokens = newTokens.buffer();
    kernel.output = result.buffer();
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
    kernel.input = input.buffer();
    kernel.output = result.buffer();
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
    kernel.input = input.buffer();
    kernel.output = result.buffer();
    kernel.padding = (std::uint32_t) ((kernelSize - 1) / 2);
    kernel.dispatch(pass, rows, inChannels, kernelSize);

    return result;
}

void addGpuOpsWarmupKernels(KernelWarmup& warmup)
{
    warmup.add<SliceRowsKernel>();
    warmup.add<SliceColumnsKernel>();
    warmup.add<WriteRowsIntoKernel>();
    warmup.add<FillZeroKernel>();
    warmup.add<ElementwiseAddKernel>();
    warmup.add<ElementwiseSubtractKernel>();
    warmup.add<FoldWithNewTokensKernel>();
    warmup.add<UnfoldLastSegmentKernel>();
    warmup.add<Conv1dUnfoldKernel>();
}
}
