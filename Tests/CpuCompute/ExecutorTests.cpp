#include <NanoTest/NanoTest.h>

#include <eacp/GPU/CpuCompute/CpuCompute.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <limits>
#include <new>
#include <span>

#if defined(_WIN32)
#include <malloc.h>
#endif

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::GPU::CpuCompute;

namespace
{
constexpr auto sentinel = -1.f;
constexpr auto sentinelBits = 0xdeadbeefu;

Vector<float> makeFloats(int count, float value)
{
    auto values = Vector<float> {};
    values.resize(count, value);
    return values;
}

Vector<std::uint32_t> makeUInts(int count, std::uint32_t value)
{
    auto values = Vector<std::uint32_t> {};
    values.resize(count, value);
    return values;
}

struct GainKernel final : ComputeKernel
{
    GainKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, input[i] * gain);
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;
    Uniform<Float> gain;

    EACP_SHADER(input, output, gain)
};

struct GridKernel final : ComputeKernel
{
    GridKernel() { compile(); }

    void define() override
    {
        auto position = threadPosition();
        auto cell = position.y * gridWidth() + position.x;
        write(output, cell, toFloat(position.x) + toFloat(position.y) * 100.f);
    }

    Uniform<OutputBuffer> output;

    EACP_SHADER(output)
};

struct VolumeKernel final : ComputeKernel
{
    VolumeKernel() { compile(); }

    void define() override
    {
        auto position = threadPosition3();
        auto cell =
            (position.z * gridHeight() + position.y) * gridWidth() + position.x;
        write(output, cell, cell * 3u + 1u);
    }

    Uniform<UIntOutputBuffer> output;

    EACP_SHADER(output)
};

struct DivergentKernel final : ComputeKernel
{
    DivergentKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto value = var(0.f);

        ifThen(i % 3u == 0u, [&] { value = 10.f; }, [&] { value = toFloat(i); });

        write(output, i, value);
    }

    Uniform<OutputBuffer> output;

    EACP_SHADER(output)
};

struct TriangleKernel final : ComputeKernel
{
    TriangleKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto n = var(0u);
        auto sum = var(0u);

        loop(n < i,
             [&]
             {
                 ifThen(n == 20u, [&] { breakLoop(); });
                 sum += n;
                 n += 1u;
             });

        write(output, i, sum);
    }

    Uniform<UIntOutputBuffer> output;

    EACP_SHADER(output)
};

struct SwizzleRotateKernel final : ComputeKernel
{
    SwizzleRotateKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, output.read4(i).yzwx());
    }

    Uniform<OutputBuffer> output;

    EACP_SHADER(output)
};

struct LargeConstantKernel final : ComputeKernel
{
    LargeConstantKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, i + 3000000000u);
    }

    Uniform<UIntOutputBuffer> output;

    EACP_SHADER(output)
};

struct BarrierKernel final : ComputeKernel
{
    BarrierKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        barrier();
        write(output, i, toFloat(i));
    }

    Uniform<OutputBuffer> output;

    EACP_SHADER(output)
};

std::uint32_t triangle(std::uint32_t n)
{
    auto limit = n < 20u ? n : 20u;
    return limit * (limit - 1u) / 2u;
}
} // namespace

auto tGainKernel = test("Executor/gainKernelGuardsAPartialLastGroup") = []
{
    constexpr auto count = 100;

    auto kernel = GainKernel {};
    kernel.gain = 2.5f;

    auto executor = Executor {kernel};
    check(executor.isValid());

    auto input = Vector<float> {};

    for (auto i = 0; i < count; ++i)
        input.add((float) i - 50.f);

    auto output = makeFloats(count + 28, sentinel);

    auto bindings = Bindings {};
    check(bindings.set(kernel.input, input));
    check(bindings.set(kernel.output, output));
    check(executor.dispatch(bindings, count));

    for (auto i = 0; i < count; ++i)
        check(output[i] == input[i] * 2.5f);

    for (auto i = count; i < output.size(); ++i)
        check(output[i] == sentinel);

    kernel.gain = -1.f;
    check(executor.dispatch(bindings, count));
    check(output[7] == -input[7]);
};

auto tBareGraph = test("Executor/bareGraphTakesUniformsBySlot") = []
{
    auto builder = ShaderBuilder {};
    auto input = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto offset = builder.uniform<Float>();
    auto i = builder.threadId();
    builder.write(output, i, input[i] + offset);

    auto executor = Executor {builder.graph()};
    check(executor.isValid());
    check(executor.plan().uniformCount() == 1);

    auto value = 0.25f;
    check(executor.setUniform(0, &value, sizeof(value)));
    check(!executor.setUniform(0, &value, 8));

    auto source = makeFloats(10, 1.f);
    auto result = makeFloats(10, 0.f);

    auto bindings = Bindings {};
    bindings.set(input, source);
    bindings.set(output, result);
    check(executor.dispatch(bindings, 10));

    for (auto element: result)
        check(element == 1.25f);
};

auto tUnboundSlot = test("Executor/anUnboundSlotRefusesTheDispatch") = []
{
    auto kernel = GainKernel {};
    auto executor = Executor {kernel};
    auto output = makeFloats(4, sentinel);

    auto bindings = Bindings {};
    bindings.set(kernel.output, output);

    check(!executor.dispatch(bindings, 4));
    check(output[0] == sentinel);
    check(!executor.dispatch(bindings, 4, 1));
};

auto tGrid = test("Executor/twoDimensionalGridIsGuarded") = []
{
    constexpr auto width = 13;
    constexpr auto height = 5;

    auto kernel = GridKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid());
    check(executor.plan().rank() == DispatchRank::TwoD);

    auto output = makeFloats(width * height + 32, sentinel);

    auto bindings = Bindings {};
    bindings.set(kernel.output, output);
    check(executor.dispatch(bindings, width, height));

    for (auto y = 0; y < height; ++y)
        for (auto x = 0; x < width; ++x)
            check(output[y * width + x] == (float) x + (float) y * 100.f);

    for (auto i = width * height; i < output.size(); ++i)
        check(output[i] == sentinel);
};

auto tVolume = test("Executor/threeDimensionalVolumeIsGuarded") = []
{
    constexpr auto width = 5;
    constexpr auto height = 6;
    constexpr auto depth = 7;
    constexpr auto cells = width * height * depth;

    auto kernel = VolumeKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid());

    auto output = makeUInts(cells + 64, sentinelBits);

    auto bindings = Bindings {};
    bindings.set(kernel.output, output);
    check(executor.dispatch(bindings, width, height, depth));

    for (auto cell = 0; cell < cells; ++cell)
        check(output[cell] == (std::uint32_t) cell * 3u + 1u);

    for (auto i = cells; i < output.size(); ++i)
        check(output[i] == sentinelBits);
};

auto tDivergentIf = test("Executor/divergentIfTakesBothBranches") = []
{
    auto kernel = DivergentKernel {};
    auto executor = Executor {kernel};
    auto output = makeFloats(70, sentinel);

    auto bindings = Bindings {};
    bindings.set(kernel.output, output);
    check(executor.dispatch(bindings, 70));

    for (auto i = 0; i < 70; ++i)
        check(output[i] == (i % 3 == 0 ? 10.f : (float) i));
};

auto tLoop = test("Executor/loopRunsPerLaneTripCountsAndBreaks") = []
{
    constexpr auto count = 40;

    auto kernel = TriangleKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid());

    auto output = makeUInts(count, sentinelBits);

    auto bindings = Bindings {};
    bindings.set(kernel.output, output);
    check(executor.dispatch(bindings, count));

    for (auto i = 0; i < count; ++i)
        check(output[i] == triangle((std::uint32_t) i));
};

auto tRecord = test("Executor/recordStoreReadsItsOwnDestination") = []
{
    constexpr auto records = 9;

    auto kernel = SwizzleRotateKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid());

    auto data = Vector<float> {};

    for (auto i = 0; i < records * 4; ++i)
        data.add((float) i);

    auto bindings = Bindings {};
    bindings.set(kernel.output, data);
    check(executor.dispatch(bindings, records));

    for (auto record = 0; record < records; ++record)
        for (auto component = 0; component < 4; ++component)
            check(data[record * 4 + component]
                  == (float) (record * 4 + (component + 1) % 4));
};

auto tLargeConstant = test("Executor/uintConstantAboveIntMax") = []
{
    auto kernel = LargeConstantKernel {};
    auto executor = Executor {kernel};
    auto output = makeUInts(5, 0u);

    auto bindings = Bindings {};
    bindings.set(kernel.output, output);
    check(executor.dispatch(bindings, 5));

    for (auto i = 0; i < 5; ++i)
        check(output[i] == 3000000000u + (std::uint32_t) i);
};

auto tBarrierGuard = test("Executor/aBarrierDropsTheGuardAndStoresStayInBounds") = []
{
    auto kernel = BarrierKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto output = makeFloats(4, sentinel);
    auto bindings = Bindings {};
    bindings.set(kernel.output, output);
    check(executor.dispatch(bindings, 4));

    for (auto i = 0; i < 4; ++i)
        check(output[i] == (float) i);
};

// ---------------------------------------------------------------------------
// Control flow: loops inside loops, each with its own Break and Continue.

namespace
{
struct NestedLoopKernel final : ComputeKernel
{
    NestedLoopKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto outer = var(0u);
        auto total = var(0u);
        auto visits = var(0u);

        loop(outer < i % 7u + 1u,
             [&]
             {
                 auto current = var(outer.get());
                 outer += 1u;

                 ifThen(current.get() == 2u, [&] { continueLoop(); });

                 auto inner = var(0u);

                 loop(inner < 6u,
                      [&]
                      {
                          auto step = var(inner.get());
                          inner += 1u;

                          ifThen((step.get() & 1u) == 1u, [&] { continueLoop(); });
                          ifThen(step.get() > current.get() + (i & 1u),
                                 [&] { breakLoop(); });

                          total += step.get() * 10u + current.get();
                          visits += 1u;
                      });

                 ifThen(total.get() > i + 40u, [&] { breakLoop(); });
                 visits += 100u;
             });

        write(output, i * 2u, total.get());
        write(output, i * 2u + 1u, visits.get());
    }

    Uniform<UIntOutputBuffer> output;

    EACP_SHADER(output)
};

struct NestedLoopResult
{
    std::uint32_t total = 0;
    std::uint32_t visits = 0;
};

NestedLoopResult nestedLoopTwin(std::uint32_t i)
{
    auto result = NestedLoopResult {};
    auto outer = 0u;

    while (outer < i % 7u + 1u)
    {
        auto current = outer;
        outer += 1u;

        if (current == 2u)
            continue;

        auto inner = 0u;

        while (inner < 6u)
        {
            auto step = inner;
            inner += 1u;

            if ((step & 1u) == 1u)
                continue;

            if (step > current + (i & 1u))
                break;

            result.total += step * 10u + current;
            result.visits += 1u;
        }

        if (result.total > i + 40u)
            break;

        result.visits += 100u;
    }

    return result;
}

struct StrayBreakKernel final : ComputeKernel
{
    StrayBreakKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        breakLoop();
        write(output, i, toFloat(i));
    }

    Uniform<OutputBuffer> output;

    EACP_SHADER(output)
};
} // namespace

auto tNestedLoops = test("Executor/nestedLoopsBreakAndContinueTheirOwnLoop") = []
{
    constexpr auto count = 90;

    auto kernel = NestedLoopKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto output = makeUInts(count * 2 + 8, sentinelBits);

    auto bindings = Bindings {};
    bindings.set(kernel.output, output);
    check(executor.dispatch(bindings, count));

    for (auto i = 0u; i < (std::uint32_t) count; ++i)
    {
        auto expected = nestedLoopTwin(i);
        check(output[(int) i * 2] == expected.total);
        check(output[(int) i * 2 + 1] == expected.visits);
    }

    for (auto i = count * 2; i < output.size(); ++i)
        check(output[i] == sentinelBits);
};

