#include "Common.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

// unpackBFloat16x2, packBFloat16x2 and InputBuffer::readBFloat16: the bf16
// sibling of the fp16 family in PackedHalfTests, for the format a modern
// checkpoint actually ships in.
//
// bf16 is fp32 with the low sixteen mantissa bits dropped - eight exponent bits
// against fp16's five - so the two are not interchangeable storage, and that is
// the point of having both. Three things have to hold:
//
//   1. Widening is the sixteen bits back at the top of a word and nothing else:
//      exact, and the same on every backend, because no dialect's float
//      conversion is involved at all.
//   2. The bits survive the trip. Every bf16 subnormal widens to an fp32
//      subnormal, and the packed words here are themselves mostly fp32
//      subnormals, so hardware that flushed a denormal on load would quietly
//      zero a weight with nothing in the arithmetic looking wrong.
//   3. The narrowing is round-to-nearest-even, bit for bit, on all three
//      backends. That is the one thing the fp16 family cannot promise, and it
//      is why packBFloat16x2 does its rounding in integer arithmetic rather
//      than through an instruction.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
// One 32-bit word out of two bf16 bit patterns, low half first - the layout
// unpackBFloat16x2 promises and the one a packer on the CPU has to match.
std::uint32_t packed(std::uint16_t low, std::uint16_t high)
{
    return (std::uint32_t) low | ((std::uint32_t) high << 16);
}

float assembled(std::uint32_t word)
{
    auto value = 0.0f;
    std::memcpy(&value, &word, sizeof(value));
    return value;
}

std::uint32_t bitsOf(float value)
{
    auto word = std::uint32_t {};
    std::memcpy(&word, &value, sizeof(word));
    return word;
}

// The CPU reference. Written out rather than called through bfloat16ToFloat
// because the host helper is under test beside the shader, and a reference that
// is the thing it checks proves nothing.
float widened(std::uint16_t bits)
{
    return assembled((std::uint32_t) bits << 16);
}

// Every bf16 class, and deliberately including the two magnitudes fp16 gets
// wrong: 0x3586 is about 1e-6, which fp16 holds only as a subnormal, and 0x7149
// is about 1e30, which fp16 cannot hold at all. Paired with each other below,
// so most of the packed words are fp32 subnormals - the entries that catch a
// load path which does not preserve bits.
const auto bfloat16Patterns = Array<std::uint16_t, 17> {
    0x0000, // +0
    0x8000, // -0
    0x0001, // smallest subnormal, 2^-133
    0x007f, // largest subnormal
    0x0080, // smallest normal, 2^-126
    0x3f80, // 1.0
    0xbf80, // -1.0
    0x4000, // 2.0
    0xc0a0, // -5.0
    0x7f7f, // largest finite
    0xff7f, // most negative finite
    0x7f80, // +inf
    0xff80, // -inf
    0x7fc0, // NaN
    0x3eab, // ~1/3
    0x3586, // ~1e-6, a subnormal in fp16
    0x7149 // ~1e30, out of fp16's range entirely
};

Vector<std::uint32_t> everyPackedPair()
{
    auto words = Vector<std::uint32_t> {};

    for (auto low: bfloat16Patterns)
        for (auto high: bfloat16Patterns)
            words.add(packed(low, high));

    return words;
}

// One thread per packed word: read it as a float, recover the bits, unpack, and
// write the two widened values out side by side.
struct UnpackKernel final : ComputeProgram
{
    UnpackKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto pair = unpackBFloat16x2(asUInt(words[i]));

        write(output, i * 2u, pair.x());
        write(output, i * 2u + 1u, pair.y());
    }

    Uniform<InputBuffer> words;
    Uniform<OutputBuffer> output;

    EACP_SHADER(words, output)
};

// Ordinary arithmetic, so nothing pulls a helper in.
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

// One element at a time out of a buffer whose elements are bf16, which is what
// a kernel walking a weight matrix writes rather than doing the word and parity
// arithmetic itself.
struct ReadBFloat16Kernel final : ComputeProgram
{
    ReadBFloat16Kernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, weights.readBFloat16(i));
    }

    Uniform<InputBuffer> weights;
    Uniform<OutputBuffer> output;

    EACP_SHADER(weights, output)
};

// The literal-index overload, which a kernel reaching for a fixed element - a
// bias, a scale - is what spells.
struct LiteralBFloat16Kernel final : ComputeProgram
{
    LiteralBFloat16Kernel() { compile(); }

