#include "CpuCrossCheck.h"

#include <array>
#include <bit>
#include <cstdint>

// The byte and nibble reads: the int8 and int4 storage a block-quantized
// checkpoint ships in, read out of an ordinary float buffer and widened to the
// integer each element stands for.
//
// The bf16 family beside this one is a float narrowed; this one is not a float
// at all. A quantized weight is a small integer and a scale that belongs to the
// block it sits in, so what a read owes is the integer, exactly, and the scale
// is a multiply the kernel does. Three things have to hold:
//
//   1. Every byte value and every nibble value widens to exactly the integer
//      it encodes, at every position in the word. An int8 row is a run of
//      numbers, and one of them read as 44 instead of -84 is not noise, it is
//      a different weight.
//   2. The sign extension is the same on every backend. It is written as an
//      exclusive-or and a subtraction rather than a cast for that reason, and
//      the host helpers do the same arithmetic so a buffer packed on the CPU
//      reads back as what was put in it.
//   3. The two index conventions address one layout: element k of a buffer is
//      readInt8(k), and it is component k % 4 of readInt8x4(k / 4).
//
// Every kernel runs on the CPU executor too, against the same references: the
// layouts below are written out independently of the helpers' C++ twins.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::GPU::CrossChecks;

namespace
{
// The layouts, written out rather than called through int8x4FromBytes, because
// the host helpers are under test beside the shader and a reference that is the
// thing it checks proves nothing.
std::uint32_t packedBytes(int a, int b, int c, int d)
{
    return (std::uint32_t) (a & 0xFF) | ((std::uint32_t) (b & 0xFF) << 8)
           | ((std::uint32_t) (c & 0xFF) << 16) | ((std::uint32_t) (d & 0xFF) << 24);
}

std::uint32_t packedNibbles(const int* values)
{
    auto word = std::uint32_t {};

    for (auto i = 0; i < 8; ++i)
        word |= (std::uint32_t) (values[i] & 0xF) << (i * 4);

    return word;
}

// What a byte pattern means read as signed, and what a nibble does: two's
// complement over [-128, 127] and over [-8, 7].
int asSignedByte(int pattern)
{
    return pattern < 128 ? pattern : pattern - 256;
}

int asSignedNibble(int pattern)
{
    return pattern < 8 ? pattern : pattern - 16;
}

// And what one means read as unsigned, which is itself - named so a check over
// the unsigned reads is spelled the same way as the signed one beside it.
int asUnsignedValue(int pattern)
{
    return pattern;
}

// Every byte value at every position of a record `width` bytes wide, which is
// what catches a shift that is right for the first byte of a record and wrong
// for the last: position p of record r holds (r + (256 / width) * p) % 256, so
// each of the 256 * width (value, position) pairs appears exactly once across
// 256 records.
//
// Taking the width rather than fixing it at four is what lets the eight- and
// sixteen-byte reads be checked at every position they have, which is the whole
// question a wider read raises - a swizzle picking the wrong word of a vector
// load is right for the first four bytes and wrong after them.
Vector<int> everyBytePatternIn(int width)
{
    auto patterns = Vector<int> {};

    for (auto record = 0; record < 256; ++record)
        for (auto position = 0; position < width; ++position)
            patterns.add((record + (256 / width) * position) % 256);

    return patterns;
}

Vector<int> everyBytePattern()
{
    return everyBytePatternIn(4);
}

// The same for nibbles: sixteen records is enough for every value to land at
// every position, whatever the record's width.
Vector<int> everyNibblePatternIn(int width)
{
    auto patterns = Vector<int> {};

    for (auto record = 0; record < 16; ++record)
        for (auto position = 0; position < width; ++position)
            patterns.add((record + position) % 16);

    return patterns;
}

Vector<int> everyNibblePattern()
{
    return everyNibblePatternIn(8);
}

Vector<std::uint32_t> wordsOfBytes(const Vector<int>& patterns)
{
    auto words = Vector<std::uint32_t> {};

    for (auto i = 0; i < patterns.size(); i += 4)
        words.add(packedBytes(
            patterns[i], patterns[i + 1], patterns[i + 2], patterns[i + 3]));

    return words;
}

Vector<std::uint32_t> wordsOfNibbles(const Vector<int>& patterns)
{
    auto words = Vector<std::uint32_t> {};

    for (auto i = 0; i < patterns.size(); i += 8)
        words.add(packedNibbles(patterns.data() + i));

    return words;
}

// One element at a time out of a buffer whose elements are bytes, which is what
// a kernel walking a quantized row writes rather than doing the word and the
// byte arithmetic itself.
struct ReadInt8Kernel final : ComputeProgram
{
    ReadInt8Kernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, weights.readInt8(i));
    }

    Uniform<InputBuffer> weights;
    Uniform<OutputBuffer> output;

    EACP_SHADER(weights, output)
};

