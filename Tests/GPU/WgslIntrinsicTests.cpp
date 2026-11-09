#include "CpuCrossCheck.h"

#include <cmath>
#include <cstdint>
#include <initializer_list>

// The WGSL built-ins run on the device and on the CPU executor, and the two
// compared. Where a value has one right answer - every bit function, every
// pack, fma of operands whose product is exact, the angle conversions, the
// half round trip of a value whose rounding no backend can disagree about -
// the backends must agree bit for bit; the transcendentals and the unpacks'
// divisions are held to a tolerance, Metal compiling with fast math.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::GPU::CrossChecks;

namespace WgslIntrinsicTests
{
namespace
{
struct InverseHyperbolicKernel final : ComputeProgram
{
    InverseHyperbolicKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto x = input[i];

        write(output, i * 3u, asinh(x));
        write(output, i * 3u + 1u, acosh(abs(x) + 1.5f));
        write(output, i * 3u + 2u, atanh(x / (abs(x) + 4.0f)));
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;

    EACP_SHADER(input, output)
};

struct VectorInverseHyperbolicKernel final : ComputeProgram
{
    VectorInverseHyperbolicKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto x = input.read4(i);

        write(output, i * 3u, asinh(x));
        write(output, i * 3u + 1u, acosh(abs(x) + 1.5f));
        write(output, i * 3u + 2u, atanh(x / (abs(x) + 4.0f)));
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;

    EACP_SHADER(input, output)
};

struct ExactArithmeticKernel final : ComputeProgram
{
    ExactArithmeticKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto x = input[i];
        auto at = i * 5u;

        write(output, at, fma(x, x * 3.0f, -x));
        write(output, at + 1u, fma(0.5f, x, 0.25f));
        write(output, at + 2u, degrees(x));
        write(output, at + 3u, radians(x));
        write(output, at + 4u, quantizeToF16(x));

        auto record = input.read4(i / 4u);
        write(vectors, i * 2u, fma(record, x, 0.5f));
        write(vectors, i * 2u + 1u, quantizeToF16(record));
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;
    Uniform<OutputBuffer> vectors;

    EACP_SHADER(input, output, vectors)
};

struct BitKernel final : ComputeProgram
{
    BitKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto word = words[i];
        auto offset = fields[i * 2u];
        auto count = fields[i * 2u + 1u];
        auto signedWord = toInt(word);
        auto at = i * 12u;

        write(output, at, countOneBits(word));
        write(output, at + 1u, reverseBits(word));
        write(output, at + 2u, countLeadingZeros(word));
        write(output, at + 3u, countTrailingZeros(word));
        write(output, at + 4u, firstLeadingBit(word));
        write(output, at + 5u, firstTrailingBit(word));
        write(output, at + 6u, extractBits(word, offset, count));
        write(output, at + 7u, insertBits(word, ~word, offset, count));
        write(output, at + 8u, toUInt(firstLeadingBit(signedWord)));
        write(output, at + 9u, toUInt(extractBits(signedWord, offset, count)));
        write(output,
              at + 10u,
              toUInt(insertBits(signedWord, -signedWord, offset, 7u)));
        write(output,
              at + 11u,
              toUInt(countLeadingZeros(signedWord) + firstTrailingBit(signedWord)
                     + countOneBits(signedWord) + reverseBits(signedWord)));

        auto record = words.read4(i / 4u);
        auto signedRecord = toInt(record.xyz());
        write(vectors,
              i * 2u,
              firstLeadingBit(record) + extractBits(record, offset, count));
        write(
            vectors, i * 2u + 1u, insertBits(record, record.yzwx(), offset, count));
        write(signedVectors,
              i,
              toUInt(firstLeadingBit(signedRecord)
                     + extractBits(signedRecord, offset, count)
                     + insertBits(signedRecord, signedRecord, offset, 5u)));
    }

    Uniform<UIntInputBuffer> words;
    Uniform<UIntInputBuffer> fields;
    Uniform<UIntOutputBuffer> output;
    Uniform<UIntOutputBuffer> vectors;
    Uniform<UIntOutputBuffer> signedVectors;

    EACP_SHADER(words, fields, output, vectors, signedVectors)
};

struct NormalizedPackKernel final : ComputeProgram
{
    NormalizedPackKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto record = floats.read4(i);
        auto word = words[i];

        write(packs, i * 4u, packUnorm8x4(record));
        write(packs, i * 4u + 1u, packSnorm8x4(record));
        write(packs, i * 4u + 2u, packUnorm16x2(record.xy()));
        write(packs, i * 4u + 3u, packSnorm16x2(record.zw()));

        write(unpacks, i * 3u, unpackUnorm8x4(word));
        write(unpacks, i * 3u + 1u, unpackSnorm8x4(word));
        write(unpacks,
              i * 3u + 2u,
              float4(unpackUnorm16x2(word), unpackSnorm16x2(word)));
    }

