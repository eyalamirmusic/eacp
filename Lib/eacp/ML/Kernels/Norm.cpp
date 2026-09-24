#include "Norm.h"

#include "../../GPU/Codegen/KernelCache.h"
#include "../../GPU/Frame/ComputePass.h"

namespace eacp::ML
{
using namespace eacp::GPU;

namespace
{
// Where run `run` of `dimension` values starts in a view that holds
// runsPerRow of them per row: run * dimension for a whole tensor.
UInt viewStart(const UInt& run,
               const UInt& dimension,
               const UInt& runsPerRow,
               const UInt& rowStride,
               const UInt& columnOffset)
{
    return (run / runsPerRow) * rowStride + columnOffset
           + (run % runsPerRow) * dimension;
}

void readWholeRows(Uniform<UInt>& runsPerRow,
                   Uniform<UInt>& rowStride,
                   Uniform<UInt>& columnOffset,
                   int dim)
{
    runsPerRow = 1u;
    rowStride = (std::uint32_t) dim;
    columnOffset = 0u;
}

void readRunsOf(Uniform<UInt>& runsPerRow,
                Uniform<UInt>& rowStride,
                Uniform<UInt>& columnOffset,
                const TensorView& view,
                int dim)
{
    runsPerRow = (std::uint32_t) (view.cols() / dim);
    rowStride = (std::uint32_t) view.rowStride();
    columnOffset = (std::uint32_t) view.columnOffset();
}
} // namespace

RMSNormKernel::RMSNormKernel()
    : ComputeProgram({normGroupWidth, 1, 1})
{
    compile();
}

void RMSNormKernel::dispatch(ComputePass& pass, int rows, int dim)
{
    dimension = (std::uint32_t) dim;
    pass.dispatch(*this, normGroupWidth, rows);
}

void RMSNormKernel::read(const TensorView& view, int dim)
{
    input = view.buffer();
    readRunsOf(runsPerRow, inputRowStride, inputColumnOffset, view, dim);
}

void RMSNormKernel::define()
{
    auto lane = threadPosition().x;
    auto row = threadPosition().y;
    auto base = row * dimension;
    auto inputBase =
        viewStart(row, dimension, runsPerRow, inputRowStride, inputColumnOffset);

    auto sumOfSquares = var(0.f);
    auto col = var(lane);

    loop(col.get() < dimension,
         [&]
         {
             auto value = input[inputBase + col.get()];
             sumOfSquares = sumOfSquares.get() + value * value;
             col = col.get() + (unsigned) normGroupWidth;
         });

    auto meanSquare = groupSum(sumOfSquares.get()) / toFloat(dimension);
    auto scale = rsqrt(meanSquare + epsilon);

    col = lane;

    loop(col.get() < dimension,
         [&]
         {
             auto value = input[inputBase + col.get()];
             write(output, base + col.get(), value * scale * gamma[col.get()]);
             col = col.get() + (unsigned) normGroupWidth;
         });
}

LayerNormKernel::LayerNormKernel()
    : ComputeProgram({normGroupWidth, 1, 1})
{
    compile();
}

void LayerNormKernel::dispatch(ComputePass& pass, int rows, int dim)
{
    dimension = (std::uint32_t) dim;
    pass.dispatch(*this, normGroupWidth, rows);
}

void LayerNormKernel::define()
{
    auto lane = threadPosition().x;
    auto row = threadPosition().y;
    auto base = row * dimension;

    auto sum = var(0.f);
    auto col = var(lane);

    loop(col.get() < dimension,
        [&]
        {
            sum = sum.get() + input[base + col.get()];
            col = col.get() + (unsigned) normGroupWidth;
        });

    auto mean = groupSum(sum.get()) / toFloat(dimension);

    auto sumSquaredDeviation = var(0.f);
    col = lane;

    loop(col.get() < dimension,
        [&]
        {
            auto centred = input[base + col.get()] - mean;
            sumSquaredDeviation = sumSquaredDeviation.get() + centred * centred;
            col = col.get() + (unsigned) normGroupWidth;
        });

    auto variance = groupSum(sumSquaredDeviation.get()) / toFloat(dimension);
    auto scale = rsqrt(variance + epsilon);

    col = lane;

    loop(col.get() < dimension,
        [&]
        {
            auto centred = input[base + col.get()] - mean;

            write(output,
                 base + col.get(),
                 centred * scale * gamma[col.get()] + beta[col.get()]);

            col = col.get() + (unsigned) normGroupWidth;
        });
}

LayerNormNoBiasKernel::LayerNormNoBiasKernel()
    : ComputeProgram({normGroupWidth, 1, 1})
{
    compile();
}

void LayerNormNoBiasKernel::dispatch(ComputePass& pass, int rows, int dim)
{
    dimension = (std::uint32_t) dim;
    pass.dispatch(*this, normGroupWidth, rows);
}

void LayerNormNoBiasKernel::define()
{
    auto lane = threadPosition().x;
    auto row = threadPosition().y;
    auto base = row * dimension;

    auto sum = var(0.f);
    auto col = var(lane);

    loop(col.get() < dimension,
        [&]
        {
            sum = sum.get() + input[base + col.get()];
            col = col.get() + (unsigned) normGroupWidth;
        });

    auto mean = groupSum(sum.get()) / toFloat(dimension);

    auto sumSquaredDeviation = var(0.f);
    col = lane;

    loop(col.get() < dimension,
        [&]
        {
            auto centred = input[base + col.get()] - mean;
            sumSquaredDeviation = sumSquaredDeviation.get() + centred * centred;
            col = col.get() + (unsigned) normGroupWidth;
        });

    auto variance = groupSum(sumSquaredDeviation.get()) / toFloat(dimension);
    auto scale = rsqrt(variance + epsilon);

    col = lane;

    loop(col.get() < dimension,
        [&]
        {
            auto centred = input[base + col.get()] - mean;
            write(output, base + col.get(), centred * scale * gamma[col.get()]);
            col = col.get() + (unsigned) normGroupWidth;
        });
}

DynamicTanhKernel::DynamicTanhKernel()
{
    compile();
}

void DynamicTanhKernel::dispatch(ComputePass& pass, int rows, int dim)
{
    dimension = (std::uint32_t) dim;
    pass.dispatch(*this, dim, rows);
}

void DynamicTanhKernel::read(const TensorView& view, int dim)
{
    input = view.buffer();
    readRunsOf(runsPerRow, inputRowStride, inputColumnOffset, view, dim);
}

void DynamicTanhKernel::define()
{
    auto position = threadPosition();
    auto index = position.y * dimension + position.x;
    auto inputIndex =
        viewStart(
            position.y, dimension, runsPerRow, inputRowStride, inputColumnOffset)
        + position.x;

    auto value = saturatingTanh(alpha * input[inputIndex]) * gamma[position.x]
                 + beta[position.x];
    write(output, index, value);
}

Tensor rmsNorm(ComputePass& pass,
              const Tensor& input,
              const Tensor& gamma,
              float epsilon,
              Device& device)
{
    auto rows = input.rows();
    auto dim = input.cols();
    auto result = Tensor::uninitializedF32({rows, dim}, device);

    auto& kernel = sharedKernel<RMSNormKernel>(device);
    kernel.input = input.buffer();
    kernel.gamma = gamma.buffer();
    kernel.output = result.buffer();
    kernel.epsilon = epsilon;
    readWholeRows(
        kernel.runsPerRow, kernel.inputRowStride, kernel.inputColumnOffset, dim);
    kernel.dispatch(pass, rows, dim);

    return result;
}

Tensor layerNorm(ComputePass& pass,
                 const Tensor& input,
                 const Tensor& gamma,
                 const Tensor* beta,
                 float epsilon,
                 Device& device)
{
    auto rows = input.rows();
    auto dim = input.cols();
    auto result = Tensor::uninitializedF32({rows, dim}, device);

    if (beta != nullptr)
    {
        auto& kernel = sharedKernel<LayerNormKernel>(device);
        kernel.input = input.buffer();
        kernel.gamma = gamma.buffer();
        kernel.beta = beta->buffer();
        kernel.output = result.buffer();
        kernel.epsilon = epsilon;
        kernel.dispatch(pass, rows, dim);
    }
    else
    {
        auto& kernel = sharedKernel<LayerNormNoBiasKernel>(device);
        kernel.input = input.buffer();
        kernel.gamma = gamma.buffer();
        kernel.output = result.buffer();
        kernel.epsilon = epsilon;
        kernel.dispatch(pass, rows, dim);
    }

    return result;
}

Tensor dynamicTanh(ComputePass& pass,
                   const Tensor& input,
                   const Tensor& gamma,
                   const Tensor& beta,
                   float alpha,
                   Device& device)
{
    auto rows = input.rows();
    auto dim = input.cols();
    auto result = Tensor::uninitializedF32({rows, dim}, device);

    auto& kernel = sharedKernel<DynamicTanhKernel>(device);
    kernel.input = input.buffer();
    kernel.gamma = gamma.buffer();
    kernel.beta = beta.buffer();
    kernel.output = result.buffer();
    kernel.alpha = alpha;
    readWholeRows(
        kernel.runsPerRow, kernel.inputRowStride, kernel.inputColumnOffset, dim);
    kernel.dispatch(pass, rows, dim);

    return result;
}

Tensor rmsNormPerHead(ComputePass& pass,
                      const TensorView& input,
                      const Tensor& gamma,
                      int headDim,
                      float epsilon,
                      Device& device)
{
    auto result = Tensor::uninitializedF32({input.rows(), input.cols()}, device);

    auto& kernel = sharedKernel<RMSNormKernel>(device);
    kernel.read(input, headDim);
    kernel.gamma = gamma.buffer();
    kernel.output = result.buffer();
    kernel.epsilon = epsilon;
    kernel.dispatch(pass, input.count() / headDim, headDim);

    return result;
}

Tensor dynamicTanhPerHead(ComputePass& pass,
                          const TensorView& input,
                          const Tensor& gamma,
                          const Tensor& beta,
                          float alpha,
                          int headDim,
                          Device& device)
{
    auto result = Tensor::uninitializedF32({input.rows(), input.cols()}, device);

    auto& kernel = sharedKernel<DynamicTanhKernel>(device);
    kernel.read(input, headDim);
    kernel.gamma = gamma.buffer();
    kernel.beta = beta.buffer();
    kernel.output = result.buffer();
    kernel.alpha = alpha;
    kernel.dispatch(pass, input.count() / headDim, headDim);

    return result;
}
}