auto tStrayBreak = test("Executor/aBreakOutsideALoopIsInvalid") = []
{
    auto kernel = StrayBreakKernel {};
    auto executor = Executor {kernel};
    check(!executor.isValid());
    check(executor.reason().find("Break") != std::string::npos);
    check(executor.reason().find("outside a loop") != std::string::npos);
};

// ---------------------------------------------------------------------------
// The integer harness: two operands per thread read as bits out of a pair of
// uint buffers and one result written back as bits, so a case states the
// operation and the operands and nothing else. The signed forms cross into
// Int with toInt and back with toUInt, both bit-preserving.

namespace
{
constexpr auto intMin = std::numeric_limits<std::int32_t>::min();
constexpr auto intMax = std::numeric_limits<std::int32_t>::max();
constexpr auto uintMax = std::numeric_limits<std::uint32_t>::max();

std::uint32_t bitsOf(std::int32_t value)
{
    return std::bit_cast<std::uint32_t>(value);
}

std::int32_t signedOf(std::uint32_t bits)
{
    return std::bit_cast<std::int32_t>(bits);
}

Vector<std::uint32_t> intBits(std::initializer_list<std::int32_t> values)
{
    auto result = Vector<std::uint32_t> {};

    for (auto value: values)
        result.add(bitsOf(value));

    return result;
}

Vector<std::uint32_t> uintValues(std::initializer_list<std::uint32_t> values)
{
    auto result = Vector<std::uint32_t> {};

    for (auto value: values)
        result.add(value);

    return result;
}

Vector<float> floatValues(std::initializer_list<float> values)
{
    auto result = Vector<float> {};

    for (auto value: values)
        result.add(value);

    return result;
}

using PairBody = std::function<UInt(const UInt&, const UInt&)>;
using IntOp = std::function<Int(const Int&, const Int&)>;
using IntRelation = std::function<Bool(const Int&, const Int&)>;
using UIntRelation = std::function<Bool(const UInt&, const UInt&)>;

struct PairKernel final : ComputeKernel
{
    explicit PairKernel(PairBody bodyToRecord)
        : body(std::move(bodyToRecord))
    {
        compile();
    }

    void define() override
    {
        auto i = threadId();
        write(output, i, body(lhs[i], rhs[i]));
    }

    PairBody body;
    Uniform<UIntInputBuffer> lhs;
    Uniform<UIntInputBuffer> rhs;
    Uniform<UIntOutputBuffer> output;

    EACP_SHADER(lhs, rhs, output)
};

PairBody signedBody(const IntOp& op)
{
    return [op](const UInt& a, const UInt& b)
    { return toUInt(op(toInt(a), toInt(b))); };
}

PairBody signedTest(const IntRelation& relation)
{
    return [relation](const UInt& a, const UInt& b)
    { return select(relation(toInt(a), toInt(b)), 1u, 0u); };
}

PairBody unsignedTest(const UIntRelation& relation)
{
    return [relation](const UInt& a, const UInt& b)
    { return select(relation(a, b), 1u, 0u); };
}

Vector<std::uint32_t> runPair(const PairBody& body,
                              const Vector<std::uint32_t>& lhs,
                              const Vector<std::uint32_t>& rhs)
{
    auto kernel = PairKernel {body};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto output = makeUInts(lhs.size() + 1, sentinelBits);

    auto bindings = Bindings {};
    bindings.set(kernel.lhs, lhs);
    bindings.set(kernel.rhs, rhs);
    bindings.set(kernel.output, output);
    check(executor.dispatch(bindings, lhs.size()));
    check(output[lhs.size()] == sentinelBits);

    return output;
}

template <typename Reference>
void checkRelation(const IntRelation& signedRelation,
                   const UIntRelation& unsignedRelation,
                   Reference reference)
{
    auto lhs = intBits({-1, 1, -5, -5, intMin, 3, 0, -2, intMax});
    auto rhs = intBits({1, -1, -5, -4, intMax, 3, -1, -3, intMin});

    auto asSigned = runPair(signedTest(signedRelation), lhs, rhs);
    auto asUnsigned = runPair(unsignedTest(unsignedRelation), lhs, rhs);

    for (auto k = 0; k < lhs.size(); ++k)
    {
        auto expectSigned = reference(signedOf(lhs[k]), signedOf(rhs[k]));
        auto expectUnsigned = reference(lhs[k], rhs[k]);
        check(asSigned[k] == (expectSigned ? 1u : 0u));
        check(asUnsigned[k] == (expectUnsigned ? 1u : 0u));
    }

    check(asSigned[0] != asUnsigned[0]);
}
} // namespace

auto tDivideByZero = test("Executor/integerDivisionByZeroIsZero") = []
{
    auto lhs = intBits({7, -7, 0, intMin, intMax});
    auto rhs = intBits({0, 0, 0, 0, 0});

    auto signedQuotient = runPair(
        signedBody([](const Int& a, const Int& b) { return a / b; }), lhs, rhs);
    auto signedRemainder = runPair(
        signedBody([](const Int& a, const Int& b) { return a % b; }), lhs, rhs);
    auto quotient =
        runPair([](const UInt& a, const UInt& b) { return a / b; }, lhs, rhs);
    auto remainder =
        runPair([](const UInt& a, const UInt& b) { return a % b; }, lhs, rhs);

    for (auto k = 0; k < lhs.size(); ++k)
    {
        check(signedQuotient[k] == 0u);
        check(signedRemainder[k] == 0u);
        check(quotient[k] == 0u);
        check(remainder[k] == 0u);
    }
};

auto tIntMinByMinusOne = test("Executor/intMinByMinusOneIsDefined") = []
{
    auto lhs = intBits({intMin, intMin, intMin});
    auto rhs = intBits({-1, 1, 2});

    auto quotient = runPair(
        signedBody([](const Int& a, const Int& b) { return a / b; }), lhs, rhs);
    auto remainder = runPair(
        signedBody([](const Int& a, const Int& b) { return a % b; }), lhs, rhs);

    check(signedOf(quotient[0]) == intMin);
    check(signedOf(remainder[0]) == 0);
    check(signedOf(quotient[1]) == intMin);
    check(signedOf(remainder[1]) == 0);
    check(signedOf(quotient[2]) == intMin / 2);
    check(signedOf(remainder[2]) == 0);
};

auto tShiftMask = test("Executor/shiftAmountIsMaskedToFiveBits") = []
{
    auto lhs = uintValues({0x80000001u,
                           0x80000001u,
                           0x80000001u,
                           0x80000001u,
                           0xfffffff0u,
                           0x12345678u});
    auto rhs = uintValues({1u, 32u, 33u, 0xffffffffu, 66u, 36u});

    auto left =
        runPair([](const UInt& a, const UInt& b) { return a << b; }, lhs, rhs);
    auto right =
        runPair([](const UInt& a, const UInt& b) { return a >> b; }, lhs, rhs);
    auto signedLeft = runPair(
        signedBody([](const Int& a, const Int& b) { return a << b; }), lhs, rhs);
    auto signedRight = runPair(
        signedBody([](const Int& a, const Int& b) { return a >> b; }), lhs, rhs);

    for (auto k = 0; k < lhs.size(); ++k)
    {
        auto amount = rhs[k] & 31u;
        check(left[k] == lhs[k] << amount);
        check(right[k] == lhs[k] >> amount);
        check(signedLeft[k] == lhs[k] << amount);
        check(signedOf(signedRight[k]) == signedOf(lhs[k]) >> amount);
    }

    check(left[1] == 0x80000001u);
    check(right[2] == 0x40000000u);
    check(signedRight[3] == 0xffffffffu);
};

auto tOverflow = test("Executor/integerOverflowWraps") = []
{
    auto lhs = intBits({intMax, intMin, 0x10000, -1, intMin, 123456789});
    auto rhs = intBits({1, 1, 0x10000, 1, -1, 987654321});

    auto sum = runPair(
        signedBody([](const Int& a, const Int& b) { return a + b; }), lhs, rhs);
    auto difference = runPair(
        signedBody([](const Int& a, const Int& b) { return a - b; }), lhs, rhs);
    auto product = runPair(
        signedBody([](const Int& a, const Int& b) { return a * b; }), lhs, rhs);
    auto negated =
        runPair(signedBody([](const Int& a, const Int&) { return -a; }), lhs, rhs);
    auto magnitude = runPair(
        signedBody([](const Int& a, const Int&) { return abs(a); }), lhs, rhs);
    auto unsignedSum =
        runPair([](const UInt& a, const UInt& b) { return a + b; }, lhs, rhs);
    auto unsignedDifference =
        runPair([](const UInt& a, const UInt& b) { return b - a; }, lhs, rhs);
    auto unsignedProduct =
        runPair([](const UInt& a, const UInt& b) { return a * b; }, lhs, rhs);

    for (auto k = 0; k < lhs.size(); ++k)
    {
        check(sum[k] == lhs[k] + rhs[k]);
        check(difference[k] == lhs[k] - rhs[k]);
        check(product[k] == lhs[k] * rhs[k]);
        check(negated[k] == 0u - lhs[k]);
        check(unsignedSum[k] == lhs[k] + rhs[k]);
        check(unsignedDifference[k] == rhs[k] - lhs[k]);
        check(unsignedProduct[k] == lhs[k] * rhs[k]);
    }

    check(signedOf(sum[0]) == intMin);
    check(signedOf(difference[1]) == intMax);
    check(product[2] == 0u);
    check(signedOf(negated[1]) == intMin);
    check(signedOf(magnitude[1]) == intMin);
    check(signedOf(magnitude[3]) == 1);
    check(unsignedSum[3] == 0u);
};

auto tSignedDivide = test("Executor/signedDivisionTruncatesTowardZero") = []
{
    auto lhs = intBits({-7, 7, -7, -8, -1, 100, intMin + 1});
    auto rhs = intBits({2, -2, -2, 3, 5, -7, 2});

    auto asSigned = runPair(
        signedBody([](const Int& a, const Int& b) { return a / b; }), lhs, rhs);
    auto asUnsigned =
        runPair([](const UInt& a, const UInt& b) { return a / b; }, lhs, rhs);

    for (auto k = 0; k < lhs.size(); ++k)
    {
        check(signedOf(asSigned[k]) == signedOf(lhs[k]) / signedOf(rhs[k]));
        check(asUnsigned[k] == lhs[k] / rhs[k]);
    }

    check(signedOf(asSigned[0]) == -3);
    check(asUnsigned[0] == 0x7ffffffcu);
};

auto tSignedRemainder = test("Executor/signedRemainderTakesTheDividendsSign") = []
{
    auto lhs = intBits({-7, 7, -7, -8, -1, 100, intMin + 1});
    auto rhs = intBits({2, -2, -2, 3, 5, -7, 2});

    auto asSigned = runPair(
        signedBody([](const Int& a, const Int& b) { return a % b; }), lhs, rhs);
    auto asUnsigned =
        runPair([](const UInt& a, const UInt& b) { return a % b; }, lhs, rhs);

    for (auto k = 0; k < lhs.size(); ++k)
    {
        check(signedOf(asSigned[k]) == signedOf(lhs[k]) % signedOf(rhs[k]));
        check(asUnsigned[k] == lhs[k] % rhs[k]);
    }

    check(signedOf(asSigned[0]) == -1);
    check(signedOf(asSigned[1]) == 1);
};

auto tLess = test("Executor/signedLessThan") = []
{
    checkRelation([](const Int& a, const Int& b) { return a < b; },
                  [](const UInt& a, const UInt& b) { return a < b; },
                  [](auto a, auto b) { return a < b; });
};

auto tLessEqual = test("Executor/signedLessThanOrEqual") = []
{
    checkRelation([](const Int& a, const Int& b) { return a <= b; },
                  [](const UInt& a, const UInt& b) { return a <= b; },
                  [](auto a, auto b) { return a <= b; });
};

