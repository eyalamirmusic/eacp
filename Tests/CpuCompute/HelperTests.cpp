#include <NanoTest/NanoTest.h>

#include <eacp/GPU/CpuCompute/CpuCompute.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::GPU::CpuCompute;

// The C++ twins of the eacp* shader helpers, checked exhaustively where the
// domain allows against a double-precision reference that shares no code with
// them, and then through the executor: a recorded kernel calling each helper
// must produce, lane for lane, what calling the twin directly does.

namespace
{
constexpr auto infinity = std::numeric_limits<double>::infinity();

std::uint32_t bitsOf(float value)
{
    return std::bit_cast<std::uint32_t>(value);
}

float floatOf(std::uint32_t bits)
{
    return std::bit_cast<float>(bits);
}

bool sameBits(float a, float b)
{
    return bitsOf(a) == bitsOf(b);
}

// A binary16 pattern's value from its fields. With `unbounded`, the all-ones
// exponent is read as an ordinary one, so 0x7c00 is 65536 - the step past the
// largest finite half that round-to-nearest measures overflow against.
double halfValue(int bits, bool unbounded = false)
{
    auto sign = (bits & 0x8000) != 0 ? -1.0 : 1.0;
    auto exponent = (bits >> 10) & 0x1f;
    auto mantissa = bits & 0x3ff;

    if (exponent == 0)
        return sign * std::ldexp((double) mantissa, -24);

    if (exponent == 0x1f && !unbounded)
        return mantissa == 0 ? sign * infinity
                             : std::numeric_limits<double>::quiet_NaN();

    return sign * std::ldexp(1.0 + mantissa / 1024.0, exponent - 15);
}

bool isHalfNaN(int bits)
{
    return (bits & 0x7c00) == 0x7c00 && (bits & 0x3ff) != 0;
}

// A bfloat16 pattern's value from its fields: eight exponent bits, seven of
// mantissa, fp32's bias.
double bFloat16Value(int bits, bool unbounded = false)
{
    auto sign = (bits & 0x8000) != 0 ? -1.0 : 1.0;
    auto exponent = (bits >> 7) & 0xff;
    auto mantissa = bits & 0x7f;

    if (exponent == 0)
        return sign * std::ldexp((double) mantissa, -133);

    if (exponent == 0xff && !unbounded)
        return mantissa == 0 ? sign * infinity
                             : std::numeric_limits<double>::quiet_NaN();

    return sign * std::ldexp(1.0 + mantissa / 128.0, exponent - 127);
}

bool isBFloat16NaN(int bits)
{
    return (bits & 0x7f80) == 0x7f80 && (bits & 0x7f) != 0;
}

// Round-to-nearest-even checked at every boundary a narrowing has: for each
// pair of neighbours below the overflow step, the midpoint goes to the even
// one and the floats either side of it to the nearer. Midpoints of both
// formats are exact in float, so the reference is exact too.
template <typename Value, typename Narrow>
int roundingFailures(int lastFinite, Value value, Narrow narrow)
{
    auto failures = 0;

    for (auto sign = 0; sign <= 0x8000; sign += 0x8000)
    {
        for (auto low = 0; low <= lastFinite; ++low)
        {
            auto high = low + 1;
            auto midpoint =
                (value(low | sign, true) + value(high | sign, true)) / 2.0;
            auto asFloat = (float) midpoint;

            if ((double) asFloat != midpoint)
                ++failures;

            auto even = (low & 1) == 0 ? low : high;
            auto towardsZero = std::nextafter(asFloat, 0.f);
            auto awayFromZero =
                std::nextafter(asFloat, sign != 0 ? -INFINITY : INFINITY);

            failures += narrow(asFloat) != (even | sign);
            failures += narrow(towardsZero) != (low | sign);
            failures += narrow(awayFromZero) != (high | sign);
            failures += narrow((float) value(low | sign, false)) != (low | sign);
        }
    }

    return failures;
}

// Abramowitz & Stegun 7.1.26 exactly as ShaderEmitter's erfHelper spells it,
// re-typed here rather than shared, so a slip in either copy shows.
float shaderErf(float x)
{
    float a = std::fabs(x);
    float t = 1.0f / (1.0f + 0.3275911f * a);
    float e = 1.0f
              - t
                    * (0.254829592f
                       + t
                             * (-0.284496736f
                                + t
                                      * (1.421413741f
                                         + t * (-1.453152027f + t * 1.061405429f))))
                    * std::exp(-a * a);
    return a == 0.0f ? x : (x < 0.0f ? -e : e);
}

float shaderErfc(float x)
{
    float a = std::fabs(x);
    float t = 1.0f / (1.0f + 0.3275911f * a);
    float e =
        t
        * (0.254829592f
           + t
                 * (-0.284496736f
                    + t * (1.421413741f + t * (-1.453152027f + t * 1.061405429f))))
        * std::exp(-a * a);
    return a == 0.0f ? 1.0f : (x < 0.0f ? 2.0f - e : e);
}

float shaderSaturatingTanh(float x)
{
    return x >= 10.0f ? 1.0f : (x <= -10.0f ? -1.0f : std::tanh(x));
}

// -12..12 in steps of 1/512, plus the specials.
Vector<float> transcendentalInputs()
{
    auto values = Vector<float> {};

    for (auto step = -12 * 512; step <= 12 * 512; ++step)
        values.add((float) step / 512.f);

    for (auto special: {0.f,
                        -0.f,
                        1e-30f,
                        -1e-30f,
                        1e-45f,
                        9.99f,
                        10.f,
                        -10.f,
                        44.f,
                        990.f,
                        -990.f,
                        1e30f,
                        INFINITY,
                        -INFINITY})
        values.add(special);

    return values;
}

// Every 16-bit pattern once, as the low half of word k and the high half of
// word k too, rotated: 32768 words covering all 65536 patterns in each half,
// every byte in each of the four positions and every nibble in each of eight.
Vector<float> everyPatternWords()
{
    auto words = Vector<float> {};

    for (auto k = 0u; k < 32768u; ++k)
    {
        auto low = 2u * k;
        auto high = (2u * k + 1u) ^ 0x5a5au;
        words.add(floatOf(low | (high << 16u)));
    }

    return words;
}

std::uint32_t wordAt(const Vector<float>& words, int index)
{
    return bitsOf(words[index]);
}
} // namespace

