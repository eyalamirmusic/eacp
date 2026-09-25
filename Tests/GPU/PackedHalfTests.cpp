#include "CpuCrossCheck.h"

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

// asUInt, asFloat, unpackHalf2, packHalf2 and InputBuffer::readHalf: reading
// and writing data in a storage buffer that is not really float.
//
// A buffer is a run of floats on both backends, so anything packed - two
// halves in a word, four bytes, a bitfield - arrives as a float whose value is
// meaningless and whose bits are the payload. Two things have to hold for that
// to be usable, and only one of them is obvious:
//
//   1. unpackHalf2 widens both halves correctly, including the awkward
//      classes - subnormals, the two zeroes, infinities, NaN.
//   2. The bits survive the trip *at all*. A packed pair of small halves makes
//      a 32-bit pattern whose float interpretation is a denormal, and hardware
//      that flushes denormals on load would quietly zero it. Nothing in the
//      arithmetic would look wrong; the weights would just be gone.
//
// The second is why this test exists at all, and why the table below is built
// out of bit patterns rather than out of values.
//
// Every kernel runs on the CPU executor too, whose half widening and narrowing
// are CpuCompute/Helpers.h - the same functions the references below call, and
// which HelperTests checks against every pattern there is.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::GPU::CrossChecks;

namespace
{
// One 32-bit word out of two fp16 bit patterns, low half first - the layout
// unpackHalf2 promises and the one a packer on the CPU has to match.
std::uint32_t packed(std::uint16_t low, std::uint16_t high)
{
    return (std::uint32_t) low | ((std::uint32_t) high << 16);
}

// The reference: an fp16 bit pattern widened to float, by the C++ twin of the
// helper, which HelperTests holds to a double-precision decoding of all 65536.
float widened(std::uint16_t bits)
{
    return CpuCompute::widenHalf(bits);
}

// The words as the float slots the kernels read them through, bit for bit.
Vector<float> asFloats(const Vector<std::uint32_t>& words)
{
    auto floats = Vector<float> {};

    for (auto word: words)
        floats.add(std::bit_cast<float>(word));

    return floats;
}

std::uint32_t wordOf(float value)
{
    return std::bit_cast<std::uint32_t>(value);
}

// Every fp16 class, and deliberately paired so that several of the packed
// words are denormal floats: (0x0001, 0x0001) is 0x00010001, and 0x0000ffff
// and 0x00003c00 are denormals too. Those are the entries that catch a load
// path that does not preserve bits.
const auto halfPatterns = Array<std::uint16_t, 17> {
    0x0000, // +0
    0x8000, // -0
    0x0001, // smallest subnormal
    0x03ff, // largest subnormal
    0x0400, // smallest normal
    0x3c00, // 1.0
    0xbc00, // -1.0
    0x4000, // 2.0
    0xc500, // -5.0
    0x7bff, // largest finite
    0xfbff, // most negative finite
    0x7c00, // +inf
    0xfc00, // -inf
    0x7e00, // NaN
    0x3555, // ~0.3333
    0x1400, // a small normal
    0x9400 // its negation
};

// Each pattern paired with each other one, so both halves of a word see every
// class - and so several of the words are denormal floats.
Vector<std::uint32_t> everyPackedPair()
{
    auto words = Vector<std::uint32_t> {};

    for (auto low: halfPatterns)
        for (auto high: halfPatterns)
            words.add(packed(low, high));

    return words;
}

// One thread per packed word: read it as a float, recover the bits, unpack,
// and write the two widened halves out side by side.
struct UnpackKernel final : ComputeProgram
{
    UnpackKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto pair = unpackHalf2(asUInt(words[i]));

        write(output, i * 2u, pair.x());
        write(output, i * 2u + 1u, pair.y());
    }

    Uniform<InputBuffer> words;
    Uniform<OutputBuffer> output;

    EACP_SHADER(words, output)
};

// Ordinary arithmetic, so nothing pulls the helper in.
struct PlainKernel final : ComputeProgram
{
    PlainKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, input[i] * 2.0f);
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;

    EACP_SHADER(input, output)
};

// One element at a time out of a buffer whose elements are fp16, which is what
// a kernel walking a weight matrix writes rather than doing the word and
// parity arithmetic itself.
struct ReadHalfKernel final : ComputeProgram
{
    ReadHalfKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, weights.readHalf(i));
    }

    Uniform<InputBuffer> weights;
    Uniform<OutputBuffer> output;

    EACP_SHADER(weights, output)
};

