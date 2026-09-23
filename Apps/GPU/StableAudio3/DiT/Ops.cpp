#include "Ops.h"

#include <eacp/GPU/Frame/ComputePass.h>

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

AddTensorsKernel::AddTensorsKernel()
{
    compile();
}

void AddTensorsKernel::dispatch(ComputePass& pass, int count)
{
    pass.dispatch(*this, count);
}

void AddTensorsKernel::define()
{
    auto i = threadId();
    write(output, i, a[i] + b[i]);
}

SubtractTensorsKernel::SubtractTensorsKernel()
{
    compile();
}

void SubtractTensorsKernel::dispatch(ComputePass& pass, int count)
{
    pass.dispatch(*this, count);
}

void SubtractTensorsKernel::define()
{
    auto i = threadId();
    write(output, i, a[i] - b[i]);
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

CopyRowsKernel::CopyRowsKernel()
{
    compile();
}

void CopyRowsKernel::dispatch(ComputePass& pass, int rowCount, int columns)
{
    columnCount = (std::uint32_t) columns;
    pass.dispatch(*this, columns, rowCount);
}

void CopyRowsKernel::define()
{
    auto position = threadPosition();
    auto sourceIndex = (sourceRowStart + position.y) * columnCount + position.x;
    auto destinationIndex =
        (destinationRowStart + position.y) * columnCount + position.x;

    write(destination, destinationIndex, source[sourceIndex]);
}

SliceColumnsKernel::SliceColumnsKernel()
{
    compile();
}

void SliceColumnsKernel::dispatch(ComputePass& pass, int rows, int sliceWidth)
{
    destinationColumnCount = (std::uint32_t) sliceWidth;
    pass.dispatch(*this, sliceWidth, rows);
}

void SliceColumnsKernel::define()
{
    auto position = threadPosition();
    auto sourceIndex = position.y * sourceColumnCount + columnStart + position.x;
    auto destinationIndex = position.y * destinationColumnCount + position.x;

    write(destination, destinationIndex, source[sourceIndex]);
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

    auto kernel = ExpoFourierFeaturesKernel {};
    kernel.output = result.buffer();
    kernel.value = value;
    kernel.logMinFreq = std::log(minFreq);
    kernel.logMaxFreq = std::log(maxFreq);
    kernel.prepare(device);
    kernel.dispatch(pass, dim / 2);

    return result;
}

Tensor addTensors(ComputePass& pass, const Tensor& a, const Tensor& b, Device& device)
{
    auto result = Tensor::uninitializedF32(a.shape(), device);

    auto kernel = AddTensorsKernel {};
    kernel.a = a.buffer();
    kernel.b = b.buffer();
    kernel.output = result.buffer();
    kernel.prepare(device);
    kernel.dispatch(pass, a.count());

    return result;
}

Tensor subtractTensors(ComputePass& pass, const Tensor& a, const Tensor& b, Device& device)
{
    auto result = Tensor::uninitializedF32(a.shape(), device);

    auto kernel = SubtractTensorsKernel {};
    kernel.a = a.buffer();
    kernel.b = b.buffer();
    kernel.output = result.buffer();
    kernel.prepare(device);
    kernel.dispatch(pass, a.count());

    return result;
}

Tensor addBroadcastRow(ComputePass& pass,
                       const Tensor& values,
                       const Tensor& addend,
                       int rowStart,
                       Device& device)
{
    auto result = Tensor::uninitializedF32(values.shape(), device);

    auto copyKernel = CopyRowsKernel {};
    copyKernel.source = values.buffer();
    copyKernel.destination = result.buffer();
    copyKernel.sourceRowStart = 0u;
    copyKernel.destinationRowStart = 0u;
    copyKernel.prepare(device);
    copyKernel.dispatch(pass, values.rows(), values.cols());

    auto addKernel = AddBroadcastRowKernel {};
    addKernel.values = result.buffer();
    addKernel.addend = addend.buffer();
    addKernel.prepare(device);
    addKernel.dispatch(pass, rowStart, values.rows() - rowStart, values.cols());

    return result;
}

Tensor adaLNModulate(ComputePass& pass,
                     const Tensor& input,
                     const Tensor& scale,
                     const Tensor& shift,
                     Device& device)
{
    auto result = Tensor::uninitializedF32(input.shape(), device);

    auto kernel = AdaLNModulateKernel {};
    kernel.input = input.buffer();
    kernel.scale = scale.buffer();
    kernel.shift = shift.buffer();
    kernel.output = result.buffer();
    kernel.prepare(device);
    kernel.dispatch(pass, input.rows(), input.cols());

    return result;
}

Tensor sigmoidGate(ComputePass& pass,
                   const Tensor& input,
                   const Tensor& gate,
                   Device& device)
{
    auto result = Tensor::uninitializedF32(input.shape(), device);

    auto kernel = SigmoidGateKernel {};
    kernel.input = input.buffer();
    kernel.gate = gate.buffer();
    kernel.output = result.buffer();
    kernel.prepare(device);
    kernel.dispatch(pass, input.rows(), input.cols());

    return result;
}

Tensor concatRows(ComputePass& pass, const Tensor& top, const Tensor& bottom, Device& device)
{
    auto columns = top.cols();
    auto totalRows = top.rows() + bottom.rows();
    auto result = Tensor::uninitializedF32({totalRows, columns}, device);

    auto topKernel = CopyRowsKernel {};
    topKernel.source = top.buffer();
    topKernel.destination = result.buffer();
    topKernel.sourceRowStart = 0u;
    topKernel.destinationRowStart = 0u;
    topKernel.prepare(device);
    topKernel.dispatch(pass, top.rows(), columns);

    auto bottomKernel = CopyRowsKernel {};
    bottomKernel.source = bottom.buffer();
    bottomKernel.destination = result.buffer();
    bottomKernel.sourceRowStart = 0u;
    bottomKernel.destinationRowStart = (std::uint32_t) top.rows();
    bottomKernel.prepare(device);
    bottomKernel.dispatch(pass, bottom.rows(), columns);

    return result;
}

Tensor sliceRows(ComputePass& pass,
                 const Tensor& input,
                 int rowStart,
                 int rowCount,
                 Device& device)
{
    auto result = Tensor::uninitializedF32({rowCount, input.cols()}, device);

    auto kernel = CopyRowsKernel {};
    kernel.source = input.buffer();
    kernel.destination = result.buffer();
    kernel.sourceRowStart = (std::uint32_t) rowStart;
    kernel.destinationRowStart = 0u;
    kernel.prepare(device);
    kernel.dispatch(pass, rowCount, input.cols());

    return result;
}

Tensor sliceColumns(ComputePass& pass,
                    const Tensor& input,
                    int columnStart,
                    int sliceWidth,
                    Device& device)
{
    auto result = Tensor::uninitializedF32({input.rows(), sliceWidth}, device);

    auto kernel = SliceColumnsKernel {};
    kernel.source = input.buffer();
    kernel.destination = result.buffer();
    kernel.sourceColumnCount = (std::uint32_t) input.cols();
    kernel.columnStart = (std::uint32_t) columnStart;
    kernel.prepare(device);
    kernel.dispatch(pass, input.rows(), sliceWidth);

    return result;
}

Tensor squeezeTrailingUnitDim(Tensor input)
{
    auto shape = input.shape();

    if (shape.size() > 2 && shape.back() == 1)
    {
        shape.pop_back();
        return Tensor(std::move(input.buffer()), shape, input.dtype());
    }

    return input;
}

Tensor reshapeFlat(Tensor input, std::vector<int> newShape)
{
    return Tensor(std::move(input.buffer()), std::move(newShape), input.dtype());
}
}
