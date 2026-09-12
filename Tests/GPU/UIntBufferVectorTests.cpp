#include "Common.h"

#include <eacp/GPU/Codegen/ShaderEmitter.h>

#include <cstdint>
#include <string>

// Records of unsigned integers: a uint buffer read and written N elements at a
// time, the index counting records the way it does on the float buffers, so a
// kernel over a pair of indices or the four lanes of a hash never spells the
// stride itself.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto uintBytes = (int) sizeof(std::uint32_t);

bool contains(const std::string& text, const char* needle)
{
    return text.find(needle) != std::string::npos;
}

int occurrences(const std::string& text, const std::string& needle)
{
    auto found = 0;

    for (auto at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + needle.size()))
        ++found;

    return found;
}

Buffer makeUInts(const Vector<std::uint32_t>& values)
{
    return Buffer {Device::shared(),
                   values.data(),
                   uintBytes * values.size(),
                   BufferUsage::Storage};
}

Buffer makeFilledUInts(int elements, std::uint32_t value)
{
    auto values = Vector<std::uint32_t> {};
    values.assign(elements, value);
    return makeUInts(values);
}

Vector<std::uint32_t> readUInts(const Buffer& buffer, int elements)
{
    auto values = Vector<std::uint32_t> {};
    values.resize(elements);
    buffer.read(values.data(), uintBytes * elements);
    return values;
}

// Values a float cannot hold: past the mantissa, and up against 2^32.
std::uint32_t sourceValue(int element)
{
    return 2654435761u * (std::uint32_t) element + 4026531840u;
}

// Three records of two out of every pair read: the swap, the sum and the mask.
constexpr auto pairsPerThread = 3;

struct PairKernel final : ComputeProgram
{
    PairKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto pair = input.read2(i);
        auto swapped = uint2(pair.y(), pair.x());
        auto record = i * (unsigned) pairsPerThread;

        write(output, record + 0u, swapped);
        write(output, record + 1u, pair + swapped);
        write(output, record + 2u, pair & 65535u);
    }

    Uniform<UIntInputBuffer> input;
    Uniform<UIntOutputBuffer> output;

    EACP_SHADER(input, output)
};

// Two records of four out of every quad: a sum that wraps at 2^32, and the
// halves of two lanes shifted into one.
constexpr auto quadsPerThread = 2;

struct QuadKernel final : ComputeProgram
{
    QuadKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto quad = input.read4(i);
        auto rotated = uint4(quad.y(), quad.z(), quad.w(), quad.x());
        auto record = i * (unsigned) quadsPerThread;

        write(output, record + 0u, quad + rotated);
        write(output, record + 1u, (quad >> 16u) | (rotated << 16u));
    }

    Uniform<UIntInputBuffer> input;
    Uniform<UIntOutputBuffer> output;

    EACP_SHADER(input, output)
};

// A record stored and read back in the same thread. Every component store has
// to stale the slot, or the read hands back what the buffer held on entry.
struct RecordRereadKernel final : ComputeProgram
{
    RecordRereadKernel() { compile(); }

    void define() override
    {
        auto i = threadId();

        write(output, i, uint4(i * 3u, i + 1000000u, 4294967295u - i, i * 65537u));

        auto back = output.read4(i);
        write(sums, i, back.x() + back.y() + back.z() + back.w());
    }

    Uniform<UIntOutputBuffer> output;
    Uniform<UIntOutputBuffer> sums;

    EACP_SHADER(output, sums)
};

struct ScalePairKernel final : ComputeProgram
{
    ScalePairKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, input.read2(i) * 10u);
    }

    Uniform<UIntInputBuffer> input;
    Uniform<UIntOutputBuffer> output;

    EACP_SHADER(input, output)
};
} // namespace