// The literal-index overload, which a kernel reaching for a fixed element - a
// bias, a scale - is what spells; the first four halves of the buffer, both
// parities of the first two words.
struct LiteralHalfKernel final : ComputeProgram
{
    LiteralHalfKernel() { compile(); }

    void define() override
    {
        auto i = threadId();

        write(output, i * 4u, weights.readHalf(0u));
        write(output, i * 4u + 1u, weights.readHalf(1u));
        write(output, i * 4u + 2u, weights.readHalf(2u));
        write(output, i * 4u + 3u, weights.readHalf(3u));
    }

    Uniform<InputBuffer> weights;
    Uniform<OutputBuffer> output;

    EACP_SHADER(weights, output)
};

// Four halves at a time, which is two words: the width a weight walk wants, and
// the one the record read underneath turns into a single eight-byte load.
struct ReadHalf4Kernel final : ComputeProgram
{
    ReadHalf4Kernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, weights.readHalf4(i));
    }

    Uniform<InputBuffer> weights;
    Uniform<OutputBuffer> output;

    EACP_SHADER(weights, output)
};

struct ReadHalf2Kernel final : ComputeProgram
{
    ReadHalf2Kernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, weights.readHalf2(i));
    }

    Uniform<InputBuffer> weights;
    Uniform<OutputBuffer> output;

    EACP_SHADER(weights, output)
};

// Widen a word and narrow it straight back, storing the packed result in the
// float slot it came out of - the whole fp16-storage round trip in one line.
struct RoundTripKernel final : ComputeProgram
{
    RoundTripKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, asFloat(packHalf2(weights.readHalf2(i))));
    }

    Uniform<InputBuffer> weights;
    Uniform<OutputBuffer> output;

    EACP_SHADER(weights, output)
};

// The same thing said the short way, which is the only claim writeHalf2 makes.
struct WriteHalf2Kernel final : ComputeProgram
{
    WriteHalf2Kernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        writeHalf2(output, i, weights.readHalf2(i));
    }

    Uniform<InputBuffer> weights;
    Uniform<OutputBuffer> output;

    EACP_SHADER(weights, output)
};

// The same round trip one width up: four halves are two words, read as one
// record and written back as one store.
struct WriteHalf4Kernel final : ComputeProgram
{
    WriteHalf4Kernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        writeHalf4(output, i, weights.readHalf4(i));
    }

    Uniform<InputBuffer> weights;
    Uniform<OutputBuffer> output;

    EACP_SHADER(weights, output)
};

struct BitcastKernel final : ComputeProgram
{
    BitcastKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, asFloat(asUInt(input[i])));
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;

    EACP_SHADER(input, output)
};

// Two ordinary fp32 values narrowed and packed, which is what a kernel writing
// fp16 output does.
struct NarrowKernel final : ComputeProgram
{
    NarrowKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, asFloat(packHalf2(input.read2(i))));
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;

    EACP_SHADER(input, output)
};

// The narrowing is the one thing here the two backends do not agree on, so it
// takes two references rather than one. Metal converts per IEEE; D3D specifies
// round-to-zero for a narrowing float conversion, and saturates a finite
// magnitude past the fp16 range to the largest finite half rather than to an
// infinity (D3D11.3 functional spec 3.2.2, which f32tof16 defers to). See
// packHalf2.
enum class Rounding
{
    NearestEven,
    TowardZero
};

// Round-to-nearest-even is the helper's C++ twin, which is Metal's rule and the
// CPU executor's. Round-toward-zero is D3D's and nothing else's, so it is
// written out here. The two places a narrowing goes wrong are the fp16
// subnormal range, where fewer mantissa bits are kept than the exponent
// suggests, and the tie.
std::uint16_t narrowed(float value, Rounding rounding)
{
    if (rounding == Rounding::NearestEven)
        return CpuCompute::narrowToHalf(value);

    auto bits = std::uint32_t {};
    std::memcpy(&bits, &value, sizeof(bits));

    auto sign = (std::uint16_t) ((bits >> 16) & 0x8000u);
    auto exponent = (int) ((bits >> 23) & 0xffu);
    auto mantissa = bits & 0x7fffffu;

    if (exponent == 0xff)
        return (std::uint16_t) (sign | 0x7c00u | (mantissa != 0 ? 0x200u : 0u));

    // Every fp32 subnormal is below 2^-126 and so far under half of the
    // smallest fp16 subnormal that it rounds to a signed zero.
    if (exponent == 0)
        return sign;

    auto unbiased = exponent - 127;
    auto significand = mantissa | 0x800000u;

    // 23 mantissa bits down to 10, and one further bit per step below the
    // smallest fp16 normal - which is what makes a subnormal lose precision.
    auto shift = unbiased < -14 ? -1 - unbiased : 13;

    if (shift >= 32)
        return sign;

    auto kept = significand >> shift;

    if (unbiased < -14)
        return (std::uint16_t) (sign | kept);

    if (unbiased > 15)
        return (std::uint16_t) (sign | 0x7bffu);

    return (std::uint16_t) (sign | ((std::uint32_t) (unbiased + 15) << 10)
                            | (kept & 0x3ffu));
}