struct ReadUInt8Kernel final : ComputeProgram
{
    ReadUInt8Kernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, weights.readUInt8(i));
    }

    Uniform<InputBuffer> weights;
    Uniform<OutputBuffer> output;

    EACP_SHADER(weights, output)
};

// The literal-index overload, which a kernel reaching for a fixed element - a
// zero point, a block header - is what spells.
struct LiteralInt8Kernel final : ComputeProgram
{
    LiteralInt8Kernel() { compile(); }

    void define() override
    {
        auto i = threadId();

        for (auto element = 0u; element < 8u; ++element)
            write(output, i * 8u + element, weights.readInt8(element));
    }

    Uniform<InputBuffer> weights;
    Uniform<OutputBuffer> output;

    EACP_SHADER(weights, output)
};

// Four bytes at a time, which is one word: the width a quantized weight walk
// wants, and one load rather than four.
struct ReadInt8x4Kernel final : ComputeProgram
{
    ReadInt8x4Kernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, weights.readInt8x4(i));
    }

    Uniform<InputBuffer> weights;
    Uniform<OutputBuffer> output;

    EACP_SHADER(weights, output)
};

struct ReadUInt8x4Kernel final : ComputeProgram
{
    ReadUInt8x4Kernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, weights.readUInt8x4(i));
    }

    Uniform<InputBuffer> weights;
    Uniform<OutputBuffer> output;

    EACP_SHADER(weights, output)
};

// Eight nibbles out of one word, written back as the two records they arrive
// in, so the output is the eight elements in the order the word holds them.
struct ReadInt4x8Kernel final : ComputeProgram
{
    ReadInt4x8Kernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto nibbles = weights.readInt4x8(i);

        write(output, i * 2u, nibbles.low);
        write(output, i * 2u + 1u, nibbles.high);
    }

    Uniform<InputBuffer> weights;
    Uniform<OutputBuffer> output;

    EACP_SHADER(weights, output)
};

struct ReadUInt4x8Kernel final : ComputeProgram
{
    ReadUInt4x8Kernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto nibbles = weights.readUInt4x8(i);

        write(output, i * 2u, nibbles.low);
        write(output, i * 2u + 1u, nibbles.high);
    }

    Uniform<InputBuffer> weights;
    Uniform<OutputBuffer> output;

    EACP_SHADER(weights, output)
};

// Eight and sixteen bytes at a time, which is the width a quantized weight walk
// actually wants: the same eight or sixteen bytes a bf16 walk fetches in one
// load, holding twice as many weights. Four bytes at a time is correct and slow
// - it issues a load per four weights where a bf16 row issues one per four, so
// the kernel runs out of load slots long before it runs out of bandwidth.
struct ReadInt8x8Kernel final : ComputeProgram
{
    ReadInt8x8Kernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto bytes = weights.readInt8x8(i);

        write(output, i * 2u, bytes.low);
        write(output, i * 2u + 1u, bytes.high);
    }

    Uniform<InputBuffer> weights;
    Uniform<OutputBuffer> output;

    EACP_SHADER(weights, output)
};

struct ReadUInt8x8Kernel final : ComputeProgram
{
    ReadUInt8x8Kernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto bytes = weights.readUInt8x8(i);

        write(output, i * 2u, bytes.low);
        write(output, i * 2u + 1u, bytes.high);
    }

    Uniform<InputBuffer> weights;
    Uniform<OutputBuffer> output;

    EACP_SHADER(weights, output)
};

struct ReadInt8x16Kernel final : ComputeProgram
{
    ReadInt8x16Kernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto bytes = weights.readInt8x16(i);

        write(output, i * 4u, bytes.a);
        write(output, i * 4u + 1u, bytes.b);
        write(output, i * 4u + 2u, bytes.c);
        write(output, i * 4u + 3u, bytes.d);
    }

    Uniform<InputBuffer> weights;
    Uniform<OutputBuffer> output;

    EACP_SHADER(weights, output)
};

struct ReadUInt8x16Kernel final : ComputeProgram
{
    ReadUInt8x16Kernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto bytes = weights.readUInt8x16(i);

        write(output, i * 4u, bytes.a);
        write(output, i * 4u + 1u, bytes.b);
        write(output, i * 4u + 2u, bytes.c);
        write(output, i * 4u + 3u, bytes.d);
    }

    Uniform<InputBuffer> weights;
    Uniform<OutputBuffer> output;

    EACP_SHADER(weights, output)
};

