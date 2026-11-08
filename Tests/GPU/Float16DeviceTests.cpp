#include "CpuCrossCheck.h"

#include <eacp/Core/Platform/Platform.h>
#include <eacp/Core/Utils/Environment.h>
#include <eacp/GPU/CpuCompute/Helpers.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <string>

// WGSL's f16 on a device, every kernel cross-checked against the CPU executor
// (which computes each f16 operation in float and rounds it to the nearest
// half, as HLSL does). Correctly rounded arithmetic - a sum, a difference, a
// product, a conversion, a literal - agrees bit for bit, and so does every
// matrix shape over quarters, which no rounding touches; the built-ins agree
// to sixteen f16 ulps of their result or of a quarter, whichever is larger -
// Metal's half library is its own, and reflect, refract and dot cancel in
// half where the CPU cancels in float before it rounds. The f16 buffers'
// elements, records and read-backs agree exactly, adjacent threads writing the
// two halves of one word. A fragment computes in f16 over an f16 buffer.
//
// The CPU halves never self-skip; the GPU halves self-skip without a device,
// or on one whose Device::supportsShaderFloat16 answers no.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::GPU::CrossChecks;

namespace Float16DeviceTests
{
namespace
{
constexpr auto threadCount = 75;
constexpr auto halfSentinel = std::uint16_t {0xcafeu};
constexpr auto targetSize = 4;
constexpr auto pixelCount = targetSize * targetSize;

struct QuadVertex
{
    float position[2];
};
} // namespace
} // namespace Float16DeviceTests

EACP_SHADER_VALUE(Float16DeviceTests::QuadVertex, Float2)

namespace Float16DeviceTests
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

std::uint16_t halfBits(float value)
{
    return CpuCompute::narrowToHalf(value);
}

float widened(std::uint16_t bits)
{
    return CpuCompute::widenHalf(bits);
}

float rounded(float value)
{
    return widened(halfBits(value));
}

Vector<std::uint16_t> halfInputs(int count, float low, float span, int salt)
{
    auto values = Vector<std::uint16_t> {};

    for (auto element = 0; element < count; ++element)
    {
        auto step = (float) ((element * 37 + salt * 11) % 101) / 101.f;
        values.add(halfBits(low + span * step));
    }

    return values;
}

// Quarters in [-2, 2]: every product and sum of a few stays exact in f16.
Vector<std::uint16_t> quarterInputs(int count)
{
    auto values = Vector<std::uint16_t> {};

    for (auto element = 0; element < count; ++element)
        values.add(halfBits((float) ((element * 7 + 3) % 17 - 8) * 0.25f));

    return values;
}

bool gpuRunsHalves()
{
    return Device::shared().isValid() && Device::shared().supportsShaderFloat16();
}

// One correctly rounded operation per element, and every conversion.
struct ArithmeticKernel final : ComputeProgram
{
    static constexpr int outputs = 10;

    ArithmeticKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto base = id * (unsigned) outputs;
        auto x = a[id];
        auto y = b[id];

        write(output, base, x + y);
        write(output, base + 1u, x - y);
        write(output, base + 2u, x * y);
        write(output, base + 3u, -x);
        write(output, base + 4u, x * 0.1f);
        write(output, base + 5u, toHalf(floats[id]));
        write(output, base + 6u, toHalf(toInt(id) * 1031 - 30000));
        write(output, base + 7u, select(x < y, x, y));
        write(output, base + 8u, abs(x) + halfConstant(1000.f));
        write(output, base + 9u, toHalf(toUInt(abs(y) * 10.f)));

        write(widenedOutput, id, toFloat(x) + 0.5f);
        write(ints, id, toInt(x * 4.f));
    }

    Uniform<HalfInputBuffer> a;
    Uniform<HalfInputBuffer> b;
    Uniform<InputBuffer> floats;
    Uniform<HalfOutputBuffer> output;
    Uniform<OutputBuffer> widenedOutput;
    Uniform<IntOutputBuffer> ints;
    EACP_SHADER(a, b, floats, output, widenedOutput, ints)
};

float arithmeticFloat(int thread)
{
    return (float) (thread - 37) * 1.37f + 0.001f * (float) thread;
}

// Every f16 built-in, one per record. a holds (0.05, 0.95), b (1.05, 3), c
// (-2, 2).
struct BuiltinKernel final : ComputeProgram
{
    static constexpr int records = 41;

    BuiltinKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto x = a.read3(id);
        auto y = b.read3(id);
        auto z = c.read3(id);
        auto at = 0u;
        auto put = [&](const Half3& value)
        { write(output, id * (unsigned) records + at++, value); };

        put(sin(x));
        put(cos(x));
        put(tan(x));
        put(asin(x));
        put(acos(x));
        put(atan(x));
        put(sinh(x));
        put(cosh(x));
        put(tanh(x));
        put(asinh(z));
        put(atanh(x));
        put(exp(x));
        put(exp2(x));
        put(log(y));
        put(log2(y));
        put(sqrt(y));
        put(rsqrt(y));
        put(floor(z));
        put(ceil(z));
        put(trunc(z));
        put(fract(z));
        put(sign(z));
        put(normalize(z));
        put(acosh(y));
        put(atan2(z, x));
        put(pow(x, z * 0.5f));
        put(min(z, x));
        put(max(z, x));
        put(step(x, z));
        put(z / x);
        put(clamp(z, x, y));
        put(mix(x, y, z));
        put(smoothstep(x, y, z));
        put(fma(x, y, z));
        put(cross(x, z));
        put(reflect(z, x));
        put(refract(z, normalize(x), x.y()));
        put(faceforward(x, y, z));
        put(half3(dot(x, z), length(z), distance(x, z)));
        put(degrees(x) * 0.01f);
        put(round(z));
    }

    Uniform<HalfInputBuffer> a;
    Uniform<HalfInputBuffer> b;
    Uniform<HalfInputBuffer> c;
    Uniform<HalfOutputBuffer> output;
    EACP_SHADER(a, b, c, output)
};

template <int Columns, int Rows, typename... Vectors>
HalfMatrixOf<Columns, Rows> halfMatrixOf(const Vectors&... columns)
{
    if constexpr (Columns == 2 && Rows == 2)
        return half2x2(columns...);
    else if constexpr (Columns == 2 && Rows == 3)
        return half2x3(columns...);
    else if constexpr (Columns == 2 && Rows == 4)
        return half2x4(columns...);
    else if constexpr (Columns == 3 && Rows == 2)
        return half3x2(columns...);
    else if constexpr (Columns == 3 && Rows == 3)
        return half3x3(columns...);
    else if constexpr (Columns == 3 && Rows == 4)
        return half3x4(columns...);
    else if constexpr (Columns == 4 && Rows == 2)
        return half4x2(columns...);
    else if constexpr (Columns == 4 && Rows == 3)
        return half4x3(columns...);
    else
        return half4x4(columns...);
}

template <int Width>
HalfVectorOf<Width> readVector(const HalfInputBuffer& values, const UInt& first)
{
    auto at = [&](unsigned offset) { return values[first + offset]; };

    if constexpr (Width == 2)
        return half2(at(0u), at(1u));
    else if constexpr (Width == 3)
        return half3(at(0u), at(1u), at(2u));
    else
        return half4(at(0u), at(1u), at(2u), at(3u));
}

template <int Columns, int Rows>
HalfMatrixOf<Columns, Rows> readMatrix(const HalfInputBuffer& values,
                                       const UInt& first)
{
    auto column = [&](int which)
    { return readVector<Rows>(values, first + (unsigned) (which * Rows)); };

    if constexpr (Columns == 2)
        return halfMatrixOf<Columns, Rows>(column(0), column(1));
    else if constexpr (Columns == 3)
        return halfMatrixOf<Columns, Rows>(column(0), column(1), column(2));
    else
        return halfMatrixOf<Columns, Rows>(
            column(0), column(1), column(2), column(3));
}

// Every operation of one f16 matrix shape, over quarters: exact on both.
template <int Columns, int Rows>
struct MatrixKernel final : ComputeProgram
{
    static constexpr int inputStride = 2 * Columns * Rows + Columns + Rows;
    static constexpr int outputStride = Rows + Columns + Rows * Rows
                                        + 4 * Columns * Rows + Rows * Columns + Rows
                                        + (Columns == Rows ? 1 : 0);

    MatrixKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto first = id * (unsigned) inputStride;
        auto cr = (unsigned) (Columns * Rows);

        auto m = readMatrix<Columns, Rows>(values, first);
        auto n = readMatrix<Columns, Rows>(values, first + cr);
        auto v = readVector<Columns>(values, first + 2u * cr);
        auto w = readVector<Rows>(values, first + 2u * cr + (unsigned) Columns);
        auto at = id * (unsigned) outputStride;

