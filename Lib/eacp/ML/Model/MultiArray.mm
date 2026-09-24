#include "MultiArrayNative.h"

#include <eacp/Core/ObjC/AutoReleasePool.h>
#include <eacp/GPU/GPU.h>

#include <Accelerate/Accelerate.h>

#include <algorithm>
#include <cstring>
#include <limits>

namespace eacp::ML
{
int elementSizeOf(DType type)
{
    return type == DType::float16 ? 2 : 4;
}

Shape toShape(NSArray<NSNumber*>* dimensions)
{
    auto shape = Shape {};

    for (NSNumber* dimension in dimensions)
        shape.dims.add(dimension.intValue);

    return shape;
}

NSArray<NSNumber*>* toNSArray(const Shape& shape)
{
    auto dimensions = [NSMutableArray arrayWithCapacity:(NSUInteger) shape.dims.size()];

    for (auto dimension: shape.dims)
        [dimensions addObject:@(dimension)];

    return dimensions;
}

bool toDType(MLMultiArrayDataType dataType, DType& type)
{
    if (dataType == MLMultiArrayDataTypeFloat32)
    {
        type = DType::float32;
        return true;
    }

    if (dataType == MLMultiArrayDataTypeInt32)
    {
        type = DType::int32;
        return true;
    }

    if (@available(macOS 12.0, iOS 16.0, *))
    {
        if (dataType == MLMultiArrayDataTypeFloat16)
        {
            type = DType::float16;
            return true;
        }
    }

    return false;
}

namespace
{
float readElement(const std::byte* row, DType type, int column)
{
    if (type == DType::int32)
        return (float) reinterpret_cast<const std::int32_t*>(row)[column];

    return reinterpret_cast<const float*>(row)[column];
}

void writeElement(std::byte* row, DType type, int column, float value)
{
    if (type == DType::int32)
        reinterpret_cast<std::int32_t*>(row)[column] = (std::int32_t) value;
    else
        reinterpret_cast<float*>(row)[column] = value;
}

vImage_Buffer planeOf(const void* data, int rows, int columns, size_t stride)
{
    return {const_cast<void*>(data),
            (vImagePixelCount) rows,
            (vImagePixelCount) columns,
            stride};
}

void copyRows(const std::byte* source,
              size_t sourceStride,
              std::byte* destination,
              size_t destinationStride,
              int rows,
              size_t rowBytes)
{
    if (sourceStride == rowBytes && destinationStride == rowBytes)
    {
        std::memcpy(destination, source, rowBytes * (size_t) rows);
        return;
    }

    for (auto row = 0; row < rows; ++row)
        std::memcpy(destination + (size_t) row * destinationStride,
                    source + (size_t) row * sourceStride,
                    rowBytes);
}

void convertThroughFloat(const std::byte* source,
                         DType sourceType,
                         size_t sourceStride,
                         std::byte* destination,
                         DType destinationType,
                         size_t destinationStride,
                         int rows,
                         int columns)
{
    for (auto row = 0; row < rows; ++row)
    {
        auto from = source + (size_t) row * sourceStride;
        auto to = destination + (size_t) row * destinationStride;

        for (auto column = 0; column < columns; ++column)
            writeElement(
                to, destinationType, column, readElement(from, sourceType, column));
    }
}

void convertWidening(const void* source,
                     size_t sourceStride,
                     void* destination,
                     size_t destinationStride,
                     int rows,
                     int columns)
{
    auto from = planeOf(source, rows, columns, sourceStride);
    auto to = planeOf(destination, rows, columns, destinationStride);
    vImageConvert_Planar16FtoPlanarF(&from, &to, kvImageNoFlags);
}

void convertNarrowing(const void* source,
                      size_t sourceStride,
                      void* destination,
                      size_t destinationStride,
                      int rows,
                      int columns)
{
    auto from = planeOf(source, rows, columns, sourceStride);
    auto to = planeOf(destination, rows, columns, destinationStride);
    vImageConvert_PlanarFtoPlanar16F(&from, &to, kvImageNoFlags);
}
} // namespace

void convertRows(const void* source,
                 DType sourceType,
                 size_t sourceStride,
                 void* destination,
                 DType destinationType,
                 size_t destinationStride,
                 int rows,
                 int columns)
{
    if (source == nullptr || destination == nullptr || rows <= 0 || columns <= 0)
        return;

    auto from = static_cast<const std::byte*>(source);
    auto to = static_cast<std::byte*>(destination);

    if (sourceType == destinationType)
    {
        auto rowBytes = (size_t) columns * (size_t) elementSizeOf(sourceType);
        copyRows(from, sourceStride, to, destinationStride, rows, rowBytes);
        return;
    }

    if (sourceType == DType::float16 && destinationType == DType::float32)
    {
        convertWidening(from, sourceStride, to, destinationStride, rows, columns);
        return;
    }

    if (sourceType == DType::float32 && destinationType == DType::float16)
    {
        convertNarrowing(from, sourceStride, to, destinationStride, rows, columns);
        return;
    }

    if (sourceType == DType::float16)
    {
        auto widened = Vector<float> {};
        widened.resize(rows * columns, 0.0f);
        auto widenedStride = (size_t) columns * sizeof(float);
        convertWidening(from, sourceStride, widened.data(), widenedStride, rows, columns);
        convertThroughFloat(reinterpret_cast<const std::byte*>(widened.data()),
                            DType::float32,
                            widenedStride,
                            to,
                            destinationType,
                            destinationStride,
                            rows,
                            columns);
        return;
    }

    if (destinationType == DType::float16)
    {
        auto staged = Vector<float> {};
        staged.resize(rows * columns, 0.0f);
        auto stagedStride = (size_t) columns * sizeof(float);
        convertThroughFloat(from,
                            sourceType,
                            sourceStride,
                            reinterpret_cast<std::byte*>(staged.data()),
                            DType::float32,
                            stagedStride,
                            rows,
                            columns);
        convertNarrowing(staged.data(), stagedStride, to, destinationStride, rows, columns);
        return;
    }

    convertThroughFloat(from,
                        sourceType,
                        sourceStride,
                        to,
                        destinationType,
                        destinationStride,
                        rows,
                        columns);
}

namespace
{
bool isAllocatable(const Shape& shape)
{
    auto isEmptyOrUnknown = [](int size) { return size <= 0; };

    return !shape.dims.empty() && shape.isFixed()
           && shape.dims.findIf(isEmptyOrUnknown) == nullptr
           && shape.count() <= std::numeric_limits<int>::max();
}

class CpuAccess
{
public:
    CpuAccess(const MultiArray::Native& nativeToUse, bool readOnly)
        : native(nativeToUse)
        , flags(readOnly ? kCVPixelBufferLock_ReadOnly : 0)
    {
        if (native.surface)
        {
            CVPixelBufferLockBaseAddress(native.surface, flags);
            bytes = static_cast<std::byte*>(CVPixelBufferGetBaseAddress(native.surface));
        }
        else
        {
            bytes = native.bytes;
        }
    }

