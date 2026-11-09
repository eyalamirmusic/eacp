#include "CountingHeap.h"

#include <NanoTest/NanoTest.h>

#include <eacp/GPU/CpuCompute/CpuCompute.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>

// The types of WGSL's stage 9, part A, on the CPU executor: every non-square
// float matrix shape - built from columns, read a column at a time at a fixed
// and a clamped dynamic index, transposed, multiplied by a vector on either
// side, by a matrix of every agreeing shape, scaled, summed and differenced -
// against a C++ reference over the same column-major floats, exactly; private
// arrays whose lanes each read and write their own elements (an insertion
// sort, a histogram binned through clamped indices, an array of matrices);
// the i32 storage buffers with their record reads and writes; and the
// realtime contract.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::GPU::CpuCompute;

namespace TypesTests
{
namespace
{
constexpr auto threadCount = 70;
constexpr auto sentinel = -1234.5f;

const auto batchings = std::array {PlanOptions {}, PlanOptions {1}};

template <int Width>
FloatVectorOf<Width> readVector(const InputBuffer& values, const UInt& first)
{
    auto at = [&](unsigned offset) { return values[first + offset]; };

    if constexpr (Width == 2)
        return float2(at(0u), at(1u));
    else if constexpr (Width == 3)
        return float3(at(0u), at(1u), at(2u));
    else
        return float4(at(0u), at(1u), at(2u), at(3u));
}

template <int Columns, int Rows>
MatrixOf<Columns, Rows> readMatrix(const InputBuffer& values, const UInt& first)
{
    auto column = [&](int which)
    { return readVector<Rows>(values, first + (unsigned) (which * Rows)); };

    if constexpr (Columns == 2 && Rows == 2)
        return float2x2(column(0), column(1));
    else if constexpr (Columns == 2 && Rows == 3)
        return float2x3(column(0), column(1));
    else if constexpr (Columns == 2 && Rows == 4)
        return float2x4(column(0), column(1));
    else if constexpr (Columns == 3 && Rows == 2)
        return float3x2(column(0), column(1), column(2));
    else if constexpr (Columns == 3 && Rows == 3)
        return float3x3(column(0), column(1), column(2));
    else if constexpr (Columns == 3 && Rows == 4)
        return float3x4(column(0), column(1), column(2));
    else if constexpr (Columns == 4 && Rows == 2)
        return float4x2(column(0), column(1), column(2), column(3));
    else if constexpr (Columns == 4 && Rows == 3)
        return float4x3(column(0), column(1), column(2), column(3));
    else
        return float4x4(column(0), column(1), column(2), column(3));
}

// What a kernel writes one thread's results with: each component in order,
// through the kernel's own store.
template <typename Store>
struct Emitter
{
    void put(const Float& value)
    {
        store(base + (unsigned) offset, value);
        ++offset;
    }

    void put(const Float2& value)
    {
        put(value.x());
        put(value.y());
    }

    void put(const Float3& value)
    {
        put(value.x());
        put(value.y());
        put(value.z());
    }

    void put(const Float4& value)
    {
        put(value.x());
        put(value.y());
        put(value.z());
        put(value.w());
    }

    template <ShaderMatrixLike M>
    void put(const M& matrix)
    {
        for (auto column = 0; column < M::columns; ++column)
            put(matrix[column]);
    }

    Store store;
    UInt base;
    int offset = 0;
};

// Per thread: A and B (C x R), S (C x C), v (C), w (R) in, every operation of
// the shape out, in the order Reference below lays it down.
template <int Columns, int Rows>
struct ShapeKernel final : ComputeKernel
{
    static constexpr int inputStride =
        2 * Columns * Rows + Columns * Columns + Columns + Rows;
    static constexpr int outputStride = Rows + Columns + 6 * Columns * Rows
                                        + Rows * Rows + Columns * Columns
                                        + Columns * Rows + 2 * Rows + Columns * Rows;

    ShapeKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto first = id * (unsigned) inputStride;
        auto cr = (unsigned) (Columns * Rows);