bool isHalfNaN(std::uint16_t bits)
{
    return (bits & 0x7c00u) == 0x7c00u && (bits & 0x3ffu) != 0;
}

// Bit-for-bit on everything but NaN, whose payload neither language pins.
bool halfMatches(std::uint16_t gpu, std::uint16_t cpu)
{
    if (isHalfNaN(cpu))
        return isHalfNaN(gpu);

    return gpu == cpu;
}

// Either backend's answer, which for a value fp16 holds exactly is the same
// single answer: the two references only part where the value has to be
// rounded at all.
bool narrowsCorrectly(std::uint16_t gpu, float value)
{
    return halfMatches(gpu, narrowed(value, Rounding::NearestEven))
           || halfMatches(gpu, narrowed(value, Rounding::TowardZero));
}

// Bit-for-bit, not within a tolerance. Widening fp16 to fp32 is exact for
// every value there is - fp32 has more exponent range and more mantissa - so
// any difference at all is a fault rather than rounding. NaN is the one
// exception, since it compares unequal to itself.
bool matches(float gpu, float cpu)
{
    if (std::isnan(cpu))
        return std::isnan(gpu);

    return gpu == cpu;
}

// Values fp16 cannot hold exactly, chosen so that every rounding case the
// narrowing has appears: both directions of a tie, the overflow boundary, and
// the subnormal range where the mantissa is shorter than the exponent implies.
// The powers of two are spelled as fractions rather than as decimals because a
// tie has to *be* one - 2.98e-8 is not 2^-25 to the last bit.
const auto narrowingValues = Array<float, 20> {
    1.0f + 1.0f / 2048.0f, // half an ulp above 1.0: the tie rounds back down
    1.0f + 3.0f / 2048.0f, // the tie one ulp up, where even is the far side
    1.0f / 3.0f,
    0.1f,
    -0.1f,
    2.0f,
    65504.0f, // the largest finite half, exactly
    65519.0f, // just under the overflow tie
    65520.0f, // the tie itself, whose even candidate is out of range
    -65520.0f,
    1.0e30f,
    1.0f / 16777216.0f, // 2^-24, the smallest subnormal, exactly
    3.0f / 33554432.0f, // 1.5 * 2^-24: a subnormal tie, up to the even one
    5.0f / 33554432.0f, // 2.5 * 2^-24: the same tie, rounding down
    1.0f / 33554432.0f, // 2^-25, half the smallest subnormal: to zero
    3.0f / 67108864.0f, // just over it, so it rounds up to the subnormal
    -1.0e-20f, // far under everything: a signed zero
    std::numeric_limits<float>::infinity(),
    -std::numeric_limits<float>::infinity(),
    std::numeric_limits<float>::quiet_NaN()};

// Both halves of every word, widened, against the output of a kernel that
// wrote them side by side.
void checkWidenedPairs(const Vector<float>& result,
                       const Vector<std::uint32_t>& words,
                       const char* name)
{
    for (auto i = 0; i < words.size(); ++i)
    {
        auto low = (std::uint16_t) (words[i] & 0xffffu);
        auto high = (std::uint16_t) (words[i] >> 16);

        check(matches(result[i * 2], widened(low)), name);
        check(matches(result[i * 2 + 1], widened(high)), name);
    }
}

// Every word of a packed store holding the pattern it was read from: the fp16
// round trip, which neither backend has a rounding decision to make in.
void checkRoundTrip(const Vector<float>& result,
                    const Vector<std::uint32_t>& words,
                    const char* name)
{
    for (auto i = 0; i < words.size(); ++i)
    {
        auto word = wordOf(result[i]);

        check(halfMatches((std::uint16_t) (word & 0xffffu),
                          (std::uint16_t) (words[i] & 0xffffu)),
              name);

        check(halfMatches((std::uint16_t) (word >> 16),
                          (std::uint16_t) (words[i] >> 16)),
              name);
    }
}
} // namespace

