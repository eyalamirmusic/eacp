#include "Ops.h"

#include <eacp/GPU/Codegen/KernelCache.h>
#include <eacp/GPU/Frame/ComputePass.h>
#include <eacp/ML/Kernels/TensorOps.h>

#include <algorithm>
#include <cmath>

namespace eacp::SA3DiT
{
using namespace eacp::GPU;
using namespace eacp::ML;

namespace
{
Float sigmoidOf(const Float& x)
{
    return 1.f / (1.f + exp(-x));
}

constexpr auto twoPi = 6.283185307179586f;
}

AddBroadcastRowKernel::AddBroadcastRowKernel()
{
    compile();
}

void AddBroadcastRowKernel::dispatch(ComputePass& pass,
                                     int rowStartToUse,
                                     int rowCount,
                                     int columns)
{
    columnCount = (std::uint32_t) columns;
    rowStart = (std::uint32_t) rowStartToUse;
    pass.dispatch(*this, columns, rowCount);
}

void AddBroadcastRowKernel::define()
{
    auto position = threadPosition();
    auto row = rowStart + position.y;
    auto index = row * columnCount + position.x;

    write(values, index, values[index] + addend[position.x]);
}

AdaLNModulateKernel::AdaLNModulateKernel()
{
    compile();
}

void AdaLNModulateKernel::dispatch(ComputePass& pass, int rows, int columns)
{
    columnCount = (std::uint32_t) columns;
    pass.dispatch(*this, columns, rows);
}

void AdaLNModulateKernel::define()
{
    auto position = threadPosition();
    auto index = position.y * columnCount + position.x;

    write(output,
         index,
         input[index] * (1.f + scale[position.x]) + shift[position.x]);
}

SigmoidGateKernel::SigmoidGateKernel()
{
    compile();
}

void SigmoidGateKernel::dispatch(ComputePass& pass, int rows, int columns)
{
    columnCount = (std::uint32_t) columns;
    pass.dispatch(*this, columns, rows);
}

void SigmoidGateKernel::define()
{
    auto position = threadPosition();
    auto index = position.y * columnCount + position.x;

    write(output, index, input[index] * sigmoidOf(1.f - gate[position.x]));
}

ExpoFourierFeaturesKernel::ExpoFourierFeaturesKernel()
{
    compile();
}

void ExpoFourierFeaturesKernel::dispatch(ComputePass& pass, int halfDimToUse)
{
    halfDim = (std::uint32_t) halfDimToUse;
    rampDenominator = (float) std::max(halfDimToUse - 1, 1);
    pass.dispatch(*this, halfDimToUse);
}

void ExpoFourierFeaturesKernel::define()
{
    auto i = threadId();
    auto ramp = toFloat(i) / rampDenominator;
    auto freq = exp(ramp * (logMaxFreq - logMinFreq) + logMinFreq);
    auto arg = value * freq * twoPi;

    write(output, i, cos(arg));
    write(output, halfDim + i, sin(arg));
}

Tensor expoFourierFeatures(ComputePass& pass,
                           float value,
                           int dim,
                           float minFreq,
                           float maxFreq,
                           Device& device)
{
    auto result = Tensor::uninitializedF32({1, dim}, device);

    auto& kernel = GPU::sharedKernel<ExpoFourierFeaturesKernel>(device);
    kernel.output = result.buffer();
    kernel.value = value;
    kernel.logMinFreq = std::log(minFreq);
    kernel.logMaxFreq = std::log(maxFreq);
    kernel.dispatch(pass, dim / 2);

    return result;
}

Tensor addBroadcastRow(ComputePass& pass,
                       const Tensor& values,
                       const Tensor& addend,
                       int rowStart,
                       Device& device)
{
    auto result = Tensor::uninitializedF32(values.shape(), device);

    copyRowsInto(pass, result, 0, values, device);

    auto& addKernel = GPU::sharedKernel<AddBroadcastRowKernel>(device);
    addKernel.values = result.buffer();
    addKernel.addend = addend.buffer();
    addKernel.dispatch(pass, rowStart, values.rows() - rowStart, values.cols());

    return result;
}

Tensor adaLNModulate(ComputePass& pass,
                     const Tensor& input,
                     const Tensor& scale,
                     const Tensor& shift,
                     Device& device)
{
    return adaLNModulate(pass,
                         input,
                         BufferRange::of(scale.buffer()),
                         BufferRange::of(shift.buffer()),
                         device);
}

Tensor adaLNModulate(ComputePass& pass,
                     const Tensor& input,
                     const BufferRange& scale,
                     const BufferRange& shift,
                     Device& device)
{
    auto result = Tensor::uninitializedF32(input.shape(), device);

    auto& kernel = GPU::sharedKernel<AdaLNModulateKernel>(device);
    kernel.input = input.buffer();
    kernel.scale = scale;
    kernel.shift = shift;
    kernel.output = result.buffer();
    kernel.dispatch(pass, input.rows(), input.cols());

    return result;
}

Tensor sigmoidGate(ComputePass& pass,
                   const Tensor& input,
                   const Tensor& gate,
                   Device& device)
{
    return sigmoidGate(pass, input, BufferRange::of(gate.buffer()), device);
}

Tensor sigmoidGate(ComputePass& pass,
                   const Tensor& input,
                   const BufferRange& gate,
                   Device& device)
{
    auto result = Tensor::uninitializedF32(input.shape(), device);

    auto& kernel = GPU::sharedKernel<SigmoidGateKernel>(device);
    kernel.input = input.buffer();
    kernel.gate = gate;
    kernel.output = result.buffer();
    kernel.dispatch(pass, input.rows(), input.cols());

    return result;
}

Tensor squeezeTrailingUnitDim(Tensor input)
{
    auto shape = input.shape();

    if (shape.size() > 2 && shape.back() == 1)
    {
        shape.pop_back();
        return reshape(std::move(input), shape);
    }

    return input;
}
}
