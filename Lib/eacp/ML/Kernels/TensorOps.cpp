#include "TensorOps.h"

#include "../../GPU/Codegen/KernelCache.h"
#include "../../GPU/Frame/ComputePass.h"

namespace eacp::ML
{
using namespace eacp::GPU;

ElementwiseKernel::ElementwiseKernel(ElementwiseOp opToUse)
    : op(opToUse)
{
    compile();
}

void ElementwiseKernel::dispatch(ComputePass& pass, int count)
{
    pass.dispatch(*this, count);
}

std::string ElementwiseKernel::name() const
{
    switch (op)
    {
        case ElementwiseOp::Add:
            return "AddKernel";
        case ElementwiseOp::Subtract:
            return "SubtractKernel";
        case ElementwiseOp::Multiply:
            return "MultiplyKernel";
    }

    return "ElementwiseKernel";
}

void ElementwiseKernel::define()
{
    auto i = threadId();

    switch (op)
    {
        case ElementwiseOp::Add:
            write(output, i, a[i] + b[i]);
            break;

        case ElementwiseOp::Subtract:
            write(output, i, a[i] - b[i]);
            break;

        case ElementwiseOp::Multiply:
            write(output, i, a[i] * b[i]);
            break;
    }
}

ScaleAndAddKernel::ScaleAndAddKernel()
{
    compile();
}

void ScaleAndAddKernel::dispatch(ComputePass& pass, int count)
{
    pass.dispatch(*this, count);
}

void ScaleAndAddKernel::define()
{
    auto i = threadId();
    write(output, i, a[i] * scaleA + b[i] * scaleB);
}

FillKernel::FillKernel()
{
    compile();
}

void FillKernel::dispatch(ComputePass& pass, int count)
{
    pass.dispatch(*this, count);
}

void FillKernel::define()
{
    write(output, threadId(), value);
}

CopyRowsKernel::CopyRowsKernel()
{
    compile();
}

void CopyRowsKernel::dispatch(ComputePass& pass, int rows, int columns)
{
    columnCount = (std::uint32_t) columns;
    pass.dispatch(*this, columns, rows);
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

void SliceColumnsKernel::dispatch(ComputePass& pass, int rows, int columns)
{
    destinationColumnCount = (std::uint32_t) columns;
    pass.dispatch(*this, columns, rows);
}

void SliceColumnsKernel::define()
{
    auto position = threadPosition();
    auto sourceIndex = position.y * sourceColumnCount + columnStart + position.x;
    auto destinationIndex = position.y * destinationColumnCount + position.x;

    write(destination, destinationIndex, source[sourceIndex]);
}

namespace
{
Tensor elementwise(ComputePass& pass,
                   ElementwiseOp op,
                   const Tensor& a,
                   const Tensor& b,
                   Device& device)
{
    auto result = Tensor::uninitializedF32(a.shape(), device);

    auto& kernel = sharedKernel<ElementwiseKernel>(device, op);
    kernel.a = a.buffer();
    kernel.b = b.buffer();
    kernel.output = result.buffer();
    kernel.dispatch(pass, a.count());

    return result;
}

void copyRows(ComputePass& pass,
              const Tensor& source,
              int sourceRow,
              const Tensor& destination,
              int destinationRow,
              int rowCount,
              Device& device)
{
    auto& kernel = sharedKernel<CopyRowsKernel>(device);
    kernel.source = source.buffer();
    kernel.destination = destination.buffer();
    kernel.sourceRowStart = (std::uint32_t) sourceRow;
    kernel.destinationRowStart = (std::uint32_t) destinationRow;
    kernel.dispatch(pass, rowCount, source.cols());
}
} // namespace

Tensor add(ComputePass& pass, const Tensor& a, const Tensor& b, Device& device)
{
    return elementwise(pass, ElementwiseOp::Add, a, b, device);
}

Tensor subtract(ComputePass& pass, const Tensor& a, const Tensor& b, Device& device)
{
    return elementwise(pass, ElementwiseOp::Subtract, a, b, device);
}

Tensor multiply(ComputePass& pass, const Tensor& a, const Tensor& b, Device& device)
{
    return elementwise(pass, ElementwiseOp::Multiply, a, b, device);
}

Tensor scaleAndAdd(ComputePass& pass,
                   const Tensor& a,
                   float scaleA,
                   const Tensor& b,
                   float scaleB,
                   Device& device)
{
    auto result = Tensor::uninitializedF32(a.shape(), device);

    auto& kernel = sharedKernel<ScaleAndAddKernel>(device);
    kernel.a = a.buffer();
    kernel.b = b.buffer();
    kernel.output = result.buffer();
    kernel.scaleA = scaleA;
    kernel.scaleB = scaleB;
    kernel.dispatch(pass, a.count());

    return result;
}

Tensor fill(ComputePass& pass, std::vector<int> shape, float value, Device& device)
{
    auto result = Tensor::uninitializedF32(std::move(shape), device);

    auto& kernel = sharedKernel<FillKernel>(device);
    kernel.output = result.buffer();
    kernel.value = value;
    kernel.dispatch(pass, result.count());

    return result;
}

Tensor zeros(ComputePass& pass, std::vector<int> shape, Device& device)
{
    return fill(pass, std::move(shape), 0.f, device);
}

Tensor reshape(Tensor tensor, std::vector<int> shape)
{
    auto dtype = tensor.dtype();
    return Tensor {std::move(tensor.buffer()), std::move(shape), dtype};
}

Tensor sliceRows(ComputePass& pass,
                 const Tensor& input,
                 int firstRow,
                 int rowCount,
                 Device& device)
{
    auto result = Tensor::uninitializedF32({rowCount, input.cols()}, device);
    copyRows(pass, input, firstRow, result, 0, rowCount, device);
    return result;
}

Tensor sliceColumns(ComputePass& pass,
                    const Tensor& input,
                    int firstColumn,
                    int columnCount,
                    Device& device)
{
    auto result = Tensor::uninitializedF32({input.rows(), columnCount}, device);

    auto& kernel = sharedKernel<SliceColumnsKernel>(device);
    kernel.source = input.buffer();
    kernel.destination = result.buffer();
    kernel.sourceColumnCount = (std::uint32_t) input.cols();
    kernel.columnStart = (std::uint32_t) firstColumn;
    kernel.dispatch(pass, input.rows(), columnCount);

    return result;
}

Tensor concatRows(ComputePass& pass,
                  std::initializer_list<std::reference_wrapper<const Tensor>> parts,
                  Device& device)
{
    auto totalRows = 0;

    for (const Tensor& part: parts)
        totalRows += part.rows();

    const Tensor& first = *parts.begin();
    auto result = Tensor::uninitializedF32({totalRows, first.cols()}, device);
    auto row = 0;

    for (const Tensor& part: parts)
    {
        copyRowsInto(pass, result, row, part, device);
        row += part.rows();
    }

    return result;
}

void copyRowsInto(ComputePass& pass,
                  const Tensor& destination,
                  int firstRow,
                  const Tensor& source,
                  Device& device)
{
    copyRows(pass, source, 0, destination, firstRow, source.rows(), device);
}

Tensor padRowsWithZeros(ComputePass& pass,
                        const Tensor& input,
                        int rowMultiple,
                        Device& device)
{
    auto rows = input.rows();
    auto remainder = rows % rowMultiple;
    auto padRows = remainder == 0 ? 0 : rowMultiple - remainder;

    auto result = zeros(pass, {rows + padRows, input.cols()}, device);
    copyRowsInto(pass, result, 0, input, device);

    return result;
}
} // namespace eacp::ML