// A pair read off a uint input and a pair stored into a uint output: two loads,
// two stores, one index expression in records, and uint elements throughout.
auto tUIntPairsEmitTheirOwnStride =
    test("UIntBufferVector/aPairIsTwoLoadsAndTwoStores") = []
{
    auto builder = ShaderBuilder {};

    auto input = builder.uintInputBuffer();
    auto output = builder.uintOutputBuffer();
    auto i = builder.threadId();

    builder.write(output, i, input.read2(i) + 1u);

    auto metal = emitMetal(builder.graph());
    auto hlsl = emitHlsl(builder.graph());

    for (const auto& text: {metal, hlsl})
    {
        check(contains(text, "uint t0 = (gid * 2u);"));
        check(contains(text, "buffer1[t0] = (t1).x;"));
        check(contains(text, "buffer1[(t0 + 1u)] = (t1).y;"));
        check(!contains(text, "t0 + 2u"));
    }

    // The read is one eight-byte load on Metal and the pair of subscripts it
    // stands in for on HLSL, which has no spelling for reinterpreting a
    // StructuredBuffer<uint>.
    check(contains(
        metal,
        "uint2 t1 = (uint2(*((device const packed_uint2*) (buffer0 + t0))) + 1u);"));
    check(contains(hlsl, "uint2 t1 = (uint2(buffer0[t0], buffer0[t0 + 1u]) + 1u);"));

    check(contains(metal, "device const uint* buffer0"));
    check(contains(metal, "device uint* buffer1"));
    check(contains(hlsl, "StructuredBuffer<uint> buffer0 : register(t0)"));
    check(contains(hlsl, "RWStructuredBuffer<uint> buffer1 : register(u1)"));

    expectGlslCompiles(builder.graph());
};

// Four read back off a uint output and three stored into another: the widths
// stride in their own units, and a read of an output is the same subscript a
// store is.
auto tUIntQuadsAndTriplesStrideInRecords =
    test("UIntBufferVector/theWidthsStrideInTheirOwnRecords") = []
{
    auto builder = ShaderBuilder {};

    auto output = builder.uintOutputBuffer();
    auto probe = builder.uintOutputBuffer();
    auto i = builder.threadId();

    auto record = output.read4(i);
    builder.write(probe, i, uint3(record.x(), record.z(), record.w()));

    for (const auto& text: {emitMetal(builder.graph()), emitHlsl(builder.graph())})
    {
        check(contains(text, "uint t0 = (gid * 3u);"));
        check(contains(text, "uint t1 = (gid * 4u);"));
        check(contains(text,
                       "uint4 t2 = uint4(buffer0[t1], buffer0[(t1 + 1u)], "
                       "buffer0[(t1 + 2u)], buffer0[(t1 + 3u)]);"));
        check(contains(text, "uint3 t3 = uint3((t2).x, (t2).z, (t2).w);"));
        check(contains(text, "buffer1[t0] = (t3).x;"));
        check(contains(text, "buffer1[(t0 + 1u)] = (t3).y;"));
        check(contains(text, "buffer1[(t0 + 2u)] = (t3).z;"));
        check(!contains(text, "t0 + 3u"));
    }

    expectGlslCompiles(builder.graph());
};

// A record store retires the name a read of the same slot was hoisted under,
// and the re-read lands after the last of its component stores.
auto tARecordStoreStalesTheSlot =
    test("UIntBufferVector/aRecordStoreStalesTheSlot") = []
{
    auto builder = ShaderBuilder {};

    auto output = builder.uintOutputBuffer();
    auto probe = builder.uintOutputBuffer();
    auto i = builder.threadId();

    auto before = output.read2(i);
    builder.write(probe, 0u, before.x() + before.y());

    builder.write(output, i, uint2(i, 9u));

    auto after = output.read2(i);
    builder.write(probe, 1u, after.x() + after.y());

    for (const auto& text: {emitMetal(builder.graph()), emitHlsl(builder.graph())})
    {
        check(occurrences(text, "= uint2(buffer0[") == 2);

        auto lastComponent = text.rfind("buffer0[t1] = ");
        auto reread = text.rfind("= uint2(buffer0[");

        check(lastComponent != std::string::npos);
        check(reread != std::string::npos);
        check(reread > lastComponent);
    }

    expectGlslCompiles(builder.graph());
};

// Records of two, run through Metal: a swap, a sum and a mask on the UInt2,
// stored back as records and read out exact.
auto tPairsComputeAndStoreExactly =
    test("UIntBufferVector/pairsRoundTripThroughAKernel") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto threads = 256;
    constexpr auto inputElements = threads * 2;
    constexpr auto outputElements = threads * pairsPerThread * 2;

    auto source = Vector<std::uint32_t> {};

    for (auto element = 0; element < inputElements; ++element)
        source.add(sourceValue(element));

    auto input = makeUInts(source);
    auto output = makeFilledUInts(outputElements, 0u);

    auto kernel = PairKernel {};
    kernel.input = input;
    kernel.output = output;
    kernel.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, threads);
    }

    commands.commit();

    auto values = readUInts(output, outputElements);

    for (auto thread = 0; thread < threads; ++thread)
    {
        auto x = source[thread * 2];
        auto y = source[thread * 2 + 1];
        auto at = thread * pairsPerThread * 2;

        check(values[at + 0] == y);
        check(values[at + 1] == x);
        check(values[at + 2] == x + y);
        check(values[at + 3] == y + x);
        check(values[at + 4] == (x & 65535u));
        check(values[at + 5] == (y & 65535u));
    }
};