auto tGreater = test("Executor/signedGreaterThan") = []
{
    checkRelation([](const Int& a, const Int& b) { return a > b; },
                  [](const UInt& a, const UInt& b) { return a > b; },
                  [](auto a, auto b) { return a > b; });
};

auto tGreaterEqual = test("Executor/signedGreaterThanOrEqual") = []
{
    checkRelation([](const Int& a, const Int& b) { return a >= b; },
                  [](const UInt& a, const UInt& b) { return a >= b; },
                  [](auto a, auto b) { return a >= b; });
};

auto tShiftRight = test("Executor/shiftRightIsArithmeticForIntLogicalForUInt") = []
{
    auto lhs = intBits({-16, -1, intMin, 64, -3});
    auto rhs = intBits({2, 5, 31, 3, 1});

    auto arithmetic = runPair(
        signedBody([](const Int& a, const Int& b) { return a >> b; }), lhs, rhs);
    auto logical =
        runPair([](const UInt& a, const UInt& b) { return a >> b; }, lhs, rhs);

    for (auto k = 0; k < lhs.size(); ++k)
    {
        check(signedOf(arithmetic[k]) == signedOf(lhs[k]) >> (rhs[k] & 31u));
        check(logical[k] == lhs[k] >> (rhs[k] & 31u));
    }

    check(signedOf(arithmetic[0]) == -4);
    check(logical[0] == 0x3ffffffcu);
    check(signedOf(arithmetic[2]) == -1);
    check(logical[2] == 1u);
};

auto tAbs = test("Executor/absOfANegativeInt") = []
{
    auto lhs = intBits({-5, 5, 0, -1, intMin + 1});
    auto rhs = intBits({0, 0, 0, 0, 0});

    auto magnitude = runPair(
        signedBody([](const Int& a, const Int&) { return abs(a); }), lhs, rhs);

    for (auto k = 0; k < lhs.size(); ++k)
        check(signedOf(magnitude[k]) == std::abs(signedOf(lhs[k])));
};

auto tMinMax = test("Executor/minAndMaxAreSignedForIntUnsignedForUInt") = []
{
    auto lhs = intBits({-3, 2, -1, intMin, 7});
    auto rhs = intBits({2, -3, 1, intMax, 7});

    auto signedMin = runPair(
        signedBody([](const Int& a, const Int& b) { return min(a, b); }), lhs, rhs);
    auto signedMax = runPair(
        signedBody([](const Int& a, const Int& b) { return max(a, b); }), lhs, rhs);
    auto unsignedMin =
        runPair([](const UInt& a, const UInt& b) { return min(a, b); }, lhs, rhs);
    auto unsignedMax =
        runPair([](const UInt& a, const UInt& b) { return max(a, b); }, lhs, rhs);

    for (auto k = 0; k < lhs.size(); ++k)
    {
        check(signedOf(signedMin[k])
              == std::min(signedOf(lhs[k]), signedOf(rhs[k])));
        check(signedOf(signedMax[k])
              == std::max(signedOf(lhs[k]), signedOf(rhs[k])));
        check(unsignedMin[k] == std::min(lhs[k], rhs[k]));
        check(unsignedMax[k] == std::max(lhs[k], rhs[k]));
    }

    check(signedOf(signedMin[0]) == -3);
    check(unsignedMin[0] == 2u);
};

auto tNegate = test("Executor/unaryMinusOfInt") = []
{
    auto lhs = intBits({5, -7, 0, intMax, -1});
    auto rhs = intBits({0, 0, 0, 0, 0});

    auto negated =
        runPair(signedBody([](const Int& a, const Int&) { return -a; }), lhs, rhs);

    for (auto k = 0; k < lhs.size(); ++k)
        check(signedOf(negated[k]) == -signedOf(lhs[k]));
};

auto tIntBits = test("Executor/toIntAndToUIntPreserveTheBits") = []
{
    auto lhs = uintValues({0xffffffffu, 0x80000000u, 5u, 0x7fffffffu});
    auto rhs = uintValues({0u, 0u, 0u, 0u});

    auto roundTrip = runPair(
        [](const UInt& a, const UInt&) { return toUInt(toInt(a)); }, lhs, rhs);
    auto negative = runPair([](const UInt& a, const UInt&)
                            { return select(toInt(a) < 0, 1u, 0u); },
                            lhs,
                            rhs);

    for (auto k = 0; k < lhs.size(); ++k)
    {
        check(roundTrip[k] == lhs[k]);
        check(negative[k] == (signedOf(lhs[k]) < 0 ? 1u : 0u));
    }
};

// ---------------------------------------------------------------------------
// Conversions between the families.

namespace
{
struct FloatToIntegerKernel final : ComputeKernel
{
    FloatToIntegerKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto x = input[i];
        write(asInt, i, toUInt(toInt(x)));
        write(asUInt, i, toUInt(x));
    }

    Uniform<InputBuffer> input;
    Uniform<UIntOutputBuffer> asInt;
    Uniform<UIntOutputBuffer> asUInt;

    EACP_SHADER(input, asInt, asUInt)
};

struct IntegerToFloatKernel final : ComputeKernel
{
    IntegerToFloatKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto bits = input[i];
        write(fromInt, i, toFloat(toInt(bits)));
        write(fromUInt, i, toFloat(bits));
    }

    Uniform<UIntInputBuffer> input;
    Uniform<OutputBuffer> fromInt;
    Uniform<OutputBuffer> fromUInt;

    EACP_SHADER(input, fromInt, fromUInt)
};

struct VectorConversionKernel final : ComputeKernel
{
    VectorConversionKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto bits = input[i];
        auto signedPair = int2(toInt(bits), toInt(bits) * -2);
        auto unsignedPair = uint2(bits, bits >> 1u);
        auto floatPair = float2(values[i], -values[i]);

        write(floats, i * 2u, float4(toFloat(signedPair), toFloat(unsignedPair)));
        write(uints, i, uint4(toUInt(toInt(floatPair)), toUInt(floatPair)));
    }

    Uniform<UIntInputBuffer> input;
    Uniform<InputBuffer> values;
    Uniform<OutputBuffer> floats;
    Uniform<UIntOutputBuffer> uints;

    EACP_SHADER(input, values, floats, uints)
};

struct BoolConversionKernel final : ComputeKernel
{
    BoolConversionKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto less = lhs[i] < rhs[i];
        write(asInt, i, toUInt(toInt(less)));
        write(asFloat, i, toFloat(less));
    }

    Uniform<InputBuffer> lhs;
    Uniform<InputBuffer> rhs;
    Uniform<UIntOutputBuffer> asInt;
    Uniform<OutputBuffer> asFloat;

    EACP_SHADER(lhs, rhs, asInt, asFloat)
};
} // namespace

auto tFloatToInteger = test("Executor/floatToIntegerSaturatesAndNaNIsZero") = []
{
    const auto nan = std::numeric_limits<float>::quiet_NaN();
    const auto infinity = std::numeric_limits<float>::infinity();

    auto input = floatValues({1.9f,
                              -1.9f,
                              -2.7f,
                              3e9f,
                              -3e9f,
                              2147483648.f,
                              -2147483648.f,
                              2147483520.f,
                              nan,
                              infinity,
                              -infinity,
                              5e9f,
                              4294967040.f,
                              4294967296.f,
                              -0.5f,
                              0.f});

    auto expectInt = intBits({1,
                              -1,
                              -2,
                              intMax,
                              intMin,
                              intMax,
                              intMin,
                              2147483520,
                              0,
                              intMax,
                              intMin,
                              intMax,
                              intMax,
                              intMax,
                              0,
                              0});

    auto expectUInt = uintValues({1u,
                                  0u,
                                  0u,
                                  3000000000u,
                                  0u,
                                  2147483648u,
                                  0u,
                                  2147483520u,
                                  0u,
                                  uintMax,
                                  0u,
                                  uintMax,
                                  4294967040u,
                                  uintMax,
                                  0u,
                                  0u});

    auto kernel = FloatToIntegerKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto asInt = makeUInts(input.size(), sentinelBits);
    auto asUInt = makeUInts(input.size(), sentinelBits);

    auto bindings = Bindings {};
    bindings.set(kernel.input, input);
    bindings.set(kernel.asInt, asInt);
    bindings.set(kernel.asUInt, asUInt);
    check(executor.dispatch(bindings, input.size()));

    for (auto k = 0; k < input.size(); ++k)
    {
        check(asInt[k] == expectInt[k]);
        check(asUInt[k] == expectUInt[k]);
    }
};

auto tIntegerToFloat = test("Executor/toFloatOfANegativeIntIsSigned") = []
{
    auto input = intBits({-3, -1, intMin, 7, intMax, 0});

    auto kernel = IntegerToFloatKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto fromInt = makeFloats(input.size(), sentinel);
    auto fromUInt = makeFloats(input.size(), sentinel);

    auto bindings = Bindings {};
    bindings.set(kernel.input, input);
    bindings.set(kernel.fromInt, fromInt);
    bindings.set(kernel.fromUInt, fromUInt);
    check(executor.dispatch(bindings, input.size()));

    for (auto k = 0; k < input.size(); ++k)
    {
        check(fromInt[k] == (float) signedOf(input[k]));
        check(fromUInt[k] == (float) input[k]);
    }

    check(fromInt[0] == -3.f);
    check(fromUInt[0] == 4294967296.f);
};

auto tVectorConversions =
    test("Executor/vectorConversionsAreSignedPerComponent") = []
{
    auto input = intBits({-3, 5, intMin, -1});
    auto values = floatValues({-2.7f, 3.5f, 5e9f, -0.25f});

    auto kernel = VectorConversionKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto floats = makeFloats(input.size() * 8, sentinel);
    auto uints = makeUInts(input.size() * 4, sentinelBits);

    auto bindings = Bindings {};
    bindings.set(kernel.input, input);
    bindings.set(kernel.values, values);
    bindings.set(kernel.floats, floats);
    bindings.set(kernel.uints, uints);
    check(executor.dispatch(bindings, input.size()));

    for (auto k = 0; k < input.size(); ++k)
    {
        auto asSigned = signedOf(input[k]);
        check(floats[k * 8 + 0] == (float) asSigned);
        check(floats[k * 8 + 1] == (float) signedOf(input[k] * (0u - 2u)));
        check(floats[k * 8 + 2] == (float) input[k]);
        check(floats[k * 8 + 3] == (float) (input[k] >> 1u));
    }

    check(signedOf(uints[0]) == -2);
    check(signedOf(uints[1]) == 2);
    check(uints[2] == 0u);
    check(uints[3] == 2u);
    check(signedOf(uints[4]) == 3);
    check(signedOf(uints[5]) == -3);
    check(uints[6] == 3u);
    check(uints[7] == 0u);
    check(signedOf(uints[8]) == intMax);
    check(signedOf(uints[9]) == intMin);
    check(uints[10] == uintMax);
    check(uints[11] == 0u);
};

auto tBoolConversions = test("Executor/boolConvertsToOneNotAllOnes") = []
{
    auto lhs = floatValues({1.f, 2.f, -1.f, 0.f});
    auto rhs = floatValues({2.f, 1.f, 0.f, 0.f});

    auto kernel = BoolConversionKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto asInt = makeUInts(lhs.size(), sentinelBits);
    auto asFloat = makeFloats(lhs.size(), sentinel);

    auto bindings = Bindings {};
    bindings.set(kernel.lhs, lhs);
    bindings.set(kernel.rhs, rhs);
    bindings.set(kernel.asInt, asInt);
    bindings.set(kernel.asFloat, asFloat);
    check(executor.dispatch(bindings, lhs.size()));

    for (auto k = 0; k < lhs.size(); ++k)
    {
        auto less = lhs[k] < rhs[k];
        check(asInt[k] == (less ? 1u : 0u));
        check(asFloat[k] == (less ? 1.f : 0.f));
    }
};

// ---------------------------------------------------------------------------
// Float records: a Float4 and a scalar per thread, one Float4 written back, so
// the broadcast, swizzle and construct cases state only the expression.

