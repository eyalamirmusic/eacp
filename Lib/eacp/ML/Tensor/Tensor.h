#pragma once

#include "../../GPU/Buffer/Buffer.h"
#include "../../GPU/Device/Device.h"

#include <cstdint>
#include <vector>

namespace eacp::ML
{
enum class DType
{
    F32,
    F16Packed
};

int elementCountOf(const std::vector<int>& shape);

class TensorView;

class Tensor
{
public:
    Tensor(GPU::Buffer bufferToUse, std::vector<int> shapeToUse, DType dtypeToUse);

    static Tensor fromHostF32(const float* data,
                              std::vector<int> shape,
                              GPU::Device& device = GPU::Device::shared());

    static Tensor fromHostPackedF16(const float* data,
                                    std::vector<int> shape,
                                    GPU::Device& device = GPU::Device::shared());

    static Tensor uninitializedF32(std::vector<int> shape,
                                   GPU::Device& device = GPU::Device::shared());

    std::vector<float> toHostF32() const;

    const std::vector<int>& shape() const { return shapeValue; }
    int rank() const { return (int) shapeValue.size(); }
    int dim(int axis) const { return shapeValue[(std::size_t) axis]; }
    int count() const { return elementCountOf(shapeValue); }

    int rows() const;
    int cols() const;

    // Columns [firstColumn, firstColumn + columnCount) of every row, read
    // where they lie rather than copied out: q, k and v of a fused projection.
    TensorView columns(int firstColumn, int columnCount) const;

    DType dtype() const { return dtypeValue; }
    bool isPacked() const { return dtypeValue == DType::F16Packed; }

    GPU::Buffer& buffer() { return bufferValue; }
    const GPU::Buffer& buffer() const { return bufferValue; }

private:
    GPU::Buffer bufferValue;
    std::vector<int> shapeValue;
    DType dtypeValue;
};

// A rows x cols window onto a tensor's buffer: row r starts rowStride
// elements after row r - 1, and the first at columnOffset. The kernels that
// take one read it in place, so a slice of columns costs no copy. A Tensor
// converts to the whole of itself, dim(0) rows of everything else.
class TensorView
{
public:
    TensorView(const Tensor& tensor);
    TensorView(const Tensor& tensor, int firstColumn, int columnCount);

    const GPU::Buffer& buffer() const { return *bufferValue; }

    int rows() const { return rowCount; }
    int cols() const { return columnCount; }
    int count() const { return rowCount * columnCount; }
    int rowStride() const { return stride; }
    int columnOffset() const { return offset; }

private:
    const GPU::Buffer* bufferValue;
    int rowCount;
    int columnCount;
    int stride;
    int offset;
};
}
