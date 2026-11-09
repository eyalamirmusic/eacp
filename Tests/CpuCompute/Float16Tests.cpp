#include "CountingHeap.h"

#include <NanoTest/NanoTest.h>

#include <eacp/GPU/CpuCompute/CpuCompute.h>
#include <eacp/GPU/CpuCompute/Helpers.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <source_location>
#include <string>

// WGSL's f16 on the CPU executor. Every f16 operation is computed in float and
// rounded to the nearest half where it lands, so a kernel written once over
// either float family gives, for an f16 result, exactly the f32 result rounded:
// the built-ins and every matrix shape are checked that way, the f32 kernel
// beside the f16 one. Then the semantics that are f16's own - literals and
// conversions rounding to the nearest half (ties to even, infinity past
// 65504), an int truncated out of one, a running sum rounded at every step -
// against hand-computed halves; the f16 buffers' records, reads back of an
// output and out-of-range elements, and their refusal of any other binding;
// and the realtime contract.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::GPU::CpuCompute;

namespace Float16Tests
{
namespace
{
constexpr auto threadCount = 37;
constexpr auto halfSentinel = std::uint16_t {0x7e01u};

const auto batchings = std::array {PlanOptions {}, PlanOptions {1}};

template <bool IsHalf>
struct Family;

template <>
struct Family<false>
{
    using Scalar = Float;
    using Input = InputBuffer;
    using Output = OutputBuffer;
};

template <>
struct Family<true>
{
    using Scalar = Half;
    using Input = HalfInputBuffer;
    using Output = HalfOutputBuffer;
};

template <bool IsHalf>
using ScalarOf = typename Family<IsHalf>::Scalar;

template <bool IsHalf>
VectorOfFamily<3, IsHalf> vector3Of(const ScalarOf<IsHalf>& x,
                                    const ScalarOf<IsHalf>& y,
                                    const ScalarOf<IsHalf>& z)
{
    if constexpr (IsHalf)
        return half3(x, y, z);
    else
        return float3(x, y, z);
}

template <int Width, bool IsHalf>
VectorOfFamily<Width, IsHalf>
    readVector(const typename Family<IsHalf>::Input& values, const UInt& first)
{
    auto at = [&](unsigned offset) { return values[first + offset]; };

    if constexpr (Width == 2)
    {
        if constexpr (IsHalf)
            return half2(at(0u), at(1u));
        else
            return float2(at(0u), at(1u));
    }
    else if constexpr (Width == 3)
    {
        if constexpr (IsHalf)
            return half3(at(0u), at(1u), at(2u));
        else
            return float3(at(0u), at(1u), at(2u));
    }
    else
    {
        if constexpr (IsHalf)
            return half4(at(0u), at(1u), at(2u), at(3u));
        else
            return float4(at(0u), at(1u), at(2u), at(3u));
    }
}

// One operation per record, so an f16 result is the f32 one rounded once.
// a holds (0.05, 0.95), b (1.05, 3), c (-2, 2): every function's domain.
template <bool IsHalf>
struct BuiltinKernel final : ComputeKernel
{
    static constexpr int records = 45;

    BuiltinKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto x = a.read3(id);
        auto y = b.read3(id);
        auto z = c.read3(id);
        auto at = 0u;
        auto put = [&](const VectorOfFamily<3, IsHalf>& value)
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
        put(asinh(x));
        put(atanh(x));
        put(exp(x));
        put(exp2(x));
        put(log(x));
        put(log2(x));
        put(sqrt(x));
        put(rsqrt(x));
        put(floor(z));
        put(ceil(z));
        put(trunc(z));
        put(round(z));
        put(fract(z));
        put(sign(z));
        put(abs(z));
        put(normalize(z));
        put(acosh(y));
        put(atan2(z, x));
        put(pow(x, z));
        put(min(z, x));
        put(max(z, x));
        put(step(x, z));
        put(z + x);
        put(z - x);
        put(z * x);
        put(z / x);
        put(-z);
        put(clamp(z, x, y));
        put(mix(x, y, z));
        put(smoothstep(x, y, z));
        put(fma(x, y, z));
        put(cross(x, z));
        put(reflect(z, x));
        put(refract(z, x, x.y()));
        put(faceforward(x, y, z));
        put(vector3Of<IsHalf>(dot(x, z), length(z), distance(x, z)));
    }

