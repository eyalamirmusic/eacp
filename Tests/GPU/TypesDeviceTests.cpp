#include "CpuCrossCheck.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>

// The types of WGSL's stage 9, part A, on a device. Every float matrix shape's
// operations run as a kernel on the GPU and the CPU executor and must agree
// with each other and with a C++ reference word for word - the inputs are
// quarters, so every product and sum is exact whichever way a backend fuses
// it. Private arrays sort and bin per lane, clamp indices past either end and
// hold matrices; the i32 buffers read and write signed records; every matrix
// shape crosses as a uniform between scalars and is read back element for
// element. The render stages are read back from the pixels: a fragment picks
// its shade out of a private array it wrote past the end of, a vertex function
// places a quad's corners out of one, and a fragment reads two-row and
// three-row uniform matrices.
//
// The kernels' CPU halves never self-skip; everything else self-skips without
// a device.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::GPU::CrossChecks;

namespace TypesDeviceTests
{
namespace
{
constexpr auto threadCount = 70;
constexpr auto sentinel = -1234.5f;
constexpr auto targetSize = 4;
constexpr auto pixelCount = targetSize * targetSize;

struct QuadVertex
{
    float position[2];
};
} // namespace
} // namespace TypesDeviceTests

EACP_SHADER_VALUE(TypesDeviceTests::QuadVertex, Float2)

namespace TypesDeviceTests
{
namespace
{
constexpr QuadVertex fullQuad[] = {
    {{-1.f, -1.f}},
    {{1.f, -1.f}},
    {{-1.f, 1.f}},
    {{1.f, -1.f}},
    {{1.f, 1.f}},
    {{-1.f, 1.f}},
};

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

// Writes each component in order through the kernel's own store.
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

template <int Columns, int Rows>
struct ShapeKernel final : ComputeProgram
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
                sum += left.at(k, row) * right.at(column, k);

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
    void put(const Matrix& m)
    {
        for (auto i = 0; i < m.columns * m.rows; ++i)
            values.add(m.elements[i]);
    }