    Uniform<InputBuffer> floats;
    Uniform<UIntInputBuffer> words;
    Uniform<UIntOutputBuffer> packs;
    Uniform<OutputBuffer> unpacks;

    EACP_SHADER(floats, words, packs, unpacks)
};

bool near(float value, double reference, double tolerance)
{
    return std::fabs((double) value - reference) <= tolerance;
}

double relativeTolerance(double reference)
{
    return 4.0e-6 + 4.0e-6 * std::fabs(reference);
}

Vector<float> hyperbolicInputs()
{
    auto values = Vector<float> {};

    for (auto i = -400; i <= 400; ++i)
        values.add((float) i / 20.0f);

    for (auto special: {0.0f, -0.0f, 1.0e-6f, -1.0e-6f, 0.5f, -0.5f, 300.0f})
        values.add(special);

    while (values.size() % 4 != 0)
        values.add(0.75f);

    return values;
}

// Eighths, so every product and sum below is exact and a fused multiply-add
// rounds nothing a separate one would not.
Vector<float> exactInputs()
{
    auto values = Vector<float> {};

    for (auto i = -96; i <= 96; ++i)
        values.add((float) i / 8.0f);

    while (values.size() % 4 != 0)
        values.add(0.125f);

    return values;
}

// Halves, and halves nudged a quarter of their spacing away from zero: both
// round to the half on every backend, nearest-even and towards-zero alike.
Vector<float> halfInputs()
{
    auto values = Vector<float> {};

    for (auto exponent = -14; exponent <= 15; exponent += 3)
    {
        for (auto mantissa = 0; mantissa < 1024; mantissa += 97)
        {
            auto half = std::ldexp(1.0f + (float) mantissa / 1024.0f, exponent);
            auto nudged = half + std::ldexp(0.25f, exponent - 10);

            for (auto value: {half, nudged})
            {
                values.add(value);
                values.add(-value);
            }
        }
    }

    values.add(0.0f);
    values.add(65504.0f);

    while (values.size() % 4 != 0)
        values.add(1.0f);

    return values;
}

Vector<std::uint32_t> bitWords()
{
    auto words = Vector<std::uint32_t> {};

    for (auto special: {0u,
                        1u,
                        0x7fffffffu,
                        0x80000000u,
                        0xffffffffu,
                        0xfffffffeu,
                        0x55555555u,
                        0x0000ff00u})
        words.add(special);

    for (auto position = 0u; position < 32u; ++position)
    {
        words.add(1u << position);
        words.add(~(1u << position));
    }

    for (auto k = 0u; k < 440u; ++k)
        words.add(k * 2654435761u + 0x9e3779b9u);

    return words;
}

// Offsets and counts from zero to past 32, with 32 itself and the far ends.
Vector<std::uint32_t> bitFields(int lanes)
{
    auto arguments = Vector<std::uint32_t> {};

    for (auto value = 0u; value <= 34u; ++value)
        arguments.add(value);

    for (auto far: {64u, 0xffffffffu})
        arguments.add(far);

    auto fields = Vector<std::uint32_t> {};

    for (auto i = 0; i < lanes; ++i)
    {
        fields.add(arguments[i % arguments.size()]);
        fields.add(arguments[(i * 7 + 3) % arguments.size()]);
    }

    return fields;
}

std::int32_t asSigned(std::uint32_t value)
{
    return std::bit_cast<std::int32_t>(value);
}

std::uint32_t asWord(std::int32_t value)
{
    return std::bit_cast<std::uint32_t>(value);
}
} // namespace

auto tInverseHyperbolics =
    test("WgslIntrinsics/inverseHyperbolicsMatchTheReference") = []
{
    auto values = hyperbolicInputs();
    auto count = values.size();

    auto kernel = InverseHyperbolicKernel {};

    CrossCheck {kernel}
        .input(kernel.input, values)
        .output(kernel.output, count * 3)
        .agreeing(2.0e-5f)
        .run(
            count,
            [&](const Readback& readback)
            {
                const auto& result = readback.floats(kernel.output);
                const auto* name = readback.name();

                for (auto i = 0; i < count; ++i)
                {
                    auto x = values[i];
                    auto asinhOf = std::asinh((double) x);
                    auto acoshOf = std::acosh((double) (std::fabs(x) + 1.5f));
                    auto atanhOf = std::atanh((double) (x / (std::fabs(x) + 4.0f)));

                    check(near(result[i * 3], asinhOf, relativeTolerance(asinhOf)),
                          name);
                    check(
                        near(result[i * 3 + 1], acoshOf, relativeTolerance(acoshOf)),
                        name);
                    check(
                        near(result[i * 3 + 2], atanhOf, relativeTolerance(atanhOf)),
                        name);
                }
            });
};