    Uniform<typename Family<IsHalf>::Input> a;
    Uniform<typename Family<IsHalf>::Input> b;
    Uniform<typename Family<IsHalf>::Input> c;
    Uniform<typename Family<IsHalf>::Output> output;
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

template <int Columns, int Rows, typename... Vectors>
MatrixOf<Columns, Rows> floatMatrixOf(const Vectors&... columns)
{
    if constexpr (Columns == 2 && Rows == 2)
        return float2x2(columns...);
    else if constexpr (Columns == 2 && Rows == 3)
        return float2x3(columns...);
    else if constexpr (Columns == 2 && Rows == 4)
        return float2x4(columns...);
    else if constexpr (Columns == 3 && Rows == 2)
        return float3x2(columns...);
    else if constexpr (Columns == 3 && Rows == 3)
        return float3x3(columns...);
    else if constexpr (Columns == 3 && Rows == 4)
        return float3x4(columns...);
    else if constexpr (Columns == 4 && Rows == 2)
        return float4x2(columns...);
    else if constexpr (Columns == 4 && Rows == 3)
        return float4x3(columns...);
    else
        return float4x4(columns...);
}

template <int Columns, int Rows, bool IsHalf>
MatrixOfFamily<Columns, Rows, IsHalf>
    readMatrix(const typename Family<IsHalf>::Input& values, const UInt& first)
{
    auto column = [&](int which)
    { return readVector<Rows, IsHalf>(values, first + (unsigned) (which * Rows)); };

    auto build = [&](const auto&... columns)
    {
        if constexpr (IsHalf)
            return halfMatrixOf<Columns, Rows>(columns...);
        else
            return floatMatrixOf<Columns, Rows>(columns...);
    };

    if constexpr (Columns == 2)
        return build(column(0), column(1));
    else if constexpr (Columns == 3)
        return build(column(0), column(1), column(2));
    else
        return build(column(0), column(1), column(2), column(3));
}

// What a kernel writes one thread's results with: each component in order,
// through the kernel's own store.
template <bool IsHalf, typename Store>
struct Emitter
{
    void put(const ScalarOf<IsHalf>& value)
    {
        store(at, value);
        at = at + 1u;
    }

    template <int Width>
    void put(const VectorOfFamily<Width, IsHalf>& value)
    {
        put(value.x());
        put(value.y());

        if constexpr (Width > 2)
            put(value.z());

        if constexpr (Width > 3)
            put(value.w());
    }

    template <ShaderMatrixLike M>
    void put(const M& matrix)
    {
        for (auto column = 0; column < M::columns; ++column)
            put<M::rows>(matrix[column]);
    }

    Store store;
    UInt at;
};

// Every operation a matrix of one shape has, one per run of the output.
template <int Columns, int Rows, bool IsHalf>
struct MatrixKernel final : ComputeKernel
{
    static constexpr int inputStride = 2 * Columns * Rows + Columns + Rows + 1;
    static constexpr int outputStride = Rows + Columns + Rows * Rows
                                        + 4 * Columns * Rows + Rows * Columns
                                        + 3 * Rows + (Columns == Rows ? 1 : 0);

    MatrixKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto first = id * (unsigned) inputStride;
        auto cr = (unsigned) (Columns * Rows);