namespace
{
using Record = std::array<float, 4>;
using RecordBody = std::function<Float4(const Float4&, const Float&)>;
using RecordReference = std::function<Record(Record, float)>;

const auto recordData = std::array<Record, 9> {Record {0.25f, -1.5f, 2.f, 0.75f},
                                               Record {-0.5f, 0.5f, 1.25f, -2.f},
                                               Record {3.f, -3.f, 0.f, 1.f},
                                               Record {0.1f, 0.9f, -0.1f, 1.1f},
                                               Record {-7.5f, 8.25f, 0.5f, -0.25f},
                                               Record {1.f, 2.f, 3.f, 4.f},
                                               Record {-1.f, -2.f, -3.f, -4.f},
                                               Record {0.6f, 0.4f, 0.2f, 0.8f},
                                               Record {0.f, 1.f, 0.5f, -0.f}};

const auto scalarData =
    std::array<float, 9> {0.5f, -0.25f, 2.f, 0.75f, -3.f, 1.5f, 0.1f, 0.3f, 1.f};

struct RecordKernel final : ComputeKernel
{
    explicit RecordKernel(RecordBody bodyToRecord)
        : body(std::move(bodyToRecord))
    {
        compile();
    }

    void define() override
    {
        auto i = threadId();
        write(output, i, body(records.read4(i), scalars[i]));
    }

    RecordBody body;
    Uniform<InputBuffer> records;
    Uniform<InputBuffer> scalars;
    Uniform<OutputBuffer> output;

    EACP_SHADER(records, scalars, output)
};

Record eachOf(Record value, const std::function<float(float)>& function)
{
    for (auto& component: value)
        component = function(component);

    return value;
}

void checkRecords(const RecordBody& body, const RecordReference& reference)
{
    constexpr auto count = (int) recordData.size();

    auto records = Vector<float> {};
    auto scalars = Vector<float> {};

    for (auto k = 0; k < count; ++k)
    {
        for (auto component: recordData[(std::size_t) k])
            records.add(component);

        scalars.add(scalarData[(std::size_t) k]);
    }

    auto kernel = RecordKernel {body};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto output = makeFloats(count * 4 + 4, sentinel);

    auto bindings = Bindings {};
    bindings.set(kernel.records, records);
    bindings.set(kernel.scalars, scalars);
    bindings.set(kernel.output, output);
    check(executor.dispatch(bindings, count));

    for (auto k = 0; k < count; ++k)
    {
        auto expected =
            reference(recordData[(std::size_t) k], scalarData[(std::size_t) k]);

        for (auto c = 0; c < 4; ++c)
            check(output[k * 4 + c] == expected[(std::size_t) c]);
    }

    for (auto k = count * 4; k < output.size(); ++k)
        check(output[k] == sentinel);
}

float clampOf(float x, float low, float high)
{
    return std::fmin(std::fmax(x, low), high);
}

float smoothstepOf(float edge0, float edge1, float x)
{
    auto t = clampOf((x - edge0) / (edge1 - edge0), 0.f, 1.f);
    return t * t * (3.f - 2.f * t);
}
} // namespace

auto tBroadcastBinary = test("Executor/broadcastAScalarAgainstAVectorInBinary") = []
{
    checkRecords([](const Float4& v, const Float& s) { return v * s; },
                 [](Record v, float s)
                 { return eachOf(v, [s](float x) { return x * s; }); });

    checkRecords([](const Float4& v, const Float& s) { return s - v; },
                 [](Record v, float s)
                 { return eachOf(v, [s](float x) { return s - x; }); });

    checkRecords([](const Float4& v, const Float& s) { return v / s; },
                 [](Record v, float s)
                 { return eachOf(v, [s](float x) { return x / s; }); });

    checkRecords([](const Float4& v, const Float& s) { return s + v - 0.5f; },
                 [](Record v, float s)
                 { return eachOf(v, [s](float x) { return s + x - 0.5f; }); });

    checkRecords(
        [](const Float4& v, const Float&) { return 2.f / v.wzyx(); },
        [](Record v, float)
        { return Record {2.f / v[3], 2.f / v[2], 2.f / v[1], 2.f / v[0]}; });
};

auto tBroadcastSelect = test("Executor/selectBroadcastsItsConditionOverAVector") = []
{
    checkRecords([](const Float4& v, const Float& s)
                 { return select(s > 0.4f, v, v.wzyx()); },
                 [](Record v, float s)
                 { return s > 0.4f ? v : Record {v[3], v[2], v[1], v[0]}; });

    checkRecords(
        [](const Float4& v, const Float& s)
        { return v * select(v.x() < v.y(), s, 3.f); },
        [](Record v, float s)
        { return eachOf(v, [&](float x) { return x * (v[0] < v[1] ? s : 3.f); }); });
};

auto tBroadcastCalls = test("Executor/componentwiseCallsBroadcastScalars") = []
{
    checkRecords(
        [](const Float4& v, const Float&) { return clamp(v, 0.f, 1.f); },
        [](Record v, float)
        { return eachOf(v, [](float x) { return clampOf(x, 0.f, 1.f); }); });

    checkRecords([](const Float4& v, const Float& s) { return clamp(v, s, 1.f); },
                 [](Record v, float s)
                 { return eachOf(v, [s](float x) { return clampOf(x, s, 1.f); }); });

    checkRecords([](const Float4& v, const Float& s) { return mix(v, v.yzwx(), s); },
                 [](Record v, float s)
                 {
                     auto rotated = Record {v[1], v[2], v[3], v[0]};
                     auto result = Record {};

                     for (auto c = 0u; c < 4u; ++c)
                         result[c] = v[c] + (rotated[c] - v[c]) * s;

                     return result;
                 });

    checkRecords(
        [](const Float4& v, const Float&) { return mix(0.5f, 1.f, v); },
        [](Record v, float)
        { return eachOf(v, [](float t) { return 0.5f + (1.f - 0.5f) * t; }); });

    checkRecords(
        [](const Float4& v, const Float&) { return step(v, 0.5f); },
        [](Record v, float)
        { return eachOf(v, [](float edge) { return 0.5f < edge ? 0.f : 1.f; }); });

    checkRecords(
        [](const Float4& v, const Float&) { return step(0.5f, v); },
        [](Record v, float)
        { return eachOf(v, [](float x) { return x < 0.5f ? 0.f : 1.f; }); });

    checkRecords([](const Float4& v, const Float& s) { return step(s, v); },
                 [](Record v, float s)
                 { return eachOf(v, [s](float x) { return x < s ? 0.f : 1.f; }); });

    checkRecords([](const Float4& v, const Float& s) { return min(v, s); },
                 [](Record v, float s)
                 { return eachOf(v, [s](float x) { return std::fmin(x, s); }); });

    checkRecords([](const Float4& v, const Float&) { return max(0.25f, v); },
                 [](Record v, float)
                 { return eachOf(v, [](float x) { return std::fmax(0.25f, x); }); });

    checkRecords(
        [](const Float4& v, const Float& s) { return pow(abs(v), s); },
        [](Record v, float s)
        { return eachOf(v, [s](float x) { return std::pow(std::fabs(x), s); }); });

    checkRecords(
        [](const Float4& v, const Float&) { return smoothstep(0.f, 1.f, v); },
        [](Record v, float)
        { return eachOf(v, [](float x) { return smoothstepOf(0.f, 1.f, x); }); });

    checkRecords(
        [](const Float4& v, const Float& s) { return smoothstep(s, 2.f, v); },
        [](Record v, float s)
        { return eachOf(v, [s](float x) { return smoothstepOf(s, 2.f, x); }); });
};

auto tSwizzleConstruct = test("Executor/swizzlesAndConstructFromMixedWidths") = []
{
    checkRecords([](const Float4& v, const Float& s)
                 { return float4(v.zw(), v.x(), s); },
                 [](Record v, float s) { return Record {v[2], v[3], v[0], s}; });

    checkRecords([](const Float4& v, const Float& s) { return float4(s, v.yzw()); },
                 [](Record v, float s) { return Record {s, v[1], v[2], v[3]}; });

    checkRecords([](const Float4& v, const Float&) { return v.xxyy(); },
                 [](Record v, float) { return Record {v[0], v[0], v[1], v[1]}; });

    checkRecords([](const Float4& v, const Float& s)
                 { return float4(float3(v.wz(), s), v.zyx().y()); },
                 [](Record v, float s) { return Record {v[3], v[2], s, v[1]}; });

    checkRecords([](const Float4& v, const Float& s)
                 { return float4(float2(s, 1.f), v.zyx().yx()); },
                 [](Record v, float s) { return Record {s, 1.f, v[1], v[2]}; });
};

// ---------------------------------------------------------------------------
// Matrices: products in each order, a scalar Binary over a matrix, transpose
// and determinant at every order. Every entry is a small integer, so the
// references are exact whatever order they sum in.

namespace
{
using Matrix = std::array<float, 16>;

const auto transformColumns = std::array<Record, 4> {Record {2.f, 0.f, 1.f, -1.f},
                                                     Record {1.f, 3.f, 0.f, 2.f},
                                                     Record {-2.f, 1.f, 1.f, 0.f},
                                                     Record {0.f, -1.f, 2.f, 1.f}};

Matrix columnsOf(const std::array<Record, 4>& columns)
{
    auto result = Matrix {};

    for (auto j = 0u; j < 4u; ++j)
        for (auto i = 0u; i < 4u; ++i)
            result[j * 4u + i] = columns[j][i];

    return result;
}

const auto transformData = columnsOf(transformColumns);

const auto matrixRecords = std::array<Record, 5> {Record {1.f, 2.f, -1.f, 3.f},
                                                  Record {0.f, -2.f, 4.f, 1.f},
                                                  Record {-3.f, 1.f, 2.f, -2.f},
                                                  Record {2.f, 2.f, 2.f, 2.f},
                                                  Record {5.f, -1.f, 0.f, 3.f}};

const auto matrixScalars = std::array<float, 5> {2.f, -1.f, 3.f, 0.5f, -4.f};

constexpr auto matrixOutputs = 14;

struct MatrixKernel final : ComputeKernel
{
    MatrixKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto v = records.read4(i);
        auto s = scalars[i];
        auto m2 = float2x2(v.xy(), v.zw());
        auto m3 = float3x3(v.xyz(), v.yzw(), v.zwx());
        auto base = i * (unsigned) matrixOutputs;

        write(output, base + 0u, transform * v);
        write(output, base + 1u, v * transform);
        write(output, base + 2u, (transform * transform) * v);
        write(output, base + 3u, transpose(transform) * v);
        write(output, base + 4u, (transform * s) * v);
        write(output, base + 5u, (s * transform) * v);
        write(output, base + 6u, (transform / 2.f) * v);
        write(output, base + 7u, float4(m2 * v.xy(), v.xy() * m2));
        write(output, base + 8u, float4((m2 * m2) * v.zw(), transpose(m2) * v.xy()));
        write(output, base + 9u, float4(m3 * v.xyz(), s));
        write(output, base + 10u, float4(v.xyz() * m3, s));
        write(output, base + 11u, float4((m3 * m3) * v.yzw(), s));
        write(output, base + 12u, float4(transpose(m3) * v.xyz(), s));
        write(output,
              base + 13u,
              float4(determinant(m2), determinant(m3), determinant(transform), s));
    }

    Uniform<Float4x4> transform;
    Uniform<InputBuffer> records;
    Uniform<InputBuffer> scalars;
    Uniform<OutputBuffer> output;

    EACP_SHADER(transform, records, scalars, output)
};

std::size_t element(int column, int row, int n)
{
    return (std::size_t) column * (std::size_t) n + (std::size_t) row;
}

Record matrixTimesVector(const Matrix& m, const Record& v, int n)
{
    auto result = Record {};

    for (auto i = 0; i < n; ++i)
        for (auto j = 0; j < n; ++j)
            result[(std::size_t) i] += m[element(j, i, n)] * v[(std::size_t) j];

    return result;
}