// Records of four, over values past 2^24 and up against 2^32: a sum that wraps
// and a pair of shifts, exact in every lane.
auto tQuadsKeepEveryBit = test("UIntBufferVector/quadsKeepBitsAFloatLoses") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto threads = 128;
    constexpr auto inputElements = threads * 4;
    constexpr auto outputElements = threads * quadsPerThread * 4;

    auto source = Vector<std::uint32_t> {};

    for (auto element = 0; element < inputElements; ++element)
        source.add(sourceValue(element));

    auto input = makeUInts(source);
    auto output = makeFilledUInts(outputElements, 0u);

    auto kernel = QuadKernel {};
    kernel.input = input;
    kernel.output = output;
    kernel.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, threads);
    }

    commands.commit();

    auto values = readUInts(output, outputElements);
    auto beyondAFloat = 0;

    for (auto thread = 0; thread < threads; ++thread)
    {
        std::uint32_t quad[4];
        std::uint32_t rotated[4];

        for (auto lane = 0; lane < 4; ++lane)
            quad[lane] = source[thread * 4 + lane];

        for (auto lane = 0; lane < 4; ++lane)
            rotated[lane] = quad[(lane + 1) % 4];

        auto at = thread * quadsPerThread * 4;

        for (auto lane = 0; lane < 4; ++lane)
        {
            check(values[at + lane] == (std::uint32_t) (quad[lane] + rotated[lane]));
            check(values[at + 4 + lane]
                  == ((quad[lane] >> 16) | (rotated[lane] << 16)));

            if ((std::uint32_t) (float) quad[lane] != quad[lane])
                ++beyondAFloat;
        }
    }

    check(beyondAFloat > 0);
};

// A record written and read back inside one thread, run through Metal: the sum
// is of what the stores put there, not of what the buffer was bound holding.
auto tARecordReadsBackWhatItStored =
    test("UIntBufferVector/aRecordReadsBackWhatItStored") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto threads = 64;

    auto output = makeFilledUInts(threads * 4, 7u);
    auto sums = makeFilledUInts(threads, 0u);

    auto kernel = RecordRereadKernel {};
    kernel.output = output;
    kernel.sums = sums;
    kernel.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, threads);
    }

    commands.commit();

    auto stored = readUInts(output, threads * 4);
    auto totals = readUInts(sums, threads);

    for (auto thread = 0; thread < threads; ++thread)
    {
        auto i = (std::uint32_t) thread;
        const std::uint32_t record[] = {
            i * 3u, i + 1000000u, 4294967295u - i, i * 65537u};

        auto total = std::uint32_t {0};

        for (auto lane = 0; lane < 4; ++lane)
        {
            check(stored[thread * 4 + lane] == record[lane]);
            total += record[lane];
        }

        check(totals[thread] == total);
    }
};

// Records through ranges bound part-way in: record zero of the kernel's buffer
// is the record at the range's offset, on the input side and on the output
// side.
//
// Both offsets are whole multiples of the device's storage-buffer alignment,
// which is the grid a bind takes - one uint on Metal and D3D12, four of them on
// lavapipe - so each offset is non-zero and bindable on every backend.
auto tRecordsReadThroughARangeStartAtItsOffset =
    test("UIntBufferVector/aRangeStartsAtItsOwnRecord") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    const auto row = device.storageBufferOffsetAlignment() / uintBytes;
    const auto rowElement = row;
    const auto firstElement = 2 * row;
    const auto records = 4;
    const auto span = records * 2;
    const auto capacity = firstElement + span + row;

    auto source = Vector<std::uint32_t> {};

    for (auto element = 0; element < capacity; ++element)
        source.add(sourceValue(element));

    auto input = makeUInts(source);
    auto output = makeFilledUInts(capacity, 0u);

    auto kernel = ScalePairKernel {};
    kernel.input = BufferRange {&input, firstElement * uintBytes, span * uintBytes};
    kernel.output = BufferRange {&output, rowElement * uintBytes, span * uintBytes};
    kernel.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, records);
    }

    commands.commit();

    auto values = readUInts(output, capacity);

    for (auto element = 0; element < capacity; ++element)
    {
        auto written = element >= rowElement && element < rowElement + span;
        auto expected =
            written ? source[firstElement + element - rowElement] * 10u : 0u;

        check(values[element] == expected);
    }
};