        auto m = readMatrix<Columns, Rows, IsHalf>(values, first);
        auto n = readMatrix<Columns, Rows, IsHalf>(values, first + cr);
        auto v = readVector<Columns, IsHalf>(values, first + 2u * cr);
        auto w =
            readVector<Rows, IsHalf>(values, first + 2u * cr + (unsigned) Columns);
        auto s = values[first + 2u * cr + (unsigned) (Columns + Rows)];

        auto store = [&](const UInt& at, const ScalarOf<IsHalf>& value)
        { write(output, at, value); };
        auto out =
            Emitter<IsHalf, decltype(store)> {store, id * (unsigned) outputStride};
        out.template put<Rows>(m * v);
        out.template put<Columns>(w * m);
        out.put(m * transpose(n));
        out.put(m + n);
        out.put(m - n);
        out.put(m * s);
        out.put(m / s);
        out.put(transpose(m));
        out.template put<Rows>(m[1]);
        out.template put<Rows>(m[toInt(id % 7u) - 2]);
        out.template put<Rows>(m[id % 5u]);

        if constexpr (Columns == Rows)
            out.put(determinant(m));
    }

    Uniform<typename Family<IsHalf>::Input> values;
    Uniform<typename Family<IsHalf>::Output> output;
    EACP_SHADER(values, output)
};

std::uint16_t halfBits(float value)
{
    return narrowToHalf(value);
}

// A half-representable value per element, in [low, low + span).
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

Vector<float> widened(const Vector<std::uint16_t>& halves)
{
    auto floats = Vector<float> {};

    for (auto bits: halves)
        floats.add(widenHalf(bits));

    return floats;
}

template <typename T>
Vector<T> filled(int count, T value)
{
    auto values = Vector<T> {};
    values.resize(count, value);
    return values;
}

// Each f16 result against its f32 twin rounded, bit for bit.
void expectRoundedTwins(const Vector<std::uint16_t>& halves,
                        const Vector<float>& floats,
                        const std::source_location& location)
{
    check(halves.size() == floats.size(), "the sizes", location);

    auto agreeing = 0;
    auto firstMiss = -1;

    for (auto i = 0; i < halves.size() && i < floats.size(); ++i)
    {
        if (halves[i] == halfBits(floats[i]))
            ++agreeing;
        else if (firstMiss < 0)
            firstMiss = i;
    }

    check(agreeing == halves.size(),
          "element " + std::to_string(firstMiss) + " is not its f32 twin rounded",
          location);
}

template <int Columns, int Rows>
void checkShape(
    const std::source_location& location = std::source_location::current())
{
    using HalfKernel = MatrixKernel<Columns, Rows, true>;
    using FloatKernel = MatrixKernel<Columns, Rows, false>;

    auto halfValues =
        halfInputs(threadCount * HalfKernel::inputStride, -2.f, 4.f, 3);
    auto floatValues = widened(halfValues);

    for (const auto& options: batchings)
    {
        auto halfKernel = HalfKernel {};
        auto floatKernel = FloatKernel {};
        auto halfExecutor = Executor {halfKernel, options};
        auto floatExecutor = Executor {floatKernel, options};
        check(halfExecutor.isValid(), halfExecutor.reason(), location);
        check(floatExecutor.isValid(), floatExecutor.reason(), location);

        auto halves = filled(threadCount * HalfKernel::outputStride, halfSentinel);
        auto floats = filled(threadCount * FloatKernel::outputStride, 0.f);

        auto halfBindings = Bindings {};
        halfBindings.set(halfKernel.values, halfValues);
        halfBindings.set(halfKernel.output, halves);

        auto floatBindings = Bindings {};
        floatBindings.set(floatKernel.values, floatValues);
        floatBindings.set(floatKernel.output, floats);

        check(halfExecutor.dispatch(halfBindings, threadCount), "f16", location);
        check(floatExecutor.dispatch(floatBindings, threadCount), "f32", location);
        expectRoundedTwins(halves, floats, location);
    }
}

// The f16 literals and conversions, each against the half it must land on.
struct ConversionKernel final : ComputeKernel
{
    static constexpr int outputs = 12;

    ConversionKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto base = id * (unsigned) outputs;
        auto at = [&](unsigned offset) { return base + offset; };
        auto x = floats[id];
        auto h = halves[id];

        write(output, at(0u), toHalf(x));
        write(output, at(1u), toHalf(toInt(id) * 2049 - 2049));
        write(output, at(2u), toHalf(id * 4097u));
        write(output, at(3u), toHalf(x > 0.f));
        write(output, at(4u), halfConstant(0.1f));
        write(output, at(5u), h * 0.1f);
        write(output, at(6u), halfConstant(65519.f));
        write(output, at(7u), halfConstant(65520.f));
        write(output, at(8u), h * 60000.f);
        write(output, at(9u), toHalf(toFloat(h) * 3.f));
        write(output, at(10u), toHalf(toInt(h)));
        write(output, at(11u), toHalf(toUInt(abs(h))));

        write(ints, id, toInt(h));
        write(roundTrip, id, toFloat(h));
    }

    Uniform<InputBuffer> floats;
    Uniform<HalfInputBuffer> halves;
    Uniform<HalfOutputBuffer> output;
    Uniform<IntOutputBuffer> ints;
    Uniform<OutputBuffer> roundTrip;
    EACP_SHADER(floats, halves, output, ints, roundTrip)
};

float conversionFloat(int thread)
{
    return (float) (thread - 18) * 0.3333f + 0.0001f * (float) thread;
}

float conversionHalf(int thread)
{
    return widenHalf(halfBits((float) (thread - 18) * 0.7f));
}

// A running sum rounded at every step, comparisons, select, a private array
// filled and indexed past its ends, and records read back from the output.
struct StatementKernel final : ComputeKernel
{
    static constexpr int outputs = 12;

    StatementKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto base = id * (unsigned) outputs;
        auto total = var(halfConstant(0.f));
        auto items = privateArray<Half, 4>();
        auto counter = var(0);

        auto body = [&]
        {
            auto value = input[id * 4u + toUInt(counter.get())];
            items.set(counter.get(), value);
            total += value * 0.3f;
            counter += 1;
        };
        loop(counter.get() < 4, body);

        auto record = input.read4(id);
        auto smaller = record.xy() < record.zw();
        auto picked = select(any(smaller), record.x(), record.w());

        auto sign = select(record.y() >= 0.f, halfConstant(1.f), -1.f);

        write(output, base, total.get());
        write(output, base + 1u, items[toInt(id) - 20]);
        write(output, base + 2u, items[id]);
        write(output, base + 3u, picked);
        write(output, base / 4u + 1u, half4(record.wzy(), items[2]));
        write(output, base + 8u, output.read4(base / 4u + 1u).w() + 1.f);
        write(output, base + 9u, sign * record.z());
    }

    Uniform<HalfInputBuffer> input;
    Uniform<HalfOutputBuffer> output;
    EACP_SHADER(input, output)
};

float statementInput(int element)
{
    return widenHalf(halfBits((float) ((element * 13) % 23 - 11) * 0.37f));
}
} // namespace