    ~CpuAccess()
    {
        if (native.surface)
            CVPixelBufferUnlockBaseAddress(native.surface, flags);
    }

    CpuAccess(const CpuAccess&) = delete;
    CpuAccess& operator=(const CpuAccess&) = delete;

    std::byte* data() const { return bytes; }

private:
    const MultiArray::Native& native;
    CVPixelBufferLockFlags flags;
    std::byte* bytes = nullptr;
};

bool toMLDataType(DType type, MLMultiArrayDataType& dataType)
{
    if (type == DType::int32)
    {
        dataType = MLMultiArrayDataTypeInt32;
        return true;
    }

    if (type == DType::float32)
    {
        dataType = MLMultiArrayDataTypeFloat32;
        return true;
    }

    if (@available(macOS 12.0, iOS 16.0, *))
    {
        dataType = MLMultiArrayDataTypeFloat16;
        return true;
    }

    return false;
}

bool readRowStride(MLMultiArray* array, int elementSize, size_t& stride)
{
    auto shape = array.shape;
    auto strides = array.strides;
    auto rank = (int) shape.count;

    if (strides[(NSUInteger) rank - 1].integerValue != 1)
        return false;

    for (auto axis = rank - 3; axis >= 0; --axis)
        if (strides[(NSUInteger) axis].integerValue
            != strides[(NSUInteger) axis + 1].integerValue
                   * shape[(NSUInteger) axis + 1].integerValue)
            return false;

    auto rowElements = rank >= 2 ? strides[(NSUInteger) rank - 2].integerValue
                                 : shape.lastObject.integerValue;
    stride = (size_t) rowElements * (size_t) elementSize;
    return true;
}

std::shared_ptr<MultiArray::Native> makePlainNative(const Shape& shape, DType type)
{
    auto dataType = MLMultiArrayDataTypeFloat32;

    if (!toMLDataType(type, dataType))
        return {};

    auto pool = ObjC::AutoReleasePool {};

    NSError* error = nil;
    auto array = ObjC::Ptr<MLMultiArray> {[[MLMultiArray alloc]
        initWithShape:toNSArray(shape)
             dataType:dataType
                error:&error]};

    if (!array)
        return {};

    auto native = std::make_shared<MultiArray::Native>();
    native->shape = shape;
    native->type = type;
    native->array = array;
    native->bytes = static_cast<std::byte*>(array.get().dataPointer);

    if (native->bytes == nullptr
        || !readRowStride(array.get(), elementSizeOf(type), native->stride))
        return {};

    std::memset(native->bytes, 0, native->stride * (size_t) (shape.count() / shape.dims.back()));
    return native;
}

std::shared_ptr<MultiArray::Native> makeSurfaceNative(const Shape& shape)
{
    if (@available(macOS 12.0, iOS 16.0, *))
    {
        auto pool = ObjC::AutoReleasePool {};
        auto columns = shape.dims.back();
        auto rows = (int) (shape.count() / columns);

        NSDictionary* attributes = @{
            (id) kCVPixelBufferIOSurfacePropertiesKey: @{},
            (id) kCVPixelBufferMetalCompatibilityKey: @YES,
        };

        CVPixelBufferRef buffer = nullptr;
        auto status = CVPixelBufferCreate(kCFAllocatorDefault,
                                          (size_t) columns,
                                          (size_t) rows,
                                          kCVPixelFormatType_OneComponent16Half,
                                          (__bridge CFDictionaryRef) attributes,
                                          &buffer);

        if (status != kCVReturnSuccess || buffer == nullptr)
            return {};

        auto native = std::make_shared<MultiArray::Native>();
        native->shape = shape;
        native->type = DType::float16;
        native->surface.reset(buffer);
        native->stride = CVPixelBufferGetBytesPerRow(buffer);
        native->array = [[MLMultiArray alloc] initWithPixelBuffer:buffer
                                                            shape:toNSArray(shape)];

        if (!native->array)
            return {};

        return native;
    }

    return {};
}

std::shared_ptr<MultiArray::Native> wrapSurface(MLMultiArray* output,
                                                CVPixelBufferRef buffer)
{
    auto native = std::make_shared<MultiArray::Native>();
    native->shape = toShape(output.shape);
    native->type = DType::float16;
    native->array.reset(output);
    native->surface.reset(CVPixelBufferRetain(buffer));
    native->stride = CVPixelBufferGetBytesPerRow(buffer);
    return native;
}

class RowOffsets
{
public:
    explicit RowOffsets(MLMultiArray* array)
    {
        for (NSNumber* extent in array.shape)
            extents.add(extent.intValue);

        for (NSNumber* stride in array.strides)
            strides.add(stride.intValue);
    }

