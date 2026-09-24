#include "SoftNormBottleneck.h"

#include <eacp/GPU/Codegen/KernelCache.h>
#include <eacp/GPU/Frame/ComputePass.h>

namespace eacp::SA3Codec
{
using namespace eacp::GPU;
using namespace eacp::ML;

namespace
{
class SoftNormEncodeKernel final : public ComputeProgram
{
public:
    SoftNormEncodeKernel()
    {
        compile();
    }

    void dispatch(ComputePass& pass, int rows, int columns)
    {
        columnCount = (std::uint32_t) columns;
        pass.dispatch(*this, columns, rows);
    }

    Uniform<InputBuffer> input;
    Uniform<InputBuffer> scalingFactor;
    Uniform<InputBuffer> bias;
    Uniform<OutputBuffer> output;
    Uniform<UInt> columnCount;
    Uniform<Float> runningStd;

    EACP_SHADER(input, scalingFactor, bias, output, columnCount, runningStd)

private:
    void define() override
    {
        auto position = threadPosition();
        auto index = position.y * columnCount + position.x;

        auto value =
            (input[index] * scalingFactor[position.x] + bias[position.x]) / runningStd;

        write(output, index, value);
    }
};

class SoftNormDecodeKernel final : public ComputeProgram
{
public:
    SoftNormDecodeKernel()
    {
        compile();
    }

    void dispatch(ComputePass& pass, int count)
    {
        pass.dispatch(*this, count);
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;
    Uniform<Float> runningStd;

    EACP_SHADER(input, output, runningStd)

private:
    void define() override
    {
        auto i = threadId();
        write(output, i, input[i] * runningStd);
    }
};
}

Tensor softNormBottleneckEncode(ComputePass& pass,
                                const Tensor& input,
                                const SoftNormBottleneckWeights& weights,
                                Device& device)
{
    auto result = Tensor::uninitializedF32(input.shape(), device);

    auto& kernel = GPU::sharedKernel<SoftNormEncodeKernel>(device);
    kernel.input = input;
    kernel.scalingFactor = weights.scalingFactor;
    kernel.bias = weights.bias;
    kernel.output = result;
    kernel.runningStd = weights.runningStd;
    kernel.dispatch(pass, input.rows(), input.cols());

    return result;
}

Tensor softNormBottleneckDecode(ComputePass& pass,
                                const Tensor& input,
                                const SoftNormBottleneckWeights& weights,
                                Device& device)
{
    auto result = Tensor::uninitializedF32(input.shape(), device);

    auto& kernel = GPU::sharedKernel<SoftNormDecodeKernel>(device);
    kernel.input = input;
    kernel.output = result;
    kernel.runningStd = weights.runningStd;
    kernel.dispatch(pass, input.count());

    return result;
}

void forEachBottleneckShaderGraph(const GPU::ShaderGraphVisitor& visit)
{
    visit(SoftNormEncodeKernel {}.graph());
    visit(SoftNormDecodeKernel {}.graph());
}
}