    void define() override
    {
        auto i = threadId();

        write(output, i * 4u, weights.readBFloat16(0u));
        write(output, i * 4u + 1u, weights.readBFloat16(1u));
        write(output, i * 4u + 2u, weights.readBFloat16(2u));
        write(output, i * 4u + 3u, weights.readBFloat16(3u));
    }

    Uniform<InputBuffer> weights;
    Uniform<OutputBuffer> output;

    EACP_SHADER(weights, output)
};

struct ReadBFloat16x2Kernel final : ComputeProgram
{
    ReadBFloat16x2Kernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, weights.readBFloat16x2(i));
    }

    Uniform<InputBuffer> weights;
    Uniform<OutputBuffer> output;

    EACP_SHADER(weights, output)
};

// Widen a word and narrow it straight back, storing the packed result in the
// float slot it came out of - the whole bf16-storage round trip in one line.
struct RoundTripKernel final : ComputeProgram
{
    RoundTripKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, asFloat(packBFloat16x2(weights.readBFloat16x2(i))));
    }

    Uniform<InputBuffer> weights;
    Uniform<OutputBuffer> output;

    EACP_SHADER(weights, output)
};

// The same thing said the short way, which is the only claim writeBFloat16x2
// makes.
struct WriteBFloat16x2Kernel final : ComputeProgram
{
    WriteBFloat16x2Kernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        writeBFloat16x2(output, i, weights.readBFloat16x2(i));
    }

    Uniform<InputBuffer> weights;
    Uniform<OutputBuffer> output;

    EACP_SHADER(weights, output)
};

// Two ordinary fp32 values narrowed and packed, which is what a kernel writing
// bf16 output does.
struct NarrowKernel final : ComputeProgram
{
    NarrowKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, asFloat(packBFloat16x2(input.read2(i))));
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;

    EACP_SHADER(input, output)
};

// The narrowing reference, and the whole reason this family exists: one rule,
// not one per backend. Round to nearest even in integer arithmetic, a NaN
// quieted rather than rounded so it cannot carry into the exponent and come
// back as an infinity.
std::uint16_t narrowed(float value)
{
    auto bits = bitsOf(value);

    if ((bits & 0x7fffffffu) > 0x7f800000u)
        return (std::uint16_t) ((bits | 0x00400000u) >> 16);

    return (std::uint16_t) ((bits + 0x7fffu + ((bits >> 16) & 1u)) >> 16);
}

bool isBFloat16NaN(std::uint16_t bits)
{
    return (bits & 0x7f80u) == 0x7f80u && (bits & 0x7fu) != 0;
}

// Bit-for-bit on everything but a NaN's payload, which is the one thing a
// buffer round trip through a float register does not owe us.
bool bfloat16Matches(std::uint16_t gpu, std::uint16_t cpu)
{
    if (isBFloat16NaN(cpu))
        return isBFloat16NaN(gpu);

    return gpu == cpu;
}

// Bit-for-bit, not within a tolerance. Widening bf16 to fp32 is exact for every
// value there is, so any difference at all is a fault rather than rounding.
bool matches(float gpu, float cpu)
{
    if (std::isnan(cpu))
        return std::isnan(gpu);

    return gpu == cpu;
}

// Values bf16 cannot hold exactly, chosen so every rounding case appears: both
// directions of a tie, the overflow into infinity, and the subnormal range,
// which for bf16 is entirely inside fp32's own subnormals. The ties are
// spelled as fractions because a tie has to *be* one to the last bit.
const auto narrowingValues = Array<float, 20> {
    1.0f + 1.0f / 256.0f, // half an ulp above 1.0: the tie rounds back down
    1.0f + 3.0f / 256.0f, // the tie one ulp up, where even is the far side
    1.0f / 3.0f,
    0.1f,
    -0.1f,
    2.0f,
    std::ldexp(1.9921875f, 127), // the largest finite bf16, exactly
    std::numeric_limits<float>::max(), // past the tie: over to infinity
    1.0e-6f, // fp16 holds this only as a subnormal
    1.0e30f, // fp16 cannot hold this at all
    std::ldexp(1.0f, -133), // the smallest bf16 subnormal, exactly
    std::ldexp(1.0f, -134), // half of it: the tie down to zero
    std::ldexp(3.0f, -134), // 1.5 ulp: the tie up to the even one
    std::ldexp(5.0f, -134), // 2.5 ulp: the same tie, rounding down
    1.0e-40f, // an fp32 subnormal of its own
    -1.0e-45f, // smaller than half a bf16 ulp: a signed zero
    -0.0f,
    std::numeric_limits<float>::infinity(),
    -std::numeric_limits<float>::infinity(),
    std::numeric_limits<float>::quiet_NaN()};