auto tBuiltins = test("Float16/everyBuiltinIsTheFloatOneRounded") = []
{
    auto count = threadCount * 3;
    auto a = halfInputs(count, 0.05f, 0.9f, 1);
    auto b = halfInputs(count, 1.05f, 1.95f, 2);
    auto c = halfInputs(count, -2.f, 4.f, 3);

    for (const auto& options: batchings)
    {
        auto halfKernel = BuiltinKernel<true> {};
        auto floatKernel = BuiltinKernel<false> {};
        auto records = BuiltinKernel<true>::records;
        auto halves = filled(threadCount * records * 3, halfSentinel);
        auto floats = filled(threadCount * records * 3, 0.f);

        auto halfExecutor = Executor {halfKernel, options};
        auto floatExecutor = Executor {floatKernel, options};
        check(halfExecutor.isValid(), halfExecutor.reason());
        check(floatExecutor.isValid(), floatExecutor.reason());

        auto halfBindings = Bindings {};
        halfBindings.set(halfKernel.a, a);
        halfBindings.set(halfKernel.b, b);
        halfBindings.set(halfKernel.c, c);
        halfBindings.set(halfKernel.output, halves);

        auto floatA = widened(a);
        auto floatB = widened(b);
        auto floatC = widened(c);
        auto floatBindings = Bindings {};
        floatBindings.set(floatKernel.a, floatA);
        floatBindings.set(floatKernel.b, floatB);
        floatBindings.set(floatKernel.c, floatC);
        floatBindings.set(floatKernel.output, floats);

        check(halfExecutor.dispatch(halfBindings, threadCount));
        check(floatExecutor.dispatch(floatBindings, threadCount));
        expectRoundedTwins(halves, floats, std::source_location::current());
    }
};

auto tShapes = test("Float16/everyMatrixShapeIsTheFloatOneRounded") = []
{
    checkShape<2, 2>();
    checkShape<2, 3>();
    checkShape<2, 4>();
    checkShape<3, 2>();
    checkShape<3, 3>();
    checkShape<3, 4>();
    checkShape<4, 2>();
    checkShape<4, 3>();
    checkShape<4, 4>();
};

auto tConversions = test("Float16/literalsAndConversionsRoundToTheNearestHalf") = []
{
    auto kernel = ConversionKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto floats = Vector<float> {};
    auto halves = Vector<std::uint16_t> {};

    for (auto thread = 0; thread < threadCount; ++thread)
    {
        floats.add(conversionFloat(thread));
        halves.add(halfBits(conversionHalf(thread)));
    }

    auto output = filled(threadCount * ConversionKernel::outputs, halfSentinel);
    auto ints = filled(threadCount, std::int32_t {99});
    auto roundTrip = filled(threadCount, 0.f);

    auto bindings = Bindings {};
    bindings.set(kernel.floats, floats);
    bindings.set(kernel.halves, halves);
    bindings.set(kernel.output, output);
    bindings.set(kernel.ints, ints);
    bindings.set(kernel.roundTrip, roundTrip);
    check(executor.dispatch(bindings, threadCount));

    constexpr auto infinity = std::uint16_t {0x7c00u};

    for (auto thread = 0; thread < threadCount; ++thread)
    {
        const auto* got = output.data() + thread * ConversionKernel::outputs;
        auto x = floats[thread];
        auto h = conversionHalf(thread);
        auto product = widenHalf(halfBits(h * widenHalf(halfBits(0.1f))));

        check(got[0] == halfBits(x));
        check(got[1] == halfBits((float) (thread * 2049 - 2049)));
        check(got[2] == halfBits((float) (thread * 4097)));
        check(got[3] == halfBits(x > 0.f ? 1.f : 0.f));
        check(got[4] == 0x2e66u);
        check(got[5] == halfBits(product));
        check(got[6] == 0x7bffu);
        check(got[7] == infinity);
        check(got[8] == halfBits(h * widenHalf(halfBits(60000.f))));
        check(got[9] == halfBits(h * 3.f));
        check(got[10] == halfBits((float) (int) h));
        check(got[11] == halfBits((float) (unsigned) std::abs(h)));
        check(ints[thread] == (int) h);
        check(roundTrip[thread] == h);
    }

    check(halfBits(2049.f) == halfBits(2048.f));
    check(halfBits(2051.f) == halfBits(2052.f));
};

