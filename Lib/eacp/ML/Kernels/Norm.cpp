#include "Norm.h"

#include "../../GPU/Frame/ComputePass.h"

namespace eacp::ML
{
using namespace eacp::GPU;

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

void RMSNormKernel::define()
{
    auto lane = threadPosition().x;
    auto row = threadPosition().y;
    auto base = row * dimension;

    auto sumOfSquares = var(0.f);
    auto col = var(lane);

    loop(col.get() < dimension,
        [&]
        {
            auto value = input[base + col.get()];
            sumOfSquares = sumOfSquares.get() + value * value;
            col = col.get() + (unsigned) normGroupWidth;
        });

    auto meanSquare = groupSum(sumOfSquares.get()) / toFloat(dimension);
    auto scale = rsqrt(meanSquare + epsilon);

    col = lane;

    loop(col.get() < dimension,
        [&]
        {
            auto value = input[base + col.get()];
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

void DynamicTanhKernel::define()
{
    auto position = threadPosition();
    auto index = position.y * dimension + position.x;

    auto value = tanh(alpha * input[index]) * gamma[position.x] + beta[position.x];
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

    auto kernel = RMSNormKernel {};
    kernel.input = input.buffer();
    kernel.gamma = gamma.buffer();
    kernel.output = result.buffer();
    kernel.epsilon = epsilon;
    kernel.prepare(device);
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
        auto kernel = LayerNormKernel {};
        kernel.input = input.buffer();
        kernel.gamma = gamma.buffer();
        kernel.beta = beta->buffer();
        kernel.output = result.buffer();
        kernel.epsilon = epsilon;
        kernel.prepare(device);
        kernel.dispatch(pass, rows, dim);
    }
    else
    {
        auto kernel = LayerNormNoBiasKernel {};
        kernel.input = input.buffer();
        kernel.gamma = gamma.buffer();
        kernel.output = result.buffer();
        kernel.epsilon = epsilon;
        kernel.prepare(device);
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

    auto kernel = DynamicTanhKernel {};
    kernel.input = input.buffer();
    kernel.gamma = gamma.buffer();
    kernel.beta = beta.buffer();
    kernel.output = result.buffer();
    kernel.alpha = alpha;
    kernel.prepare(device);
    kernel.dispatch(pass, rows, dim);

    return result;
}
}