void runKernel(Device& device, ComputeProgram& kernel, int threads)
{
    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, threads);
    }

    commands.commit();
}

Vector<float> floatsOf(const Buffer& buffer)
{
    auto values = Vector<float>(buffer.size() / (int) sizeof(float));
    buffer.read(values.data(), buffer.size());
    return values;
}

// The same bytes read as the words they are, for the tests whose subject is a
// bit pattern rather than a value.
Vector<std::uint32_t> wordsOf(const Buffer& buffer)
{
    auto words = Vector<std::uint32_t>(buffer.size() / (int) sizeof(std::uint32_t));
    buffer.read(words.data(), buffer.size());
    return words;
}

Buffer storageOf(Device& device, const Vector<std::uint32_t>& words)
{
    return device.makeBuffer(words.data(),
                             words.size() * (int) sizeof(std::uint32_t),
                             BufferUsage::Storage);
}

bool contains(const std::string& text, const char* needle)
{
    return text.find(needle) != std::string::npos;
}
} // namespace

// What the encoding *is*, pinned against values rather than against the shift
// that implements it: a reference written as "the top sixteen bits" would agree
// with any widening that shifted, right or wrong.
auto tEncoding = test("PackedBFloat16/patternsAreTheTopHalfOfTheFloat") = []
{
    check(widened(0x3f80) == 1.0f);
    check(widened(0xc0a0) == -5.0f);
    check(widened(0x4000) == 2.0f);
    check(widened(0x0080) == std::ldexp(1.0f, -126));
    check(widened(0x0001) == std::ldexp(1.0f, -133));
    check(widened(0x7f7f) == std::ldexp(1.9921875f, 127));
    check(std::isinf(widened(0x7f80)));
    check(std::isnan(widened(0x7fc0)));

    // Every bf16 subnormal is an fp32 subnormal, which is what makes the load
    // path worth testing at all.
    check(widened(0x0001) > 0.0f);
    check(widened(0x0001) < std::numeric_limits<float>::min());
};

// The host pair is the same encoding as the shader helpers, which is what lets
// a loader pack a buffer the GPU will read back unchanged.
auto tHostHelpers = test("PackedBFloat16/hostHelpersAreTheSameEncoding") = []
{
    for (auto pattern: bfloat16Patterns)
    {
        check(matches(bfloat16ToFloat(pattern), widened(pattern)));
        check(bfloat16Matches(bfloat16FromFloat(widened(pattern)), pattern));
    }

    for (auto value: narrowingValues)
        check(bfloat16FromFloat(value) == narrowed(value));

    // A signalling NaN is quieted rather than turned into an infinity, which is
    // what rounding one would do.
    check(isBFloat16NaN(bfloat16FromFloat(assembled(0x7f800001u))));
};

auto tUnpack = test("PackedBFloat16/unpacksEveryBFloat16Class") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto words = everyPackedPair();
    auto count = words.size();

    auto input = storageOf(device, words);
    auto output = device.makeBuffer(count * 2 * (int) sizeof(float));

    auto kernel = UnpackKernel {};
    kernel.words = input;
    kernel.output = output;
    kernel.prepare(device);

    runKernel(device, kernel, count);
    auto result = floatsOf(output);

    for (auto i = 0; i < count; ++i)
    {
        auto low = (std::uint16_t) (words[i] & 0xffffu);
        auto high = (std::uint16_t) (words[i] >> 16);

        check(matches(result[i * 2], widened(low)));
        check(matches(result[i * 2 + 1], widened(high)));
    }
};

auto tReadBFloat16 = test("PackedBFloat16/readsEachElementByIndex") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto words = everyPackedPair();
    auto count = words.size();

    auto input = storageOf(device, words);
    auto output = device.makeBuffer(count * 2 * (int) sizeof(float));

    auto kernel = ReadBFloat16Kernel {};
    kernel.weights = input;
    kernel.output = output;
    kernel.prepare(device);

    runKernel(device, kernel, count * 2);
    auto result = floatsOf(output);

    for (auto i = 0; i < count; ++i)
    {
        auto low = (std::uint16_t) (words[i] & 0xffffu);
        auto high = (std::uint16_t) (words[i] >> 16);

        check(matches(result[i * 2], widened(low)));
        check(matches(result[i * 2 + 1], widened(high)));
    }
};