        auto a = readMatrix<Columns, Rows>(values, first);
        auto b = readMatrix<Columns, Rows>(values, first + cr);
        auto s = readMatrix<Columns, Columns>(values, first + 2u * cr);
        auto v = readVector<Columns>(
            values, first + 2u * cr + (unsigned) (Columns * Columns));
        auto w = readVector<Rows>(
            values, first + 2u * cr + (unsigned) (Columns * Columns + Columns));

        auto store = [&](const UInt& at, const Float& value)
        { write(output, at, value); };
        auto out = Emitter<decltype(store)> {store, id * (unsigned) outputStride};
        out.put(a * v);
        out.put(w * a);
        out.put(a + b);
        out.put(a - b);
        out.put(2.f * a);
        out.put(a * 0.5f);
        out.put(a / 4.f);
        out.put(transpose(a));
        out.put(a * transpose(b));
        out.put(transpose(a) * b);
        out.put(a * s);
        out.put(a[toInt(id % 7u) - 2]);
        out.put(a[id % 5u]);

        auto pair = privateArray(a, b);
        out.put(pair[id % 2u]);
    }

    Uniform<InputBuffer> values;
    Uniform<OutputBuffer> output;
    EACP_SHADER(values, output)
};

float inputValue(int thread, int element)
{
    return (float) ((thread * 7 + element * 3) % 17 - 8) * 0.25f;
}

// Column-major, as the shader holds it: element (column, row) at
// column * rows + row.
struct Matrix
{
    int columns = 0;
    int rows = 0;
    Array<float, 16> elements {};

    float& at(int column, int row) { return elements[column * rows + row]; }
    float at(int column, int row) const { return elements[column * rows + row]; }
};

Matrix readReference(const Vector<float>& values, int first, int columns, int rows)
{
    auto matrix = Matrix {columns, rows};

    for (auto i = 0; i < columns * rows; ++i)
        matrix.elements[i] = values[first + i];

    return matrix;
}

Matrix transposed(const Matrix& m)
{
    auto result = Matrix {m.rows, m.columns};

    for (auto column = 0; column < m.columns; ++column)
        for (auto row = 0; row < m.rows; ++row)
            result.at(row, column) = m.at(column, row);

    return result;
}

Matrix product(const Matrix& left, const Matrix& right)
{
    auto result = Matrix {right.columns, left.rows};

    for (auto column = 0; column < right.columns; ++column)
    {
        for (auto row = 0; row < left.rows; ++row)
        {
            auto sum = 0.f;

            for (auto k = 0; k < left.columns; ++k)
            {
                auto term = left.at(k, row) * right.at(column, k);
                sum = k == 0 ? term : sum + term;
            }

            result.at(column, row) = sum;
        }
    }

    return result;
}

template <typename Combine>
Matrix elementwise(const Matrix& a, const Matrix& b, Combine combine)
{
    auto result = Matrix {a.columns, a.rows};

    for (auto i = 0; i < a.columns * a.rows; ++i)
        result.elements[i] = combine(a.elements[i], b.elements[i]);

    return result;
}

struct Reference
{
    void put(float value) { values.add(value); }

    void put(const Matrix& m)
    {
        for (auto i = 0; i < m.columns * m.rows; ++i)
            put(m.elements[i]);
    }

    void putColumn(const Matrix& m, int column)
    {
        for (auto row = 0; row < m.rows; ++row)
            put(m.at(column, row));
    }