// Sixteen nibbles, which is the eight bytes readInt8x8 fetches read four ways
// instead of two.
struct ReadInt4x16Kernel final : ComputeProgram
{
    ReadInt4x16Kernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto nibbles = weights.readInt4x16(i);

        write(output, i * 4u, nibbles.a);
        write(output, i * 4u + 1u, nibbles.b);
        write(output, i * 4u + 2u, nibbles.c);
        write(output, i * 4u + 3u, nibbles.d);
    }

    Uniform<InputBuffer> weights;
    Uniform<OutputBuffer> output;

    EACP_SHADER(weights, output)
};

struct ReadUInt4x16Kernel final : ComputeProgram
{
    ReadUInt4x16Kernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto nibbles = weights.readUInt4x16(i);

        write(output, i * 4u, nibbles.a);
        write(output, i * 4u + 1u, nibbles.b);
        write(output, i * 4u + 2u, nibbles.c);
        write(output, i * 4u + 3u, nibbles.d);
    }

    Uniform<InputBuffer> weights;
    Uniform<OutputBuffer> output;

    EACP_SHADER(weights, output)
};

// Widen a word and pack it straight back into the float slot it came out of -
// the whole int8-storage round trip in one line.
struct RoundTripInt8Kernel final : ComputeProgram
{
    RoundTripInt8Kernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, asFloat(packInt8x4(toInt(weights.readInt8x4(i)))));
    }

    Uniform<InputBuffer> weights;
    Uniform<OutputBuffer> output;

    EACP_SHADER(weights, output)
};

// The same thing said the short way, which is the only claim writeInt8x4
// makes.
struct WriteInt8x4Kernel final : ComputeProgram
{
    WriteInt8x4Kernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        writeInt8x4(output, i, toInt(weights.readInt8x4(i)));
    }

    Uniform<InputBuffer> weights;
    Uniform<OutputBuffer> output;

    EACP_SHADER(weights, output)
};

struct RoundTripUInt8Kernel final : ComputeProgram
{
    RoundTripUInt8Kernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, asFloat(packUInt8x4(toUInt(weights.readUInt8x4(i)))));
    }

    Uniform<InputBuffer> weights;
    Uniform<OutputBuffer> output;

    EACP_SHADER(weights, output)
};

struct WriteUInt8x4Kernel final : ComputeProgram
{
    WriteUInt8x4Kernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        writeUInt8x4(output, i, toUInt(weights.readUInt8x4(i)));
    }

    Uniform<InputBuffer> weights;
    Uniform<OutputBuffer> output;

    EACP_SHADER(weights, output)
};

// The wide round trips: eight and sixteen bytes read as one record and written
// back as one store, at the index each read counts in. The values go back as
// integer vectors rather than as the Float4Pair and Float4Quad they arrived in,
// because the rounding back down is the caller's decision on exactly the terms
// writeInt8x4 sets.
struct WriteInt8x8Kernel final : ComputeProgram
{
    WriteInt8x8Kernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto values = weights.readInt8x8(i);

        writeInt8x8(output, i, toInt(values.low), toInt(values.high));
    }

    Uniform<InputBuffer> weights;
    Uniform<OutputBuffer> output;

    EACP_SHADER(weights, output)
};

struct WriteUInt8x8Kernel final : ComputeProgram
{
    WriteUInt8x8Kernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto values = weights.readUInt8x8(i);

        writeUInt8x8(output, i, toUInt(values.low), toUInt(values.high));
    }

    Uniform<InputBuffer> weights;
    Uniform<OutputBuffer> output;

    EACP_SHADER(weights, output)
};

struct WriteInt8x16Kernel final : ComputeProgram
{
    WriteInt8x16Kernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto values = weights.readInt8x16(i);

        writeInt8x16(output,
                     i,
                     toInt(values.a),
                     toInt(values.b),
                     toInt(values.c),
                     toInt(values.d));
    }

    Uniform<InputBuffer> weights;
    Uniform<OutputBuffer> output;

    EACP_SHADER(weights, output)
};

struct WriteUInt8x16Kernel final : ComputeProgram
{
    WriteUInt8x16Kernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto values = weights.readUInt8x16(i);

        writeUInt8x16(output,
                      i,
                      toUInt(values.a),
                      toUInt(values.b),
                      toUInt(values.c),
                      toUInt(values.d));
    }

    Uniform<InputBuffer> weights;
    Uniform<OutputBuffer> output;

    EACP_SHADER(weights, output)
};

// The same eight and sixteen bytes as plain floats, which is what the wide
// reads have to cost: one record read of that width, whatever the backend
// spells that as.
struct PlainRead2Kernel final : ComputeProgram
{
    PlainRead2Kernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, weights.read2(i));
    }

    Uniform<InputBuffer> weights;
    Uniform<OutputBuffer> output;

    EACP_SHADER(weights, output)
};