auto tUnpackHalf2 = test("PackedHalf/unpacksEveryFloat16Class") = []
{
    auto words = everyPackedPair();
    auto count = words.size();

    auto kernel = UnpackKernel {};

    CrossCheck {kernel}
        .input(kernel.words, asFloats(words))
        .output(kernel.output, count * 2)
        .run(count,
             [&](const Readback& readback)
             {
                 checkWidenedPairs(
                     readback.floats(kernel.output), words, readback.name());
             });
};

// The helper is emitted only into shaders that call it, so a kernel doing
// ordinary arithmetic carries no definition for one.
auto tHelperIsNotAlwaysEmitted = test("PackedHalf/emitsTheHelperOnlyWhenUsed") = []
{
    auto contains = [](const std::string& text, const char* needle)
    { return text.find(needle) != std::string::npos; };

    auto plain = PlainKernel {};
    auto unpacking = UnpackKernel {};

    check(!contains(plain.source().source, "eacpUnpackHalf2"));
    check(contains(unpacking.source().source, "eacpUnpackHalf2"));

    // And the definition arrives before the body that calls it.
    const auto& source = unpacking.source().source;
    check(source.find("eacpUnpackHalf2") < source.rfind("eacpUnpackHalf2"));
};

auto tReadHalf = test("PackedHalf/readsEachHalfElementByIndex") = []
{
    auto words = everyPackedPair();
    auto count = words.size();

    auto kernel = ReadHalfKernel {};

    CrossCheck {kernel}
        .input(kernel.weights, asFloats(words))
        .output(kernel.output, count * 2)
        .run(count * 2,
             [&](const Readback& readback)
             {
                 checkWidenedPairs(
                     readback.floats(kernel.output), words, readback.name());
             });
};

auto tReadHalfLiteral = test("PackedHalf/readsHalfElementsAtLiteralIndices") = []
{
    auto words = everyPackedPair();

    auto kernel = LiteralHalfKernel {};

    CrossCheck {kernel}
        .input(kernel.weights, asFloats(words))
        .output(kernel.output, 4)
        .run(1,
             [&](const Readback& readback)
             {
                 const auto& result = readback.floats(kernel.output);

                 for (auto i = 0; i < 4; ++i)
                 {
                     auto word = words[i / 2];
                     auto half =
                         (std::uint16_t) (i % 2 == 0 ? word & 0xffffu : word >> 16);

                     check(matches(result[i], widened(half)), readback.name());
                 }
             });
};

auto tReadHalf2 = test("PackedHalf/readHalf2ReadsBothHalvesOfAWord") = []
{
    auto words = everyPackedPair();
    auto count = words.size();

    auto kernel = ReadHalf2Kernel {};

    CrossCheck {kernel}
        .input(kernel.weights, asFloats(words))
        .output(kernel.output, count * 2)
        .run(count,
             [&](const Readback& readback)
             {
                 checkWidenedPairs(
                     readback.floats(kernel.output), words, readback.name());
             });
};

// Four halves across two words, in the order readHalf walks them: the low half
// of the first word, its high half, then the second word's two.
auto tReadHalf4 = test("PackedHalf/readHalf4ReadsFourAcrossTwoWords") = []
{
    auto words = everyPackedPair();
    auto records = words.size() / 2;

    auto kernel = ReadHalf4Kernel {};

    CrossCheck {kernel}
        .input(kernel.weights, asFloats(words))
        .output(kernel.output, records * 4)
        .run(records,
             [&](const Readback& readback)
             {
                 const auto& result = readback.floats(kernel.output);

                 // Element by element it is the same walk readHalf makes, which
                 // is what says the two spellings address one layout.
                 for (auto element = 0; element < records * 4; ++element)
                 {
                     auto word = words[element / 2];
                     auto half = (element % 2) == 0
                                     ? (std::uint16_t) (word & 0xffffu)
                                     : (std::uint16_t) (word >> 16);

                     check(matches(result[element], widened(half)), readback.name());
                 }
             });
};