auto tReadLiteral = test("PackedBFloat16/readsElementsAtLiteralIndices") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto words = everyPackedPair();

    auto input = storageOf(device, words);
    auto output = device.makeBuffer(4 * (int) sizeof(float));

    auto kernel = LiteralBFloat16Kernel {};
    kernel.weights = input;
    kernel.output = output;
    kernel.prepare(device);

    runKernel(device, kernel, 1);
    auto result = floatsOf(output);

    for (auto i = 0; i < 4; ++i)
    {
        auto word = words[i / 2];
        auto element = (std::uint16_t) (i % 2 == 0 ? word & 0xffffu : word >> 16);

        check(matches(result[i], widened(element)));
    }
};

auto tReadPair = test("PackedBFloat16/readBFloat16x2ReadsBothHalvesOfAWord") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto words = everyPackedPair();
    auto count = words.size();

    auto input = storageOf(device, words);
    auto output = device.makeBuffer(count * 2 * (int) sizeof(float));

    auto kernel = ReadBFloat16x2Kernel {};
    kernel.weights = input;
    kernel.output = output;
    kernel.prepare(device);

    runKernel(device, kernel, count);
    auto result = floatsOf(output);

    for (auto i = 0; i < count; ++i)
    {
        check(matches(result[i * 2], widened((std::uint16_t) (words[i] & 0xffffu))));
        check(matches(result[i * 2 + 1], widened((std::uint16_t) (words[i] >> 16))));
    }
};

// Widening and narrowing back is exact for everything bf16 can hold, which is
// every pattern in the table: the fp32 in between has every bit of it and more,
// so nothing is rounded on either leg.
auto tRoundTrip = test("PackedBFloat16/packRoundTripsEveryPattern") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto words = everyPackedPair();
    auto count = words.size();

    auto input = storageOf(device, words);
    auto output = device.makeBuffer(count * (int) sizeof(std::uint32_t));

    auto kernel = RoundTripKernel {};
    kernel.weights = input;
    kernel.output = output;
    kernel.prepare(device);

    runKernel(device, kernel, count);
    auto result = wordsOf(output);

    for (auto i = 0; i < count; ++i)
    {
        check(bfloat16Matches((std::uint16_t) (result[i] & 0xffffu),
                              (std::uint16_t) (words[i] & 0xffffu)));

        check(bfloat16Matches((std::uint16_t) (result[i] >> 16),
                              (std::uint16_t) (words[i] >> 16)));
    }
};

// writeBFloat16x2 is the store the round trip spells out by hand, so the two
// kernels have to emit the same body and produce the same bytes.
auto tWritePair = test("PackedBFloat16/writeBFloat16x2IsThePackedStore") = []
{
    auto spelledOut = RoundTripKernel {};
    auto shorthand = WriteBFloat16x2Kernel {};

    check(spelledOut.source().source == shorthand.source().source);

    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto words = everyPackedPair();
    auto count = words.size();

    auto input = storageOf(device, words);
    auto output = device.makeBuffer(count * (int) sizeof(std::uint32_t));

    shorthand.weights = input;
    shorthand.output = output;
    shorthand.prepare(device);

    runKernel(device, shorthand, count);
    auto result = wordsOf(output);

    for (auto i = 0; i < count; ++i)
    {
        check(bfloat16Matches((std::uint16_t) (result[i] & 0xffffu),
                              (std::uint16_t) (words[i] & 0xffffu)));

        check(bfloat16Matches((std::uint16_t) (result[i] >> 16),
                              (std::uint16_t) (words[i] >> 16)));
    }
};

// The narrowing itself, against one reference rather than one per backend -
// which is the claim packHalf2 cannot make and this one can, the rounding being
// integer arithmetic the emitter writes out rather than an instruction each
// language defines its own way.
auto tBFloat16Narrowing = test("PackedBFloat16/narrowsToNearestEvenEverywhere") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    // The table has to actually contain a tie, or this would pass on data that
    // never makes a rounding decision at all.
    auto ties = 0;

    for (auto value: narrowingValues)
        if ((bitsOf(value) & 0xffffu) == 0x8000u)
            ++ties;

    check(ties > 0);

    auto count = narrowingValues.size() / 2;

    auto input = device.makeBuffer(narrowingValues.data(),
                                   narrowingValues.size() * (int) sizeof(float),
                                   BufferUsage::Storage);

    auto output = device.makeBuffer(count * (int) sizeof(std::uint32_t));

    auto kernel = NarrowKernel {};
    kernel.input = input;
    kernel.output = output;
    kernel.prepare(device);

    runKernel(device, kernel, count);
    auto result = wordsOf(output);

    for (auto i = 0; i < count; ++i)
    {
        check(bfloat16Matches((std::uint16_t) (result[i] & 0xffffu),
                              narrowed(narrowingValues[i * 2])));

        check(bfloat16Matches((std::uint16_t) (result[i] >> 16),
                              narrowed(narrowingValues[i * 2 + 1])));
    }
};