auto tHalfWidening = test("Helpers/everyHalfWidensExactly") = []
{
    auto failures = 0;

    for (auto bits = 0; bits < 65536; ++bits)
    {
        auto widened = widenHalf((std::uint16_t) bits);
        auto negative = (bits & 0x8000) != 0;

        if (isHalfNaN(bits))
        {
            failures += !std::isnan(widened) || std::signbit(widened) != negative
                        || ((bitsOf(widened) >> 13) & 0x3ffu) != (bits & 0x3ffu);
            continue;
        }

        failures += (double) widened != halfValue(bits);
        failures += std::signbit(widened) != negative;
    }

    check(failures == 0);
};

auto tHalfPairs = test("Helpers/unpackHalf2AndReadHalfTakeTheirHalves") = []
{
    auto failures = 0;

    for (auto bits = 0u; bits < 65536u; ++bits)
    {
        auto other = (bits * 40503u + 1u) & 0xffffu;
        auto word = bits | (other << 16u);
        auto pair = unpackHalf2(word);

        failures += !sameBits(pair[0], widenHalf((std::uint16_t) bits));
        failures += !sameBits(pair[1], widenHalf((std::uint16_t) other));
        failures += !sameBits(readHalf(word, 0u), pair[0]);
        failures += !sameBits(readHalf(word, 1u), pair[1]);
    }

    // The shift is 16 * parity taken modulo 32, as the hardware takes it.
    failures += !sameBits(readHalf(0x3c004000u, 2u), readHalf(0x3c004000u, 0u));

    check(failures == 0);
};

auto tHalfRoundTrip = test("Helpers/everyHalfRoundTripsThroughPackHalf2") = []
{
    auto failures = 0;

    for (auto bits = 0u; bits < 65536u; ++bits)
    {
        if (isHalfNaN((int) bits))
            continue;

        auto value = widenHalf((std::uint16_t) bits);
        failures += narrowToHalf(value) != bits;
        failures += packHalf2({value, value}) != (bits | (bits << 16u));
        failures += packHalf2({0.f, value}) != (bits << 16u);
    }

    check(failures == 0);
};