struct PlainRead4Kernel final : ComputeProgram
{
    PlainRead4Kernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, weights.read4(i));
    }

    Uniform<InputBuffer> weights;
    Uniform<OutputBuffer> output;

    EACP_SHADER(weights, output)
};

// Ordinary arithmetic, so nothing pulls a helper in.
struct PlainQuantizedKernel final : ComputeProgram
{
    PlainQuantizedKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, input[i] * 2.0f);
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;

    EACP_SHADER(input, output)
};

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

using VerifyOutput = std::function<void(const Vector<float>&, const Readback&)>;

// Every kernel here reads `weights` and writes `output`: one CrossCheck over
// the packed words, `outputs` elements out, each backend's output handed on.
template <typename Kernel>
void runOnBoth(Kernel& kernel,
               const Vector<std::uint32_t>& words,
               int outputs,
               int threads,
               const VerifyOutput& verify)
{
    CrossCheck {kernel}
        .input(kernel.weights, asFloats(words))
        .output(kernel.output, outputs)
        .run(threads,
             [&](const Readback& readback)
             { verify(readback.floats(kernel.output), readback); });
}

// Each backend's output of one kernel, kept for comparing against another's.
struct Readings
{
    Vector<float> cpu;
    Vector<float> gpu;
};

template <typename Kernel>
Readings readOnBoth(const Vector<std::uint32_t>& words, int outputs, int threads)
{
    auto kernel = Kernel {};
    auto readings = Readings {};

    runOnBoth(kernel,
              words,
              outputs,
              threads,
              [&](const Vector<float>& result, const Readback& readback)
              {
                  auto& slot =
                      readback.backend == Backend::Cpu ? readings.cpu : readings.gpu;
                  slot = result;
              });

    return readings;
}

// Two kernels' readings agreeing element for element, each backend against
// itself.
void checkSameReadings(const Readings& a, const Readings& b)
{
    check(a.cpu.size() == b.cpu.size(), "cpu");
    check(a.gpu.size() == b.gpu.size(), "gpu");

    for (auto i = 0; i < a.cpu.size() && i < b.cpu.size(); ++i)
        check(a.cpu[i] == b.cpu[i], "cpu");

    for (auto i = 0; i < a.gpu.size() && i < b.gpu.size(); ++i)
        check(a.gpu[i] == b.gpu[i], "gpu");
}

// Every word of a packed store holding the word it was read from, to the bit.
void checkWordsKept(const Vector<float>& result,
                    const Vector<std::uint32_t>& words,
                    const char* name)
{
    for (auto i = 0; i < words.size(); ++i)
        check(wordOf(result[i]) == words[i], name);
}

// One wide read over a buffer holding every value at every position of its
// record: run it and check each widened float against what the pattern at that
// element says it stands for.
//
// Exactly, not within a tolerance - every value a byte or a nibble encodes is
// an integer a float holds outright, so any difference at all is a fault.
template <typename Kernel>
void checkWidensEveryPattern(const Vector<std::uint32_t>& words,
                             const Vector<int>& patterns,
                             int width,
                             int (*meaning)(int))
{
    auto kernel = Kernel {};

    runOnBoth(kernel,
              words,
              patterns.size(),
              patterns.size() / width,
              [&](const Vector<float>& result, const Readback& readback)
              {
                  for (auto i = 0; i < patterns.size(); ++i)
                      check(result[i] == (float) meaning(patterns[i]),
                            readback.name());
              });
}

bool contains(const std::string& text, const char* needle)
{
    return text.find(needle) != std::string::npos;
}

int countOccurrences(const std::string& text, const char* needle)
{
    const auto length = std::string_view(needle).size();
    auto count = 0;

    for (auto found = text.find(needle); found != std::string::npos;
         found = text.find(needle, found + length))
        ++count;

    return count;
}
} // namespace

// What the encoding is, pinned against values rather than against the shift
// that implements it.
auto tQuantizedEncoding = test("PackedQuantized/elementZeroIsInTheLowBits") = []
{
    check(packedBytes(1, 2, 3, 4) == 0x04030201u);
    check(packedBytes(0, 0, 0, 255) == 0xFF000000u);
    check(packedBytes(255, 0, 0, 0) == 0x000000FFu);

    const auto ascending = std::array<int, 8> {0, 1, 2, 3, 4, 5, 6, 7};
    check(packedNibbles(ascending.data()) == 0x76543210u);

    check(asSignedByte(0) == 0);
    check(asSignedByte(127) == 127);
    check(asSignedByte(128) == -128);
    check(asSignedByte(255) == -1);

    check(asSignedNibble(0) == 0);
    check(asSignedNibble(7) == 7);
    check(asSignedNibble(8) == -8);
    check(asSignedNibble(15) == -1);
};