        auto putScalar = [&](const Half& value)
        {
            write(output, at, value);
            at = at + 1u;
        };

        auto putColumn = [&](const HalfVectorOf<Rows>& value)
        {
            putScalar(value.x());
            putScalar(value.y());

            if constexpr (Rows > 2)
                putScalar(value.z());

            if constexpr (Rows > 3)
                putScalar(value.w());
        };

        auto putRow = [&](const HalfVectorOf<Columns>& value)
        {
            putScalar(value.x());
            putScalar(value.y());

            if constexpr (Columns > 2)
                putScalar(value.z());

            if constexpr (Columns > 3)
                putScalar(value.w());
        };

        auto putMatrix = [&](const HalfMatrixOf<Columns, Rows>& matrix)
        {
            for (auto column = 0; column < Columns; ++column)
                putColumn(matrix[column]);
        };

        putColumn(m * v);
        putRow(w * m);

        auto square = m * transpose(n);

        for (auto column = 0; column < Rows; ++column)
        {
            auto picked = square[column];
            putScalar(picked.x());
            putScalar(picked.y());

            if constexpr (Rows > 2)
                putScalar(picked.z());

            if constexpr (Rows > 3)
                putScalar(picked.w());
        }

        putMatrix(m + n);
        putMatrix(m - n);
        putMatrix(m * 2.f);
        putMatrix(m / 4.f);

        auto flipped = transpose(m);

        for (auto row = 0; row < Rows; ++row)
            putRow(flipped[row]);

        putColumn(m[toInt(id % 7u) - 2]);

        if constexpr (Columns == Rows)
            putScalar(determinant(wholeOf(m)));
    }

    // The matrix floored to integers in [-2, 2], whose determinant every
    // backend holds exactly whatever order it multiplies in.
    static HalfMatrixOf<Columns, Rows> wholeOf(const HalfMatrixOf<Columns, Rows>& m)
    {
        if constexpr (Columns == 2)
            return halfMatrixOf<Columns, Rows>(floor(m[0]), floor(m[1]));
        else if constexpr (Columns == 3)
            return halfMatrixOf<Columns, Rows>(
                floor(m[0]), floor(m[1]), floor(m[2]));
        else
            return halfMatrixOf<Columns, Rows>(
                floor(m[0]), floor(m[1]), floor(m[2]), floor(m[3]));
    }

    Uniform<HalfInputBuffer> values;
    Uniform<HalfOutputBuffer> output;
    EACP_SHADER(values, output)
};

// Adjacent threads write the two halves of one word, records go both ways,
// and an output is read back after it was written.
struct BufferKernel final : ComputeProgram
{
    BufferKernel() { compile(); }

    void define() override
    {
        auto id = threadId();

        write(scalars, id, input[id] * 2.f);
        write(records, id, input.read3(id).zxy());
        write(scalars,
              id + (unsigned) threadCount,
              records.read3(id).y() + input.read2(id).x());
        write(quads, id, half4(input.read4(id).wzy(), scalars[id]));
    }

    Uniform<HalfInputBuffer> input;
    Uniform<HalfOutputBuffer> scalars;
    Uniform<HalfOutputBuffer> records;
    Uniform<HalfOutputBuffer> quads;
    EACP_SHADER(input, scalars, records, quads)
};

// A sum rounded at every step through a var, out of a private array.
struct SumKernel final : ComputeProgram
{
    static constexpr int terms = 9;

    SumKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto items = privateArray<Half, terms>();
        auto total = var(halfConstant(0.f));
        auto counter = var(0);

        auto fill = [&]
        {
            items.set(counter.get(),
                      input[id * (unsigned) terms + toUInt(counter.get())]);
            counter += 1;
        };
        loop(counter.get() < terms, fill);

        counter = 0;

        auto add = [&]
        {
            total += items[counter.get()];
            counter += 1;
        };
        loop(counter.get() < terms, add);

        write(output, id, total.get());
    }

    Uniform<HalfInputBuffer> input;
    Uniform<HalfOutputBuffer> output;
    EACP_SHADER(input, output)
};

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