    Vector<float> values;
};

Vector<float> shapeInputs(int stride)
{
    auto values = Vector<float> {};

    for (auto thread = 0; thread < threadCount; ++thread)
        for (auto element = 0; element < stride; ++element)
            values.add(inputValue(thread, element));

    return values;
}

template <int Columns, int Rows>
Vector<float> expectedShape(const Vector<float>& values)
{
    using Kernel = ShapeKernel<Columns, Rows>;
    auto reference = Reference {};
    auto cr = Columns * Rows;

    for (auto thread = 0; thread < threadCount; ++thread)
    {
        auto first = thread * Kernel::inputStride;
        auto a = readReference(values, first, Columns, Rows);
        auto b = readReference(values, first + cr, Columns, Rows);
        auto s = readReference(values, first + 2 * cr, Columns, Columns);
        auto v =
            readReference(values, first + 2 * cr + Columns * Columns, 1, Columns);
        auto w = readReference(
            values, first + 2 * cr + Columns * Columns + Columns, Rows, 1);

        auto plus = [](float x, float y) { return x + y; };
        auto minus = [](float x, float y) { return x - y; };
        auto twice = [](float x, float) { return 2.f * x; };
        auto half = [](float x, float) { return x * 0.5f; };
        auto quarter = [](float x, float) { return x / 4.f; };

        reference.put(product(a, v));
        reference.put(product(w, a));
        reference.put(elementwise(a, b, plus));
        reference.put(elementwise(a, b, minus));
        reference.put(elementwise(a, a, twice));
        reference.put(elementwise(a, a, half));
        reference.put(elementwise(a, a, quarter));
        reference.put(transposed(a));
        reference.put(product(a, transposed(b)));
        reference.put(product(transposed(a), b));
        reference.put(product(a, s));
        reference.putColumn(a, std::clamp(thread % 7 - 2, 0, Columns - 1));
        reference.putColumn(a, std::min(thread % 5, Columns - 1));
        reference.put(thread % 2 == 0 ? a : b);
    }

    return reference.values;
}

template <int Columns, int Rows>
void checkShape(
    const std::source_location& location = std::source_location::current())
{
    using Kernel = ShapeKernel<Columns, Rows>;

    for (const auto& options: batchings)
    {
        auto kernel = Kernel {};
        auto executor = Executor {kernel, options};
        check(executor.isValid(), executor.reason(), location);

        auto values = shapeInputs(Kernel::inputStride);
        auto output = Vector<float> {};
        output.resize(threadCount * Kernel::outputStride, sentinel);

        auto bindings = Bindings {};
        bindings.set(kernel.values, values);
        bindings.set(kernel.output, output);
        check(executor.dispatch(bindings, threadCount), "dispatch", location);

        auto expected = expectedShape<Columns, Rows>(values);
        check(expected.size() == output.size(), "the reference's size", location);
        check(output == expected,
              "matrix " + std::to_string(Columns) + "x" + std::to_string(Rows),
              location);
    }
}

// Eight values a lane loads, insertion-sorts in a private array and writes
// back: every lane's loops run a different number of times and read and
// write different elements.
struct SortKernel final : ComputeKernel
{
    SortKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto items = privateArray<Float, 8>();
        auto i = var(0);

        auto load = [&]
        {
            items.set(i.get(), input[id * 8u + toUInt(i.get())]);
            i += 1;
        };
        loop(i.get() < 8, load);

        auto k = var(1);
        auto insert = [&]
        {
            auto key = var(items[k.get()]);
            auto j = var(k.get() - 1);
            auto shift = [&]
            {
                items.set(j.get() + 1, items[j.get()]);
                j -= 1;
            };

            loop(j.get() >= 0 && items[j.get()] > key.get(), shift);
            items.set(j.get() + 1, key.get());
            k += 1;
        };
        loop(k.get() < 8, insert);

        auto o = var(0u);
        auto store = [&]
        {
            write(output, id * 8u + o.get(), items[o.get()]);
            o += 1u;
        };
        loop(o.get() < 8u, store);
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;
    EACP_SHADER(input, output)
};

float sortValue(int index)
{
    return (float) ((index * 37 + 11) % 23) - 9.5f;
}

// Thirty-two signed samples a lane bins into eight counters; a sample below 0
// or above 7 is clamped by the private array's index into the first or the
// last bin.
struct HistogramKernel final : ComputeKernel
{
    static constexpr int samples = 32;
    static constexpr int bins = 8;

    HistogramKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto counts = privateArray<UInt, bins>();
        auto i = var(0u);

        auto bin = [&]
        {
            auto sample = input[id * (unsigned) samples + i.get()];
            counts.set(sample, counts[sample] + 1u);
            i += 1u;
        };
        loop(i.get() < (unsigned) samples, bin);

