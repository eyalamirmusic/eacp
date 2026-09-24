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

Tensor::Tensor(GPU::Buffer bufferToUse,
               std::vector<int> shapeToUse,
               DType dtypeToUse)
    : storage(std::make_shared<const GPU::Buffer>(std::move(bufferToUse)))
    , shapeValue(std::move(shapeToUse))
    , dtypeValue(dtypeToUse)
{
}

namespace
{
std::shared_ptr<const GPU::Buffer> copyOf(const GPU::Buffer& buffer,
                                          std::int64_t offset,
                                          std::int64_t bytes,
                                          GPU::Device& device)
{
    auto words = std::vector<std::uint32_t>((std::size_t) bytes / 4);
    buffer.read(words.data(), bytes, offset);

    return std::make_shared<const GPU::Buffer>(
        device.makeBuffer(words.data(), bytes, GPU::BufferUsage::Storage));
}
} // namespace

Tensor::Tensor(std::shared_ptr<const GPU::Buffer> sharedBuffer,
               std::int64_t byteOffset,
               std::vector<int> shapeToUse,
               DType dtypeToUse,
               GPU::Device& device)
    : storage(std::move(sharedBuffer))
    , offset(byteOffset)
    , shapeValue(std::move(shapeToUse))
    , dtypeValue(dtypeToUse)
{
    assert(storage != nullptr && offset >= 0
           && offset + byteCount() <= storage->size());

    if (offset % device.storageBufferOffsetAlignment() != 0)
    {
        storage = copyOf(*storage, offset, byteCount(), device);
        offset = 0;
    }
}

Tensor Tensor::fromHostF32(const float* data,
                           std::vector<int> shape,
                           GPU::Device& device)
{
    auto count = elementCountOf(shape);
    auto buffer = device.makeBuffer(
        data, (std::int64_t) count * sizeof(float), GPU::BufferUsage::Storage);

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
    auto buffer = device.makeBuffer((std::int64_t) count * sizeof(float),
                                    GPU::BufferUsage::Storage);

    return Tensor {std::move(buffer), std::move(shape), DType::F32};
}

std::vector<float> Tensor::toHostF32() const
{
    auto total = count();

    if (dtypeValue == DType::F32)
    {
        auto values = std::vector<float>((std::size_t) total);
        storage->read(values.data(), byteCount(), offset);
        return values;
    }

    auto wordCount = (total + 1) / 2;
    auto words = std::vector<std::uint32_t>((std::size_t) wordCount);
    storage->read(words.data(), byteCount(), offset);

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

Tensor Tensor::reshaped(std::vector<int> newShape) &&
{
    assert(elementCountOf(newShape) == count());
    auto reshapedTensor = Tensor {std::move(*this)};
    reshapedTensor.shapeValue = std::move(newShape);
    return reshapedTensor;
}

std::int64_t Tensor::byteCount() const
{
    auto elements = (std::int64_t) count();
    auto words = dtypeValue == DType::F32 ? elements : (elements + 1) / 2;

    return words * (std::int64_t) sizeof(float);
}

TensorView::TensorView(const Tensor& tensor)
    : tensorRange(tensor.range())
    , rowCount(tensor.rank() == 0 ? 1 : tensor.dim(0))
    , columnCount(tensor.count() / rowCount)
    , stride(columnCount)
    , offset(0)
{
}

TensorView::TensorView(const Tensor& tensor, int firstColumn, int columnCountToUse)
    : tensorRange(tensor.range())
    , rowCount(tensor.rows())
    , columnCount(columnCountToUse)
    , stride(tensor.cols())
    , offset(firstColumn)
{
    assert(firstColumn >= 0 && firstColumn + columnCountToUse <= tensor.cols());
}
} // namespace eacp::ML
