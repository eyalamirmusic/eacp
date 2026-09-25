#pragma once

#include "../Graph/Tensor.h"

namespace eacp::GPU
{
class Buffer;
}

namespace eacp::ML
{
// A model's input or output tensor, in the storage Core ML reads and writes
// with no copy of its own. An fp16 array is an IOSurface-backed pixel buffer,
// the one form the Neural Engine takes without converting, unless the OS
// cannot make a surface of that size, when it is plain memory and
// isSurfaceBacked() is false; fp32 and int32 arrays are plain contiguous
// memory.
//
// The last dimension is a row and every other dimension counts rows, so the
// storage is rows() rows of columns() elements, rowStride() bytes apart. Only
// an fp16 array can have a stride wider than its row, because an IOSurface
// pads each row to its own alignment.
//
// Copying a MultiArray shares its storage, the way a handle does: a MultiArray
// put in Inputs or Outputs is the same memory the caller keeps.
class MultiArray
{
public:
    MultiArray();

    // An invalid MultiArray when the OS has no Core ML array of that type (fp16
    // wants macOS 12 / iOS 16), the shape is empty, or a dimension is unknown
    // or not positive.
    static MultiArray create(const Shape& shape, DType type);

    bool isValid() const;
    bool isSurfaceBacked() const;

    const Shape& shape() const;
    DType type() const;

    int elementCount() const;
    int rows() const;
    int columns() const;
    int elementSize() const;

    size_t rowStride() const;
    size_t byteCount() const;

    // Every element in row order with the padding left out, widened to fp32.
    Vector<float> toFloats() const;

    // Fills the array from row-ordered fp32 values, narrowing for fp16 and
    // truncating for int32. Fewer values than elements leave the rest as
    // they were.
    void fromFloats(Span<const float> values);

    // The seam to the kernel path: the array into a buffer of tightly packed
    // elements of bufferType, and back. They go through Buffer::update() and
    // Buffer::read(), so a copy from a buffer waits for the GPU work that
    // wrote it. Converts fp16 <-> fp32 on the way.
    void copyTo(GPU::Buffer& buffer, DType bufferType = DType::float32) const;
    void copyFrom(const GPU::Buffer& buffer, DType bufferType = DType::float32);

    // The same through part of a buffer: rows() rows of columns() elements,
    // the first offset bytes into it and each bufferRowStride bytes after the
    // one before, the bytes between them left as they are. The packed forms
    // above are these at offset 0 with a stride of one row. Nothing is copied
    // when the stride is shorter than a row or the last row would end past
    // the buffer.
    void copyTo(GPU::Buffer& buffer,
                int offset,
                size_t bufferRowStride,
                DType bufferType = DType::float32) const;
    void copyFrom(const GPU::Buffer& buffer,
                  int offset,
                  size_t bufferRowStride,
                  DType bufferType = DType::float32);

    // Element for element from another array of the same element count.
    void copyFrom(const MultiArray& other);

    struct Native;

    std::shared_ptr<Native> native() const { return impl; }
    explicit MultiArray(const std::shared_ptr<Native>& nativeToUse);

private:
    std::shared_ptr<Native> impl;
};
} // namespace eacp::ML