// Each column's shade in f16: its x through a half matrix and an f16 buffer.
struct FragmentShader final : ShaderProgram
{
    FragmentShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);
        setPosition(float4(position, 0.f, 1.f));

        auto column = toHalf(floor(fragmentCoordinate().x()));
        auto scale = half2x2(half2(shades[0u], 0.f), half2(0.f, shades[1u]));
        auto shaded = scale * half2(column, 1.f);
        setFragment(float4(toFloat(shaded.x() + shaded.y()), 0.f, 0.f, 1.f));
    }

    Uniform<HalfInputBuffer> shades;
    EACP_SHADER(shades)
};
} // namespace

auto tQuery = test("Float16Device/theDeviceAnswersTheQuery") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
    {
        check(!device.supportsShaderFloat16());
        return;
    }

    if (Platform::isApple() || Platform::isWindows())
        check(device.supportsShaderFloat16());

    // Mesa's lavapipe, the CI device, has both features, so there the GPU
    // halves of this suite are known to run rather than to have skipped.
    if (Platform::isLinux() && getEnvValue("EACP_VK_SOFTWARE") == "1")
        check(device.supportsShaderFloat16());
};

auto tArithmetic = test("Float16Device/correctlyRoundedOperationsAgreeExactly") = []
{
    auto kernel = ArithmeticKernel {};
    auto a = halfInputs(threadCount, -40.f, 80.f, 1);
    auto b = halfInputs(threadCount, -3.f, 6.f, 2);
    auto floats = Vector<float> {};

    for (auto thread = 0; thread < threadCount; ++thread)
        floats.add(arithmeticFloat(thread));

    auto verify = [&](const Readback& readback)
    {
        const auto& halves = readback.halves(kernel.output);
        auto ints = readback.ints(kernel.ints);
        auto agreeing = 0;

        for (auto thread = 0; thread < threadCount; ++thread)
        {
            const auto* got = halves.data() + thread * ArithmeticKernel::outputs;
            auto x = widened(a[thread]);
            auto y = widened(b[thread]);
            auto tenth = rounded(0.1f);
            auto truncated = (float) (unsigned) (rounded(std::abs(y) * 10.f));

            auto expected = Array<std::uint16_t, ArithmeticKernel::outputs> {
                halfBits(x + y),
                halfBits(x - y),
                halfBits(x * y),
                halfBits(-x),
                halfBits(x * tenth),
                halfBits(floats[thread]),
                halfBits((float) (thread * 1031 - 30000)),
                halfBits(x < y ? x : y),
                halfBits(std::abs(x) + 1000.f),
                halfBits(truncated)};

            auto matches =
                std::equal(expected.begin(), expected.end(), got)
                && ints[thread] == (int) rounded(x * 4.f)
                && readback.floats(kernel.widenedOutput)[thread] == x + 0.5f;
            agreeing += matches ? 1 : 0;
        }

        check(agreeing == threadCount, readback.name());
    };

    CrossCheck {kernel}
        .input(kernel.a, a)
        .input(kernel.b, b)
        .input(kernel.floats, floats)
        .output(kernel.output,
                filled(threadCount * ArithmeticKernel::outputs, halfSentinel))
        .output(kernel.widenedOutput, threadCount)
        .output(kernel.ints, filled(threadCount, std::int32_t {0}))
        .agreeing()
        .run(threadCount, verify);
};

auto tBuiltins = test("Float16Device/everyBuiltinAgreesToAFewUlps") = []
{
    auto kernel = BuiltinKernel {};
    auto count = threadCount * 3;
    auto a = halfInputs(count, 0.05f, 0.9f, 1);
    auto b = halfInputs(count, 1.05f, 1.95f, 2);
    auto c = halfInputs(count, -2.f, 4.f, 3);
    auto cpu = Vector<float> {};
    auto worstUlps = 0.f;
    auto worstRecord = -1;

    auto verify = [&](const Readback& readback)
    {
        const auto& halves = readback.halves(kernel.output);

        if (readback.backend == Backend::Cpu)
        {
            for (auto bits: halves)
                cpu.add(widened(bits));

            return;
        }

        auto agreeing = 0;

        for (auto i = 0; i < halves.size() && i < cpu.size(); ++i)
        {
            auto magnitude = std::max(std::abs(cpu[i]), 0.25f);
            auto ulp = std::ldexp(1.f, std::ilogb(magnitude) - 10);
            auto ulps = std::abs(widened(halves[i]) - cpu[i]) / ulp;

            if (ulps > worstUlps)
            {
                worstUlps = ulps;
                worstRecord = (i / 3) % BuiltinKernel::records;
            }

            agreeing += ulps <= 16.f ? 1 : 0;
        }

        check(agreeing == cpu.size(),
              "worst disagreement " + std::to_string(worstUlps)
                  + " f16 ulps, in record " + std::to_string(worstRecord));
    };

    CrossCheck {kernel}
        .input(kernel.a, a)
        .input(kernel.b, b)
        .input(kernel.c, c)
        .output(kernel.output,
                filled(threadCount * BuiltinKernel::records * 3, halfSentinel))
        .run(threadCount, verify);
};