auto tHalfRounding = test("Helpers/narrowingToHalfRoundsToNearestEven") = []
{
    check(roundingFailures(0x7bff, halfValue, narrowToHalf) == 0);

    check(narrowToHalf(65504.f) == 0x7bff);
    check(narrowToHalf(std::nextafter(65520.f, 0.f)) == 0x7bff);
    check(narrowToHalf(65520.f) == 0x7c00);
    check(narrowToHalf(-65520.f) == 0xfc00);
    check(narrowToHalf(1e10f) == 0x7c00);
    check(narrowToHalf(INFINITY) == 0x7c00);
    check(narrowToHalf(-INFINITY) == 0xfc00);

    check(narrowToHalf(0.f) == 0x0000);
    check(narrowToHalf(-0.f) == 0x8000);
    check(narrowToHalf(0x1p-25f) == 0x0000);
    check(narrowToHalf(-0x1p-25f) == 0x8000);
    check(narrowToHalf(std::nextafter(0x1p-25f, 1.f)) == 0x0001);
    check(narrowToHalf(0x1p-24f) == 0x0001);
    check(narrowToHalf(1e-45f) == 0x0000);
    check(narrowToHalf(-1e-45f) == 0x8000);
    check(narrowToHalf(0x1.ffcp-15f) == 0x0400);
    check(narrowToHalf(std::nextafter(0x1.ffcp-15f, 0.f)) == 0x03ff);
    check(narrowToHalf(0x1.ff8p-15f) == 0x03ff);

    for (auto nanBits:
         {0x7fc00000u, 0x7f800001u, 0xffc00000u, 0xff812345u, 0x7fffffffu})
    {
        auto narrowed = narrowToHalf(floatOf(nanBits));
        check(isHalfNaN(narrowed));
        check(((narrowed & 0x8000u) != 0) == ((nanBits & 0x80000000u) != 0));
    }
};

// clang-cl on x64 has the type but links no compiler-rt, so the conversion's
// __truncsfhf2 libcall is left unresolved; ARM64 converts in an instruction.
#if defined(__FLT16_MANT_DIG__) && !(defined(_MSC_VER) && defined(_M_X64))
// A second reference where the compiler has one: its own float -> _Float16
// conversion, round-to-nearest-even per IEEE, over a stride through every
// float bit pattern.
auto tHalfAgainstCompiler = test("Helpers/narrowingToHalfMatchesTheCompiler") = []
{
    auto failures = 0;

    for (auto bits = std::uint64_t {0}; bits <= 0xffffffffu; bits += 4099u)
    {
        auto value = floatOf((std::uint32_t) bits);

        if (std::isnan(value))
            continue;

        auto expected = std::bit_cast<std::uint16_t>((_Float16) value);
        failures += narrowToHalf(value) != expected;
    }

    check(failures == 0);
};
#endif

auto tBFloat16Widening = test("Helpers/everyBFloat16WidensExactly") = []
{
    auto failures = 0;

    for (auto bits = 0u; bits < 65536u; ++bits)
    {
        auto other = (bits * 40503u + 7u) & 0xffffu;
        auto word = bits | (other << 16u);
        auto pair = unpackBFloat16x2(word);
        auto negative = (bits & 0x8000u) != 0;

        failures += !sameBits(readBFloat16(word, 0u), pair[0]);
        failures += !sameBits(readBFloat16(word, 1u), pair[1]);
        failures += bitsOf(pair[1]) != (other << 16u);

        if (isBFloat16NaN((int) bits))
        {
            failures += !std::isnan(pair[0]) || std::signbit(pair[0]) != negative;
            continue;
        }

        failures += (double) pair[0] != bFloat16Value((int) bits);
        failures += std::signbit(pair[0]) != negative;
    }

    check(failures == 0);
};

auto tBFloat16RoundTrip = test("Helpers/everyBFloat16RoundTripsThroughItsPack") = []
{
    auto failures = 0;

    for (auto bits = 0u; bits < 65536u; ++bits)
    {
        if (isBFloat16NaN((int) bits))
            continue;

        auto value = unpackBFloat16x2(bits)[0];
        failures += packBFloat16x2({value, value}) != (bits | (bits << 16u));
    }

    check(failures == 0);
};