// The host pair is the same layout the shader reads, which is what lets a
// loader pack a buffer the GPU reads back unchanged, and the same two's
// complement in both directions.
auto tQuantizedHostHelpers = test("PackedQuantized/hostHelpersAreTheSameLayout") = []
{
    for (auto pattern = 0; pattern < 256; ++pattern)
    {
        for (auto position = 0; position < 4; ++position)
        {
            auto bytes = std::array<std::int8_t, 4> {};
            auto unsignedBytes = std::array<std::uint8_t, 4> {};

            bytes[(std::size_t) position] = (std::int8_t) asSignedByte(pattern);
            unsignedBytes[(std::size_t) position] = (std::uint8_t) pattern;

            auto expected = std::array<int, 4> {};
            expected[(std::size_t) position] = pattern;

            const auto word =
                packedBytes(expected[0], expected[1], expected[2], expected[3]);

            check(int8x4FromBytes(bytes) == word);
            check(uint8x4FromBytes(unsignedBytes) == word);
            check(int8x4ToByte(word, position) == asSignedByte(pattern));
            check(uint8x4ToByte(word, position) == pattern);
        }
    }

    for (auto pattern = 0; pattern < 16; ++pattern)
    {
        for (auto position = 0; position < 8; ++position)
        {
            auto nibbles = std::array<std::int8_t, 8> {};
            auto unsignedNibbles = std::array<std::uint8_t, 8> {};

            nibbles[(std::size_t) position] = (std::int8_t) asSignedNibble(pattern);
            unsignedNibbles[(std::size_t) position] = (std::uint8_t) pattern;

            auto expected = std::array<int, 8> {};
            expected[(std::size_t) position] = pattern;

            const auto word = packedNibbles(expected.data());

            check(int4x8FromNibbles(nibbles) == word);
            check(uint4x8FromNibbles(unsignedNibbles) == word);
            check(int4x8ToNibble(word, position) == asSignedNibble(pattern));
            check(uint4x8ToNibble(word, position) == pattern);
        }
    }
};

// Exactly, not within a tolerance: every value a byte encodes is an integer a
// float holds outright, so any difference at all is a fault.
auto tReadInt8 = test("PackedQuantized/readsEverySignedByteByIndex") = []
{
    auto patterns = everyBytePattern();

    checkWidensEveryPattern<ReadInt8Kernel>(
        wordsOfBytes(patterns), patterns, 1, asSignedByte);
};

auto tReadUInt8 = test("PackedQuantized/readsEveryUnsignedByteByIndex") = []
{
    auto patterns = everyBytePattern();

    checkWidensEveryPattern<ReadUInt8Kernel>(
        wordsOfBytes(patterns), patterns, 1, asUnsignedValue);
};

auto tQuantizedReadLiteral = test("PackedQuantized/readsBytesAtLiteralIndices") = []
{
    auto patterns = everyBytePattern();

    auto kernel = LiteralInt8Kernel {};

    runOnBoth(kernel,
              wordsOfBytes(patterns),
              8,
              1,
              [&](const Vector<float>& result, const Readback& readback)
              {
                  for (auto i = 0; i < 8; ++i)
                      check(result[i] == (float) asSignedByte(patterns[i]),
                            readback.name());
              });
};

// Component by component this is the same walk readInt8 makes, which is what
// says the two index conventions address one layout.
auto tReadInt8x4 = test("PackedQuantized/readInt8x4ReadsAllFourOfAWord") = []
{
    auto patterns = everyBytePattern();

    checkWidensEveryPattern<ReadInt8x4Kernel>(
        wordsOfBytes(patterns), patterns, 4, asSignedByte);
};

auto tReadUInt8x4 = test("PackedQuantized/readUInt8x4ReadsAllFourOfAWord") = []
{
    auto patterns = everyBytePattern();

    checkWidensEveryPattern<ReadUInt8x4Kernel>(
        wordsOfBytes(patterns), patterns, 4, asUnsignedValue);
};

// The low four then the high four, which is nibble 0 through nibble 7 of the
// word in order.
auto tReadInt4x8 = test("PackedQuantized/readInt4x8ReadsEightNibbles") = []
{
    auto patterns = everyNibblePattern();

    checkWidensEveryPattern<ReadInt4x8Kernel>(
        wordsOfNibbles(patterns), patterns, 8, asSignedNibble);
};