        for (auto b = 0; b < bins; ++b)
            write(output, id * (unsigned) bins + (unsigned) b, counts[b]);
    }

    Uniform<IntInputBuffer> input;
    Uniform<UIntOutputBuffer> output;
    EACP_SHADER(input, output)
};

std::int32_t histogramSample(int index)
{
    return (index * 13 + index / 5) % 15 - 4;
}

// Reads and writes at indices past either end, signed and unsigned, and an
// array given its initial values.
struct ClampKernel final : ComputeKernel
{
    ClampKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto items = privateArray(constant(10.f), constant(20.f), constant(30.f));
        auto offset = toInt(id % 9u) - 4;
        auto far = id * 1000u;

        write(output, id * 6u, items[offset]);
        write(output, id * 6u + 1u, items[far]);

        items.set(offset, toFloat(id));
        write(output, id * 6u + 2u, items[0]);
        write(output, id * 6u + 3u, items[2]);

        items.set(far, -1.f);
        write(output, id * 6u + 4u, items[2]);

        auto flags = privateArray<Bool, 2>();
        flags.set(offset, true);
        write(output, id * 6u + 5u, toFloat(flags[0]) + 2.f * toFloat(flags[1]));
    }

    Uniform<OutputBuffer> output;
    EACP_SHADER(output)
};

Array<float, 6> expectedClamp(int thread)
{
    auto items = Array<float, 3> {10.f, 20.f, 30.f};
    auto offset = std::clamp(thread % 9 - 4, 0, 2);
    auto far = thread * 1000 > 2 ? 2 : thread * 1000;
    auto result = Array<float, 6> {};

    result[0] = items[offset];
    result[1] = items[far];
    items[offset] = (float) thread;
    result[2] = items[0];
    result[3] = items[2];
    items[far] = -1.f;
    result[4] = items[2];
    result[5] = std::clamp(thread % 9 - 4, 0, 1) == 0 ? 1.f : 2.f;
    return result;
}

// The i32 buffers: an element, the three record reads, negatives through every
// write, and a record read back from the output it was written to.
struct IntBufferKernel final : ComputeKernel
{
    IntBufferKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto one = input[id];
        auto pair = input.read2(id);
        auto triple = input.read3(id);
        auto quad = input.read4(id);
        auto sum = one + pair.x() + pair.y() + triple.z() + quad.w();

        write(scalars, id, -sum);
        write(
            records, id, int4(-quad.x(), quad.y() * 2, quad.z() * -3, quad.w() * 4));
        write2(pairs, id, int2(pair.x() - 100, pair.y() - 100));
        write3(triples, id, triple);
        write(scalars, id + (unsigned) threadCount, records.read4(id).x() + 7);
    }

    Uniform<IntInputBuffer> input;
    Uniform<IntOutputBuffer> scalars;
    Uniform<IntOutputBuffer> records;
    Uniform<IntOutputBuffer> pairs;
    Uniform<IntOutputBuffer> triples;
    EACP_SHADER(input, scalars, records, pairs, triples)
};

std::int32_t intInput(int index)
{
    return (index * 29) % 41 - 20;
}

template <typename T>
Vector<T> filled(int count, T value)
{
    auto values = Vector<T> {};
    values.resize(count, value);
    return values;
}
} // namespace

auto tShapes2 = test("Types/matricesWithTwoColumnsMatchTheReference") = []
{
    checkShape<2, 2>();
    checkShape<2, 3>();
    checkShape<2, 4>();
};

auto tShapes3 = test("Types/matricesWithThreeColumnsMatchTheReference") = []
{
    checkShape<3, 2>();
    checkShape<3, 3>();
    checkShape<3, 4>();
};

auto tShapes4 = test("Types/matricesWithFourColumnsMatchTheReference") = []
{
    checkShape<4, 2>();
    checkShape<4, 3>();
    checkShape<4, 4>();
};