// Laplace along the first column of a column-major order x order matrix.
float determinantOf(const Array<float, 16>& matrix, int order)
{
    if (order == 1)
        return matrix[0];

    auto total = 0.f;

    for (auto skipped = 0; skipped < order; ++skipped)
    {
        auto minor = Array<float, 16> {};
        auto at = 0;

        for (auto column = 1; column < order; ++column)
            for (auto row = 0; row < order; ++row)
                if (row != skipped)
                    minor[at++] = matrix[column * order + row];

        auto sign = skipped % 2 == 0 ? 1.f : -1.f;
        total += sign * matrix[skipped] * determinantOf(minor, order - 1);
    }

    return total;
}

// What every f16 result must equal: the f32 one over the same quarters, which
// no rounding touches.
template <int Columns, int Rows>
void checkShape(
    const std::source_location& location = std::source_location::current())
{
    using Kernel = MatrixKernel<Columns, Rows>;
    auto kernel = Kernel {};
    auto values = quarterInputs(threadCount * Kernel::inputStride);

    auto at = [&](int thread, int element)
    { return widened(values[thread * Kernel::inputStride + element]); };

    auto expected = Vector<std::uint16_t> {};

    for (auto thread = 0; thread < threadCount; ++thread)
    {
        auto cr = Columns * Rows;
        auto m = [&](int column, int row)
        { return at(thread, column * Rows + row); };
        auto n = [&](int column, int row)
        { return at(thread, cr + column * Rows + row); };
        auto v = [&](int column) { return at(thread, 2 * cr + column); };
        auto w = [&](int row) { return at(thread, 2 * cr + Columns + row); };
        auto put = [&](float value) { expected.add(halfBits(value)); };

        for (auto row = 0; row < Rows; ++row)
        {
            auto sum = 0.f;

            for (auto k = 0; k < Columns; ++k)
                sum += m(k, row) * v(k);

            put(sum);
        }

        for (auto column = 0; column < Columns; ++column)
        {
            auto sum = 0.f;

            for (auto k = 0; k < Rows; ++k)
                sum += w(k) * m(column, k);

            put(sum);
        }

        for (auto column = 0; column < Rows; ++column)
            for (auto row = 0; row < Rows; ++row)
            {
                auto sum = 0.f;

                for (auto k = 0; k < Columns; ++k)
                    sum += m(k, row) * n(k, column);

                put(sum);
            }

        for (auto column = 0; column < Columns; ++column)
            for (auto row = 0; row < Rows; ++row)
                put(m(column, row) + n(column, row));

        for (auto column = 0; column < Columns; ++column)
            for (auto row = 0; row < Rows; ++row)
                put(m(column, row) - n(column, row));

        for (auto column = 0; column < Columns; ++column)
            for (auto row = 0; row < Rows; ++row)
                put(m(column, row) * 2.f);

        for (auto column = 0; column < Columns; ++column)
            for (auto row = 0; row < Rows; ++row)
                put(m(column, row) / 4.f);

        for (auto row = 0; row < Rows; ++row)
            for (auto column = 0; column < Columns; ++column)
                put(m(column, row));

        auto picked = std::clamp(thread % 7 - 2, 0, Columns - 1);

        for (auto row = 0; row < Rows; ++row)
            put(m(picked, row));

        if constexpr (Columns == Rows)
        {
            auto whole = Array<float, 16> {};

            for (auto column = 0; column < Columns; ++column)
                for (auto row = 0; row < Rows; ++row)
                    whole[column * Rows + row] = std::floor(m(column, row));

            put(determinantOf(whole, Columns));
        }
    }

    auto verify = [&](const Readback& readback)
    {
        const auto& got = readback.halves(kernel.output);
        auto agreeing = 0;

        for (auto i = 0; i < got.size() && i < expected.size(); ++i)
            agreeing += widened(got[i]) == widened(expected[i]) ? 1 : 0;

        check(agreeing == expected.size(), readback.name(), location);
    };

    CrossCheck {kernel}
        .input(kernel.values, values)
        .output(kernel.output,
                filled(threadCount * Kernel::outputStride, halfSentinel))
        .agreeing()
        .run(threadCount, verify, location);
}