auto tStatements = test("Float16/aRunningSumRoundsAtEveryStep") = []
{
    for (const auto& options: batchings)
    {
        auto kernel = StatementKernel {};
        auto executor = Executor {kernel, options};
        check(executor.isValid(), executor.reason());

        auto input = Vector<std::uint16_t> {};

        for (auto element = 0; element < threadCount * 4; ++element)
            input.add(halfBits(statementInput(element)));

        auto output = filled(threadCount * StatementKernel::outputs, halfSentinel);

        auto bindings = Bindings {};
        bindings.set(kernel.input, input);
        bindings.set(kernel.output, output);
        check(executor.dispatch(bindings, threadCount));

        auto scale = widenHalf(halfBits(0.3f));
        auto rounded = [](float value) { return widenHalf(halfBits(value)); };

        for (auto thread = 0; thread < threadCount; ++thread)
        {
            auto items = Array<float, 4> {};
            auto total = 0.f;

            for (auto i = 0; i < 4; ++i)
            {
                items[i] = statementInput(thread * 4 + i);
                total = rounded(total + rounded(items[i] * scale));
            }

            auto smaller = items[0] < items[2] || items[1] < items[3];
            const auto* got = output.data() + thread * StatementKernel::outputs;
            auto first = std::clamp(thread - 20, 0, 3);
            auto second = std::min(thread, 3);
            auto signedZ = rounded((items[1] >= 0.f ? 1.f : -1.f) * items[2]);

            check(got[0] == halfBits(total));
            check(got[1] == halfBits(items[first]));
            check(got[2] == halfBits(items[second]));
            check(got[3] == halfBits(smaller ? items[0] : items[3]));
            check(got[4] == halfBits(items[3]));
            check(got[5] == halfBits(items[2]));
            check(got[6] == halfBits(items[1]));
            check(got[7] == halfBits(items[2]));
            check(got[8] == halfBits(items[2] + 1.f));
            check(got[9] == halfBits(signedZ));
            check(got[10] == halfSentinel && got[11] == halfSentinel);
        }
    }
};

auto tRecords = test("Float16/halfBuffersReadAndWriteRecords") = []
{
    auto builder = ShaderBuilder {};
    auto input = builder.halfInputBuffer();
    auto output = builder.halfOutputBuffer();
    auto id = builder.threadId();

    builder.write(output, id, input.read3(id).zyx());
    builder.write(output, id * 3u + 200u, input[id * 3u] + input.read2(id).y());
    builder.write(output, id + 400u, output.read3(id).x());

    auto executor = Executor {builder.graph()};
    check(executor.isValid(), executor.reason());

    auto values = Vector<std::uint16_t> {};

    for (auto element = 0; element < threadCount * 3; ++element)
        values.add(halfBits((float) element * 0.25f - 7.f));

    auto written = filled(500, halfSentinel);

    auto bindings = Bindings {};
    bindings.set(input, values);
    bindings.set(output, written);
    check(executor.dispatch(bindings, threadCount));

    auto valueAt = [&](int element) { return (float) element * 0.25f - 7.f; };

    for (auto thread = 0; thread < threadCount; ++thread)
    {
        check(written[thread * 3] == halfBits(valueAt(thread * 3 + 2)));
        check(written[thread * 3 + 1] == halfBits(valueAt(thread * 3 + 1)));
        check(written[thread * 3 + 2] == halfBits(valueAt(thread * 3)));
        check(written[thread + 400] == halfBits(valueAt(thread * 3 + 2)));
    }

    for (auto thread = 0; thread < threadCount && thread * 3 + 200 < 400; ++thread)
    {
        auto sum = valueAt(thread * 3) + valueAt(thread * 2 + 1);
        check(written[thread * 3 + 200] == halfBits(sum));
    }

    check(written[499] == halfSentinel);
};