Record vectorTimesMatrix(const Record& v, const Matrix& m, int n)
{
    auto result = Record {};

    for (auto j = 0; j < n; ++j)
        for (auto i = 0; i < n; ++i)
            result[(std::size_t) j] += v[(std::size_t) i] * m[element(j, i, n)];

    return result;
}

Matrix matrixTimesMatrix(const Matrix& a, const Matrix& b, int n)
{
    auto result = Matrix {};

    for (auto j = 0; j < n; ++j)
        for (auto i = 0; i < n; ++i)
            for (auto k = 0; k < n; ++k)
                result[element(j, i, n)] +=
                    a[element(k, i, n)] * b[element(j, k, n)];

    return result;
}

Matrix transposed(const Matrix& m, int n)
{
    auto result = Matrix {};

    for (auto j = 0; j < n; ++j)
        for (auto i = 0; i < n; ++i)
            result[element(j, i, n)] = m[element(i, j, n)];

    return result;
}

Matrix scaled(const Matrix& m, float by)
{
    auto result = m;

    for (auto& entry: result)
        entry *= by;

    return result;
}

double determinantOf(const Matrix& m, int n)
{
    if (n == 1)
        return m[0];

    auto total = 0.0;

    for (auto column = 0; column < n; ++column)
    {
        auto minor = Matrix {};
        auto size = n - 1;

        for (auto j = 0, target = 0; j < n; ++j)
        {
            if (j == column)
                continue;

            for (auto i = 1; i < n; ++i)
                minor[element(target, i - 1, size)] = m[element(j, i, n)];

            ++target;
        }

        auto sign = column % 2 == 0 ? 1.0 : -1.0;
        total += sign * m[element(column, 0, n)] * determinantOf(minor, size);
    }

    return total;
}

Record joined(const Record& first, const Record& second)
{
    return {first[0], first[1], second[0], second[1]};
}

Record withLast(Record value, float last)
{
    value[3] = last;
    return value;
}

Record tail(const Record& v)
{
    return {v[1], v[2], v[3], 0.f};
}

Record head(const Record& v, int count)
{
    auto result = Record {};

    for (auto c = 0; c < count; ++c)
        result[(std::size_t) c] = v[(std::size_t) c];

    return result;
}
} // namespace

auto tMatrices = test("Executor/matrixProductsTransposeAndDeterminant") = []
{
    constexpr auto count = (int) matrixRecords.size();

    auto kernel = MatrixKernel {};

    for (auto k = 0; k < 16; ++k)
        kernel.transform.value[k] = transformData[(std::size_t) k];

    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto records = Vector<float> {};
    auto scalars = Vector<float> {};

    for (auto k = 0; k < count; ++k)
    {
        for (auto component: matrixRecords[(std::size_t) k])
            records.add(component);

        scalars.add(matrixScalars[(std::size_t) k]);
    }

    auto output = makeFloats(count * matrixOutputs * 4, sentinel);

    auto bindings = Bindings {};
    bindings.set(kernel.records, records);
    bindings.set(kernel.scalars, scalars);
    bindings.set(kernel.output, output);
    check(executor.dispatch(bindings, count));

    const auto& t = transformData;

    for (auto k = 0; k < count; ++k)
    {
        auto v = matrixRecords[(std::size_t) k];
        auto s = matrixScalars[(std::size_t) k];
        auto m2 = Matrix {v[0], v[1], v[2], v[3]};
        auto m3 = Matrix {v[0], v[1], v[2], v[1], v[2], v[3], v[2], v[3], v[0]};
        auto xy = head(v, 2);
        auto zw = Record {v[2], v[3], 0.f, 0.f};
        auto xyz = head(v, 3);

        auto expected = std::array<Record, matrixOutputs> {
            matrixTimesVector(t, v, 4),
            vectorTimesMatrix(v, t, 4),
            matrixTimesVector(matrixTimesMatrix(t, t, 4), v, 4),
            matrixTimesVector(transposed(t, 4), v, 4),
            matrixTimesVector(scaled(t, s), v, 4),
            matrixTimesVector(scaled(t, s), v, 4),
            matrixTimesVector(scaled(t, 0.5f), v, 4),
            joined(matrixTimesVector(m2, xy, 2), vectorTimesMatrix(xy, m2, 2)),
            joined(matrixTimesVector(matrixTimesMatrix(m2, m2, 2), zw, 2),
                   matrixTimesVector(transposed(m2, 2), xy, 2)),
            withLast(matrixTimesVector(m3, xyz, 3), s),
            withLast(vectorTimesMatrix(xyz, m3, 3), s),
            withLast(matrixTimesVector(matrixTimesMatrix(m3, m3, 3), tail(v), 3), s),
            withLast(matrixTimesVector(transposed(m3, 3), xyz, 3), s),
            Record {(float) determinantOf(m2, 2),
                    (float) determinantOf(m3, 3),
                    (float) determinantOf(t, 4),
                    s}};

        for (auto r = 0; r < matrixOutputs; ++r)
            for (auto c = 0; c < 4; ++c)
                check(output[(k * matrixOutputs + r) * 4 + c]
                      == expected[(std::size_t) r][(std::size_t) c]);
    }

    check(determinantOf(t, 4) != 0.0);
};

// ---------------------------------------------------------------------------
// The integer vectors: signed per component, and a scalar broadcast either
// way round.

namespace
{
constexpr auto intVectorOutputs = 12;

struct IntVectorKernel final : ComputeKernel
{
    IntVectorKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto v = int2(toInt(lhs[i]), toInt(rhs[i]));
        auto w = v.yx();
        auto base = i * (unsigned) intVectorOutputs;

        auto store = [&](unsigned slot, const Int& value)
        { write(output, base + slot, toUInt(value)); };

        store(0, min(v, w).x());
        store(1, max(v, w).x());
        store(2, abs(v).x());
        store(3, (-v).x());
        store(4, (v / w).x());
        store(5, (v % w).x());
        store(6, (v >> 1).x());
        store(7, (v >> w).x());
        store(8, select((v < w).x(), 1, 0));
        store(9, select((v <= w).x(), 1, 0));
        store(10, select((v > w).x(), 1, 0));
        store(11, select((v >= w).x(), 1, 0));
    }

    Uniform<UIntInputBuffer> lhs;
    Uniform<UIntInputBuffer> rhs;
    Uniform<UIntOutputBuffer> output;

    EACP_SHADER(lhs, rhs, output)
};

constexpr auto broadcastOutputs = 4;

struct IntegerBroadcastKernel final : ComputeKernel
{
    IntegerBroadcastKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto a = lhs[i];
        auto b = rhs[i];
        auto vector = uint4(a, b, a * 3u, 7u);
        auto signedVector = int4(toInt(a), toInt(b), -1, 5);
        auto base = i * (unsigned) broadcastOutputs;

        write(output, base + 0u, vector + b);
        write(output, base + 1u, b - vector);
        write(output, base + 2u, toUInt(signedVector >> toInt(b % 4u)));
        write(output, base + 3u, toUInt(toInt(a) - signedVector));
    }

    Uniform<UIntInputBuffer> lhs;
    Uniform<UIntInputBuffer> rhs;
    Uniform<UIntOutputBuffer> output;

    EACP_SHADER(lhs, rhs, output)
};
} // namespace

auto tIntVectors = test("Executor/intVectorsAreSignedPerComponent") = []
{
    auto lhs = intBits({-7, 7, -16, intMin, -1});
    auto rhs = intBits({2, -3, 3, -1, 1});

    auto kernel = IntVectorKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto output = makeUInts(lhs.size() * intVectorOutputs, sentinelBits);

    auto bindings = Bindings {};
    bindings.set(kernel.lhs, lhs);
    bindings.set(kernel.rhs, rhs);
    bindings.set(kernel.output, output);
    check(executor.dispatch(bindings, lhs.size()));

    for (auto k = 0; k < lhs.size(); ++k)
    {
        auto a = signedOf(lhs[k]);
        auto b = signedOf(rhs[k]);
        auto at = [&](int slot)
        { return signedOf(output[k * intVectorOutputs + slot]); };

        check(at(0) == std::min(a, b));
        check(at(1) == std::max(a, b));
        check(at(2) == signedOf(a < 0 ? 0u - lhs[k] : lhs[k]));
        check(at(3) == signedOf(0u - lhs[k]));
        check(at(4) == (a == intMin && b == -1 ? intMin : a / b));
        check(at(5) == (a == intMin && b == -1 ? 0 : a % b));
        check(at(6) == a >> 1);
        check(at(7) == a >> (rhs[k] & 31u));
        check(at(8) == (a < b ? 1 : 0));
        check(at(9) == (a <= b ? 1 : 0));
        check(at(10) == (a > b ? 1 : 0));
        check(at(11) == (a >= b ? 1 : 0));
    }
};

auto tIntegerBroadcast =
    test("Executor/integerVectorsBroadcastAScalarEitherWay") = []
{
    auto lhs = uintValues({5u, 0xfffffff0u, 100u});
    auto rhs = uintValues({3u, 6u, 0xffffffffu});

    auto kernel = IntegerBroadcastKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto output = makeUInts(lhs.size() * broadcastOutputs * 4, sentinelBits);

    auto bindings = Bindings {};
    bindings.set(kernel.lhs, lhs);
    bindings.set(kernel.rhs, rhs);
    bindings.set(kernel.output, output);
    check(executor.dispatch(bindings, lhs.size()));

    for (auto k = 0; k < lhs.size(); ++k)
    {
        auto a = lhs[k];
        auto b = rhs[k];
        auto vector = std::array<std::uint32_t, 4> {a, b, a * 3u, 7u};
        auto signedVector =
            std::array<std::int32_t, 4> {signedOf(a), signedOf(b), -1, 5};
        auto at = [&](int record, int component)
        { return output[(k * broadcastOutputs + record) * 4 + component]; };

        for (auto c = 0; c < 4; ++c)
        {
            auto index = (std::size_t) c;
            check(at(0, c) == vector[index] + b);
            check(at(1, c) == b - vector[index]);
            check(signedOf(at(2, c)) == signedVector[index] >> (b % 4u));
            check(signedOf(at(3, c))
                  == signedOf(bitsOf(signedOf(a)) - bitsOf(signedVector[index])));
        }
    }
};

// ---------------------------------------------------------------------------
// Memory: every out-of-range read is zero and every out-of-range store is
// dropped, for scalars, records and wide stores alike.

namespace
{
struct OffsetReadKernel final : ComputeKernel
{
    OffsetReadKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(shifted, i, input[i + offset] + 1.f);
        write(records, i, input.read4(i));
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> shifted;
    Uniform<OutputBuffer> records;
    Uniform<UInt> offset;

    EACP_SHADER(input, shifted, records, offset)
};

struct ScatterKernel final : ComputeKernel
{
    ScatterKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(scalars, i * 3u, toFloat(i) + 0.5f);
        write(records, i, float4(toFloat(i), 1.f, 2.f, 3.f));
        write4(wide, i, float4(toFloat(i), 4.f, 5.f, 6.f));
    }

    Uniform<OutputBuffer> scalars;
    Uniform<OutputBuffer> records;
    Uniform<OutputBuffer> wide;

    EACP_SHADER(scalars, records, wide)
};

Vector<float> ramp(int count)
{
    auto values = Vector<float> {};

    for (auto k = 0; k < count; ++k)
        values.add((float) (k + 1));

    return values;
}

float elementOr(const Vector<float>& values, std::uint32_t index)
{
    return index < (std::uint32_t) values.size() ? values[(int) index] : 0.f;
}
} // namespace