// The reason the bf16 family exists rather than the fp16 one serving: the
// magnitudes a checkpoint is full of are ones fp16 loses outright.
auto tRangeFloat16Lacks = test("PackedBFloat16/holdsWhatFloat16Cannot") = []
{
    auto small = bfloat16ToFloat(bfloat16FromFloat(1.0e-6f));
    auto large = bfloat16ToFloat(bfloat16FromFloat(1.0e30f));

    check(std::abs(small - 1.0e-6f) < 1.0e-8f);
    check(std::abs(large - 1.0e30f) < 1.0e28f);

    // What the same two values become through fp16, which is why a bf16
    // checkpoint may not be routed through readHalf: one loses most of its
    // mantissa to the subnormal range, the other is not a finite number at all.
    check(halfToFloat(halfFromFloat(1.0e-6f)) != small);
    check(std::isinf(halfToFloat(halfFromFloat(1.0e30f))));
};

// Both backends' source, generated on whichever host runs the suite - the
// Windows half being the one that cannot be executed here and the one whose
// spelling shares nothing with the other's.
auto tSourceIsRight = test("PackedBFloat16/bothBackendsSpellTheHelpers") = []
{
    auto builder = ShaderBuilder {};

    auto weights = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto i = builder.threadId();

    builder.write(output, i, weights.readBFloat16(i));
    builder.write(
        output, i + 1u, asFloat(packBFloat16x2(weights.readBFloat16x2(i))));

    auto metal = emitMetal(builder.graph());
    auto hlsl = emitHlsl(builder.graph());

    check(contains(metal, "float eacpReadBFloat16(uint bits, uint parity)"));
    check(contains(metal, "as_type<float>((bits >> (16u * parity)) << 16u)"));
    check(contains(metal, "inline float2 eacpUnpackBFloat16x2(uint bits)"));
    check(contains(metal, "inline uint eacpPackBFloat16x2(float2 values)"));
    check(contains(metal, "as_type<float>(bits << 16u)"));
    check(contains(metal, "as_type<uint>(values.x)"));

    check(contains(hlsl, "float eacpReadBFloat16(uint bits, uint parity)"));
    check(contains(hlsl, "asfloat((bits >> (16u * parity)) << 16u)"));
    check(contains(hlsl, "float2 eacpUnpackBFloat16x2(uint bits)"));
    check(contains(hlsl, "uint eacpPackBFloat16x2(float2 values)"));
    check(contains(hlsl, "asfloat(bits << 16u)"));
    check(contains(hlsl, "asuint(values.x)"));

    // The rounding, which is the same arithmetic in both because nothing
    // delegates it to the language.
    check(contains(metal, "low + 0x7fffu + ((low >> 16u) & 1u)"));
    check(contains(hlsl, "low + 0x7fffu + ((low >> 16u) & 1u)"));

    // Nothing in either goes near fp16, whose exponent is too short for bf16's
    // range, and the HLSL carries no MSL spelling anywhere.
    check(!contains(metal, "half2"));
    check(!contains(hlsl, "f16tof32"));
    check(!contains(hlsl, "f32tof16"));
    check(!contains(hlsl, "as_type"));
};

// Each helper is emitted only into shaders that call it, so a kernel narrowing
// nothing carries no packer and one reading whole words carries no reader.
auto tBFloat16HelpersAreNotAlwaysEmitted =
    test("PackedBFloat16/emitsEachHelperOnlyWhenUsed") = []
{
    auto plain = PlainKernel {};
    auto reading = ReadBFloat16Kernel {};
    auto unpacking = UnpackKernel {};
    auto packing = RoundTripKernel {};

    check(!contains(plain.source().source, "eacpReadBFloat16"));
    check(!contains(plain.source().source, "eacpUnpackBFloat16x2"));
    check(!contains(plain.source().source, "eacpPackBFloat16x2"));

    check(contains(reading.source().source, "eacpReadBFloat16"));
    check(!contains(reading.source().source, "eacpPackBFloat16x2"));

    check(contains(unpacking.source().source, "eacpUnpackBFloat16x2"));
    check(!contains(unpacking.source().source, "eacpPackBFloat16x2"));

    check(contains(packing.source().source, "eacpPackBFloat16x2"));
    check(!contains(packing.source().source, "eacpReadBFloat16"));

    // And the definition arrives before the body that calls it.
    const auto& source = unpacking.source().source;
    check(source.find("eacpUnpackBFloat16x2")
          < source.rfind("eacpUnpackBFloat16x2"));
};