auto tShapes2 = test("Float16Device/halfMatricesWithTwoColumnsAgreeExactly") = []
{
    checkShape<2, 2>();
    checkShape<2, 3>();
    checkShape<2, 4>();
};

auto tShapes3 = test("Float16Device/halfMatricesWithThreeColumnsAgreeExactly") = []
{
    checkShape<3, 2>();
    checkShape<3, 3>();
    checkShape<3, 4>();
};

auto tShapes4 = test("Float16Device/halfMatricesWithFourColumnsAgreeExactly") = []
{
    checkShape<4, 2>();
    checkShape<4, 3>();
    checkShape<4, 4>();
};

auto tBuffers = test("Float16Device/halfBuffersShareWordsBetweenThreads") = []
{
    auto kernel = BufferKernel {};
    auto input = halfInputs(threadCount * 4, -500.f, 1000.f, 7);
    auto at = [&](int element) { return widened(input[element]); };

    auto verify = [&](const Readback& readback)
    {
        const auto& scalars = readback.halves(kernel.scalars);
        const auto& records = readback.halves(kernel.records);
        const auto& quads = readback.halves(kernel.quads);
        auto agreeing = 0;

        for (auto t = 0; t < threadCount; ++t)
        {
            auto doubled = halfBits(at(t) * 2.f);
            auto matches =
                scalars[t] == doubled && records[t * 3] == input[t * 3 + 2]
                && records[t * 3 + 1] == input[t * 3]
                && records[t * 3 + 2] == input[t * 3 + 1]
                && scalars[t + threadCount] == halfBits(at(t * 3) + at(t * 2))
                && quads[t * 4] == input[t * 4 + 3]
                && quads[t * 4 + 1] == input[t * 4 + 2]
                && quads[t * 4 + 2] == input[t * 4 + 1]
                && quads[t * 4 + 3] == doubled;
            agreeing += matches ? 1 : 0;
        }

        check(agreeing == threadCount, readback.name());
        check(scalars.back() == halfSentinel, readback.name());
    };

    CrossCheck {kernel}
        .input(kernel.input, input)
        .output(kernel.scalars, filled(threadCount * 2 + 1, halfSentinel))
        .output(kernel.records, filled(threadCount * 3, halfSentinel))
        .output(kernel.quads, filled(threadCount * 4, halfSentinel))
        .agreeing()
        .run(threadCount, verify);
};

auto tSum = test("Float16Device/aRunningSumRoundsAtEveryStep") = []
{
    auto kernel = SumKernel {};
    auto input = halfInputs(threadCount * SumKernel::terms, -300.f, 600.f, 9);

    auto verify = [&](const Readback& readback)
    {
        const auto& got = readback.halves(kernel.output);
        auto agreeing = 0;

        for (auto t = 0; t < threadCount; ++t)
        {
            auto total = 0.f;

            for (auto i = 0; i < SumKernel::terms; ++i)
                total = rounded(total + widened(input[t * SumKernel::terms + i]));

            agreeing += got[t] == halfBits(total) ? 1 : 0;
        }

        check(agreeing == threadCount, readback.name());
    };

    CrossCheck {kernel}
        .input(kernel.input, input)
        .output(kernel.output, filled(threadCount, halfSentinel))
        .agreeing()
        .run(threadCount, verify);
};

auto tFragment = test("Float16Device/aFragmentComputesInHalves") = []
{
    if (!gpuRunsHalves())
        return;

    auto shades = Vector<std::uint16_t> {};
    shades.add(halfBits(0.25f));
    shades.add(halfBits(0.125f));
    auto buffer = Device::shared().makeBuffer(
        shades.data(), (std::int64_t) (shades.size() * 2), BufferUsage::Storage);

    auto program = FragmentShader {};
    program.shades = buffer;
    program.setVertices(fullQuad);
    program.prepare(describeFloatPipeline());
    check(program.pipeline().isValid());

    auto draw = [&](RenderPass& pass) { pass.draw(program); };
    auto pixels = drawAndRead(draw);

    for (auto i = 0; i < pixelCount; ++i)
    {
        auto expected = (float) (i % targetSize) * 0.25f + 0.125f;
        check(pixels[i] == expected, std::to_string(pixels[i]));
    }
};
} // namespace Float16DeviceTests