auto tReadOutOfBounds = test("Executor/aBufferReadOutOfBoundsIsZero") = []
{
    constexpr auto count = 12;

    auto input = ramp(10);

    auto kernel = OffsetReadKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto shifted = makeFloats(count, sentinel);
    auto records = makeFloats(count * 4, sentinel);

    auto bindings = Bindings {};
    bindings.set(kernel.input, input);
    bindings.set(kernel.shifted, shifted);
    bindings.set(kernel.records, records);

    for (auto offset: {4u, 0xfffffffeu})
    {
        kernel.offset = offset;
        check(executor.dispatch(bindings, count));

        for (auto i = 0u; i < (std::uint32_t) count; ++i)
            check(shifted[(int) i] == elementOr(input, i + offset) + 1.f);
    }

    for (auto e = 0u; e < (std::uint32_t) count * 4u; ++e)
        check(records[(int) e] == elementOr(input, e));

    check(records[8] == 9.f);
    check(records[10] == 0.f);
};

auto tReadEmpty = test("Executor/aReadFromAnEmptySpanIsZero") = []
{
    constexpr auto count = 5;

    auto kernel = OffsetReadKernel {};
    kernel.offset = 1u;

    auto executor = Executor {kernel};
    auto shifted = makeFloats(count, sentinel);
    auto records = makeFloats(count * 4, sentinel);

    auto bindings = Bindings {};
    bindings.set(kernel.input, std::span<const float> {});
    bindings.set(kernel.shifted, shifted);
    bindings.set(kernel.records, records);
    check(executor.dispatch(bindings, count));

    for (auto value: shifted)
        check(value == 1.f);

    for (auto value: records)
        check(value == 0.f);
};

auto tStoreOutOfBounds = test("Executor/aStoreOutOfBoundsIsDropped") = []
{
    constexpr auto count = 8;
    constexpr auto bound = 10;

    auto kernel = ScatterKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto scalars = makeFloats(40, sentinel);
    auto records = makeFloats(40, sentinel);
    auto wide = makeFloats(40, sentinel);

    auto bindings = Bindings {};
    bindings.set(kernel.scalars, std::span<float>(scalars.data(), bound));
    bindings.set(kernel.records, std::span<float>(records.data(), bound));
    bindings.set(kernel.wide, std::span<float>(wide.data(), bound));
    check(executor.dispatch(bindings, count));

    for (auto e = 0; e < scalars.size(); ++e)
    {
        auto writer = e / 3;
        check(scalars[e]
              == (e < bound && e % 3 == 0 ? static_cast<float>(writer) + 0.5f
                                          : sentinel));
    }

    for (auto e = 0; e < records.size(); ++e)
    {
        auto record = e / 4;
        auto component = e % 4;
        auto inRange = e < bound;

        check(records[e]
              == (!inRange         ? sentinel
                  : component == 0 ? (float) record
                                   : (float) component));
        check(wide[e]
              == (!inRange         ? sentinel
                  : component == 0 ? (float) record
                                   : (float) component + 3.f));
    }

    auto emptyBindings = Bindings {};
    emptyBindings.set(kernel.scalars, std::span<float> {});
    emptyBindings.set(kernel.records, std::span<float> {});
    emptyBindings.set(kernel.wide, std::span<float> {});

    auto scalarsBefore = scalars;
    auto recordsBefore = records;
    auto wideBefore = wide;
    check(executor.dispatch(emptyBindings, count));

    for (auto e = 0; e < records.size(); ++e)
    {
        check(scalars[e] == scalarsBefore[e]);
        check(records[e] == recordsBefore[e]);
        check(wide[e] == wideBefore[e]);
    }
};

auto tArrayOutOfBounds = test("Executor/anArrayReadOutOfBoundsIsZero") = []
{
    constexpr auto count = 7;

    auto builder = ShaderBuilder {};
    auto output = builder.outputBuffer();
    auto pairs = builder.outputBuffer();
    auto scale = builder.uniform<Float>();
    auto i = builder.threadId();

    auto table = builder.array(scale * 1.5f, scale * 2.5f, scale * 3.5f);
    auto pairTable = builder.array(float2(scale, 1.f), float2(2.f, scale));
    auto index = toInt(i) - 2;

    builder.write(output, i, table[index]);
    builder.write(pairs, i, pairTable[index - 1]);

    auto executor = Executor {builder.graph()};
    check(executor.isValid(), executor.reason());

    auto value = 2.f;
    check(executor.setUniform(0, &value, sizeof(value)));

    auto values = makeFloats(count, sentinel);
    auto pairValues = makeFloats(count * 2, sentinel);

    auto bindings = Bindings {};
    bindings.set(output, values);
    bindings.set(pairs, pairValues);
    check(executor.dispatch(bindings, count));

    auto expected = std::array<float, count> {0.f, 0.f, 3.f, 5.f, 7.f, 0.f, 0.f};
    auto expectedPairs = std::array<float, count * 2> {
        0.f, 0.f, 0.f, 0.f, 0.f, 0.f, 2.f, 1.f, 2.f, 2.f, 0.f, 0.f, 0.f, 0.f};

    for (auto k = 0; k < count; ++k)
        check(values[k] == expected[(std::size_t) k]);

    for (auto k = 0; k < count * 2; ++k)
        check(pairValues[k] == expectedPairs[(std::size_t) k]);
};

// ---------------------------------------------------------------------------
// Group shapes: a partial last group in each rank with a custom shape, and a 1D
// kernel whose group is two rows tall.

namespace
{
struct WideGroupKernel final : ComputeKernel
{
    WideGroupKernel()
        : ComputeKernel({256})
    {
        compile();
    }

    void define() override
    {
        auto i = threadId();
        write(output, i, toFloat(i) * 2.f + toFloat(gridCount()));
    }

    Uniform<OutputBuffer> output;

    EACP_SHADER(output)
};

struct TallGroupKernel final : ComputeKernel
{
    TallGroupKernel()
        : ComputeKernel({32, 2})
    {
        compile();
    }

    void define() override
    {
        auto i = threadId();
        write(output, i, output[i] + 1.f);
        write(ids, i, i);
    }

    Uniform<OutputBuffer> output;
    Uniform<UIntOutputBuffer> ids;

    EACP_SHADER(output, ids)
};

struct ShapedGridKernel final : ComputeKernel
{
    explicit ShapedGridKernel(ThreadGroupShape shape)
        : ComputeKernel(shape)
    {
        compile();
    }

    void define() override
    {
        auto position = threadPosition();
        auto cell = position.y * gridWidth() + position.x;
        write(output, cell, position.x * 1000u + position.y);
    }

    Uniform<UIntOutputBuffer> output;

    EACP_SHADER(output)
};

struct ShapedVolumeKernel final : ComputeKernel
{
    explicit ShapedVolumeKernel(ThreadGroupShape shape)
        : ComputeKernel(shape)
    {
        compile();
    }

    void define() override
    {
        auto position = threadPosition3();
        auto cell =
            (position.z * gridHeight() + position.y) * gridWidth() + position.x;
        write(output, cell, position.x * 10000u + position.y * 100u + position.z);
    }

    Uniform<UIntOutputBuffer> output;

    EACP_SHADER(output)
};
} // namespace

auto tWideGroup = test("Executor/aCustomOneDimensionalGroupGuardsItsLastGroup") = []
{
    constexpr auto count = 1000;

    auto kernel = WideGroupKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());
    check(executor.plan().lanes() == 256);

    auto output = makeFloats(1024 + 16, sentinel);

    auto bindings = Bindings {};
    bindings.set(kernel.output, output);
    check(executor.dispatch(bindings, count));

    for (auto i = 0; i < count; ++i)
        check(output[i] == (float) i * 2.f + (float) count);

    for (auto i = count; i < output.size(); ++i)
        check(output[i] == sentinel);
};

auto tTallGroup = test("Executor/aOneDimensionalGroupWithRowsRepeatsTheId") = []
{
    constexpr auto count = 50;

    auto kernel = TallGroupKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());
    check(executor.plan().lanes() == 64);
    check(executor.plan().groupShape().y == 2);

    auto output = makeFloats(count + 30, 0.f);
    auto ids = makeUInts(count + 30, sentinelBits);

    auto bindings = Bindings {};
    bindings.set(kernel.output, output);
    bindings.set(kernel.ids, ids);
    check(executor.dispatch(bindings, count));

    // Both rows of a group carry the same ids, and every lane of one statement
    // reads before any lane commits, so the pair adds one between them rather
    // than two - lockstep's answer to what is a race on the GPU.
    for (auto i = 0; i < count; ++i)
    {
        check(output[i] == 1.f);
        check(ids[i] == (std::uint32_t) i);
    }

    for (auto i = count; i < output.size(); ++i)
    {
        check(output[i] == 0.f);
        check(ids[i] == sentinelBits);
    }
};

auto tShapedGrid = test("Executor/aCustomTwoDimensionalGroupGuardsBothAxes") = []
{
    constexpr auto width = 37;
    constexpr auto height = 9;

    auto kernel = ShapedGridKernel {{16, 4}};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());
    check(executor.plan().lanes() == 64);

    auto output = makeUInts(48 * 12, sentinelBits);

    auto bindings = Bindings {};
    bindings.set(kernel.output, output);
    check(executor.dispatch(bindings, width, height));

    for (auto y = 0u; y < (std::uint32_t) height; ++y)
        for (auto x = 0u; x < (std::uint32_t) width; ++x)
            check(output[(int) (y * width + x)] == x * 1000u + y);

    for (auto i = width * height; i < output.size(); ++i)
        check(output[i] == sentinelBits);
};

auto tShapedVolume =
    test("Executor/aCustomThreeDimensionalGroupGuardsEveryAxis") = []
{
    constexpr auto width = 5;
    constexpr auto height = 7;
    constexpr auto depth = 9;
    constexpr auto cells = width * height * depth;

    auto kernel = ShapedVolumeKernel {{2, 3, 4}};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());
    check(executor.plan().lanes() == 24);

    auto output = makeUInts(6 * 9 * 12, sentinelBits);

    auto bindings = Bindings {};
    bindings.set(kernel.output, output);
    check(executor.dispatch(bindings, width, height, depth));

    for (auto z = 0u; z < (std::uint32_t) depth; ++z)
        for (auto y = 0u; y < (std::uint32_t) height; ++y)
            for (auto x = 0u; x < (std::uint32_t) width; ++x)
                check(output[(int) ((z * height + y) * width + x)]
                      == x * 10000u + y * 100u + z);

    for (auto i = cells; i < output.size(); ++i)
        check(output[i] == sentinelBits);
};

// ---------------------------------------------------------------------------
// Uniforms of every shape, read through the member walk on each dispatch.

namespace
{
struct UniformKindsKernel final : ComputeKernel
{
    UniformKindsKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto moved = origin + uint2(i, i * 2u);

        write(floats, i, float4(offset + toFloat(i), toFloat(bias)));
        write(uints, i, uint4(moved, toUInt(bias), toUInt(bias * -2)));
        write(transformed, i, transform * float4(toFloat(i), 1.f, 0.f, 1.f));
    }

    Uniform<Float3> offset;
    Uniform<UInt2> origin;
    Uniform<Int> bias;
    Uniform<Float4x4> transform;
    Uniform<OutputBuffer> floats;
    Uniform<UIntOutputBuffer> uints;
    Uniform<OutputBuffer> transformed;

    EACP_SHADER(offset, origin, bias, transform, floats, uints, transformed)
};