auto tBFloat16Rounding = test("Helpers/narrowingToBFloat16RoundsToNearestEven") = []
{
    auto low = [](float value) { return (int) packBFloat16x2({value, 0.f}); };
    auto high = [](float value)
    { return (int) (packBFloat16x2({0.f, value}) >> 16u); };

    check(roundingFailures(0x7f7f, bFloat16Value, low) == 0);
    check(roundingFailures(0x7f7f, bFloat16Value, high) == 0);

    check(low(std::numeric_limits<float>::max()) == 0x7f80);
    check(low(-std::numeric_limits<float>::max()) == 0xff80);
    check(low(INFINITY) == 0x7f80);
    check(low(-INFINITY) == 0xff80);
    check(low(-0.f) == 0x8000);
    check(high(-0.f) == 0x8000);
    check(low(std::numeric_limits<float>::denorm_min()) == 0x0000);
    check(low(floatOf(0x00008000u)) == 0x0000);
    check(low(floatOf(0x00018000u)) == 0x0002);

    // A NaN is quieted, never carried into the exponent.
    check(low(floatOf(0x7f800001u)) == 0x7fc0);
    check(low(floatOf(0xff800001u)) == 0xffc0);
    check(high(floatOf(0x7fffffffu)) == 0x7fff);
    check(low(floatOf(0x7fc00000u)) == 0x7fc0);
};

auto tBytes = test("Helpers/everyByteWidensInEveryPosition") = []
{
    auto failures = 0;

    for (auto byte = 0u; byte < 256u; ++byte)
    {
        auto word = byte | (((byte + 85u) & 0xffu) << 8u)
                    | (((byte + 170u) & 0xffu) << 16u)
                    | (((byte * 7u + 3u) & 0xffu) << 24u);
        auto signedValues = unpackInt8x4(word);
        auto unsignedValues = unpackUInt8x4(word);

        for (auto position = 0u; position < 4u; ++position)
        {
            auto value = (word >> (8u * position)) & 0xffu;
            auto asSigned =
                (double) (value >= 128u ? (int) value - 256 : (int) value);

            failures += (double) signedValues[position] != asSigned;
            failures += (double) unsignedValues[position] != (double) value;
            failures += (double) readInt8(word, position) != asSigned;
            failures += (double) readUInt8(word, position) != (double) value;
        }
    }

    failures += readUInt8(0x000000abu, 4u) != 171.f;
    check(failures == 0);
};

auto tNibbles = test("Helpers/everyNibbleWidensInEveryPosition") = []
{
    auto failures = 0;

    for (auto nibble = 0u; nibble < 16u; ++nibble)
    {
        for (auto position = 0u; position < 4u; ++position)
        {
            auto word = 0xabcd0000u | (nibble << (4u * position))
                        | ((15u - nibble) << (4u * ((position + 1u) % 4u)));
            auto signedValues = unpackInt4x4(word);
            auto unsignedValues = unpackUInt4x4(word);

            for (auto at = 0u; at < 4u; ++at)
            {
                auto value = (word >> (4u * at)) & 0xfu;
                auto asSigned =
                    (double) (value >= 8u ? (int) value - 16 : (int) value);

                failures += (double) signedValues[at] != asSigned;
                failures += (double) unsignedValues[at] != (double) value;
            }
        }
    }

    check(failures == 0);
};

auto tInt8Pack = test("Helpers/int8PacksKeepTheLowByteAndRoundTrip") = []
{
    auto failures = 0;

    for (auto value = -128; value <= 127; ++value)
    {
        auto word = packInt8x4({value, -value - 1, value / 2, 127 - (value + 128)});
        auto back = unpackInt8x4(word);

        failures += back[0] != (float) value;
        failures += back[1] != (float) (-value - 1);
        failures += back[2] != (float) (value / 2);
        failures += back[3] != (float) (127 - (value + 128));
    }

    for (auto value = 0u; value <= 255u; ++value)
    {
        auto word = packUInt8x4({value, 255u - value, value / 3u, value ^ 0x5au});
        auto back = unpackUInt8x4(word);

        failures += back[0] != (float) value;
        failures += back[1] != (float) (255u - value);
        failures += back[2] != (float) (value / 3u);
        failures += back[3] != (float) (value ^ 0x5au);
    }

    // Out of range wraps to the low byte: never a saturation.
    auto lowByte = [](long long value)
    { return (std::uint32_t) (((value % 256) + 256) % 256); };

    for (auto value = -70000; value <= 70000; value += 7)
    {
        failures += packInt8x4({value, 0, 0, 0}) != lowByte(value);
        failures += packInt8x4({0, 0, 0, value}) != lowByte(value) << 24u;
        failures += packUInt8x4({(std::uint32_t) value, 0u, 0u, 0u})
                    != lowByte((std::uint32_t) value);
    }

    auto extremes = packInt8x4({std::numeric_limits<std::int32_t>::min(),
                                std::numeric_limits<std::int32_t>::max(),
                                128,
                                -129});
    failures += extremes != 0x7f80ff00u;
    failures += packUInt8x4({0xffffffffu, 256u, 0x1234u, 0x80u}) != 0x803400ffu;

    check(failures == 0);
};