auto tReadUInt4x8 = test("PackedQuantized/readUInt4x8ReadsEightNibbles") = []
{
    auto patterns = everyNibblePattern();

    checkWidensEveryPattern<ReadUInt4x8Kernel>(
        wordsOfNibbles(patterns), patterns, 8, asUnsignedValue);
};

// The wide reads, over buffers holding every byte value at every one of the
// eight or sixteen positions of a record. A four-wide read has four places a
// shift can be wrong in; a sixteen-wide read has sixteen, and the four extra
// ones a swizzle of the vector load can be wrong in on top of that, which is
// exactly what a narrower fixture would miss.
auto tReadInt8x8 = test("PackedQuantized/readInt8x8ReadsEightAcrossTwoWords") = []
{
    auto patterns = everyBytePatternIn(8);

    checkWidensEveryPattern<ReadInt8x8Kernel>(
        wordsOfBytes(patterns), patterns, 8, asSignedByte);
};

auto tReadUInt8x8 = test("PackedQuantized/readUInt8x8ReadsEightAcrossTwoWords") = []
{
    auto patterns = everyBytePatternIn(8);

    checkWidensEveryPattern<ReadUInt8x8Kernel>(
        wordsOfBytes(patterns), patterns, 8, asUnsignedValue);
};

auto tReadInt8x16 =
    test("PackedQuantized/readInt8x16ReadsSixteenAcrossFourWords") = []
{
    auto patterns = everyBytePatternIn(16);

    checkWidensEveryPattern<ReadInt8x16Kernel>(
        wordsOfBytes(patterns), patterns, 16, asSignedByte);
};

auto tReadUInt8x16 =
    test("PackedQuantized/readUInt8x16ReadsSixteenAcrossFourWords") = []
{
    auto patterns = everyBytePatternIn(16);

    checkWidensEveryPattern<ReadUInt8x16Kernel>(
        wordsOfBytes(patterns), patterns, 16, asUnsignedValue);
};

auto tReadInt4x16 = test("PackedQuantized/readInt4x16ReadsSixteenNibbles") = []
{
    auto patterns = everyNibblePatternIn(16);

    checkWidensEveryPattern<ReadInt4x16Kernel>(
        wordsOfNibbles(patterns), patterns, 16, asSignedNibble);
};

auto tReadUInt4x16 = test("PackedQuantized/readUInt4x16ReadsSixteenNibbles") = []
{
    auto patterns = everyNibblePatternIn(16);

    checkWidensEveryPattern<ReadUInt4x16Kernel>(
        wordsOfNibbles(patterns), patterns, 16, asUnsignedValue);
};

// One layout, three widths of read over it: element 16k + j of a byte buffer is
// component j of readInt8x16(k) and component j % 8 of readInt8x8(2k + j / 8).
//
// Said by reading the same buffer three ways and comparing the results, rather
// than by checking each against the same table: a table that is wrong about
// what the bytes mean would let all three pass, where this only passes if they
// agree with each other, and the tests above already pin what they agree on.
auto tWideReadsAgreeWithTheByteIndex =
    test("PackedQuantized/theWideReadsAgreeWithTheByteIndex") = []
{
    auto patterns = everyBytePatternIn(16);
    auto words = wordsOfBytes(patterns);
    auto count = patterns.size();

    auto scalar = readOnBoth<ReadInt8Kernel>(words, count, count);
    auto pairs = readOnBoth<ReadInt8x8Kernel>(words, count, count / 8);
    auto quads = readOnBoth<ReadInt8x16Kernel>(words, count, count / 16);

    checkSameReadings(pairs, scalar);
    checkSameReadings(quads, scalar);
};

// And the same for nibbles: sixteen of them are two of readInt4x8's words, in
// that order.
auto tWideNibbleReadsAgree =
    test("PackedQuantized/theWideNibbleReadAgreesWithTheNarrowOne") = []
{
    auto patterns = everyNibblePatternIn(16);
    auto words = wordsOfNibbles(patterns);
    auto count = patterns.size();

    auto narrow = readOnBoth<ReadInt4x8Kernel>(words, count, words.size());
    auto wide = readOnBoth<ReadInt4x16Kernel>(words, count, count / 16);

    checkSameReadings(wide, narrow);
};