void checkUniformKinds(UniformKindsKernel& kernel,
                       Executor& executor,
                       Array<float, 3> offset,
                       Array<std::uint32_t, 2> origin,
                       std::int32_t bias,
                       float translate)
{
    constexpr auto count = 6;

    kernel.offset = offset;
    kernel.origin = origin;
    kernel.bias = bias;

    auto identity = Array<float, 16> {};

    for (auto k = 0; k < 16; ++k)
        identity[k] = k % 5 == 0 ? 1.f : 0.f;

    identity[12] = translate;
    identity[13] = -translate;
    kernel.transform = identity;

    auto floats = makeFloats(count * 4, sentinel);
    auto uints = makeUInts(count * 4, sentinelBits);
    auto transformed = makeFloats(count * 4, sentinel);

    auto bindings = Bindings {};
    bindings.set(kernel.floats, floats);
    bindings.set(kernel.uints, uints);
    bindings.set(kernel.transformed, transformed);
    check(executor.dispatch(bindings, count));

    for (auto i = 0; i < count; ++i)
    {
        auto index = (float) i;
        check(floats[i * 4 + 0] == offset[0] + index);
        check(floats[i * 4 + 1] == offset[1] + index);
        check(floats[i * 4 + 2] == offset[2] + index);
        check(floats[i * 4 + 3] == (float) bias);

        check(uints[i * 4 + 0] == origin[0] + (std::uint32_t) i);
        check(uints[i * 4 + 1] == origin[1] + (std::uint32_t) i * 2u);
        check(uints[i * 4 + 2] == bitsOf(bias));
        check(uints[i * 4 + 3] == bitsOf(bias) * (0u - 2u));

        check(transformed[i * 4 + 0] == index + translate);
        check(transformed[i * 4 + 1] == 1.f - translate);
        check(transformed[i * 4 + 2] == 0.f);
        check(transformed[i * 4 + 3] == 1.f);
    }
}
} // namespace

auto tUniformKinds = test("Executor/uniformsOfEveryShapeAreReadEachDispatch") = []
{
    auto kernel = UniformKindsKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());
    check(executor.plan().uniformCount() == 4);

    checkUniformKinds(
        kernel, executor, {0.5f, -1.f, 2.25f}, {4000000000u, 17u}, -9, 3.f);
    checkUniformKinds(
        kernel, executor, {-8.f, 0.f, 1e6f}, {1u, 0xffffffffu}, 12, -0.5f);
};

auto tGainChanges = test("Executor/aUniformChangedBetweenDispatchesIsSeen") = []
{
    constexpr auto count = 70;

    auto kernel = GainKernel {};
    auto executor = Executor {kernel};

    auto input = ramp(count);
    auto output = makeFloats(count, sentinel);

    auto bindings = Bindings {};
    bindings.set(kernel.input, input);
    bindings.set(kernel.output, output);

    for (auto gain: {0.5f, -3.f, 0.f, 1e-3f})
    {
        kernel.gain = gain;
        check(executor.dispatch(bindings, count));

        for (auto i = 0; i < count; ++i)
            check(output[i] == input[i] * gain);
    }
};

// ---------------------------------------------------------------------------
// Refusals: what dispatch will not run, and what a plan will not accept.

namespace
{
struct ImageKernel final : ComputeKernel
{
    ImageKernel() { compile(); }

    void define() override
    {
        auto position = threadPosition();
        write(output, position.x, 1.f + toFloat(position.y));
        write(image,
              position.x,
              position.y,
              float4(toFloat(position.x), 0.f, 0.f, 1.f));
    }

    Uniform<OutputBuffer> output;
    Uniform<WritableTexture2D> image;

    EACP_SHADER(output, image)
};

struct ErfKernel final : ComputeKernel
{
    ErfKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, erf(input[i]));
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;

    EACP_SHADER(input, output)
};

bool refusedNaming(const Executor& executor, const char* name)
{
    return !executor.isValid() && executor.reason().find(name) != std::string::npos;
}
} // namespace

auto tUnboundWrite = test("Executor/anUnboundWriteSlotRefusesTheDispatch") = []
{
    auto kernel = GainKernel {};
    auto executor = Executor {kernel};
    auto input = makeFloats(4, 1.f);

    auto bindings = Bindings {};
    bindings.set(kernel.input, input);
    check(!executor.dispatch(bindings, 4));

    auto output = makeFloats(4, sentinel);
    bindings.set(kernel.output, output);
    check(executor.dispatch(bindings, 4));

    bindings.clear(kernel.output.slot);
    check(!executor.dispatch(bindings, 4));
};

auto tWrongElement = test("Executor/aSlotBoundAsTheWrongElementTypeRefuses") = []
{
    auto kernel = GainKernel {};
    auto executor = Executor {kernel};
    auto uints = makeUInts(4, 1u);
    auto output = makeFloats(4, sentinel);

    auto impostor = UIntInputBuffer {};
    impostor.slot = kernel.input.slot;

    auto bindings = Bindings {};
    bindings.set(impostor, uints);
    bindings.set(kernel.output, output);
    check(!executor.dispatch(bindings, 4));
    check(output[0] == sentinel);
};

auto tWrongAccess = test("Executor/aSlotBoundWithTheWrongAccessRefuses") = []
{
    auto kernel = GainKernel {};
    auto executor = Executor {kernel};
    auto input = makeFloats(4, 1.f);
    auto output = makeFloats(4, sentinel);

    auto impostor = OutputBuffer {};
    impostor.slot = kernel.input.slot;

    auto bindings = Bindings {};
    bindings.set(impostor, input);
    bindings.set(kernel.output, output);
    check(!executor.dispatch(bindings, 4));
    check(output[0] == sentinel);

    auto readOnlyOutput = InputBuffer {};
    readOnlyOutput.slot = kernel.output.slot;

    auto swapped = Bindings {};
    swapped.set(kernel.input, input);
    swapped.set(readOnlyOutput, std::span<const float>(output));
    check(!executor.dispatch(swapped, 4));
    check(output[0] == sentinel);
};

auto tWrongRank = test("Executor/aDispatchOfTheWrongRankRefuses") = []
{
    auto oneD = GainKernel {};
    auto grid = GridKernel {};
    auto volume = VolumeKernel {};

    auto runOneD = Executor {oneD};
    auto runGrid = Executor {grid};
    auto runVolume = Executor {volume};

    auto input = makeFloats(16, 1.f);
    auto floats = makeFloats(16, sentinel);
    auto uints = makeUInts(16, sentinelBits);

    auto oneDBindings = Bindings {};
    oneDBindings.set(oneD.input, input);
    oneDBindings.set(oneD.output, floats);

    auto gridBindings = Bindings {};
    gridBindings.set(grid.output, floats);

    auto volumeBindings = Bindings {};
    volumeBindings.set(volume.output, uints);

    check(!runOneD.dispatch(oneDBindings, 4, 4));
    check(!runOneD.dispatch(oneDBindings, 2, 2, 4));
    check(!runGrid.dispatch(gridBindings, 16));
    check(!runGrid.dispatch(gridBindings, 2, 2, 4));
    check(!runVolume.dispatch(volumeBindings, 16));
    check(!runVolume.dispatch(volumeBindings, 4, 4));

    for (auto k = 0; k < 16; ++k)
    {
        check(floats[k] == sentinel);
        check(uints[k] == sentinelBits);
    }
};

auto tEmptyDispatch =
    test("Executor/aDispatchOfNoThreadsRunsNothingAndSucceeds") = []
{
    auto kernel = GainKernel {};
    kernel.gain = 2.f;

    auto grid = GridKernel {};
    auto volume = VolumeKernel {};

    auto input = makeFloats(8, 1.f);
    auto floats = makeFloats(8, sentinel);
    auto uints = makeUInts(8, sentinelBits);

    auto executor = Executor {kernel};
    auto runGrid = Executor {grid};
    auto runVolume = Executor {volume};

    auto bindings = Bindings {};
    bindings.set(kernel.input, input);
    bindings.set(kernel.output, floats);

    auto gridBindings = Bindings {};
    gridBindings.set(grid.output, floats);

    auto volumeBindings = Bindings {};
    volumeBindings.set(volume.output, uints);

    check(executor.dispatch(bindings, 0));
    check(executor.dispatch(bindings, -5));
    check(runGrid.dispatch(gridBindings, 0, 4));
    check(runGrid.dispatch(gridBindings, 4, -1));
    check(runVolume.dispatch(volumeBindings, 2, 2, 0));
    check(runVolume.dispatch(volumeBindings, -1, 2, 2));

    for (auto k = 0; k < 8; ++k)
    {
        check(floats[k] == sentinel);
        check(uints[k] == sentinelBits);
    }

    auto unbound = Bindings {};
    check(!executor.dispatch(unbound, 0));
};

auto tOutOfTier = test("Executor/everyOutOfTierConstructIsRefusedByName") = []
{
    auto image = ImageKernel {};
    auto errorFunction = ErfKernel {};

    check(refusedNaming(Executor {image}, "TextureStore"));
    check(refusedNaming(Executor {errorFunction}, "eacpErf"));

    auto builder = ShaderBuilder {};
    auto output = builder.outputBuffer();
    auto texture = builder.texture();
    auto i = builder.threadId();
    builder.write(output, i, sample(texture, float2(toFloat(i), 0.5f)).x());

    auto sampled = Executor {builder.graph()};
    check(refusedNaming(sampled, "Sample"));
    check(refusedNaming(sampled, "texture"));

    auto values = makeFloats(4, sentinel);
    auto bindings = Bindings {};
    bindings.set(output, values);
    check(!sampled.dispatch(bindings, 4));
    check(values[0] == sentinel);
};

// ---------------------------------------------------------------------------
// Bare graphs the typed builder cannot produce, each refused with a reason.

namespace
{
struct BareGraph
{
    BareGraph()
    {
        output = graph.addStorageBuffer(BufferAccess::Write, ValueType::Float);
        thread = graph.addThreadId();
        one = graph.addConstant(1.f);
        yes = graph.addBoolConstant(true);
    }

    void storeOne() { graph.addStore(output, thread, one); }

    ShaderGraph graph;
    int output = -1;
    int thread = -1;
    int one = -1;
    int yes = -1;
};
} // namespace

auto tStoreIntoReadSlot = test("Executor/aStoreIntoAReadOnlySlotIsInvalid") = []
{
    auto bare = BareGraph {};
    auto input = bare.graph.addStorageBuffer(BufferAccess::Read, ValueType::Float);
    bare.graph.addStore(input, bare.thread, bare.one);

    check(refusedNaming(Executor {bare.graph}, "read-only"));
};

auto tStoreFamily = test("Executor/aStoreOfTheWrongFamilyIsInvalid") = []
{
    auto floatIntoUInt = BareGraph {};
    auto uints =
        floatIntoUInt.graph.addStorageBuffer(BufferAccess::Write, ValueType::UInt);
    floatIntoUInt.graph.addStore(uints, floatIntoUInt.thread, floatIntoUInt.one);

    check(refusedNaming(Executor {floatIntoUInt.graph}, "into storage slot"));

    auto uintIntoFloat = BareGraph {};
    uintIntoFloat.graph.addStore(
        uintIntoFloat.output, uintIntoFloat.thread, uintIntoFloat.thread);

    check(refusedNaming(Executor {uintIntoFloat.graph}, "into storage slot"));
};

auto tDeclareType = test("Executor/aDeclareOfTheWrongTypeIsInvalid") = []
{
    auto bare = BareGraph {};
    bare.graph.addVariable(ValueType::Float4, bare.one);
    bare.storeOne();

    check(refusedNaming(Executor {bare.graph}, "Declare gives variable 0"));
};

auto tAssignType = test("Executor/anAssignOfTheWrongTypeIsInvalid") = []
{
    auto bare = BareGraph {};
    auto variable = bare.graph.addVariable(ValueType::Float, bare.one);
    bare.graph.assign(variable, bare.thread);
    bare.storeOne();

    check(refusedNaming(Executor {bare.graph}, "Assign gives variable 0"));
};

auto tMissingBlock = test("Executor/aBodyNamingNoBlockIsInvalid") = []
{
    auto bare = BareGraph {};
    bare.graph.addIf(bare.yes, 7, -1);
    bare.storeOne();

    check(refusedNaming(Executor {bare.graph}, "block 7, which does not exist"));

    auto negative = BareGraph {};
    negative.graph.addLoop(negative.yes, -3);
    negative.storeOne();

    check(refusedNaming(Executor {negative.graph}, "block -3"));
};