auto tErf = test("Helpers/erfIsTheShaderFormula") = []
{
    auto failures = 0;
    auto worst = 0.0;

    for (auto x: transcendentalInputs())
    {
        auto value = errorFunction(x);
        failures += !sameBits(value, shaderErf(x));
        failures += !sameBits(errorFunction(-x), -value);
        worst = std::fmax(worst, std::fabs((double) value - std::erf((double) x)));
    }

    check(failures == 0);
    check(worst < 1.0e-6);
    check(sameBits(errorFunction(-0.f), -0.f));
    check(errorFunction(INFINITY) == 1.f);
    check(errorFunction(-INFINITY) == -1.f);
    check(std::isnan(errorFunction(NAN)));
};

auto tErfc = test("Helpers/erfcIsTheShaderFormula") = []
{
    auto failures = 0;
    auto worst = 0.0;

    for (auto x: transcendentalInputs())
    {
        auto value = complementaryErrorFunction(x);
        failures += !sameBits(value, shaderErfc(x));
        worst = std::fmax(worst, std::fabs((double) value - std::erfc((double) x)));
    }

    check(failures == 0);
    check(worst < 1.0e-6);
    check(complementaryErrorFunction(0.f) == 1.f);
    check(complementaryErrorFunction(-0.f) == 1.f);
    check(complementaryErrorFunction(INFINITY) == 0.f);
    check(complementaryErrorFunction(-INFINITY) == 2.f);
    check(complementaryErrorFunction(4.f) > 0.f);
    check(std::isnan(complementaryErrorFunction(NAN)));
};

auto tTanh = test("Helpers/saturatingTanhIsTheShaderFormula") = []
{
    auto failures = 0;
    auto worst = 0.0;

    for (auto x: transcendentalInputs())
    {
        auto value = saturatingTanh(x);
        failures += !sameBits(value, shaderSaturatingTanh(x));
        worst = std::fmax(worst, std::fabs((double) value - std::tanh((double) x)));
    }

    check(failures == 0);
    check(worst < 1.0e-6);
    check(saturatingTanh(10.f) == 1.f);
    check(saturatingTanh(-10.f) == -1.f);
    check(saturatingTanh(990.f) == 1.f);
    check(saturatingTanh(std::nextafter(10.f, 0.f))
          == std::tanh(std::nextafter(10.f, 0.f)));
};

// ---------------------------------------------------------------------------
// Through the executor.

namespace
{
constexpr auto helperSentinel = -3.f;

Vector<float> helperFloats(int count, float value)
{
    auto values = Vector<float> {};
    values.resize(count, value);
    return values;
}

struct ScalarTranscendentalKernel final : ComputeKernel
{
    ScalarTranscendentalKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto x = input[i];
        write(erfOut, i, erf(x));
        write(erfcOut, i, erfc(x));
        write(tanhOut, i, saturatingTanh(x));
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> erfOut;
    Uniform<OutputBuffer> erfcOut;
    Uniform<OutputBuffer> tanhOut;
    EACP_SHADER(input, erfOut, erfcOut, tanhOut)
};

struct VectorTranscendentalKernel final : ComputeKernel
{
    VectorTranscendentalKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write4(erfOut, i, erf(input.read4(i)));
        write2(erfcOut, i, erfc(input.read2(i)));
        write3(tanhOut, i, saturatingTanh(input.read3(i)));
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> erfOut;
    Uniform<OutputBuffer> erfcOut;
    Uniform<OutputBuffer> tanhOut;
    EACP_SHADER(input, erfOut, erfcOut, tanhOut)
};

struct ElementReadKernel final : ComputeKernel
{
    ElementReadKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(halves, i, words.readHalf(i));
        write(bFloat16s, i, words.readBFloat16(i));
        write(int8s, i, words.readInt8(i));
        write(uint8s, i, words.readUInt8(i));
        write(literal, i, words.readHalf(3u) + words.readInt8(6u));
    }

    Uniform<InputBuffer> words;
    Uniform<OutputBuffer> halves;
    Uniform<OutputBuffer> bFloat16s;
    Uniform<OutputBuffer> int8s;
    Uniform<OutputBuffer> uint8s;
    Uniform<OutputBuffer> literal;
    EACP_SHADER(words, halves, bFloat16s, int8s, uint8s, literal)
};