auto tOutOfRange = test("Float16/anElementPastTheEndReadsZeroAndIsNotWritten") = []
{
    auto builder = ShaderBuilder {};
    auto input = builder.halfInputBuffer();
    auto output = builder.halfOutputBuffer();
    auto id = builder.threadId();

    builder.write(output, id * 2u, input[id * 2u + 1u] + 1.f);
    builder.write(output, id * 2u + 1u, 5.f);

    auto executor = Executor {builder.graph()};
    check(executor.isValid(), executor.reason());

    auto values = Vector<std::uint16_t> {};
    values.add(halfBits(2.f));
    values.add(halfBits(3.f));
    values.add(halfBits(4.f));

    auto written = filled(5, halfSentinel);
    auto bindings = Bindings {};
    bindings.set(input, values);
    bindings.set(output, written);
    check(executor.dispatch(bindings, 4));

    check(written[0] == halfBits(4.f));
    check(written[1] == halfBits(5.f));
    check(written[2] == halfBits(1.f));
    check(written[3] == halfBits(5.f));
    check(written[4] == halfBits(1.f));
};

auto tRefusals = test("Float16/aHalfSlotTakesOnlyAHalfBinding") = []
{
    auto builder = ShaderBuilder {};
    auto input = builder.halfInputBuffer();
    auto output = builder.halfOutputBuffer();
    auto id = builder.threadId();
    builder.write(output, id, input[id]);

    auto executor = Executor {builder.graph()};
    check(executor.isValid(), executor.reason());

    auto halves = filled(4, std::uint16_t {0});
    auto words = filled(4, 0u);
    auto floats = filled(4, 0.f);

    auto asWords = Bindings {};
    asWords.set(input, halves);
    asWords.set(UIntOutputBuffer {output.graph, output.slot}, words);
    check(!executor.dispatch(asWords, 4));

    auto asFloats = Bindings {};
    asFloats.set(InputBuffer {input.graph, input.slot}, floats);
    asFloats.set(output, halves);
    check(!executor.dispatch(asFloats, 4));

    auto asHalves = Bindings {};
    asHalves.set(input, halves);
    asHalves.set(output, halves);
    check(executor.dispatch(asHalves, 4));
};

auto tNoAllocation = test("Float16/aDispatchAfterTheFirstAllocatesNothing") = []
{
    auto builtins = BuiltinKernel<true> {};
    auto shape = MatrixKernel<3, 2, true> {};
    auto statements = StatementKernel {};
    auto builtinExecutor = Executor {builtins};
    auto shapeExecutor = Executor {shape};
    auto statementExecutor = Executor {statements};

    auto a = halfInputs(threadCount * 3, 0.05f, 0.9f, 1);
    auto b = halfInputs(threadCount * 3, 1.05f, 1.95f, 2);
    auto c = halfInputs(threadCount * 3, -2.f, 4.f, 3);
    auto builtinOutput =
        filled(threadCount * BuiltinKernel<true>::records * 3, halfSentinel);
    auto shapeInput = halfInputs(
        threadCount * MatrixKernel<3, 2, true>::inputStride, -2.f, 4.f, 4);
    auto shapeOutput =
        filled(threadCount * MatrixKernel<3, 2, true>::outputStride, halfSentinel);
    auto statementInput = halfInputs(threadCount * 4, -3.f, 6.f, 5);
    auto statementOutput =
        filled(threadCount * StatementKernel::outputs, halfSentinel);

    auto builtinBindings = Bindings {};
    builtinBindings.set(builtins.a, a);
    builtinBindings.set(builtins.b, b);
    builtinBindings.set(builtins.c, c);
    builtinBindings.set(builtins.output, builtinOutput);

    auto shapeBindings = Bindings {};
    shapeBindings.set(shape.values, shapeInput);
    shapeBindings.set(shape.output, shapeOutput);

    auto statementBindings = Bindings {};
    statementBindings.set(statements.input, statementInput);
    statementBindings.set(statements.output, statementOutput);

    auto dispatchAll = [&]
    {
        return builtinExecutor.dispatch(builtinBindings, threadCount)
               && shapeExecutor.dispatch(shapeBindings, threadCount)
               && statementExecutor.dispatch(statementBindings, threadCount);
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
} // namespace Float16Tests