    void putColumn(const Matrix& m, int column)
    {
        for (auto row = 0; row < m.rows; ++row)
            values.add(m.at(column, row));
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

    auto plus = [](float x, float y) { return x + y; };
    auto minus = [](float x, float y) { return x - y; };
    auto twice = [](float x, float) { return 2.f * x; };
    auto half = [](float x, float) { return x * 0.5f; };
    auto quarter = [](float x, float) { return x / 4.f; };

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
void checkShape(bool cpuOnly = false)
{
    using Kernel = ShapeKernel<Columns, Rows>;
    auto kernel = Kernel {};
    auto values = shapeInputs(Kernel::inputStride);
    auto expected = expectedShape<Columns, Rows>(values);
    auto name = std::to_string(Columns) + "x" + std::to_string(Rows);

    auto verify = [&](const Readback& readback)
    {
        check(readback.floats(kernel.output) == expected,
              std::string(readback.name()) + " " + name);
    };

    CrossCheck {kernel}
        .input(kernel.values, values)
        .output(kernel.output, threadCount * Kernel::outputStride, sentinel)
        .agreeing()
        .cpuOnlyWhen(cpuOnly)
        .run(threadCount, verify);
}

struct SortKernel final : ComputeProgram
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

struct HistogramKernel final : ComputeProgram
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

struct ClampKernel final : ComputeProgram
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

Vector<float> expectedClamp()
{
    auto expected = Vector<float> {};

    for (auto thread = 0; thread < threadCount; ++thread)
    {
        auto items = Array<float, 3> {10.f, 20.f, 30.f};
        auto offset = std::clamp(thread % 9 - 4, 0, 2);
        auto far = thread == 0 ? 0 : 2;

        expected.add(items[offset]);
        expected.add(items[far]);
        items[offset] = (float) thread;
        expected.add(items[0]);
        expected.add(items[2]);
        items[far] = -1.f;
        expected.add(items[2]);
        expected.add(std::clamp(thread % 9 - 4, 0, 1) == 0 ? 1.f : 2.f);
    }

    return expected;
}

struct IntBufferKernel final : ComputeProgram
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

// Every matrix shape between scalars, read back element by element through
// its columns: the GPU's block is the padded one the upload walk lays down,
// the CPU's the tight one, and both must read the same numbers.
struct UniformKernel final : ComputeProgram
{
    UniformKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto store = [&](const UInt& at, const Float& value)
        { write(output, at, value); };
        auto out = Emitter<decltype(store)> {store, id * 0u};

        out.put(s0);
        out.put(m22);
        out.put(s1);
        out.put(m23);
        out.put(m24);
        out.put(m32);
        out.put(m33);
        out.put(s2);
        out.put(m34);
        out.put(m42);
        out.put(m43);
        out.put(m44);
        out.put(s3);
    }

    Uniform<Float> s0;
    Uniform<Float2x2> m22;
    Uniform<Float> s1;
    Uniform<Float2x3> m23;
    Uniform<Float2x4> m24;
    Uniform<Float3x2> m32;
    Uniform<Float3x3> m33;
    Uniform<Float> s2;
    Uniform<Float3x4> m34;
    Uniform<Float4x2> m42;
    Uniform<Float4x3> m43;
    Uniform<Float4x4> m44;
    Uniform<Float> s3;
    Uniform<OutputBuffer> output;
    EACP_SHADER(s0, m22, s1, m23, m24, m32, m33, s2, m34, m42, m43, m44, s3, output)
};

constexpr auto uniformFloats = 4 + 4 + 6 + 8 + 6 + 9 + 12 + 8 + 12 + 16;

template <typename Cpu>
Cpu numbered(float& next)
{
    auto value = Cpu {};

    for (auto& element: value)
        element = next++;

    return value;
}

void fillUniforms(UniformKernel& kernel)
{
    auto next = 1.f;
    kernel.s0 = next++;
    kernel.m22 = numbered<Array<float, 4>>(next);
    kernel.s1 = next++;
    kernel.m23 = numbered<Array<float, 6>>(next);
    kernel.m24 = numbered<Array<float, 8>>(next);
    kernel.m32 = numbered<Array<float, 6>>(next);
    kernel.m33 = numbered<Array<float, 9>>(next);
    kernel.s2 = next++;
    kernel.m34 = numbered<Array<float, 12>>(next);
    kernel.m42 = numbered<Array<float, 8>>(next);
    kernel.m43 = numbered<Array<float, 12>>(next);
    kernel.m44 = numbered<Array<float, 16>>(next);
    kernel.s3 = next++;
}

bool canTest()
{
    return Device::shared().isValid();
}

TextureDescriptor describeFloatTarget()
{
    auto descriptor = TextureDescriptor {};
    descriptor.width = targetSize;
    descriptor.height = targetSize;
    descriptor.format = TextureFormat::R32Float;
    descriptor.renderTarget = true;
    return descriptor;
}

RenderPipelineDescriptor describeFloatPipeline()
{
    auto descriptor = RenderPipelineDescriptor {};
    descriptor.sampleCount = 1;
    descriptor.colorFormat = pixelFormatFor(TextureFormat::R32Float);
    return descriptor;
}

struct FrameRunner final : GPUView
{
    FrameRunner() { setSampleCount(1); }

    void render(Frame& frame) override
    {
        draw(frame);
        frame.beginPass({Graphics::Color::black()});
    }

    std::function<void(Frame&)> draw = [](Frame&) {};
};

using FloatPixels = Array<float, pixelCount>;

// One pass into a fresh R32Float target cleared to -1, read back.
FloatPixels drawAndRead(const std::function<void(RenderPass&)>& draw)
{
    auto target = Device::shared().makeTexture(describeFloatTarget());
    auto pixels = FloatPixels {};
    auto view = FrameRunner {};

    auto drawFrame = [&](Frame& frame)
    {
        {
            auto descriptor = RenderPassDescriptor {};
            descriptor.clearColor = {-1.f, 0.f, 0.f, 1.f};

            auto pass = frame.beginPass(target, descriptor);
            draw(pass);
        }

        frame.flush();
        target.read(pixels.data());
    };

    view.draw = drawFrame;
    view.setBounds({0.f, 0.f, (float) targetSize, (float) targetSize});
    check(view.renderToImage(1.f).isValid());
    return pixels;
}

bool near(float value, float expected)
{
    return std::abs(value - expected) < 0.002f;
}

// Each column's shade out of a private array, after a write one element to
// its right - which the last column's index clamps onto its own element.
struct FragmentArrayShader final : ShaderProgram
{
    FragmentArrayShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);
        setPosition(float4(position, 0.f, 1.f));

