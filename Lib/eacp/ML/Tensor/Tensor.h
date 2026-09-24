#pragma once

#include "../../GPU/Buffer/Buffer.h"
#include "../../GPU/Device/Device.h"

#include <cstdint>
#include <memory>
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

// Shape and dtype over bytes in a GPU buffer. The bytes start byteOffset()
// into the buffer, which is how a tensor lies inside a buffer it shares with
// others: every tensor of a mapped weights file is one of these over the one
// buffer the whole file became. A kernel binds a tensor by its range, so
// kernel.input = tensor binds exactly its bytes wherever they start.
//
// Move-only, like the Buffer it used to hold by value: a tensor sharing its
// buffer is something a loader decides, not something a copy does by accident.
class Tensor
{
public:
    Tensor(GPU::Buffer bufferToUse, std::vector<int> shapeToUse, DType dtypeToUse);

    // A tensor over bytes of a buffer other tensors may share, starting
    // byteOffset bytes in. The buffer lives as long as the last tensor over it.
    Tensor(std::shared_ptr<const GPU::Buffer> sharedBuffer,
           std::int64_t byteOffset,
           std::vector<int> shapeToUse,
           DType dtypeToUse);

    Tensor(Tensor&&) noexcept = default;
    Tensor& operator=(Tensor&&) noexcept = default;
    Tensor(const Tensor&) = delete;
    Tensor& operator=(const Tensor&) = delete;

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

    // The same bytes under another shape of the same count.
    Tensor reshaped(std::vector<int> newShape) &&;

    DType dtype() const { return dtypeValue; }
    bool isPacked() const { return dtypeValue == DType::F16Packed; }

    std::int64_t byteOffset() const { return offset; }
    std::int64_t byteCount() const;

    GPU::BufferRange range() const { return {storage.get(), offset, byteCount()}; }
    operator GPU::BufferRange() const { return range(); }

private:
    std::shared_ptr<const GPU::Buffer> storage;
    std::int64_t offset = 0;
    std::vector<int> shapeValue;
    DType dtypeValue;
};

// A rows x cols window onto a tensor: row r starts rowStride elements after
// row r - 1, and the first at columnOffset, both counted from the start of the
// tensor's range. The kernels that take one bind range() and read it in place,
// so a slice of columns costs no copy. A Tensor converts to the whole of
// itself, dim(0) rows of everything else.
class TensorView
{
public:
    TensorView(const Tensor& tensor);
    TensorView(const Tensor& tensor, int firstColumn, int columnCount);

    const GPU::BufferRange& range() const { return tensorRange; }

    int rows() const { return rowCount; }
    int cols() const { return columnCount; }
    int count() const { return rowCount * columnCount; }
    int rowStride() const { return stride; }
    int columnOffset() const { return offset; }

private:
    GPU::BufferRange tensorRange;
    int rowCount;
    int columnCount;
    int stride;
    int offset;
};
} // namespace eacp::ML
