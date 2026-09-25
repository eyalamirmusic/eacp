#include "ModelTestCommon.h"

#include <eacp/GPU/GPU.h>

using namespace nano;
using namespace ModelTests;

// ML::MultiArray's storage and its seam to the kernel path. Ten fp16 columns are
// twenty bytes a row, which an IOSurface pads out to its row alignment, so the
// copies have to go row by row rather than as one memcpy.
namespace
{
constexpr auto paddedColumns = 10;

Vector<float> buffered(eacp::GPU::Buffer& buffer, int count)
{
    auto values = Vector<float> {};
    values.resize(count, 0.0f);
    buffer.read(values.data(), count * (int) sizeof(float));
    return values;
}

eacp::GPU::Buffer storageBuffer(int bytes)
{
    return eacp::GPU::Device::shared().makeBuffer(bytes,
                                                  eacp::GPU::BufferUsage::Storage);
}
} // namespace

auto tFloat16IsSurfaceBacked =
    test("MLMultiArray/anFp16ArrayIsAnIOSurfaceWithPaddedRows") = []
{
    if (!isSupported())
        return;

    auto array = MultiArray::create({2, 3, paddedColumns}, DType::float16);
    check(array.isValid());
    check(array.isSurfaceBacked());
    check(array.rows() == 6);
    check(array.columns() == paddedColumns);
    check(array.elementCount() == 60);

    check(array.rowStride() > (size_t) (paddedColumns * 2),
          "an IOSurface row of 20 bytes is padded");
    check(array.byteCount() == array.rowStride() * 6);
};

auto tFloat32IsPlain = test("MLMultiArray/anFp32ArrayIsTightlyPackedMemory") = []
{
    if (!isSupported())
        return;

    auto array = MultiArray::create({6, paddedColumns}, DType::float32);
    check(array.isValid());
    check(!array.isSurfaceBacked());
    check(array.rowStride() == (size_t) (paddedColumns * 4));
    check(array.byteCount() == array.rowStride() * 6);
};

auto tFloatsRoundTrip = test("MLMultiArray/floatsRoundTripThroughEveryType") = []
{
    if (!isSupported())
        return;

    auto values = TestPrograms::seededValues(6 * paddedColumns, 21u, 3.0f);

    for (auto type: {DType::float16, DType::float32})
    {
        auto array = arrayOf(values, {6, paddedColumns}, type);
        check(array.toFloats() == values, toString(type));
    }

    auto integers = Vector<float> {};

    for (auto i = 0; i < 12; ++i)
        integers.add((float) (i - 5));

    check(arrayOf(integers, {3, 4}, DType::int32).toFloats() == integers);
};

auto tBufferSeamRoundTrips =
    test("MLMultiArray/copiesThroughAGPUBufferRoundTripWithPadding") = []
{
    if (!isSupported() || !eacp::GPU::Device::shared().isValid())
        return;

    constexpr auto rows = 6;
    constexpr auto count = rows * paddedColumns;

    auto values = TestPrograms::seededValues(count, 17u, 2.0f);

    for (auto type: {DType::float16, DType::float32})
    {
        auto source = arrayOf(values, {rows, paddedColumns}, type);
        auto buffer = storageBuffer(count * (int) sizeof(float));

        source.copyTo(buffer);
        check(buffered(buffer, count) == values, "into fp32 from " + toString(type));

        auto destination = MultiArray::create({rows, paddedColumns}, type);
        destination.copyFrom(buffer);
        check(destination.toFloats() == values, "back into " + toString(type));
    }
};

auto tBufferSeamKeepsHalves =
    test("MLMultiArray/copiesIntoAPackedHalfBufferAsHalves") = []
{
    if (!isSupported() || !eacp::GPU::Device::shared().isValid())
        return;

    constexpr auto rows = 6;
    constexpr auto count = rows * paddedColumns;

    auto values = TestPrograms::seededValues(count, 19u, 2.0f);
    auto source = arrayOf(values, {rows, paddedColumns}, DType::float16);
    auto buffer = storageBuffer(count * 2);

    source.copyTo(buffer, DType::float16);

    auto halves = Vector<std::uint16_t> {};
    halves.resize(count, 0);
    buffer.read(halves.data(), count * 2);
    check(halves == TestPrograms::toHalves(values));

    auto destination = MultiArray::create({rows, paddedColumns}, DType::float32);
    destination.copyFrom(buffer, DType::float16);
    check(destination.toFloats() == values, "widened on the way in");
};