auto tVectorInverseHyperbolics =
    test("WgslIntrinsics/inverseHyperbolicsApplyComponentwise") = []
{
    auto values = hyperbolicInputs();
    auto records = values.size() / 4;

    auto kernel = VectorInverseHyperbolicKernel {};

    CrossCheck {kernel}
        .input(kernel.input, values)
        .output(kernel.output, records * 12)
        .agreeing(2.0e-5f)
        .run(records,
             [&](const Readback& readback)
             {
                 const auto& result = readback.floats(kernel.output);
                 const auto* name = readback.name();

                 for (auto i = 0; i < records; ++i)
                 {
                     for (auto c = 0; c < 4; ++c)
                     {
                         auto x = values[i * 4 + c];
                         auto asinhOf = std::asinh((double) x);
                         auto acoshOf = std::acosh((double) (std::fabs(x) + 1.5f));
                         auto atanhOf =
                             std::atanh((double) (x / (std::fabs(x) + 4.0f)));

                         check(near(result[i * 12 + c],
                                    asinhOf,
                                    relativeTolerance(asinhOf)),
                               name);
                         check(near(result[i * 12 + 4 + c],
                                    acoshOf,
                                    relativeTolerance(acoshOf)),
                               name);
                         check(near(result[i * 12 + 8 + c],
                                    atanhOf,
                                    relativeTolerance(atanhOf)),
                               name);
                     }
                 }
             });
};

auto tExactArithmetic =
    test("WgslIntrinsics/fmaAnglesAndHalfQuantizingAreExact") = []
{
    auto values = exactInputs();
    auto count = values.size();

    auto kernel = ExactArithmeticKernel {};

    CrossCheck {kernel}
        .input(kernel.input, values)
        .output(kernel.output, count * 5)
        .output(kernel.vectors, count * 8)
        .agreeing()
        .run(count,
             [&](const Readback& readback)
             {
                 const auto& result = readback.floats(kernel.output);
                 const auto& vectors = readback.floats(kernel.vectors);
                 const auto* name = readback.name();

                 for (auto i = 0; i < count; ++i)
                 {
                     auto x = values[i];

                     check(result[i * 5] == x * (x * 3.0f) - x, name);
                     check(result[i * 5 + 1] == 0.5f * x + 0.25f, name);
                     check(result[i * 5 + 2] == x * degreesPerRadian, name);
                     check(result[i * 5 + 3] == x * radiansPerDegree, name);
                     check(result[i * 5 + 4] == x, name);

                     for (auto c = 0; c < 4; ++c)
                     {
                         auto component = values[(i / 4) * 4 + c];
                         check(vectors[i * 8 + c] == component * x + 0.5f, name);
                         check(vectors[i * 8 + 4 + c] == component, name);
                     }
                 }
             });
};

auto tQuantize = test("WgslIntrinsics/quantizeToF16RoundsThroughAHalf") = []
{
    auto values = halfInputs();
    auto count = values.size();

    auto kernel = ExactArithmeticKernel {};

    CrossCheck {kernel}
        .input(kernel.input, values)
        .output(kernel.output, count * 5)
        .output(kernel.vectors, count * 8)
        .run(count,
             [&](const Readback& readback)
             {
                 const auto& result = readback.floats(kernel.output);
                 const auto& vectors = readback.floats(kernel.vectors);
                 const auto* name = readback.name();

                 for (auto i = 0; i < count; ++i)
                 {
                     auto x = values[i];
                     check(result[i * 5 + 4] == CpuCompute::quantizeToF16(x), name);

                     for (auto c = 0; c < 4; ++c)
                     {
                         auto component = values[(i / 4) * 4 + c];
                         check(vectors[i * 8 + 4 + c]
                                   == CpuCompute::quantizeToF16(component),
                               name);
                     }
                 }
             });
};

