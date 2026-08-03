#include "Common.h"

#include <cmath>
#include <cstdint>
#include <cstring>

// asUInt and unpackHalf2: reading data out of a storage buffer that is not
// really float.
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

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
// One 32-bit word out of two fp16 bit patterns, low half first - the layout
// unpackHalf2 promises and the one a packer on the CPU has to match.
std::uint32_t packed(std::uint16_t low, std::uint16_t high)
{
    return (std::uint32_t) low | ((std::uint32_t) high << 16);
}

// The CPU reference: an fp16 bit pattern widened to float. Written out rather
// than taken from a library because it is the thing under test, and because
// the subnormal branch is where a widening usually goes wrong - its exponent
// is one less than the naive shift suggests.
float widened(std::uint16_t bits)
{
    auto sign = (std::uint32_t) (bits & 0x8000u) << 16;
    auto exponent = (std::uint32_t) (bits >> 10) & 0x1fu;
    auto mantissa = (std::uint32_t) bits & 0x3ffu;

    auto assemble = [](std::uint32_t word)
    {
        auto value = 0.0f;
        std::memcpy(&value, &word, sizeof(value));
        return value;
    };

    if (exponent == 0)
    {
        if (mantissa == 0)
            return assemble(sign);

        // Subnormal: normalise it by hand. The leading one is not stored, so
        // shift until it appears and take the exponent down for each step.
        auto shift = 0u;

        while ((mantissa & 0x400u) == 0)
        {
            mantissa <<= 1;
            ++shift;
        }

        mantissa &= 0x3ffu;
        auto exponent32 = 127u - 15u - shift + 1u;

        return assemble(sign | (exponent32 << 23) | (mantissa << 13));
    }

    if (exponent == 0x1fu)
        return assemble(sign | 0x7f800000u | (mantissa << 13));

    return assemble(sign | ((exponent + 127u - 15u) << 23) | (mantissa << 13));
}

// Every fp16 class, and deliberately paired so that several of the packed
// words are denormal floats: (0x0001, 0x0001) is 0x00010001, and 0x0000ffff
// and 0x00003c00 are denormals too. Those are the entries that catch a load
// path that does not preserve bits.
const auto halfPatterns = Array<std::uint16_t, 16> {
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
    0x3555, // ~0.3333
    0x1400, // a small normal
    0x9400 // its negation
};

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
} // namespace

auto tUnpackHalf2 = test("PackedHalf/unpacksEveryFloat16Class") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    // Each pattern paired with each other one, so the packed words cover both
    // halves of the word against every class - and cover the denormal-looking
    // words the load path has to carry through untouched.
    auto words = Vector<std::uint32_t> {};

    for (auto low: halfPatterns)
        for (auto high: halfPatterns)
            words.add(packed(low, high));

    auto count = words.size();

    auto input = device.makeBuffer(words.data(),
                                   (std::size_t) count * sizeof(std::uint32_t),
                                   BufferUsage::Storage);

    auto output = device.makeBuffer((std::size_t) count * 2 * sizeof(float));

    auto kernel = UnpackKernel {};
    kernel.words = input;
    kernel.output = output;
    kernel.prepare(device);

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, count);
    }

    commands.commit();

    auto result = Vector<float>(count * 2);
    output.read(result.data(), (std::size_t) result.size() * sizeof(float));

    for (auto i = 0; i < count; ++i)
    {
        auto low = (std::uint16_t) (words[i] & 0xffffu);
        auto high = (std::uint16_t) (words[i] >> 16);

        check(matches(result[i * 2], widened(low)));
        check(matches(result[i * 2 + 1], widened(high)));
    }
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