auto tSharedBlock = test("Executor/aBlockReachedTwiceIsInvalid") = []
{
    auto shared = BareGraph {};
    auto body = shared.graph.pushBlock();
    shared.storeOne();
    shared.graph.popBlock();
    shared.graph.addIf(shared.yes, body, -1);
    shared.graph.addIf(shared.yes, body, -1);

    check(refusedNaming(Executor {shared.graph}, "reached twice"));

    auto cycle = BareGraph {};
    auto loopBody = cycle.graph.pushBlock();
    cycle.storeOne();
    cycle.graph.addIf(cycle.yes, loopBody, -1);
    cycle.graph.popBlock();
    cycle.graph.addLoop(cycle.yes, loopBody);

    check(refusedNaming(Executor {cycle.graph}, "reached twice"));

    auto root = BareGraph {};
    root.graph.addIf(root.yes, ShaderGraph::rootBlock, -1);
    root.storeOne();

    check(refusedNaming(Executor {root.graph}, "reached twice"));
};

auto tThreadIdAxis = test("Executor/aThreadIdNamingNoAxisIsInvalid") = []
{
    auto graph = ShaderGraph {};
    auto output = graph.addStorageBuffer(BufferAccess::Write, ValueType::UInt);
    auto axis = graph.addThreadPosition(5);
    graph.addStore(output, axis, axis);

    auto executor = Executor {graph};
    check(refusedNaming(executor, "ThreadId"));
    check(refusedNaming(executor, "no axis"));

    auto handle = UIntOutputBuffer {};
    handle.slot = output;

    auto values = makeUInts(4, sentinelBits);
    auto bindings = Bindings {};
    bindings.set(handle, values);
    check(!executor.dispatch(bindings, 2, 2));
    check(values[0] == sentinelBits);
};

auto tHugeGroup = test("Executor/aGroupTooLargeToAddressIsInvalid") = []
{
    auto bare = BareGraph {};
    bare.graph.setThreadGroupShape({46341, 46341, 1});
    bare.storeOne();

    auto executor = Executor {bare.graph};
    check(refusedNaming(executor, "threads"));
    check(refusedNaming(executor, "at most 1048576"));
    check(executor.plan().totalWords() == 0);

    auto capped = BareGraph {};
    capped.graph.setThreadGroupShape({1024, 1024, 2});
    capped.storeOne();
    check(refusedNaming(Executor {capped.graph}, "2097152 threads"));

    auto handle = OutputBuffer {};
    handle.slot = bare.output;

    auto values = makeFloats(4, sentinel);
    auto bindings = Bindings {};
    bindings.set(handle, values);
    check(!executor.dispatch(bindings, 4));
    check(values[0] == sentinel);
};

// ---------------------------------------------------------------------------
// The realtime contract: a dispatch after the first allocates nothing. Global
// operator new and delete are replaced at the bottom of this file with forms
// that count while `countingHeap` is set, which is why this must stay the only
// translation unit in the binary that replaces them.

namespace
{
std::atomic<bool> countingHeap {false};
std::atomic<int> allocationsCounted {0};
std::atomic<int> releasesCounted {0};

struct RealtimeKernel final : ComputeKernel
{
    RealtimeKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto accumulated = var(0.f);
        auto turns = var(0u);

        loop(turns < 8u,
             [&]
             {
                 ifThen(turns.get() == limit + (i & 1u), [&] { breakLoop(); });
                 accumulated += input[i] * gain;
                 turns += 1u;
             });

        write(output,
              i,
              float4(accumulated.get(), toFloat(turns.get()), gain, input[i]));
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;
    Uniform<Float> gain;
    Uniform<UInt> limit;

    EACP_SHADER(input, output, gain, limit)
};
} // namespace

auto tNoAllocation = test("Executor/aDispatchAfterTheFirstAllocatesNothing") = []
{
    constexpr auto count = 150;

    auto kernel = RealtimeKernel {};
    kernel.gain = 0.5f;
    kernel.limit = 3u;

    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto input = ramp(count);
    auto output = makeFloats(count * 4, sentinel);

    auto bindings = Bindings {};
    bindings.set(kernel.input, input);
    bindings.set(kernel.output, output);

    auto warmedUp = executor.dispatch(bindings, count);

    kernel.gain = 0.25f;

    allocationsCounted = 0;
    releasesCounted = 0;
    countingHeap = true;
    ::operator delete(::operator new(16));
    countingHeap = false;

    auto counterWorks =
        allocationsCounted.load() == 1 && releasesCounted.load() == 1;

    allocationsCounted = 0;
    releasesCounted = 0;
    countingHeap = true;
    auto ran = executor.dispatch(bindings, count);
    countingHeap = false;

    check(counterWorks);
    check(warmedUp);
    check(ran);
    check(allocationsCounted.load() == 0);
    check(releasesCounted.load() == 0);

    for (auto i = 0; i < count; ++i)
    {
        auto turns = 3 + (i & 1);
        auto accumulated = 0.f;

        for (auto turn = 0; turn < turns; ++turn)
            accumulated += input[i] * 0.25f;

        check(output[i * 4 + 0] == accumulated);
        check(output[i * 4 + 1] == (float) turns);
        check(output[i * 4 + 2] == 0.25f);
        check(output[i * 4 + 3] == input[i]);
    }
};

namespace
{
constexpr auto realtimeGroupWidth = 96;

struct RealtimeGroupKernel final : ComputeKernel
{
    RealtimeGroupKernel()
        : ComputeKernel({realtimeGroupWidth})
    {
        compile();
    }

    void define() override
    {
        auto lane = localId();
        auto i = threadId();
        auto width = (unsigned) realtimeGroupWidth;
        auto tile = shared<Float>(realtimeGroupWidth);

        write(tile, lane, input[i]);
        barrier();

        auto total = groupSum(tile[(lane + 1u) % width]);
        auto peak = simdMax(tile[lane]);
        auto ticket = atomicAdd(counter, 0u, 1u);
        auto place = groupId() * 10u + simdGroupIndex();

        ifThen(i < gridCount(),
               [&]
               {
                   write(output,
                         i,
                         float4(total, peak, toFloat(place), toFloat(ticket)));
               });
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;
    Uniform<AtomicBuffer> counter;

    EACP_SHADER(input, output, counter)
};

bool matchesRealtimeGroups(const Vector<float>& input, const Vector<float>& output)
{
    auto count = input.size();
    auto valueAt = [&](int i) { return i < count ? input[i] : 0.f; };
    auto matches = true;

    for (auto i = 0; i < count; ++i)
    {
        auto group = i / realtimeGroupWidth;
        auto block = i / simdGroupWidth;
        auto total = 0.f;
        auto peak = 0.f;

        for (auto k = 0; k < realtimeGroupWidth; ++k)
            total += valueAt(group * realtimeGroupWidth + k);

        for (auto k = 0; k < simdGroupWidth; ++k)
            peak = std::max(peak, valueAt(block * simdGroupWidth + k));

        auto simdGroup = (i % realtimeGroupWidth) / simdGroupWidth;

        matches = matches && output[i * 4 + 0] == total && output[i * 4 + 1] == peak
                  && output[i * 4 + 2] == (float) (group * 10 + simdGroup)
                  && output[i * 4 + 3] == (float) i;
    }

    return matches;
}
} // namespace

auto tNoGroupAllocation =
    test("Executor/aGroupDispatchAfterTheFirstAllocatesNothing") = []
{
    constexpr auto count = 150;

    auto kernel = RealtimeGroupKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto input = ramp(count);
    auto output = makeFloats(count * 4, sentinel);
    auto counter = makeUInts(1, 0u);
    auto arguments = std::array<std::uint32_t, 3> {2u, 1u, 1u};

    auto bindings = Bindings {};
    bindings.set(kernel.input, input);
    bindings.set(kernel.output, output);
    bindings.set(kernel.counter, counter);

    auto warmedUp = executor.dispatch(bindings, count);
    counter[0] = 0u;

    allocationsCounted = 0;
    releasesCounted = 0;
    countingHeap = true;
    auto ran = executor.dispatch(bindings, count);
    countingHeap = false;

    auto direct = matchesRealtimeGroups(input, output);
    output = makeFloats(count * 4, sentinel);
    counter[0] = 0u;
    bindings.set(kernel.output, output);

    countingHeap = true;
    auto ranIndirect = executor.dispatchIndirect(bindings, arguments, count);
    countingHeap = false;

    check(warmedUp);
    check(ran);
    check(ranIndirect);
    check(allocationsCounted.load() == 0);
    check(releasesCounted.load() == 0);
    check(direct);
    check(matchesRealtimeGroups(input, output));
    check(counter[0] == 2u * realtimeGroupWidth);
};

namespace
{
void noteAllocation()
{
    if (countingHeap.load(std::memory_order_relaxed))
        allocationsCounted.fetch_add(1, std::memory_order_relaxed);
}

void noteRelease(void* pointer)
{
    if (pointer != nullptr && countingHeap.load(std::memory_order_relaxed))
        releasesCounted.fetch_add(1, std::memory_order_relaxed);
}

void* allocateBytes(std::size_t size)
{
    noteAllocation();
    return std::malloc(size == 0 ? 1 : size);
}

void* allocateAligned(std::size_t size, std::align_val_t alignment)
{
    noteAllocation();

    auto align = std::max((std::size_t) alignment, sizeof(void*));
    auto bytes = size == 0 ? align : size;

#if defined(_WIN32)
    return _aligned_malloc(bytes, align);
#else
    void* pointer = nullptr;
    return posix_memalign(&pointer, align, bytes) == 0 ? pointer : nullptr;
#endif
}

void releaseBytes(void* pointer) noexcept
{
    noteRelease(pointer);
    std::free(pointer);
}

void releaseAligned(void* pointer) noexcept
{
    noteRelease(pointer);

#if defined(_WIN32)
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}

void* allocateOrThrow(void* pointer)
{
    if (pointer == nullptr)
        throw std::bad_alloc {};

    return pointer;
}
} // namespace

void* operator new(std::size_t size)
{
    return allocateOrThrow(allocateBytes(size));
}

void* operator new[](std::size_t size)
{
    return allocateOrThrow(allocateBytes(size));
}

void* operator new(std::size_t size, std::align_val_t alignment)
{
    return allocateOrThrow(allocateAligned(size, alignment));
}

void* operator new[](std::size_t size, std::align_val_t alignment)
{
    return allocateOrThrow(allocateAligned(size, alignment));
}

void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
    return allocateBytes(size);
}

void* operator new[](std::size_t size, const std::nothrow_t&) noexcept
{
    return allocateBytes(size);
}

void* operator new(std::size_t size,
                   std::align_val_t alignment,
                   const std::nothrow_t&) noexcept
{
    return allocateAligned(size, alignment);
}

void* operator new[](std::size_t size,
                     std::align_val_t alignment,
                     const std::nothrow_t&) noexcept
{
    return allocateAligned(size, alignment);
}

void operator delete(void* pointer) noexcept
{
    releaseBytes(pointer);
}

void operator delete[](void* pointer) noexcept
{
    releaseBytes(pointer);
}

void operator delete(void* pointer, std::size_t) noexcept
{
    releaseBytes(pointer);
}

void operator delete[](void* pointer, std::size_t) noexcept
{
    releaseBytes(pointer);
}

void operator delete(void* pointer, std::align_val_t) noexcept
{
    releaseAligned(pointer);
}

void operator delete[](void* pointer, std::align_val_t) noexcept
{
    releaseAligned(pointer);
}

void operator delete(void* pointer, std::size_t, std::align_val_t) noexcept
{
    releaseAligned(pointer);
}

void operator delete[](void* pointer, std::size_t, std::align_val_t) noexcept
{
    releaseAligned(pointer);
}

void operator delete(void* pointer, const std::nothrow_t&) noexcept
{
    releaseBytes(pointer);
}

void operator delete[](void* pointer, const std::nothrow_t&) noexcept
{
    releaseBytes(pointer);
}

void operator delete(void* pointer, std::align_val_t, const std::nothrow_t&) noexcept
{
    releaseAligned(pointer);
}

void operator delete[](void* pointer,
                       std::align_val_t,
                       const std::nothrow_t&) noexcept
{
    releaseAligned(pointer);
}
