#include "Tensor.h"

#include "../../GPU/Codegen/PackedVertex.h"

#include <cassert>

namespace eacp::ML
{
int elementCountOf(const std::vector<int>& shape)
{
    auto total = 1;

    for (auto extent: shape)
        total *= extent;

    return total;
}

Tensor::Tensor(GPU::Buffer bufferToUse, std::vector<int> shapeToUse, DType dtypeToUse)
    : bufferValue(std::move(bufferToUse))
    , shapeValue(std::move(shapeToUse))
    , dtypeValue(dtypeToUse)
{
}

Tensor Tensor::fromHostF32(const float* data,
                           std::vector<int> shape,
                           GPU::Device& device)
{
    auto count = elementCountOf(shape);
    auto buffer =
        device.makeBuffer(data, (std::int64_t) count * sizeof(float), GPU::BufferUsage::Storage);

    return Tensor {std::move(buffer), std::move(shape), DType::F32};
}

Tensor Tensor::fromHostPackedF16(const float* data,
                                 std::vector<int> shape,
                                 GPU::Device& device)
{
    auto count = elementCountOf(shape);
    auto wordCount = (count + 1) / 2;
    auto words = std::vector<std::uint32_t>((std::size_t) wordCount, 0u);

    for (auto i = 0; i < count; ++i)
    {
        auto bits = (std::uint32_t) GPU::halfFromFloat(data[(std::size_t) i]);
        words[(std::size_t) (i / 2)] |= bits << (16 * (i % 2));
    }

    auto byteCount = (std::int64_t) wordCount * sizeof(std::uint32_t);
    auto buffer =
        device.makeBuffer(words.data(), byteCount, GPU::BufferUsage::Storage);

    return Tensor {std::move(buffer), std::move(shape), DType::F16Packed};
}

Tensor Tensor::uninitializedF32(std::vector<int> shape, GPU::Device& device)
{
    auto count = elementCountOf(shape);
    auto buffer =
        device.makeBuffer((std::int64_t) count * sizeof(float), GPU::BufferUsage::Storage);

    return Tensor {std::move(buffer), std::move(shape), DType::F32};
}

std::vector<float> Tensor::toHostF32() const
{
    auto total = count();

    if (dtypeValue == DType::F32)
    {
        auto values = std::vector<float>((std::size_t) total);
        bufferValue.read(values.data(),
                         (std::int64_t) values.size() * sizeof(float));
        return values;
    }

    auto wordCount = (total + 1) / 2;
    auto words = std::vector<std::uint32_t>((std::size_t) wordCount);
    bufferValue.read(words.data(), (std::int64_t) words.size() * sizeof(std::uint32_t));

    auto values = std::vector<float>((std::size_t) total);

    for (auto i = 0; i < total; ++i)
    {
        auto word = words[(std::size_t) (i / 2)];
        auto bits = (std::uint16_t) ((word >> (16 * (i % 2))) & 0xffffu);
        values[(std::size_t) i] = GPU::halfToFloat(bits);
    }

    return values;
}

int Tensor::rows() const
{
    auto total = 1;

    for (auto axis = 0; axis < rank() - 1; ++axis)
        total *= shapeValue[(std::size_t) axis];

    return total;
}

int Tensor::cols() const
{
    return rank() == 0 ? 1 : shapeValue.back();
}

TensorView Tensor::columns(int firstColumn, int columnCount) const
{
    return {*this, firstColumn, columnCount};
}

TensorView::TensorView(const Tensor& tensor)
    : bufferValue(&tensor.buffer())
    , rowCount(tensor.rank() == 0 ? 1 : tensor.dim(0))
    , columnCount(tensor.count() / rowCount)
    , stride(columnCount)
    , offset(0)
{
}

TensorView::TensorView(const Tensor& tensor, int firstColumn, int columnCountToUse)
    : bufferValue(&tensor.buffer())
    , rowCount(tensor.rows())
    , columnCount(columnCountToUse)
    , stride(tensor.cols())
    , offset(firstColumn)
{
    assert(firstColumn >= 0 && firstColumn + columnCountToUse <= tensor.cols());
}
}
