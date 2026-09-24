#pragma once

#include "../../GPU/Codegen/ComputeProgram.h"
#include "../Tensor/Tensor.h"

#include <functional>
#include <initializer_list>
#include <vector>

namespace eacp::ML
{
enum class ElementwiseOp
{
    Add,
    Subtract,
    Multiply
};

class ElementwiseKernel final : public GPU::ComputeProgram
{
public:
    explicit ElementwiseKernel(ElementwiseOp opToUse);

    void dispatch(GPU::ComputePass& pass, int count);

    std::string name() const override;

    GPU::Uniform<GPU::InputBuffer> a;
    GPU::Uniform<GPU::InputBuffer> b;
    GPU::Uniform<GPU::OutputBuffer> output;

    EACP_SHADER(a, b, output)

private:
    void define() override;

    ElementwiseOp op;
};

class ScaleAndAddKernel final : public GPU::ComputeProgram
{
public:
    ScaleAndAddKernel();

    void dispatch(GPU::ComputePass& pass, int count);

    GPU::Uniform<GPU::InputBuffer> a;
    GPU::Uniform<GPU::InputBuffer> b;
    GPU::Uniform<GPU::OutputBuffer> output;
    GPU::Uniform<GPU::Float> scaleA;
    GPU::Uniform<GPU::Float> scaleB;

    EACP_SHADER(a, b, output, scaleA, scaleB)

private:
    void define() override;
};

class FillKernel final : public GPU::ComputeProgram
{
public:
    FillKernel();

    void dispatch(GPU::ComputePass& pass, int count);

    GPU::Uniform<GPU::OutputBuffer> output;
    GPU::Uniform<GPU::Float> value;

    EACP_SHADER(output, value)

private:
    void define() override;
};

class CopyRowsKernel final : public GPU::ComputeProgram
{
public:
    CopyRowsKernel();

    void dispatch(GPU::ComputePass& pass, int rows, int columns);

    GPU::Uniform<GPU::InputBuffer> source;
    GPU::Uniform<GPU::OutputBuffer> destination;
    GPU::Uniform<GPU::UInt> columnCount;
    GPU::Uniform<GPU::UInt> sourceRowStart;
    GPU::Uniform<GPU::UInt> destinationRowStart;

    EACP_SHADER(
        source, destination, columnCount, sourceRowStart, destinationRowStart)

private:
    void define() override;
};

class SliceColumnsKernel final : public GPU::ComputeProgram
{
public:
    SliceColumnsKernel();

    void dispatch(GPU::ComputePass& pass, int rows, int columns);

    GPU::Uniform<GPU::InputBuffer> source;
    GPU::Uniform<GPU::OutputBuffer> destination;
    GPU::Uniform<GPU::UInt> sourceColumnCount;
    GPU::Uniform<GPU::UInt> columnStart;
    GPU::Uniform<GPU::UInt> destinationColumnCount;

    EACP_SHADER(
        source, destination, sourceColumnCount, columnStart, destinationColumnCount)

private:
    void define() override;
};

// a + b, a - b and a * b, element by element; b has a's element count and the
// result has a's shape.
Tensor add(GPU::ComputePass& pass,
           const Tensor& a,
           const Tensor& b,
           GPU::Device& device = GPU::Device::shared());

Tensor subtract(GPU::ComputePass& pass,
                const Tensor& a,
                const Tensor& b,
                GPU::Device& device = GPU::Device::shared());

Tensor multiply(GPU::ComputePass& pass,
                const Tensor& a,
                const Tensor& b,
                GPU::Device& device = GPU::Device::shared());

// a * scaleA + b * scaleB, element by element.
Tensor scaleAndAdd(GPU::ComputePass& pass,
                   const Tensor& a,
                   float scaleA,
                   const Tensor& b,
                   float scaleB,
                   GPU::Device& device = GPU::Device::shared());

Tensor fill(GPU::ComputePass& pass,
            std::vector<int> shape,
            float value,
            GPU::Device& device = GPU::Device::shared());

Tensor zeros(GPU::ComputePass& pass,
             std::vector<int> shape,
             GPU::Device& device = GPU::Device::shared());

// The same buffer under another shape of the same element count: no copy.
Tensor reshape(Tensor tensor, std::vector<int> shape);

// Row and column ranges of a rows x columns tensor, copied out.
Tensor sliceRows(GPU::ComputePass& pass,
                 const Tensor& input,
                 int firstRow,
                 int rowCount,
                 GPU::Device& device = GPU::Device::shared());

Tensor sliceColumns(GPU::ComputePass& pass,
                    const Tensor& input,
                    int firstColumn,
                    int columnCount,
                    GPU::Device& device = GPU::Device::shared());

// Tensors of one column count stacked top to bottom:
//     auto joined = concatRows(pass, {head, body, tail});
Tensor concatRows(GPU::ComputePass& pass,
                  std::initializer_list<std::reference_wrapper<const Tensor>> parts,
                  GPU::Device& device = GPU::Device::shared());

// Every row of source written over destination's, from firstRow down.
void copyRowsInto(GPU::ComputePass& pass,
                  const Tensor& destination,
                  int firstRow,
                  const Tensor& source,
                  GPU::Device& device = GPU::Device::shared());

// input with rows of zeros below it up to the next multiple of rowMultiple.
Tensor padRowsWithZeros(GPU::ComputePass& pass,
                        const Tensor& input,
                        int rowMultiple,
                        GPU::Device& device = GPU::Device::shared());
} // namespace eacp::ML