        auto shades = privateArray(
            constant(0.1f), constant(0.2f), constant(0.3f), constant(0.4f));
        auto column = toInt(fragmentCoordinate().x());
        shades.set(column + 1, 0.9f);
        setFragment(float4(shades[column], 0.f, 0.f, 1.f));
    }

    EACP_SHADER()
};

// The quad's left half, each corner's x picked out of a private array by
// vertexId().
struct VertexArrayShader final : ShaderProgram
{
    VertexArrayShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);

        auto place = [&]
        {
            auto xs = privateArray(constant(-1.f),
                                   constant(0.f),
                                   constant(-1.f),
                                   constant(0.f),
                                   constant(0.f),
                                   constant(-1.f));
            setPosition(float4(xs[vertexId()], position.y(), 0.f, 1.f));
        };

        vertexStage(place);
        setFragment(float4(constant(1.f), 0.f, 0.f, 1.f));
    }

    EACP_SHADER()
};

// A fragment reading a two-row and a three-row uniform matrix.
struct UniformMatrixShader final : ShaderProgram
{
    UniformMatrixShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);
        setPosition(float4(position, 0.f, 1.f));

        auto ones = float3(constant(1.f), 1.f, 1.f);
        auto value = (wide * ones).y() + narrow[1].x() + tall[1].z();
        setFragment(float4(value, 0.f, 0.f, 1.f));
    }

    Uniform<Float3x2> wide;
    Uniform<Float2x2> narrow;
    Uniform<Float2x3> tall;
    EACP_SHADER(wide, narrow, tall)
};
} // namespace

auto tShapes2 = test("TypesDevice/matricesWithTwoColumnsAgreeWithTheReference") = []
{
    checkShape<2, 2>();
    checkShape<2, 3>();
    checkShape<2, 4>();
};

auto tShapes3 =
    test("TypesDevice/matricesWithThreeColumnsAgreeWithTheReference") = []
{
    checkShape<3, 2>();
    checkShape<3, 3>();
    checkShape<3, 4>();
};

auto tShapes4 = test("TypesDevice/matricesWithFourColumnsAgreeWithTheReference") = []
{
    checkShape<4, 2>();
    checkShape<4, 3>();
    checkShape<4, 4>(isWarpOnArm64());
};

auto tSort = test("TypesDevice/eachLaneSortsItsOwnPrivateArray") = []
{
    auto kernel = SortKernel {};
    auto input = Vector<float> {};

    for (auto i = 0; i < threadCount * 8; ++i)
        input.add((float) ((i * 37 + 11) % 23) - 9.5f);

    auto expected = input;

    for (auto thread = 0; thread < threadCount; ++thread)
        std::sort(expected.begin() + thread * 8, expected.begin() + thread * 8 + 8);

    auto verify = [&](const Readback& readback)
    { check(readback.floats(kernel.output) == expected, readback.name()); };

    CrossCheck {kernel}
        .input(kernel.input, input)
        .output(kernel.output, threadCount * 8, sentinel)
        .agreeing()
        .run(threadCount, verify);
};

auto tHistogram = test("TypesDevice/aHistogramClampsItsBinsIntoTheArray") = []
{
    using Kernel = HistogramKernel;
    auto kernel = Kernel {};
    auto input = Vector<std::int32_t> {};

    for (auto i = 0; i < threadCount * Kernel::samples; ++i)
        input.add((i * 13 + i / 5) % 15 - 4);

    auto expected = filled(threadCount * Kernel::bins, 0u);

    for (auto thread = 0; thread < threadCount; ++thread)
        for (auto s = 0; s < Kernel::samples; ++s)
            ++expected[thread * Kernel::bins
                       + std::clamp(input[thread * Kernel::samples + s], 0, 7)];

    auto verify = [&](const Readback& readback)
    { check(readback.uints(kernel.output) == expected, readback.name()); };

    CrossCheck {kernel}
        .input(kernel.input, input)
        .output(kernel.output, threadCount * Kernel::bins, 0xdeadbeefu)
        .agreeing()
        .run(threadCount, verify);
};