// asFloat is asUInt run backwards, so the pair is the identity on bits - and
// on these bits in particular, most of which are denormal floats.
auto tBitcastRoundTrip = test("PackedHalf/asFloatUndoesAsUInt") = []
{
    auto words = everyPackedPair();
    auto count = words.size();

    auto kernel = BitcastKernel {};

    CrossCheck {kernel}
        .input(kernel.input, asFloats(words))
        .output(kernel.output, count)
        .run(count,
             [&](const Readback& readback)
             {
                 const auto& result = readback.floats(kernel.output);

                 for (auto i = 0; i < count; ++i)
                     check(wordOf(result[i]) == words[i], readback.name());
             });
};

// Widening and narrowing back is exact for everything fp16 can hold, which is
// every pattern in the table: the fp32 in between has more of both exponent
// and mantissa, so nothing is rounded on either leg. That is what makes this
// the one bit-for-bit assertion the backends' different rounding cannot reach
// - there is no rounding decision to make. NaN is the exception, its payload
// being what neither language pins.
auto tPackRoundTrip = test("PackedHalf/packHalf2RoundTripsEveryPattern") = []
{
    auto words = everyPackedPair();
    auto count = words.size();

    auto kernel = RoundTripKernel {};

    CrossCheck {kernel}
        .input(kernel.weights, asFloats(words))
        .output(kernel.output, count)
        .run(count,
             [&](const Readback& readback)
             {
                 checkRoundTrip(
                     readback.floats(kernel.output), words, readback.name());
             });
};

// writeHalf2 is the store the round trip spells out by hand, so the two
// kernels have to emit the same body and produce the same bytes.
auto tWriteHalf2 = test("PackedHalf/writeHalf2IsThePackedStore") = []
{
    auto spelledOut = RoundTripKernel {};
    auto shorthand = WriteHalf2Kernel {};

    check(spelledOut.source().source == shorthand.source().source);

    auto words = everyPackedPair();
    auto count = words.size();

    CrossCheck {shorthand}
        .input(shorthand.weights, asFloats(words))
        .output(shorthand.output, count)
        .run(count,
             [&](const Readback& readback)
             {
                 checkRoundTrip(
                     readback.floats(shorthand.output), words, readback.name());
             });
};

// writeHalf4 is readHalf4 run backwards, at the index readHalf4 counts in: two
// words out and the same two words back, put there by one store.
auto tWriteHalf4 = test("PackedHalf/writeHalf4IsTheWidePackedStore") = []
{
    auto words = everyPackedPair();

    // The wide store addresses two words at a time, so an odd count would leave
    // a last word nothing writes rather than one written wrong.
    while (words.size() % 2 != 0)
        words.add(0u);

    auto count = words.size();

    auto kernel = WriteHalf4Kernel {};

    CrossCheck {kernel}
        .input(kernel.weights, asFloats(words))
        .output(kernel.output, count)
        .run(count / 2,
             [&](const Readback& readback)
             {
                 checkRoundTrip(
                     readback.floats(kernel.output), words, readback.name());
             });
};

// The narrowing itself, against values fp16 cannot hold exactly: the ties, the
// overflow boundary, and the subnormal range where a half loses mantissa bits
// one at a time. Each has to land on what one of the two rounding rules gives,
// which for the values fp16 does hold is a single answer on both backends. The
// CPU follows Metal, so its answer is the nearest-even one, exactly.
auto tNarrowing = test("PackedHalf/packHalf2NarrowsAsItsBackendRounds") = []
{
    // The table has to actually contain the disagreement, or this test would
    // pass on data that never exercises a rounding decision at all.
    auto rounded = 0;

    for (auto value: narrowingValues)
        if (narrowed(value, Rounding::NearestEven)
            != narrowed(value, Rounding::TowardZero))
            ++rounded;

    check(rounded > 0);

    auto values = Vector<float> {};

    for (auto value: narrowingValues)
        values.add(value);

    auto count = values.size() / 2;

    auto kernel = NarrowKernel {};

    CrossCheck {kernel}
        .input(kernel.input, values)
        .output(kernel.output, count)
        .run(count,
             [&](const Readback& readback)
             {
                 const auto& result = readback.floats(kernel.output);
                 const auto* name = readback.name();

                 for (auto i = 0; i < count; ++i)
                 {
                     auto word = wordOf(result[i]);
                     auto low = (std::uint16_t) (word & 0xffffu);
                     auto high = (std::uint16_t) (word >> 16);

                     check(narrowsCorrectly(low, values[i * 2]), name);
                     check(narrowsCorrectly(high, values[i * 2 + 1]), name);

                     if (readback.backend == Backend::Cpu)
                     {
                         check(halfMatches(
                                   low,
                                   narrowed(values[i * 2], Rounding::NearestEven)),
                               name);
                         check(halfMatches(high,
                                           narrowed(values[i * 2 + 1],
                                                    Rounding::NearestEven)),
                               name);
                     }
                 }
             });
};