struct WideReadKernel final : ComputeKernel
{
    WideReadKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write2(halves, i, words.readHalf2(i));
        write2(bFloat16s, i, words.readBFloat16x2(i));
        write4(int8s, i, words.readInt8x4(i));
        write4(uint8s, i, words.readUInt8x4(i));

        auto nibbles = words.readInt4x8(i);
        write4(int4s, i * 2u, nibbles.low);
        write4(int4s, i * 2u + 1u, nibbles.high);

        auto unsignedNibbles = words.readUInt4x8(i);
        write4(uint4s, i * 2u, unsignedNibbles.low);
        write4(uint4s, i * 2u + 1u, unsignedNibbles.high);
    }

    Uniform<InputBuffer> words;
    Uniform<OutputBuffer> halves;
    Uniform<OutputBuffer> bFloat16s;
    Uniform<OutputBuffer> int8s;
    Uniform<OutputBuffer> uint8s;
    Uniform<OutputBuffer> int4s;
    Uniform<OutputBuffer> uint4s;
    EACP_SHADER(words, halves, bFloat16s, int8s, uint8s, int4s, uint4s)
};

struct PackKernel final : ComputeKernel
{
    PackKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto pair = floats.read2(i);
        writeHalf2(halves, i, pair);
        writeBFloat16x2(bFloat16s, i, pair);

        auto quad = integers.read4(i);
        writeInt8x4(int8s, i, toInt(quad));
        writeUInt8x4(uint8s, i, quad);
    }

    Uniform<InputBuffer> floats;
    Uniform<UIntInputBuffer> integers;
    Uniform<OutputBuffer> halves;
    Uniform<OutputBuffer> bFloat16s;
    Uniform<OutputBuffer> int8s;
    Uniform<OutputBuffer> uint8s;
    EACP_SHADER(floats, integers, halves, bFloat16s, int8s, uint8s)
};

// Floats a narrowing has to decide about: every half widened, the midpoints
// between them, and a spread of arbitrary bit patterns, NaNs included.
Vector<float> narrowingInputs()
{
    auto values = Vector<float> {};

    for (auto bits = 0; bits < 65536; ++bits)
    {
        values.add(widenHalf((std::uint16_t) bits));
        values.add(
            (float) ((halfValue(bits, true) + halfValue(bits + 1, true)) / 2.0));
    }

    for (auto k = 0u; k < 20000u; ++k)
        values.add(floatOf(k * 2654435761u));

    return values;
}

// Signed and unsigned integers, in range and far out of it.
Vector<std::uint32_t> packingIntegers(int quads)
{
    auto values = Vector<std::uint32_t> {};

    for (auto k = 0u; k < (std::uint32_t) quads * 4u; ++k)
        values.add(k % 3u == 0u ? k * 2654435761u : (k & 0x1ffu) - 256u);

    return values;
}

bool refusedNaming(const Executor& executor, const char* name)
{
    return !executor.isValid() && executor.reason().find(name) != std::string::npos;
}
} // namespace

auto tScalarTranscendentals =
    test("Executor/theTranscendentalHelpersRunOnScalarLanes") = []
{
    auto input = transcendentalInputs();
    auto count = input.size();

    auto kernel = ScalarTranscendentalKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto erfOut = helperFloats(count + 5, helperSentinel);
    auto erfcOut = helperFloats(count + 5, helperSentinel);
    auto tanhOut = helperFloats(count + 5, helperSentinel);

    auto bindings = Bindings {};
    bindings.set(kernel.input, input);
    bindings.set(kernel.erfOut, erfOut);
    bindings.set(kernel.erfcOut, erfcOut);
    bindings.set(kernel.tanhOut, tanhOut);
    check(executor.dispatch(bindings, count));

    auto failures = 0;

    for (auto i = 0; i < count; ++i)
    {
        failures += !sameBits(erfOut[i], errorFunction(input[i]));
        failures += !sameBits(erfcOut[i], complementaryErrorFunction(input[i]));
        failures += !sameBits(tanhOut[i], saturatingTanh(input[i]));
    }

    check(failures == 0);
    check(erfOut[count] == helperSentinel);
};