    size_t of(int row) const
    {
        auto offset = size_t {0};

        for (auto axis = extents.size() - 2; axis >= 0; --axis)
        {
            offset += (size_t) (row % extents[axis]) * (size_t) strides[axis];
            row /= extents[axis];
        }

        return offset;
    }

    int columnStride() const { return strides.back(); }

private:
    Vector<int> extents;
    Vector<int> strides;
};

void copyStridedInto(MultiArray& copy, MLMultiArray* output)
{
    if (@available(macOS 12.3, iOS 15.4, *))
    {
        auto access = CpuAccess {*copy.native(), false};
        auto destination = access.data();
        auto destinationStride = copy.rowStride();
        auto type = copy.type();
        auto rows = copy.rows();
        auto columns = copy.columns();
        auto elementSize = (size_t) elementSizeOf(type);
        auto offsets = RowOffsets {output};
        auto columnStride = (size_t) offsets.columnStride();

        auto readRows = ^(const void* bytes, NSInteger)
        {
            auto source = static_cast<const std::byte*>(bytes);

            for (auto row = 0; row < rows; ++row)
            {
                auto from = source + offsets.of(row) * elementSize;
                auto to = destination + (size_t) row * destinationStride;

                if (columnStride == 1)
                {
                    std::memcpy(to, from, (size_t) columns * elementSize);
                    continue;
                }

                for (auto column = 0; column < columns; ++column)
                    std::memcpy(to + (size_t) column * elementSize,
                                from + (size_t) column * columnStride * elementSize,
                                elementSize);
            }
        };

        [output getBytesWithHandler:readRows];
    }
}
} // namespace

MLMultiArray* nativeArray(const MultiArray& array)
{
    auto native = array.native();
    return native ? native->array.get() : nil;
}

MultiArray adoptOutput(MLMultiArray* output)
{
    auto type = DType::float32;

    if (output == nil || !toDType(output.dataType, type))
        return {};

    if (@available(macOS 12.0, iOS 16.0, *))
        if (auto buffer = output.pixelBuffer)
            return MultiArray {wrapSurface(output, buffer)};

    auto copy = MultiArray::create(toShape(output.shape), type);

    if (copy.isValid())
        copyStridedInto(copy, output);

    return copy;
}

MultiArray::MultiArray() = default;

MultiArray::MultiArray(const std::shared_ptr<Native>& nativeToUse)
    : impl(nativeToUse)
{
}

MultiArray MultiArray::create(const Shape& shape, DType type)
{
    if (!isAllocatable(shape))
        return {};

    if (type == DType::float16)
        if (auto surface = makeSurfaceNative(shape))
            return MultiArray {surface};

    return MultiArray {makePlainNative(shape, type)};
}

bool MultiArray::isValid() const
{
    return impl != nullptr;
}

bool MultiArray::isSurfaceBacked() const
{
    return impl != nullptr && impl->surface;
}

const Shape& MultiArray::shape() const
{
    static const auto empty = Shape {};
    return impl ? impl->shape : empty;
}

DType MultiArray::type() const
{
    return impl ? impl->type : DType::float32;
}

int MultiArray::elementCount() const
{
    return impl ? (int) impl->shape.count() : 0;
}

int MultiArray::columns() const
{
    return impl ? impl->shape.dims.back() : 0;
}

int MultiArray::rows() const
{
    auto count = columns();
    return count == 0 ? 0 : elementCount() / count;
}

int MultiArray::elementSize() const
{
    return elementSizeOf(type());
}

size_t MultiArray::rowStride() const
{
    return impl ? impl->stride : 0;
}

size_t MultiArray::byteCount() const
{
    return rowStride() * (size_t) rows();
}

Vector<float> MultiArray::toFloats() const
{
    auto values = Vector<float> {};

    if (!isValid())
        return values;

    auto access = CpuAccess {*impl, true};
    values.resize(elementCount(), 0.0f);
    convertRows(access.data(),
                type(),
                rowStride(),
                values.data(),
                DType::float32,
                (size_t) columns() * sizeof(float),
                rows(),
                columns());
    return values;
}

void MultiArray::fromFloats(Span<const float> values)
{
    if (!isValid())
        return;

    auto access = CpuAccess {*impl, false};
    auto packedStride = (size_t) columns() * sizeof(float);
    auto wholeRows = std::min((int) values.size() / columns(), rows());

    convertRows(values.data(),
                DType::float32,
                packedStride,
                access.data(),
                type(),
                rowStride(),
                wholeRows,
                columns());

    auto remainder = std::min((int) values.size(), elementCount()) - wholeRows * columns();

    if (remainder > 0)
        convertRows(values.data() + wholeRows * columns(),
                    DType::float32,
                    packedStride,
                    access.data() + (size_t) wholeRows * rowStride(),
                    type(),
                    rowStride(),
                    1,
                    remainder);
}

void MultiArray::copyTo(GPU::Buffer& buffer, DType bufferType) const
{
    if (!isValid() || !buffer.isValid())
        return;

    auto access = CpuAccess {*impl, true};
    auto packedStride = (size_t) columns() * (size_t) elementSizeOf(bufferType);
    auto total = packedStride * (size_t) rows();

    if (bufferType == type() && rowStride() == packedStride)
    {
        buffer.update(access.data(), (int) total);
        return;
    }

    auto staging = Vector<std::byte> {};
    staging.resize((int) total, std::byte {0});
    convertRows(access.data(),
                type(),
                rowStride(),
                staging.data(),
                bufferType,
                packedStride,
                rows(),
                columns());
    buffer.update(staging.data(), (int) total);
}

void MultiArray::copyFrom(const GPU::Buffer& buffer, DType bufferType)
{
    if (!isValid() || !buffer.isValid())
        return;

    auto access = CpuAccess {*impl, false};
    auto packedStride = (size_t) columns() * (size_t) elementSizeOf(bufferType);
    auto total = packedStride * (size_t) rows();

    if (bufferType == type() && rowStride() == packedStride)
    {
        buffer.read(access.data(), (int) total);
        return;
    }

    auto staging = Vector<std::byte> {};
    staging.resize((int) total, std::byte {0});
    buffer.read(staging.data(), (int) total);
    convertRows(staging.data(),
                bufferType,
                packedStride,
                access.data(),
                type(),
                rowStride(),
                rows(),
                columns());
}

void MultiArray::copyFrom(const MultiArray& other)
{
    if (!isValid() || !other.isValid() || other.impl == impl)
        return;

    if (other.columns() == columns() && other.rows() == rows())
    {
        auto source = CpuAccess {*other.impl, true};
        auto destination = CpuAccess {*impl, false};
        convertRows(source.data(),
                    other.type(),
                    other.rowStride(),
                    destination.data(),
                    type(),
                    rowStride(),
                    rows(),
                    columns());
        return;
    }

    auto values = other.toFloats();
    fromFloats(values);
}
} // namespace eacp::ML