// What the wide reads are for, on the real ComputeProgram path: sixteen bytes
// reach the kernel through the one record read sixteen bytes of floats take,
// and no more.
//
// Correctness would survive four separate reads of the same address - the
// reason to store weights as bytes would not. A kernel issuing a load per four
// weights issues as many as a bf16 kernel does for twice the data and becomes
// bound by how fast it can ask, which is the whole point of the widths.
//
// Counted against a read4 of plain floats rather than against a Metal pointer
// cast, so the assertion is the same one on whichever dialect the machine
// running the suite emits.
auto tWideReadCostsOneRecordRead =
    test("PackedQuantized/aWideReadCostsOneRecordRead") = []
{
    auto twoFloats = PlainRead2Kernel {};
    auto fourFloats = PlainRead4Kernel {};
    auto pair = ReadInt8x8Kernel {};
    auto quad = ReadInt8x16Kernel {};
    auto nibbles = ReadInt4x16Kernel {};

    auto accesses = [](const ComputeProgram& kernel)
    { return countOccurrences(kernel.source().source, "buffer0"); };

    check(accesses(quad) == accesses(fourFloats));
    check(accesses(pair) == accesses(twoFloats));

    // Sixteen nibbles are eight bytes, so they cost the narrower read and
    // twice as many widenings of what it brought back.
    check(accesses(nibbles) == accesses(twoFloats));

    // One definition and four calls each: every word of the record widened
    // once, off the one value the load landed in.
    check(countOccurrences(quad.source().source, "eacpUnpackInt8x4(") == 5);
    check(countOccurrences(nibbles.source().source, "eacpUnpackInt4x4(") == 5);
};

// Widening and packing back is exact for every byte there is: the integer in
// between holds all eight bits and nothing is rounded on either leg.
auto tQuantizedRoundTrip = test("PackedQuantized/packRoundTripsEveryByte") = []
{
    auto patterns = everyBytePattern();
    auto words = wordsOfBytes(patterns);
    auto count = words.size();

    auto keepsTheWords = [&](const Vector<float>& result, const Readback& readback)
    { checkWordsKept(result, words, readback.name()); };

    auto signedKernel = RoundTripInt8Kernel {};
    runOnBoth(signedKernel, words, count, count, keepsTheWords);

    auto unsignedKernel = RoundTripUInt8Kernel {};
    runOnBoth(unsignedKernel, words, count, count, keepsTheWords);
};

// writeInt8x4 is the store the round trip spells out by hand, so the two
// kernels have to emit the same body and produce the same bytes.
auto tWriteIsTheStore = test("PackedQuantized/writeInt8x4IsThePackedStore") = []
{
    auto spelledOut = RoundTripInt8Kernel {};
    auto shorthand = WriteInt8x4Kernel {};

    check(spelledOut.source().source == shorthand.source().source);

    auto spelledOutUnsigned = RoundTripUInt8Kernel {};
    auto shorthandUnsigned = WriteUInt8x4Kernel {};

    check(spelledOutUnsigned.source().source == shorthandUnsigned.source().source);

    auto patterns = everyBytePattern();
    auto words = wordsOfBytes(patterns);
    auto count = words.size();

    runOnBoth(shorthand,
              words,
              count,
              count,
              [&](const Vector<float>& result, const Readback& readback)
              { checkWordsKept(result, words, readback.name()); });
};

// The wide stores, each against the read it mirrors: a run of bytes read as one
// record and written back as one store has to leave the buffer holding the
// bytes it started with, to the bit. The unsigned twin runs beside each,
// because the widening they undo is the half of this that differs.
auto tWideByteStores = test("PackedQuantized/theWideStoresAreTheWideReads") = []
{
    auto patterns = everyBytePattern();
    auto words = wordsOfBytes(patterns);

    // Sixteen bytes is four words, so the widest store here addresses records
    // of four and a partial one at the end would be a record nothing writes.
    while (words.size() % 4 != 0)
        words.add(0u);

    auto count = words.size();

    auto keepsTheWords = [&](const Vector<float>& result, const Readback& readback)
    { checkWordsKept(result, words, readback.name()); };

    auto signedPair = WriteInt8x8Kernel {};
    runOnBoth(signedPair, words, count, count / 2, keepsTheWords);

    auto unsignedPair = WriteUInt8x8Kernel {};
    runOnBoth(unsignedPair, words, count, count / 2, keepsTheWords);

    auto signedQuad = WriteInt8x16Kernel {};
    runOnBoth(signedQuad, words, count, count / 4, keepsTheWords);

    auto unsignedQuad = WriteUInt8x16Kernel {};
    runOnBoth(unsignedQuad, words, count, count / 4, keepsTheWords);
};