auto tSort = test("Types/eachLaneSortsItsOwnPrivateArray") = []
{
    for (const auto& options: batchings)
    {
        auto kernel = SortKernel {};
        auto executor = Executor {kernel, options};
        check(executor.isValid(), executor.reason());

        auto input = Vector<float> {};

        for (auto i = 0; i < threadCount * 8; ++i)
            input.add(sortValue(i));

        auto output = filled(threadCount * 8, sentinel);
        auto bindings = Bindings {};
        bindings.set(kernel.input, input);
        bindings.set(kernel.output, output);
        check(executor.dispatch(bindings, threadCount));

        auto expected = input;

        for (auto thread = 0; thread < threadCount; ++thread)
            std::sort(expected.begin() + thread * 8,
                      expected.begin() + thread * 8 + 8);

        check(output == expected);
    }
};

auto tHistogram = test("Types/aHistogramClampsItsBinsIntoTheArray") = []
{
    using Kernel = HistogramKernel;

    for (const auto& options: batchings)
    {
        auto kernel = Kernel {};
        auto executor = Executor {kernel, options};
        check(executor.isValid(), executor.reason());

        auto input = Vector<std::int32_t> {};

        for (auto i = 0; i < threadCount * Kernel::samples; ++i)
            input.add(histogramSample(i));

        auto output = filled(threadCount * Kernel::bins, 0xdeadbeefu);
        auto bindings = Bindings {};
        bindings.set(kernel.input, input);
        bindings.set(kernel.output, output);
        check(executor.dispatch(bindings, threadCount));

        auto expected = filled(threadCount * Kernel::bins, 0u);

        for (auto thread = 0; thread < threadCount; ++thread)
        {
            for (auto s = 0; s < Kernel::samples; ++s)
            {
                auto sample = input[thread * Kernel::samples + s];
                ++expected[thread * Kernel::bins + std::clamp(sample, 0, 7)];
            }
        }

        check(output == expected);
    }
};

auto tClamp = test("Types/anIndexPastEitherEndReachesTheEndElement") = []
{
    auto kernel = ClampKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto output = filled(threadCount * 6, sentinel);
    auto bindings = Bindings {};
    bindings.set(kernel.output, output);
    check(executor.dispatch(bindings, threadCount));

    auto mismatches = 0;

    for (auto thread = 0; thread < threadCount; ++thread)
    {
        auto expected = expectedClamp(thread);

        for (auto i = 0; i < 6; ++i)
            mismatches += output[thread * 6 + i] != expected[i];
    }

    check(mismatches == 0);
};

auto tIntBuffers = test("Types/intBuffersReadAndWriteSignedRecords") = []
{
    for (const auto& options: batchings)
    {
        auto kernel = IntBufferKernel {};
        auto executor = Executor {kernel, options};
        check(executor.isValid(), executor.reason());

        auto input = Vector<std::int32_t> {};

        for (auto i = 0; i < threadCount * 4; ++i)
            input.add(intInput(i));

        auto scalars = filled(threadCount * 2, std::int32_t {99});
        auto records = filled(threadCount * 4, std::int32_t {99});
        auto pairs = filled(threadCount * 2, std::int32_t {99});
        auto triples = filled(threadCount * 3, std::int32_t {99});

        auto bindings = Bindings {};
        bindings.set(kernel.input, input);
        bindings.set(kernel.scalars, scalars);
        bindings.set(kernel.records, records);
        bindings.set(kernel.pairs, pairs);
        bindings.set(kernel.triples, triples);
        check(executor.dispatch(bindings, threadCount));

        auto mismatches = 0;

        for (auto t = 0; t < threadCount; ++t)
        {
            auto sum = input[t] + input[2 * t] + input[2 * t + 1] + input[3 * t + 2]
                       + input[4 * t + 3];
            mismatches += scalars[t] != -sum;
            mismatches += records[4 * t] != -input[4 * t];
            mismatches += records[4 * t + 1] != 2 * input[4 * t + 1];
            mismatches += records[4 * t + 2] != -3 * input[4 * t + 2];
            mismatches += records[4 * t + 3] != 4 * input[4 * t + 3];
            mismatches += pairs[2 * t] != input[2 * t] - 100;
            mismatches += pairs[2 * t + 1] != input[2 * t + 1] - 100;

            for (auto c = 0; c < 3; ++c)
                mismatches += triples[3 * t + c] != input[3 * t + c];

            mismatches += scalars[threadCount + t] != -input[4 * t] + 7;
        }

        check(mismatches == 0);
    }
};