auto tClamp = test("TypesDevice/anIndexPastEitherEndReachesTheEndElement") = []
{
    auto kernel = ClampKernel {};
    auto expected = expectedClamp();

    auto verify = [&](const Readback& readback)
    { check(readback.floats(kernel.output) == expected, readback.name()); };

    CrossCheck {kernel}
        .output(kernel.output, threadCount * 6, sentinel)
        .agreeing()
        .run(threadCount, verify);
};

auto tIntBuffers = test("TypesDevice/intBuffersReadAndWriteSignedRecords") = []
{
    auto kernel = IntBufferKernel {};
    auto input = Vector<std::int32_t> {};

    for (auto i = 0; i < threadCount * 4; ++i)
        input.add(intInput(i));

    auto verify = [&](const Readback& readback)
    {
        auto scalars = readback.ints(kernel.scalars);
        auto records = readback.ints(kernel.records);
        auto pairs = readback.ints(kernel.pairs);
        auto triples = readback.ints(kernel.triples);
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

        check(mismatches == 0, readback.name());
    };

    CrossCheck {kernel}
        .input(kernel.input, input)
        .output(kernel.scalars, filled(threadCount * 2, std::int32_t {99}))
        .output(kernel.records, filled(threadCount * 4, std::int32_t {99}))
        .output(kernel.pairs, filled(threadCount * 2, std::int32_t {99}))
        .output(kernel.triples, filled(threadCount * 3, std::int32_t {99}))
        .agreeing()
        .run(threadCount, verify);
};

auto tUniforms = test("TypesDevice/everyMatrixShapeCrossesAsAUniform") = []
{
    auto kernel = UniformKernel {};
    fillUniforms(kernel);

    auto expected = Vector<float> {};

    for (auto i = 0; i < uniformFloats; ++i)
        expected.add((float) (i + 1));

    auto verify = [&](const Readback& readback)
    { check(readback.floats(kernel.output) == expected, readback.name()); };

    CrossCheck {kernel}
        .output(kernel.output, uniformFloats, sentinel)
        .agreeing()
        .run(1, verify);
};

auto tFragmentArray =
    test("TypesDevice/aFragmentPicksItsShadeOutOfAPrivateArray") = []
{
    if (!canTest())
        return;

    auto program = FragmentArrayShader {};
    program.setVertices(fullQuad);
    program.prepare(describeFloatPipeline());

    auto draw = [&](RenderPass& pass) { pass.draw(program); };
    auto pixels = drawAndRead(draw);
    constexpr auto shades = Array<float, targetSize> {0.1f, 0.2f, 0.3f, 0.9f};

    for (auto i = 0; i < pixelCount; ++i)
        check(near(pixels[i], shades[i % targetSize]), std::to_string(pixels[i]));
};

auto tVertexArray = test("TypesDevice/aVertexPlacesTheQuadOutOfAPrivateArray") = []
{
    if (!canTest())
        return;

    auto program = VertexArrayShader {};
    program.setVertices(fullQuad);
    program.prepare(describeFloatPipeline());

    auto draw = [&](RenderPass& pass) { pass.draw(program); };
    auto pixels = drawAndRead(draw);

    for (auto i = 0; i < pixelCount; ++i)
        check(near(pixels[i], i % targetSize < targetSize / 2 ? 1.f : -1.f),
              std::to_string(pixels[i]));
};

auto tUniformRender = test("TypesDevice/aFragmentReadsUniformMatrices") = []
{
    if (!canTest())
        return;

    auto program = UniformMatrixShader {};
    program.wide = {1.f, 2.f, 3.f, 4.f, 5.f, 6.f};
    program.narrow = {7.f, 8.f, 9.f, 10.f};
    program.tall = {11.f, 12.f, 13.f, 14.f, 15.f, 16.f};
    program.setVertices(fullQuad);
    program.prepare(describeFloatPipeline());

    auto draw = [&](RenderPass& pass) { pass.draw(program); };
    auto pixels = drawAndRead(draw);
    auto expected = (2.f + 4.f + 6.f) + 9.f + 16.f;

    for (auto i = 0; i < pixelCount; ++i)
        check(near(pixels[i], expected), std::to_string(pixels[i]));
};
} // namespace TypesDeviceTests