auto tVectorTranscendentals =
    test("Executor/theTranscendentalHelpersRunOnVectorLanes") = []
{
    auto input = transcendentalInputs();

    while (input.size() % 12 != 0)
        input.add(0.25f);

    auto count = input.size() / 4;

    auto kernel = VectorTranscendentalKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto erfOut = helperFloats(input.size(), helperSentinel);
    auto erfcOut = helperFloats(input.size(), helperSentinel);
    auto tanhOut = helperFloats(input.size(), helperSentinel);

    auto bindings = Bindings {};
    bindings.set(kernel.input, input);
    bindings.set(kernel.erfOut, erfOut);
    bindings.set(kernel.erfcOut, erfcOut);
    bindings.set(kernel.tanhOut, tanhOut);
    check(executor.dispatch(bindings, count));

    auto failures = 0;

    for (auto i = 0; i < count * 4; ++i)
        failures += !sameBits(erfOut[i], errorFunction(input[i]));

    for (auto i = 0; i < count * 2; ++i)
        failures += !sameBits(erfcOut[i], complementaryErrorFunction(input[i]));

    for (auto i = 0; i < count * 3; ++i)
        failures += !sameBits(tanhOut[i], saturatingTanh(input[i]));

    check(failures == 0);
    check(erfcOut[count * 2] == helperSentinel);
};

auto tElementReads = test("Executor/theElementReadHelpersReadEveryPattern") = []
{
    auto words = everyPatternWords();
    auto count = words.size() * 2;

    auto kernel = ElementReadKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto halves = helperFloats(count, helperSentinel);
    auto bFloat16s = helperFloats(count, helperSentinel);
    auto int8s = helperFloats(count, helperSentinel);
    auto uint8s = helperFloats(count, helperSentinel);
    auto literal = helperFloats(count, helperSentinel);

    auto bindings = Bindings {};
    bindings.set(kernel.words, words);
    bindings.set(kernel.halves, halves);
    bindings.set(kernel.bFloat16s, bFloat16s);
    bindings.set(kernel.int8s, int8s);
    bindings.set(kernel.uint8s, uint8s);
    bindings.set(kernel.literal, literal);
    check(executor.dispatch(bindings, count));

    auto failures = 0;
    auto literalValue =
        readHalf(wordAt(words, 1), 1u) + readInt8(wordAt(words, 1), 2u);

    for (auto i = 0; i < count; ++i)
    {
        auto halfWord = wordAt(words, i / 2);
        auto parity = (std::uint32_t) i % 2u;
        failures += !sameBits(halves[i], readHalf(halfWord, parity));
        failures += !sameBits(bFloat16s[i], readBFloat16(halfWord, parity));

        auto byteWord = wordAt(words, i / 4);
        auto position = (std::uint32_t) i % 4u;
        failures += !sameBits(int8s[i], readInt8(byteWord, position));
        failures += !sameBits(uint8s[i], readUInt8(byteWord, position));
        failures += !sameBits(literal[i], literalValue);
    }

    check(failures == 0);
};

auto tWideReads = test("Executor/theUnpackHelpersWidenEveryWord") = []
{
    auto words = everyPatternWords();
    auto count = words.size();

    auto kernel = WideReadKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto halves = helperFloats(count * 2, helperSentinel);
    auto bFloat16s = helperFloats(count * 2, helperSentinel);
    auto int8s = helperFloats(count * 4, helperSentinel);
    auto uint8s = helperFloats(count * 4, helperSentinel);
    auto int4s = helperFloats(count * 8, helperSentinel);
    auto uint4s = helperFloats(count * 8, helperSentinel);

    auto bindings = Bindings {};
    bindings.set(kernel.words, words);
    bindings.set(kernel.halves, halves);
    bindings.set(kernel.bFloat16s, bFloat16s);
    bindings.set(kernel.int8s, int8s);
    bindings.set(kernel.uint8s, uint8s);
    bindings.set(kernel.int4s, int4s);
    bindings.set(kernel.uint4s, uint4s);
    check(executor.dispatch(bindings, count));

    auto failures = 0;

    for (auto i = 0; i < count; ++i)
    {
        auto word = wordAt(words, i);
        auto half = unpackHalf2(word);
        auto bFloat16 = unpackBFloat16x2(word);
        auto bytes = unpackInt8x4(word);
        auto unsignedBytes = unpackUInt8x4(word);
        auto lowNibbles = unpackInt4x4(word);
        auto highNibbles = unpackInt4x4(word >> 16u);
        auto lowUnsigned = unpackUInt4x4(word);
        auto highUnsigned = unpackUInt4x4(word >> 16u);

        for (auto c = 0; c < 2; ++c)
        {
            failures += !sameBits(halves[i * 2 + c], half[(std::size_t) c]);
            failures += !sameBits(bFloat16s[i * 2 + c], bFloat16[(std::size_t) c]);
        }

        for (auto c = 0; c < 4; ++c)
        {
            auto at = (std::size_t) c;
            failures += !sameBits(int8s[i * 4 + c], bytes[at]);
            failures += !sameBits(uint8s[i * 4 + c], unsignedBytes[at]);
            failures += !sameBits(int4s[i * 8 + c], lowNibbles[at]);
            failures += !sameBits(int4s[i * 8 + 4 + c], highNibbles[at]);
            failures += !sameBits(uint4s[i * 8 + c], lowUnsigned[at]);
            failures += !sameBits(uint4s[i * 8 + 4 + c], highUnsigned[at]);
        }
    }

    check(failures == 0);
};