// A buffer packed by the host helpers, read back by the shader: the two sides
// of the layout, checked against each other rather than each against itself.
auto tHostPackedBufferReadsBack =
    test("PackedQuantized/aHostPackedBufferReadsBackExactly") = []
{
    auto patterns = everyBytePattern();
    auto words = Vector<std::uint32_t> {};

    for (auto i = 0; i < patterns.size(); i += 4)
        words.add(int8x4FromBytes({(std::int8_t) asSignedByte(patterns[i]),
                                   (std::int8_t) asSignedByte(patterns[i + 1]),
                                   (std::int8_t) asSignedByte(patterns[i + 2]),
                                   (std::int8_t) asSignedByte(patterns[i + 3])}));

    checkWidensEveryPattern<ReadInt8Kernel>(words, patterns, 1, asSignedByte);

    auto nibblePatterns = everyNibblePattern();
    auto nibbleWords = Vector<std::uint32_t> {};

    for (auto i = 0; i < nibblePatterns.size(); i += 8)
    {
        auto nibbles = std::array<std::int8_t, 8> {};

        for (auto n = std::size_t {}; n < nibbles.size(); ++n)
            nibbles[n] = (std::int8_t) asSignedNibble(nibblePatterns[i + (int) n]);

        nibbleWords.add(int4x8FromNibbles(nibbles));
    }

    checkWidensEveryPattern<ReadInt4x8Kernel>(
        nibbleWords, nibblePatterns, 8, asSignedNibble);
};

// Both backends' source, generated on whichever host runs the suite - the
// Windows half being the one that cannot be executed here.
auto tQuantizedSourceIsRight =
    test("PackedQuantized/bothBackendsSpellTheHelpers") = []
{
    auto builder = ShaderBuilder {};

    auto weights = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto i = builder.threadId();

    auto nibbles = weights.readInt4x8(i);

    builder.write(output, i, weights.readInt8(i));
    builder.write(output, i + 1u, weights.readUInt8(i));
    builder.write(output, i * 4u + 4u, weights.readInt8x4(i));
    builder.write(output, i * 4u + 5u, nibbles.low + nibbles.high);
    builder.writeInt8x4(output, i + 6u, toInt(weights.readInt8x4(i)));

    auto metal = emitMetal(builder.graph());
    auto hlsl = emitHlsl(builder.graph());

    for (const auto& source: {metal, hlsl})
    {
        check(contains(source, "float eacpReadInt8(uint bits, uint byteIndex)"));
        check(contains(source, "float eacpReadUInt8(uint bits, uint byteIndex)"));
        check(contains(source, "float4 eacpUnpackInt8x4(uint bits)"));
        check(contains(source, "float4 eacpUnpackInt4x4(uint bits)"));
        check(contains(source, "uint eacpPackInt8x4(int4 values)"));

        // The sign extension, which is the same arithmetic in both because
        // nothing delegates it to the language.
        check(contains(source, "return float(value ^ 0x80u) - 128.0;"));
        check(contains(source, "float((bits & 0xffu) ^ 0x80u) - 128.0"));
        check(contains(source, "float((bits & 0xfu) ^ 0x8u) - 8.0"));

        // And no dialect's own cast anywhere near it.
        check(!contains(source, "char4"));
        check(!contains(source, "int8_t"));
    }

    check(contains(metal, "as_type<uint>("));
    check(contains(hlsl, "asuint("));
    check(!contains(hlsl, "as_type"));
};

// Each helper is emitted only into shaders that call it, so a kernel reading
// bytes carries no nibble helper and one that quantizes nothing carries none.
auto tQuantizedHelpersAreNotAlwaysEmitted =
    test("PackedQuantized/emitsEachHelperOnlyWhenUsed") = []
{
    auto plain = PlainQuantizedKernel {};
    auto readingBytes = ReadInt8Kernel {};
    auto readingNibbles = ReadInt4x8Kernel {};
    auto packing = RoundTripInt8Kernel {};

    check(!contains(plain.source().source, "eacpReadInt8"));
    check(!contains(plain.source().source, "eacpUnpackInt8x4"));
    check(!contains(plain.source().source, "eacpUnpackInt4x4"));
    check(!contains(plain.source().source, "eacpPackInt8x4"));

    check(contains(readingBytes.source().source, "eacpReadInt8"));
    check(!contains(readingBytes.source().source, "eacpUnpackInt4x4"));
    check(!contains(readingBytes.source().source, "eacpPackInt8x4"));

    check(contains(readingNibbles.source().source, "eacpUnpackInt4x4"));
    check(!contains(readingNibbles.source().source, "eacpUnpackUInt4x4"));
    check(!contains(readingNibbles.source().source, "eacpReadInt8"));

    check(contains(packing.source().source, "eacpPackInt8x4"));
    check(contains(packing.source().source, "eacpUnpackInt8x4"));
    check(!contains(packing.source().source, "eacpPackUInt8x4"));

    // And the definition arrives before the body that calls it.
    const auto& source = readingNibbles.source().source;
    check(source.find("eacpUnpackInt4x4") < source.rfind("eacpUnpackInt4x4"));
};