auto tBareGraph = test("Types/aBareGraphsArrayElementPlans") = []
{
    auto graph = ShaderGraph {};
    auto output = graph.addStorageBuffer(BufferAccess::Write, ValueType::UInt);
    auto thread = graph.addThreadId();
    auto slot =
        graph.addArrayVariable(ValueType::UInt, 4, graph.addZero(ValueType::UInt));
    graph.assignElement(slot, thread, thread);
    graph.addStore(
        output, thread, graph.addVarElementRead(slot, graph.addUIntConstant(3u)));

    auto executor = Executor {graph};
    check(executor.isValid(), executor.reason());

    auto words = filled(8, 0xdeadbeefu);
    auto bindings = Bindings {};
    bindings.set(UIntOutputBuffer {&graph, output}, words);
    check(executor.dispatch(bindings, 8));

    auto expected = Vector<std::uint32_t> {0u, 0u, 0u, 3u, 4u, 5u, 6u, 7u};
    check(words == expected);
};

auto tNoAllocation = test("Types/aDispatchAfterTheFirstAllocatesNothing") = []
{
    auto shape = ShapeKernel<3, 2> {};
    auto sort = SortKernel {};
    auto histogram = HistogramKernel {};
    auto ints = IntBufferKernel {};
    auto shapeExecutor = Executor {shape};
    auto sortExecutor = Executor {sort};
    auto histogramExecutor = Executor {histogram};
    auto intExecutor = Executor {ints};

    auto shapeInput = shapeInputs(ShapeKernel<3, 2>::inputStride);
    auto shapeOutput = filled(threadCount * ShapeKernel<3, 2>::outputStride, 0.f);
    auto sortInput = filled(threadCount * 8, 1.f);
    auto sortOutput = filled(threadCount * 8, 0.f);
    auto samples = filled(threadCount * HistogramKernel::samples, std::int32_t {3});
    auto counts = filled(threadCount * HistogramKernel::bins, 0u);
    auto intInputs = filled(threadCount * 4, std::int32_t {5});
    auto intScalars = filled(threadCount * 2, std::int32_t {0});
    auto intRecords = filled(threadCount * 4, std::int32_t {0});
    auto intPairs = filled(threadCount * 2, std::int32_t {0});
    auto intTriples = filled(threadCount * 3, std::int32_t {0});

    auto shapeBindings = Bindings {};
    shapeBindings.set(shape.values, shapeInput);
    shapeBindings.set(shape.output, shapeOutput);

    auto sortBindings = Bindings {};
    sortBindings.set(sort.input, sortInput);
    sortBindings.set(sort.output, sortOutput);

    auto histogramBindings = Bindings {};
    histogramBindings.set(histogram.input, samples);
    histogramBindings.set(histogram.output, counts);

    auto intBindings = Bindings {};
    intBindings.set(ints.input, intInputs);
    intBindings.set(ints.scalars, intScalars);
    intBindings.set(ints.records, intRecords);
    intBindings.set(ints.pairs, intPairs);
    intBindings.set(ints.triples, intTriples);

    auto dispatchAll = [&]
    {
        return shapeExecutor.dispatch(shapeBindings, threadCount)
               && sortExecutor.dispatch(sortBindings, threadCount)
               && histogramExecutor.dispatch(histogramBindings, threadCount)
               && intExecutor.dispatch(intBindings, threadCount);
    };

    auto warmedUp = dispatchAll();

    ExecutorTests::allocationsCounted = 0;
    ExecutorTests::countingHeap = true;
    auto ran = dispatchAll();
    ExecutorTests::countingHeap = false;

    check(warmedUp);
    check(ran);
    check(ExecutorTests::allocationsCounted.load() == 0);
};
} // namespace TypesTests