auto tPacks = test("Executor/thePackHelpersNarrowEveryLane") = []
{
    auto floats = narrowingInputs();

    if (floats.size() % 2 != 0)
        floats.add(1.f);

    auto count = floats.size() / 2;
    auto integers = packingIntegers(count);

    auto kernel = PackKernel {};
    auto executor = Executor {kernel};
    check(executor.isValid(), executor.reason());

    auto halves = helperFloats(count, helperSentinel);
    auto bFloat16s = helperFloats(count, helperSentinel);
    auto int8s = helperFloats(count, helperSentinel);
    auto uint8s = helperFloats(count, helperSentinel);

    auto bindings = Bindings {};
    bindings.set(kernel.floats, floats);
    bindings.set(kernel.integers, integers);
    bindings.set(kernel.halves, halves);
    bindings.set(kernel.bFloat16s, bFloat16s);
    bindings.set(kernel.int8s, int8s);
    bindings.set(kernel.uint8s, uint8s);
    check(executor.dispatch(bindings, count));

    auto failures = 0;

    for (auto i = 0; i < count; ++i)
    {
        auto pair = HelperFloat2 {floats[i * 2], floats[i * 2 + 1]};
        auto quad = HelperUInt4 {integers[i * 4],
                                 integers[i * 4 + 1],
                                 integers[i * 4 + 2],
                                 integers[i * 4 + 3]};
        auto signedQuad = HelperInt4 {(std::int32_t) quad[0],
                                      (std::int32_t) quad[1],
                                      (std::int32_t) quad[2],
                                      (std::int32_t) quad[3]};

        failures += bitsOf(halves[i]) != packHalf2(pair);
        failures += bitsOf(bFloat16s[i]) != packBFloat16x2(pair);
        failures += bitsOf(int8s[i]) != packInt8x4(signedQuad);
        failures += bitsOf(uint8s[i]) != packUInt8x4(quad);
    }

    check(failures == 0);
};

auto tMalformedHelpers = test("Executor/aMalformedHelperCallIsRefusedByName") = []
{
    auto unknown = ShaderGraph {};
    auto output = unknown.addStorageBuffer(BufferAccess::Write, ValueType::Float);
    auto thread = unknown.addThreadId();
    auto args = Vector<int> {};
    args.add(unknown.addConstant(1.f));
    unknown.addStore(
        output, thread, unknown.addCall(ValueType::Float, "eacpNoSuchHelper", args));

    check(refusedNaming(Executor {unknown}, "eacpNoSuchHelper"));
    check(refusedNaming(Executor {unknown}, "unknown helper"));

    auto wrongType = ShaderGraph {};
    auto wrongOutput =
        wrongType.addStorageBuffer(BufferAccess::Write, ValueType::Float);
    auto wrongThread = wrongType.addThreadId();
    auto floatArgument = Vector<int> {};
    floatArgument.add(wrongType.addConstant(1.f));
    auto unpacked =
        wrongType.addCall(ValueType::Float2, "eacpUnpackHalf2", floatArgument);
    wrongType.addStore(wrongOutput,
                       wrongThread,
                       wrongType.addSwizzle(ValueType::Float, unpacked, "x"));

    check(refusedNaming(Executor {wrongType}, "eacpUnpackHalf2"));

    auto integer = ShaderGraph {};
    auto integerOutput =
        integer.addStorageBuffer(BufferAccess::Write, ValueType::Float);
    auto integerThread = integer.addThreadId();
    auto uintArgument = Vector<int> {};
    uintArgument.add(integerThread);
    integer.addStore(integerOutput,
                     integerThread,
                     integer.addCall(ValueType::Float, "eacpErf", uintArgument));

    check(refusedNaming(Executor {integer}, "eacpErf"));
};