// Both backends' source, generated on whichever host runs the suite - the
// Windows half being the one that cannot be executed here and the one whose
// spelling shares nothing with the other's.
auto tHalfSourceIsRight = test("PackedHalf/bothBackendsSpellTheHalfHelpers") = []
{
    auto builder = ShaderBuilder {};

    auto weights = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto i = builder.threadId();

    builder.write(output, i, weights.readHalf(i));
    builder.write(output, i + 1u, asFloat(packHalf2(weights.readHalf2(i))));

    auto metal = emitMetal(builder.graph());
    auto hlsl = emitHlsl(builder.graph());

    auto has = [](const std::string& source, const char* text)
    { return source.find(text) != std::string::npos; };

    check(has(metal, "eacpReadHalf"));
    check(has(metal, "as_type<half2>"));
    check(has(metal, "as_type<uint>(half2("));
    check(has(metal, "as_type<float>("));

    check(has(hlsl, "eacpReadHalf"));
    check(has(hlsl, "f16tof32"));
    check(has(hlsl, "f32tof16"));
    check(has(hlsl, "asfloat("));

    // The HLSL carries no MSL spelling anywhere - not in a helper body, not in
    // the kernel - which is the failure a shared emitter invites.
    check(!has(hlsl, "as_type"));
};

// The four-wide read, per backend: two words fetched as one record - a single
// packed load on Metal and two subscripts where there is no such spelling -
// then one unpack helper over each of them.
auto tHalf4SourceIsRight = test("PackedHalf/theFourWideReadIsOneRecordRead") = []
{
    auto builder = ShaderBuilder {};

    auto weights = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto i = builder.threadId();

    builder.write(output, i, weights.readHalf4(i));

    const auto& graph = builder.graph();
    auto metal = emitMetal(graph);
    auto hlsl = emitHlsl(graph);
    auto glsl = emitGlsl(graph);

    auto has = [](const std::string& source, const std::string& text)
    { return source.find(text) != std::string::npos; };

    // The index counts records of four halves, which is two words.
    for (const auto& source: {metal, hlsl, glsl})
        check(has(source, "uint t1 = (gid * 2u);"));

    check(has(metal,
              "float2 t2 = float2(*((device const packed_float2*) "
              "(buffer0 + t1)));"));
    check(has(metal,
              "float4 t3 = float4(eacpUnpackHalf2(as_type<uint>((t2).x)), "
              "eacpUnpackHalf2(as_type<uint>((t2).y)));"));
    check(!has(metal, "buffer0[t1]"));

    check(has(hlsl, "float2 t2 = float2(buffer0[t1], buffer0[t1 + 1u]);"));
    check(has(hlsl,
              "float4 t3 = float4(eacpUnpackHalf2(asuint((t2).x)), "
              "eacpUnpackHalf2(asuint((t2).y)));"));

    check(has(glsl, "vec2 t2 = vec2(buffer0[t1], buffer0[t1 + 1u]);"));
    check(has(glsl,
              "vec4 t3 = vec4(eacpUnpackHalf2(floatBitsToUint((t2).x)), "
              "eacpUnpackHalf2(floatBitsToUint((t2).y)));"));

    expectGlslCompiles(graph);
};

// Each helper is emitted only into shaders that call it, so a kernel narrowing
// nothing carries no packer and one reading whole words carries no reader.
auto tHalfHelpersAreNotAlwaysEmitted =
    test("PackedHalf/emitsEachHalfHelperOnlyWhenUsed") = []
{
    auto contains = [](const std::string& text, const char* needle)
    { return text.find(needle) != std::string::npos; };

    auto plain = PlainKernel {};
    auto reading = ReadHalfKernel {};
    auto packing = RoundTripKernel {};

    check(!contains(plain.source().source, "eacpReadHalf"));
    check(!contains(plain.source().source, "eacpPackHalf2"));

    check(contains(reading.source().source, "eacpReadHalf"));
    check(!contains(reading.source().source, "eacpPackHalf2"));

    check(contains(packing.source().source, "eacpPackHalf2"));
    check(!contains(packing.source().source, "eacpReadHalf"));
};