auto tBits = test("WgslIntrinsics/bitFunctionsMatchTheirTwinsEverywhere") = []
{
    auto words = bitWords();
    auto count = words.size();
    auto fields = bitFields(count);

    auto kernel = BitKernel {};

    CrossCheck {kernel}
        .input(kernel.words, words)
        .input(kernel.fields, fields)
        .output(kernel.output, count * 12)
        .output(kernel.vectors, count * 8)
        .output(kernel.signedVectors, count * 3)
        .agreeing()
        .run(count,
             [&](const Readback& readback)
             {
                 const auto& output = readback.uints(kernel.output);
                 const auto& vectors = readback.uints(kernel.vectors);
                 const auto& signedVectors = readback.uints(kernel.signedVectors);
                 const auto* name = readback.name();
                 auto failures = 0;

                 for (auto i = 0; i < count; ++i)
                 {
                     using namespace CpuCompute;

                     auto word = words[i];
                     auto offset = fields[i * 2];
                     auto fieldCount = fields[i * 2 + 1];
                     auto signedWord = asSigned(word);
                     const auto* lane = output.data() + i * 12;

                     failures += lane[0] != countOneBits(word);
                     failures += lane[1] != reverseBits(word);
                     failures += lane[2] != countLeadingZeros(word);
                     failures += lane[3] != countTrailingZeros(word);
                     failures += lane[4] != firstLeadingBit(word);
                     failures += lane[5] != firstTrailingBit(word);
                     failures += lane[6] != extractBits(word, offset, fieldCount);
                     failures +=
                         lane[7] != insertBits(word, ~word, offset, fieldCount);
                     failures += lane[8] != asWord(firstLeadingBit(signedWord));
                     failures +=
                         lane[9]
                         != asWord(extractBits(signedWord, offset, fieldCount));
                     failures += lane[10] != insertBits(word, 0u - word, offset, 7u);
                     failures += lane[11]
                                 != countLeadingZeros(word) + firstTrailingBit(word)
                                        + countOneBits(word) + reverseBits(word);

                     auto base = (i / 4) * 4;

                     for (auto c = 0; c < 4; ++c)
                     {
                         auto component = words[base + c];
                         auto rotated = words[base + (c + 1) % 4];

                         failures +=
                             vectors[i * 8 + c]
                             != firstLeadingBit(component)
                                    + extractBits(component, offset, fieldCount);
                         failures +=
                             vectors[i * 8 + 4 + c]
                             != insertBits(component, rotated, offset, fieldCount);
                     }

                     for (auto c = 0; c < 3; ++c)
                     {
                         auto component = words[base + c];
                         auto expected =
                             asWord(firstLeadingBit(asSigned(component)))
                             + asWord(extractBits(
                                 asSigned(component), offset, fieldCount))
                             + insertBits(component, component, offset, 5u);

                         failures += signedVectors[i * 3 + c] != expected;
                     }
                 }

                 check(failures == 0, name);
             });
};

// Every pack on values whose scaled forms are exact, ties included: m / 256
// times any of the four scales, plus a half, is a float with nothing rounded, so
// floor(0.5 + scaled) is one answer on every backend however it contracts.
auto tNormalizedPacks = test("WgslIntrinsics/normalizedPacksMatchTheirTwins") = []
{
    auto floats = Vector<float> {};

    for (auto m = -384; m <= 384; ++m)
        floats.add((float) m / 256.0f);

    while (floats.size() % 4 != 0)
        floats.add(0.5f);

    auto count = floats.size() / 4;
    auto words = Vector<std::uint32_t> {};

    for (auto k = 0; k < count; ++k)
        words.add((std::uint32_t) k * 2654435761u + 0x80008000u * (k % 2u));

    auto kernel = NormalizedPackKernel {};

    CrossCheck {kernel}
        .input(kernel.floats, floats)
        .input(kernel.words, words)
        .output(kernel.packs, count * 4)
        .output(kernel.unpacks, count * 12)
        .agreeing(5.0e-7f)
        .run(count,
             [&](const Readback& readback)
             {
                 using namespace CpuCompute;

                 const auto& packs = readback.uints(kernel.packs);
                 const auto& unpacks = readback.floats(kernel.unpacks);
                 const auto* name = readback.name();
                 auto failures = 0;

                 for (auto i = 0; i < count; ++i)
                 {
                     auto record = HelperFloat4 {floats[i * 4],
                                                 floats[i * 4 + 1],
                                                 floats[i * 4 + 2],
                                                 floats[i * 4 + 3]};

                     failures += packs[i * 4] != packUnorm8x4(record);
                     failures += packs[i * 4 + 1] != packSnorm8x4(record);
                     failures +=
                         packs[i * 4 + 2] != packUnorm16x2({record[0], record[1]});
                     failures +=
                         packs[i * 4 + 3] != packSnorm16x2({record[2], record[3]});

                     auto unorm = unpackUnorm8x4(words[i]);
                     auto snorm = unpackSnorm8x4(words[i]);
                     auto unorm16 = unpackUnorm16x2(words[i]);
                     auto snorm16 = unpackSnorm16x2(words[i]);
                     auto wide = HelperFloat4 {
                         unorm16[0], unorm16[1], snorm16[0], snorm16[1]};

                     for (auto c = 0u; c < 4u; ++c)
                     {
                         auto at = i * 12 + (int) c;
                         failures += !near(unpacks[at], unorm[c], 5.0e-7);
                         failures += !near(unpacks[at + 4], snorm[c], 5.0e-7);
                         failures += !near(unpacks[at + 8], wide[c], 5.0e-7);
                     }
                 }

                 check(failures == 0, name);
             });
};
} // namespace WgslIntrinsicTests