// The mel's case: each row of the array is the start of a longer row in the
// buffer, which begins part way in. The gaps between rows are the buffer's own
// and are left as they were.
auto tBufferSeamStrided =
    test("MLMultiArray/copiesThroughAnOffsetAndARowStride") = []
{
    if (!isSupported() || !eacp::GPU::Device::shared().isValid())
        return;

    constexpr auto rows = 3;
    constexpr auto columns = 4;
    constexpr auto strideFloats = 8;
    constexpr auto offsetFloats = 4;
    constexpr auto bufferFloats = offsetFloats + rows * strideFloats;
    constexpr auto sentinel = -7.0f;

    auto values = TestPrograms::seededValues(rows * columns, 23u, 1.0f);
    auto offset = offsetFloats * (int) sizeof(float);
    auto stride = (size_t) strideFloats * sizeof(float);

    for (auto type: {DType::float16, DType::float32})
    {
        auto filled = Vector<float> {};
        filled.resize(bufferFloats, sentinel);
        auto buffer = eacp::GPU::Device::shared().makeBuffer(
            filled.data(),
            bufferFloats * (int) sizeof(float),
            eacp::GPU::BufferUsage::Storage);

        auto source = arrayOf(values, {rows, columns}, type);
        source.copyTo(buffer, offset, stride);

        auto expected = filled;

        for (auto row = 0; row < rows; ++row)
            for (auto column = 0; column < columns; ++column)
                expected[offsetFloats + row * strideFloats + column] =
                    values[row * columns + column];

        check(buffered(buffer, bufferFloats) == expected,
              "strided into fp32 from " + toString(type));

        auto destination = MultiArray::create({rows, columns}, type);
        destination.copyFrom(buffer, offset, stride);
        check(destination.toFloats() == values,
              "strided back into " + toString(type));

        auto zeros = TestPrograms::zeros(rows * columns);
        auto untouched = arrayOf(zeros, {rows, columns}, type);
        untouched.copyFrom(buffer, offset, columns * sizeof(float) - 4);
        untouched.copyFrom(buffer, offset + (int) stride, stride);
        check(untouched.toFloats() == zeros,
              "a stride shorter than a row or a row past the end copies nothing");
    }

    auto halfBuffer = storageBuffer(bufferFloats * 2);
    auto halves = arrayOf(values, {rows, columns}, DType::float16);
    halves.copyTo(halfBuffer, offsetFloats * 2, strideFloats * 2, DType::float16);

    auto back = MultiArray::create({rows, columns}, DType::float32);
    back.copyFrom(halfBuffer, offsetFloats * 2, strideFloats * 2, DType::float16);
    check(back.toFloats() == values, "strided through an fp16 buffer");
};

auto tArrayCopiesConvert =
    test("MLMultiArray/anArrayCopiesFromAnotherOfAnotherType") = []
{
    if (!isSupported())
        return;

    auto values = TestPrograms::seededValues(4 * paddedColumns, 23u, 1.0f);
    auto source = arrayOf(values, {4, paddedColumns}, DType::float16);
    auto destination = MultiArray::create({4, paddedColumns}, DType::float32);

    destination.copyFrom(source);
    check(destination.toFloats() == values);
};

auto tCopiesShareStorage = test("MLMultiArray/aCopiedArrayIsTheSameStorage") = []
{
    if (!isSupported())
        return;

    auto array = MultiArray::create({2, 4}, DType::float32);
    auto alias = array;
    auto values = Vector<float> {1, 2, 3, 4, 5, 6, 7, 8};
    alias.fromFloats(values);

    check(array.native() == alias.native());
    check(array.toFloats() == values);
};

auto tEmptyShapeIsInvalid = test("MLMultiArray/anEmptyShapeMakesNoArray") = []
{
    check(!MultiArray::create({}, DType::float32).isValid());
    check(!MultiArray {}.isValid());
    check(MultiArray {}.toFloats().empty());
};
