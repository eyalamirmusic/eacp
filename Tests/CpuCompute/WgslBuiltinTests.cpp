#include "CountingHeap.h"

#include <NanoTest/NanoTest.h>

#include <eacp/GPU/CpuCompute/CpuCompute.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <string>

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::GPU::CpuCompute;

namespace WgslBuiltinTests
{

// The WGSL built-ins' C++ twins against references written bit by bit from the
// WGSL specification's own wording, sharing no code with them, and then the
// executor against the twins: every lane of a recorded kernel must be what
// calling the twin directly gives.

namespace
{
constexpr auto allOnes = 0xffffffffu;

std::uint32_t bitAt(std::uint32_t value, std::uint32_t position)
{
    return (value >> position) & 1u;
}

std::uint32_t referenceCountOneBits(std::uint32_t value)
{
    auto count = 0u;

    for (auto position = 0u; position < 32u; ++position)
        count += bitAt(value, position);

    return count;
}

std::uint32_t referenceReverseBits(std::uint32_t value)
{
    auto result = 0u;

    for (auto position = 0u; position < 32u; ++position)
        result |= bitAt(value, position) << (31u - position);

    return result;
}

std::uint32_t referenceLeadingZeros(std::uint32_t value)
{
    auto count = 0u;

    for (auto position = 32u; position-- > 0u && bitAt(value, position) == 0u;)
        ++count;

    return count;
}

std::uint32_t referenceTrailingZeros(std::uint32_t value)
{
    auto count = 0u;

    for (auto position = 0u; position < 32u && bitAt(value, position) == 0u;
         ++position)
        ++count;

    return count;
}

// The highest bit unlike `background`: 0 for a u32, the sign bit for an i32.
std::uint32_t referenceLeadingBit(std::uint32_t value, std::uint32_t background)
{
    for (auto position = 32u; position-- > 0u;)
        if (bitAt(value, position) != background)
            return position;

    return allOnes;
}

std::uint32_t referenceTrailingBit(std::uint32_t value)
{
    for (auto position = 0u; position < 32u; ++position)
        if (bitAt(value, position) != 0u)
            return position;

    return allOnes;
}

struct ReferenceField
{
    std::uint32_t offset = 0;
    std::uint32_t count = 0;
};

ReferenceField referenceField(std::uint32_t offset, std::uint32_t count)
{
    auto o = offset < 32u ? offset : 32u;
    return {o, count < 32u - o ? count : 32u - o};
}

std::uint32_t
    referenceExtract(std::uint32_t value, ReferenceField field, bool isSigned)
{
    auto result = 0u;

    for (auto position = 0u; position < field.count; ++position)
        result |= bitAt(value, field.offset + position) << position;

    if (isSigned && field.count > 0u)
        for (auto position = field.count; position < 32u; ++position)
            result |= bitAt(result, field.count - 1u) << position;

    return result;
}

std::uint32_t
    referenceInsert(std::uint32_t value, std::uint32_t newBits, ReferenceField field)
{
    auto result = value;

    for (auto position = 0u; position < field.count; ++position)
    {
        auto target = field.offset + position;
        result = (result & ~(1u << target)) | (bitAt(newBits, position) << target);
    }

    return result;
}

Vector<std::uint32_t> interestingWords()
{
    auto words = Vector<std::uint32_t> {};

    for (auto special: {0u,
                        1u,
                        2u,
                        3u,
                        0x7fffffffu,
                        0x80000000u,
                        0x80000001u,
                        allOnes,
                        0xfffffffeu,
                        0x55555555u,
                        0xaaaaaaaau,
                        0x0000ff00u})
        words.add(special);

    for (auto position = 0u; position < 32u; ++position)
    {
        words.add(1u << position);
        words.add(~(1u << position));
        words.add(allOnes >> position);
        words.add(allOnes << position);
    }

    for (auto k = 0u; k < 2000u; ++k)
        words.add(k * 2654435761u + 0x9e3779b9u);

    return words;
}

Vector<std::uint32_t> fieldArguments()
{
    auto values = Vector<std::uint32_t> {};

    for (auto value = 0u; value <= 34u; ++value)
        values.add(value);

    for (auto far: {63u, 64u, 100u, 0x7fffffffu, allOnes})
        values.add(far);

    return values;
}

std::uint32_t bitsOf(float value)
{
    return std::bit_cast<std::uint32_t>(value);
}

bool sameBits(float a, float b)
{
    return bitsOf(a) == bitsOf(b);
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

auto tCounts = test("WgslHelpers/countsMatchTheBitByBitReference") = []
{
    auto failures = 0;

    for (auto word: interestingWords())
    {
        failures += countOneBits(word) != referenceCountOneBits(word);
        failures += reverseBits(word) != referenceReverseBits(word);
        failures += countLeadingZeros(word) != referenceLeadingZeros(word);
        failures += countTrailingZeros(word) != referenceTrailingZeros(word);
        failures += reverseBits(reverseBits(word)) != word;
    }

    check(failures == 0);
    check(countLeadingZeros(0u) == 32u);
    check(countTrailingZeros(0u) == 32u);
    check(countOneBits(allOnes) == 32u);
    check(reverseBits(1u) == 0x80000000u);
};

// -1 and 0xffffffff answer for "no such bit", and an i32's leading bit is the
// highest one that differs from its sign.
auto tFirstBits = test("WgslHelpers/firstBitsMatchTheBitByBitReference") = []
{
    auto failures = 0;

    for (auto word: interestingWords())
    {
        auto sign = word >> 31u;
        failures += firstLeadingBit(word) != referenceLeadingBit(word, 0u);
        failures += asWord(firstLeadingBit(asSigned(word)))
                    != referenceLeadingBit(word, sign);
        failures += firstTrailingBit(word) != referenceTrailingBit(word);
    }

    check(failures == 0);

    check(firstLeadingBit(0u) == allOnes);
    check(firstLeadingBit(1u) == 0u);
    check(firstLeadingBit(0x80000000u) == 31u);
    check(firstLeadingBit(0) == -1);
    check(firstLeadingBit(-1) == -1);
    check(firstLeadingBit(1) == 0);
    check(firstLeadingBit(-2) == 0);
    check(firstLeadingBit(std::int32_t {INT32_MIN}) == 30);
    check(firstLeadingBit(std::int32_t {INT32_MAX}) == 30);
    check(firstTrailingBit(0u) == allOnes);
    check(firstTrailingBit(0x80000000u) == 31u);
};

// Every offset and count from 0 to 34 and a few far past, against every
// interesting word: the clamping, a count of zero and of 32 included.
auto tExtract = test("WgslHelpers/extractBitsClampsAndSignExtends") = []
{
    auto failures = 0;
    auto words = interestingWords();
    auto arguments = fieldArguments();

    for (auto i = 0; i < words.size(); i += 7)
    {
        auto word = words[i];

        for (auto offset: arguments)
        {
            for (auto count: arguments)
            {
                auto field = referenceField(offset, count);
                failures += extractBits(word, offset, count)
                            != referenceExtract(word, field, false);
                failures += asWord(extractBits(asSigned(word), offset, count))
                            != referenceExtract(word, field, true);
            }
        }
    }

    check(failures == 0);

    check(extractBits(0xabcd1234u, 0u, 32u) == 0xabcd1234u);
    check(extractBits(0xabcd1234u, 32u, 5u) == 0u);
    check(extractBits(0xabcd1234u, 4u, 0u) == 0u);
    check(extractBits(0xabcd1234u, 28u, 100u) == 0xau);
    check(extractBits(std::int32_t {0x000000f0}, 4u, 4u) == -1);
    check(extractBits(std::int32_t {0x00000070}, 4u, 4u) == 7);
    check(extractBits(std::int32_t {-1}, 0u, 32u) == -1);
    check(extractBits(std::int32_t {INT32_MIN}, 31u, 1u) == -1);
    check(extractBits(std::int32_t {-1}, 3u, 0u) == 0);
};

auto tInsert = test("WgslHelpers/insertBitsClampsAndKeepsTheRest") = []
{
    auto failures = 0;
    auto words = interestingWords();
    auto arguments = fieldArguments();

    for (auto i = 0; i < words.size(); i += 11)
    {
        auto word = words[i];
        auto newBits = words[(i * 5 + 3) % words.size()];

        for (auto offset: arguments)
            for (auto count: arguments)
                failures +=
                    insertBits(word, newBits, offset, count)
                    != referenceInsert(word, newBits, referenceField(offset, count));
    }

    check(failures == 0);

    check(insertBits(0u, allOnes, 0u, 32u) == allOnes);
    check(insertBits(0x12345678u, 0u, 32u, 4u) == 0x12345678u);
    check(insertBits(0x12345678u, 0u, 4u, 0u) == 0x12345678u);
    check(insertBits(0u, 0xfu, 30u, 8u) == 0xc0000000u);
};

// floor(0.5 + scaled) rounds a tie up, where round-to-even would take -63.5 to
// -64 and a driver's own pack may do either.
auto tPackTies = test("WgslHelpers/normalizedPacksRoundTiesUpAndClamp") = []
{
    check(packUnorm8x4({0.5f, 0.f, 1.f, 2.f}) == 0xffff0080u);
    check(packUnorm8x4({-1.f, 0.25f, 0.75f, 1.f / 255.f}) == 0x01bf4000u);
    check(packSnorm8x4({-0.5f, 0.5f, -1.f, 1.f}) == 0x7f8140c1u);
    check(packSnorm8x4({-3.f, 3.f, 0.f, -0.f}) == 0x00007f81u);
    check(packUnorm16x2({0.5f, 1.f}) == 0xffff8000u);
    check(packSnorm16x2({-0.5f, 0.5f}) == 0x4000c001u);
    check(packSnorm16x2({-1.f, -7.f}) == 0x80018001u);
};

// Every code of every width back and forth: an unpack is the code over its
// scale, and packing that gives the code again - except the signed -128 and
// -32768, which read as -1 and so pack as -127 and -32767.
auto tPackRoundTrips = test("WgslHelpers/everyNormalizedCodeRoundTrips") = []
{
    auto failures = 0;

    for (auto code = 0u; code < 256u; ++code)
    {
        auto word = code | (code << 8u) | (code << 16u) | (code << 24u);
        auto unorm = unpackUnorm8x4(word);
        auto snorm = unpackSnorm8x4(word);
        auto signedCode = code >= 128u ? (float) code - 256.f : (float) code;

        for (auto lane = 0u; lane < 4u; ++lane)
        {
            failures += !sameBits(unorm[lane], (float) code / 255.f);
            failures += !sameBits(snorm[lane], std::fmax(signedCode / 127.f, -1.f));
        }

        failures += packUnorm8x4(unorm) != word;

        auto expected = code == 0x80u ? 0x81818181u : word;
        failures += packSnorm8x4(snorm) != expected;
    }

    for (auto code = 0u; code < 65536u; ++code)
    {
        auto word = code | (code << 16u);
        auto unorm = unpackUnorm16x2(word);
        auto snorm = unpackSnorm16x2(word);
        auto signedCode = code >= 32768u ? (float) code - 65536.f : (float) code;

        for (auto lane = 0u; lane < 2u; ++lane)
        {
            failures += !sameBits(unorm[lane], (float) code / 65535.f);
            failures +=
                !sameBits(snorm[lane], std::fmax(signedCode / 32767.f, -1.f));
        }

        failures += packUnorm16x2(unorm) != word;

        auto expected = code == 0x8000u ? 0x80018001u : word;
        failures += packSnorm16x2(snorm) != expected;
    }

    check(failures == 0);
};

auto tQuantize = test("WgslHelpers/quantizeToF16IsTheHalfRoundTrip") = []
{
    auto failures = 0;

    for (auto k = 0u; k < 20000u; ++k)
    {
        auto x = std::bit_cast<float>(k * 2654435761u);
        auto quantized = quantizeToF16(x);

        if (x != x)
            failures += quantized == quantized;
        else
            failures += !sameBits(quantized, widenHalf(narrowToHalf(x)));
    }

    check(failures == 0);
    check(quantizeToF16(1.f / 3.f) == 0.333251953125f);
    check(quantizeToF16(65504.f) == 65504.f);
    check(quantizeToF16(0x1p-24f) == 0x1p-24f);
    check(quantizeToF16(1e-8f) == 0.f);
    check(quantizeToF16(1.f + 0x1p-11f) == 1.f);
    check(quantizeToF16(1.f + 0x1p-10f) == 1.f + 0x1p-10f);
};

// ---------------------------------------------------------------------------
// Through the executor.

namespace
{
constexpr auto floatSentinel = -5.f;
constexpr auto wordSentinel = 0xdeadbeefu;

Vector<float> floatsOf(int count, float value)
{
    auto values = Vector<float> {};
    values.resize(count, value);
    return values;
}

Vector<std::uint32_t> wordsOf(int count, std::uint32_t value)
{
    auto values = Vector<std::uint32_t> {};
    values.resize(count, value);
    return values;
}

// Eight results a lane, the vector forms over the first three or four lanes'
// worth of input so every width is exercised.
struct FloatBuiltinKernel final : ComputeKernel
{
    FloatBuiltinKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto x = input[i];
        auto at = i * 8u;

        write(output, at, asinh(x));
        write(output, at + 1u, acosh(abs(x) + 1.f));
        write(output, at + 2u, atanh(x / (abs(x) + 1.f)));
        write(output, at + 3u, fma(x, x * 3.f, -x));
        write(output, at + 4u, degrees(x));
        write(output, at + 5u, radians(x));
        write(output, at + 6u, quantizeToF16(x));
        write(output, at + 7u, fma(1.5f, x, 0.25f));

        auto record = input.read4(i);
        write4(vectors, i * 3u, asinh(record));
        write4(vectors, i * 3u + 1u, quantizeToF16(record));
        write4(vectors, i * 3u + 2u, fma(record, x, record.wzyx()));
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;
    Uniform<OutputBuffer> vectors;
    EACP_SHADER(input, output, vectors)
};

struct BitBuiltinKernel final : ComputeKernel
{
    BitBuiltinKernel() { compile(); }

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
        write(output, at + 11u, toUInt(countOneBits(signedWord)));

        auto record = words.read4(i / 4u);
        auto signedRecord = toInt(record.xyz());
        write4(vectors,
               i * 2u,
               firstLeadingBit(record) + extractBits(record, offset, count));
        write3(signedVectors,
               i,
               toUInt(firstLeadingBit(signedRecord)
                      + extractBits(signedRecord, 3u, count)
                      + insertBits(signedRecord, signedRecord, offset, 5u)));
        write4(
            vectors, i * 2u + 1u, insertBits(record, record.yzwx(), offset, count));
    }

    Uniform<UIntInputBuffer> words;
    Uniform<UIntInputBuffer> fields;
    Uniform<UIntOutputBuffer> output;
    Uniform<UIntOutputBuffer> vectors;
    Uniform<UIntOutputBuffer> signedVectors;
    EACP_SHADER(words, fields, output, vectors, signedVectors)
};

struct PackBuiltinKernel final : ComputeKernel
{
    PackBuiltinKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto record = floats.read4(i);
        auto word = words[i];

        write(packs, i * 4u, packUnorm8x4(record));
        write(packs, i * 4u + 1u, packSnorm8x4(record));
        write(packs, i * 4u + 2u, packUnorm16x2(record.xy()));
        write(packs, i * 4u + 3u, packSnorm16x2(record.zw()));

        write4(unpacks, i * 3u, unpackUnorm8x4(word));
        write4(unpacks, i * 3u + 1u, unpackSnorm8x4(word));
        write4(unpacks,
               i * 3u + 2u,
               float4(unpackUnorm16x2(word), unpackSnorm16x2(word)));
    }

    Uniform<InputBuffer> floats;
    Uniform<UIntInputBuffer> words;
    Uniform<UIntOutputBuffer> packs;
    Uniform<OutputBuffer> unpacks;
    EACP_SHADER(floats, words, packs, unpacks)
};

Vector<float> builtinInputs()
{
    auto values = Vector<float> {};

    for (auto step = -2000; step <= 2000; ++step)
        values.add((float) step / 300.f);

    for (auto special: {0.f, -0.f, 1.f, -1.f, 0.5f, -0.5f, 65504.f, 1e-8f, 0x1p-24f})
        values.add(special);

    while (values.size() % 4 != 0)
        values.add(0.125f);

    return values;
}

bool refusedNaming(const Executor& executor, const char* name)
{
    return !executor.isValid() && executor.reason().find(name) != std::string::npos;
}
} // namespace

auto tFloatLanes = test("WgslHelpers/theFloatBuiltinsRunOnEveryLane") = []
{
    auto input = builtinInputs();
    auto records = input.size() / 4;

    auto kernel = FloatBuiltinKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto output = floatsOf(records * 8 + 3, floatSentinel);
    auto vectors = floatsOf(records * 12, floatSentinel);

    auto bindings = Bindings {};
    bindings.set(kernel.input, input);
    bindings.set(kernel.output, output);
    bindings.set(kernel.vectors, vectors);
    check(executor.dispatch(bindings, records));

    auto failures = 0;

    for (auto i = 0; i < records; ++i)
    {
        auto x = input[i];
        const auto* lane = output.data() + i * 8;

        failures += !sameBits(lane[0], std::asinh(x));
        failures += !sameBits(lane[1], std::acosh(std::fabs(x) + 1.f));
        failures += !sameBits(lane[2], std::atanh(x / (std::fabs(x) + 1.f)));
        failures += !sameBits(lane[3], std::fma(x, x * 3.f, -x));
        failures += !sameBits(lane[4], x * degreesPerRadian);
        failures += !sameBits(lane[5], x * radiansPerDegree);
        failures += !sameBits(lane[6], widenHalf(narrowToHalf(x)));
        failures += !sameBits(lane[7], std::fma(1.5f, x, 0.25f));

        for (auto c = 0; c < 4; ++c)
        {
            auto component = input[i * 4 + c];
            auto reversed = input[i * 4 + 3 - c];
            failures += !sameBits(vectors[i * 12 + c], std::asinh(component));
            failures += !sameBits(vectors[i * 12 + 4 + c], quantizeToF16(component));
            failures +=
                !sameBits(vectors[i * 12 + 8 + c], std::fma(component, x, reversed));
        }
    }

    check(failures == 0);
    check(output[records * 8] == floatSentinel);
};

auto tBitLanes = test("WgslHelpers/theBitBuiltinsRunOnEveryLane") = []
{
    auto words = interestingWords();

    while (words.size() % 4 != 0)
        words.add(0u);

    auto count = words.size();
    auto arguments = fieldArguments();
    auto fields = Vector<std::uint32_t> {};

    for (auto i = 0; i < count; ++i)
    {
        fields.add(arguments[i % arguments.size()]);
        fields.add(arguments[(i * 7 + 3) % arguments.size()]);
    }

    auto kernel = BitBuiltinKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto output = wordsOf(count * 12 + 1, wordSentinel);
    auto vectors = wordsOf(count * 8, wordSentinel);
    auto signedVectors = wordsOf(count * 3, wordSentinel);

    auto bindings = Bindings {};
    bindings.set(kernel.words, words);
    bindings.set(kernel.fields, fields);
    bindings.set(kernel.output, output);
    bindings.set(kernel.vectors, vectors);
    bindings.set(kernel.signedVectors, signedVectors);
    check(executor.dispatch(bindings, count));

    auto failures = 0;

    for (auto i = 0; i < count; ++i)
    {
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
        failures += lane[7] != insertBits(word, ~word, offset, fieldCount);
        failures += lane[8] != asWord(firstLeadingBit(signedWord));
        failures += lane[9] != asWord(extractBits(signedWord, offset, fieldCount));
        failures += lane[10] != insertBits(word, 0u - word, offset, 7u);
        failures += lane[11] != countOneBits(word);

        auto base = (i / 4) * 4;

        for (auto c = 0; c < 4; ++c)
        {
            auto component = words[base + c];
            auto rotated = words[base + (c + 1) % 4];
            failures += vectors[i * 8 + c]
                        != firstLeadingBit(component)
                               + extractBits(component, offset, fieldCount);
            failures += vectors[i * 8 + 4 + c]
                        != insertBits(component, rotated, offset, fieldCount);
        }

        for (auto c = 0; c < 3; ++c)
        {
            auto component = words[base + c];
            auto expected =
                asWord(firstLeadingBit(asSigned(component)))
                + asWord(extractBits(asSigned(component), 3u, fieldCount))
                + insertBits(component, component, offset, 5u);
            failures += signedVectors[i * 3 + c] != expected;
        }
    }

    check(failures == 0);
    check(output[count * 12] == wordSentinel);
};

auto tPackLanes = test("WgslHelpers/theNormalizedPacksRunOnEveryLane") = []
{
    auto floats = builtinInputs();

    for (auto& value: floats)
        value = value / 4.f;

    auto count = floats.size() / 4;
    auto words = Vector<std::uint32_t> {};

    for (auto k = 0; k < count; ++k)
        words.add((std::uint32_t) k * 2654435761u);

    auto kernel = PackBuiltinKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto packs = wordsOf(count * 4, wordSentinel);
    auto unpacks = floatsOf(count * 12, floatSentinel);

    auto bindings = Bindings {};
    bindings.set(kernel.floats, floats);
    bindings.set(kernel.words, words);
    bindings.set(kernel.packs, packs);
    bindings.set(kernel.unpacks, unpacks);
    check(executor.dispatch(bindings, count));

    auto failures = 0;

    for (auto i = 0; i < count; ++i)
    {
        auto record = HelperFloat4 {
            floats[i * 4], floats[i * 4 + 1], floats[i * 4 + 2], floats[i * 4 + 3]};

        failures += packs[i * 4] != packUnorm8x4(record);
        failures += packs[i * 4 + 1] != packSnorm8x4(record);
        failures += packs[i * 4 + 2] != packUnorm16x2({record[0], record[1]});
        failures += packs[i * 4 + 3] != packSnorm16x2({record[2], record[3]});

        auto unorm = unpackUnorm8x4(words[i]);
        auto snorm = unpackSnorm8x4(words[i]);
        auto unorm16 = unpackUnorm16x2(words[i]);
        auto snorm16 = unpackSnorm16x2(words[i]);
        auto wide = HelperFloat4 {unorm16[0], unorm16[1], snorm16[0], snorm16[1]};

        for (auto c = 0u; c < 4u; ++c)
        {
            failures += !sameBits(unpacks[i * 12 + (int) c], unorm[c]);
            failures += !sameBits(unpacks[i * 12 + 4 + (int) c], snorm[c]);
            failures += !sameBits(unpacks[i * 12 + 8 + (int) c], wide[c]);
        }
    }

    check(failures == 0);
};

// What the plan checks of a bit helper: integers only, the value arguments the
// result's type, and the offset and count scalar UInts.
auto tMalformedBits = test("WgslHelpers/aMalformedBitHelperIsRefusedByName") = []
{
    auto floatValue = ShaderGraph {};
    auto floatOutput =
        floatValue.addStorageBuffer(BufferAccess::Write, ValueType::Float);
    auto floatThread = floatValue.addThreadId();
    auto floatArgument = Vector<int> {};
    floatArgument.add(floatValue.addConstant(1.f));
    floatValue.addStore(
        floatOutput,
        floatThread,
        floatValue.addCall(ValueType::Float, "eacpCountOneBits", floatArgument));

    check(refusedNaming(Executor {floatValue}, "eacpCountOneBits"));

    auto signedField = ShaderGraph {};
    auto signedOutput =
        signedField.addStorageBuffer(BufferAccess::Write, ValueType::UInt);
    auto signedThread = signedField.addThreadId();
    auto signedArguments = Vector<int> {};
    signedArguments.add(signedThread);
    signedArguments.add(signedField.addIntConstant(1));
    signedArguments.add(signedThread);
    signedField.addStore(
        signedOutput,
        signedThread,
        signedField.addCall(ValueType::UInt, "eacpExtractBits", signedArguments));

    check(refusedNaming(Executor {signedField}, "eacpExtractBits"));
    check(refusedNaming(Executor {signedField}, "scalar UInts"));
};

// No dispatch allocates, the new operations included.
auto tNoAllocation = test("WgslHelpers/aDispatchAfterTheFirstAllocatesNothing") = []
{
    auto words = interestingWords();

    while (words.size() % 4 != 0)
        words.add(0u);

    auto count = words.size();
    auto fields = wordsOf(count * 2, 5u);

    auto kernel = BitBuiltinKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto output = wordsOf(count * 12, 0u);
    auto vectors = wordsOf(count * 8, 0u);
    auto signedVectors = wordsOf(count * 3, 0u);

    auto bindings = Bindings {};
    bindings.set(kernel.words, words);
    bindings.set(kernel.fields, fields);
    bindings.set(kernel.output, output);
    bindings.set(kernel.vectors, vectors);
    bindings.set(kernel.signedVectors, signedVectors);

    auto warmedUp = executor.dispatch(bindings, count);

    ExecutorTests::allocationsCounted = 0;
    ExecutorTests::countingHeap = true;
    auto ran = executor.dispatch(bindings, count);
    ExecutorTests::countingHeap = false;

    check(warmedUp);
    check(ran);
    check(ExecutorTests::allocationsCounted.load() == 0);
};
} // namespace WgslBuiltinTests
