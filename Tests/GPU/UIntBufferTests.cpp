#include "Common.h"

#include <eacp/GPU/Codegen/ShaderEmitter.h>

#include <cstdint>
#include <iterator>

// Storage buffers whose elements are unsigned integers rather than floats.
//
// What they are for is a chain of kernels that carries ids on the device: an
// embedding gather reads token ids, an argmax writes the next one, the gather
// of the following step reads that back, and nothing goes through the CPU. A
// float buffer cannot hold the middle of that - 51863 survives a round trip
// through a float, but a count above 2^24 does not, and an id read as a float
// has to be bitcast before it can index anything.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto floatBytes = (int) sizeof(float);
constexpr auto uintBytes = (int) sizeof(std::uint32_t);

// The four ids the gather test looks up: the first two rows of the table, and
// two near the end of a vocabulary-sized one.
constexpr std::uint32_t gatheredIds[] = {0u, 1u, 50257u, 51863u};
constexpr auto vocabulary = 51864;
constexpr auto rowWidth = 2u;

// One row of a table, per column, so a wrong row and a wrong column are
// different wrong answers.
float tableValue(std::uint32_t row, std::uint32_t column)
{
    return (float) (row * rowWidth + column);
}

// A gather: the ids are read as integers and index the table directly.
struct GatherKernel final : ComputeProgram
{
    GatherKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto row = ids[i / rowWidth];

        write(output, i, table[row * rowWidth + i % rowWidth]);
    }

    Uniform<UIntInputBuffer> ids;
    Uniform<InputBuffer> table;
    Uniform<OutputBuffer> output;

    EACP_SHADER(ids, table, output)
};

// The squares that do not fit a float's mantissa.
struct SquareKernel final : ComputeProgram
{
    SquareKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, i * i);
    }

    Uniform<UIntOutputBuffer> output;

    EACP_SHADER(output)
};

// The first half of the device-side handoff: ids written as integers.
struct IdKernel final : ComputeProgram
{
    IdKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(ids, i, (i * 7u + 3u) % 64u);
    }

    Uniform<UIntOutputBuffer> ids;

    EACP_SHADER(ids)
};

// The second half: the same buffer, read through a uint input.
struct SmallGatherKernel final : ComputeProgram
{
    SmallGatherKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, table[ids[i]]);
    }

    Uniform<UIntInputBuffer> ids;
    Uniform<InputBuffer> table;
    Uniform<OutputBuffer> output;

    EACP_SHADER(ids, table, output)
};

struct BinKernel final : ComputeProgram
{
    BinKernel() { compile(); }

    void define() override { atomicAdd(counts, threadId() % 4u, 1u); }

    Uniform<AtomicBuffer> counts;

    EACP_SHADER(counts)
};

// An atomic buffer's storage, read back by a later kernel as plain integers.
struct CopyUIntKernel final : ComputeProgram
{
    CopyUIntKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, input[i]);
    }

    Uniform<UIntInputBuffer> input;
    Uniform<UIntOutputBuffer> output;

    EACP_SHADER(input, output)
};

struct ScaleUIntKernel final : ComputeProgram
{
    ScaleUIntKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, input[i] * 10u);
    }

    Uniform<UIntInputBuffer> input;
    Uniform<UIntOutputBuffer> output;

    EACP_SHADER(input, output)
};

// A read, a store to the same element, and a read of it afterwards. The second
// read must see what the store put there.
struct RereadKernel final : ComputeProgram
{
    RereadKernel() { compile(); }

    void define() override
    {
        auto i = threadId();

        auto before = output[i];
        write(output, i, before + before);

        auto after = output[i];
        write(output, i, after + after);
    }

    Uniform<UIntOutputBuffer> output;

    EACP_SHADER(output)
};

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

Buffer makeFloats(const Vector<float>& values)
{
    return Buffer {Device::shared(),
                   values.data(),
                   floatBytes * values.size(),
                   BufferUsage::Storage};
}

Buffer makeFilledFloats(int elements, float value)
{
    auto values = Vector<float> {};
    values.assign(elements, value);
    return makeFloats(values);
}

Vector<std::uint32_t> readUInts(const Buffer& buffer, int elements)
{
    auto values = Vector<std::uint32_t> {};
    values.resize(elements);
    buffer.read(values.data(), uintBytes * elements);
    return values;
}

Vector<float> readFloats(const Buffer& buffer, int elements)
{
    auto values = Vector<float> {};
    values.resize(elements);
    buffer.read(values.data(), floatBytes * elements);
    return values;
}

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
} // namespace

// Ids read as integers index a table directly, with no bitcast on the way in
// and no rounding to lose the large ones.
auto tGatherReadsIdsAsIntegers = test("UIntBuffer/aGatherIndexesWithReadIds") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto idCount = (int) std::size(gatheredIds);
    constexpr auto threads = idCount * (int) rowWidth;

    auto rows = Vector<float> {};
    rows.resize(vocabulary * (int) rowWidth);

    for (auto row = 0; row < vocabulary; ++row)
        for (auto column = 0u; column < rowWidth; ++column)
            rows[row * (int) rowWidth + (int) column] =
                tableValue((std::uint32_t) row, column);

    auto ids = Vector<std::uint32_t> {};

    for (auto id: gatheredIds)
        ids.add(id);

    auto idBuffer = makeUInts(ids);
    auto table = makeFloats(rows);
    auto output = makeFilledFloats(threads, -1.0f);

    auto kernel = GatherKernel {};
    kernel.ids = idBuffer;
    kernel.table = table;
    kernel.output = output;
    kernel.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, threads);
    }

    commands.commit();

    auto values = readFloats(output, threads);

    for (auto i = 0; i < threads; ++i)
        check(values[i]
              == tableValue(gatheredIds[i / (int) rowWidth],
                            (std::uint32_t) i % rowWidth));
};

// The squares of the thread indices, read back as uint32. Past 2^24 a float
// output would round them, so the last of these is what says the elements are
// integers all the way through.
auto tUIntOutputKeepsEveryBit = test("UIntBuffer/anOutputKeepsBitsAFloatLoses") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto threads = 6000;

    auto output = makeFilledUInts(threads, 0u);

    auto kernel = SquareKernel {};
    kernel.output = output;
    kernel.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, threads);
    }

    commands.commit();

    auto values = readUInts(output, threads);
    auto beyondAFloat = 0;

    for (auto i = 0; i < threads; ++i)
    {
        auto square = (std::uint32_t) i * (std::uint32_t) i;
        check(values[i] == square);

        if ((std::uint32_t) (float) square != square)
            ++beyondAFloat;
    }

    check(beyondAFloat > 0);
};

// One kernel's uint output is the next kernel's uint input, inside a single
// command buffer: the ids never reach the CPU.
auto tOneKernelHandsIdsToTheNext =
    test("UIntBuffer/idsCrossBetweenKernelsOnTheDevice") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto threads = 64;

    auto rows = Vector<float> {};

    for (auto i = 0; i < threads; ++i)
        rows.add((float) i * 0.5f);

    auto ids = makeFilledUInts(threads, 0u);
    auto table = makeFloats(rows);
    auto output = makeFilledFloats(threads, -1.0f);

    auto write = IdKernel {};
    write.ids = ids;
    write.prepare();

    auto gather = SmallGatherKernel {};
    gather.ids = ids;
    gather.table = table;
    gather.output = output;
    gather.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(write, threads);
    }

    {
        auto pass = commands.beginCompute();
        pass.dispatch(gather, threads);
    }

    commands.commit();

    auto values = readFloats(output, threads);

    for (auto i = 0; i < threads; ++i)
        check(values[i] == rows[(i * 7 + 3) % 64]);
};

// The counters an atomic kernel left behind, read by a later kernel as plain
// integers rather than loaded atomically or converted first.
auto tAtomicStorageReadsBackAsUInts =
    test("UIntBuffer/atomicCountsAreReadableAsIntegers") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto threads = 400;
    constexpr auto bins = 4;

    auto counts = makeFilledUInts(bins, 0u);
    auto output = makeFilledUInts(bins, 0u);

    auto bin = BinKernel {};
    bin.counts = counts;
    bin.prepare();

    auto copy = CopyUIntKernel {};
    copy.input = counts;
    copy.output = output;
    copy.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(bin, threads);
    }

    {
        auto pass = commands.beginCompute();
        pass.dispatch(copy, bins);
    }

    commands.commit();

    auto values = readUInts(output, bins);

    for (auto i = 0; i < bins; ++i)
        check(values[i] == (std::uint32_t) (threads / bins));
};

// A uint buffer bound part-way in: element zero of the kernel's buffer is the
// element at the range's offset, on the input side and on the output side.
//
// Both offsets are whole multiples of the device's storage-buffer alignment,
// which is the grid a bind takes - one uint on Metal and D3D12, four of them on
// lavapipe - so each offset is non-zero and bindable on every backend.
auto tUIntRangesBindAtTheirOffset =
    test("UIntBuffer/aRangeStartsAtItsOwnOffset") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    const auto row = device.storageBufferOffsetAlignment() / uintBytes;
    const auto first = 2 * row;
    const auto count = 3;
    const auto capacity = first + count;

    auto source = Vector<std::uint32_t> {};

    for (auto i = 0; i < capacity; ++i)
        source.add((std::uint32_t) i + 1u);

    auto input = makeUInts(source);
    auto output = makeFilledUInts(capacity, 0u);

    auto kernel = ScaleUIntKernel {};
    kernel.input = BufferRange {&input, first * uintBytes, count * uintBytes};
    kernel.output = BufferRange {&output, row * uintBytes, count * uintBytes};
    kernel.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, count);
    }

    commands.commit();

    auto values = readUInts(output, capacity);

    for (auto i = 0; i < capacity; ++i)
    {
        auto written = i >= row && i < row + count;
        check(values[i] == (written ? source[first + i - row] * 10u : 0u));
    }
};

// A store retires the name a read of the same slot was hoisted under, so the
// read after it fetches again. Both backends have to print two reads.
auto tAStoreStalesAUIntRead = test("UIntBuffer/aStoreStalesAnEarlierRead") = []
{
    auto builder = ShaderBuilder {};

    auto output = builder.uintOutputBuffer();
    auto i = builder.threadId();

    auto before = output[i];
    builder.write(output, i, before + before);

    auto after = output[i];
    builder.write(output, i, after + after);

    const auto& graph = builder.graph();

    for (const auto& source: {emitMetal(graph), emitHlsl(graph), emitGlsl(graph)})
    {
        check(occurrences(source, "uint t0 = buffer0[") == 1);
        check(occurrences(source, "uint t1 = buffer0[") == 1);
    }

    expectGlslCompiles(graph);
};

// The re-read run through Metal: four doublings of one is sixteen, one doubling
// is two, and a stale name would give either the wrong one.
auto tTheRereadSeesTheStore = test("UIntBuffer/aRereadSeesWhatWasStored") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto threads = 8;

    auto output = makeFilledUInts(threads, 1u);

    auto kernel = RereadKernel {};
    kernel.output = output;
    kernel.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, threads);
    }

    commands.commit();

    auto values = readUInts(output, threads);

    for (auto i = 0; i < threads; ++i)
        check(values[i] == 4u);
};

// What each backend declares a uint buffer as, beside the float pair it is a
// sibling of - and the registers, which follow the access and not the element
// type.
auto tUIntBuffersDeclareTheirElementType =
    test("UIntBuffer/bothBackendsDeclareUIntElements") = []
{
    auto builder = ShaderBuilder {};

    auto floats = builder.inputBuffer();
    auto ids = builder.uintInputBuffer();
    auto out = builder.outputBuffer();
    auto tokens = builder.uintOutputBuffer();
    auto i = builder.threadId();

    builder.write(out, i, floats[ids[i]]);
    builder.write(tokens, i, ids[i] + 1u);

    const auto& graph = builder.graph();
    auto metal = emitMetal(graph);
    auto hlsl = emitHlsl(graph);
    auto glsl = emitGlsl(graph);

    check(contains(metal, "device const float* buffer0"));
    check(contains(metal, "device const uint* buffer1"));
    check(contains(metal, "device float* buffer2"));
    check(contains(metal, "device uint* buffer3"));

    check(contains(hlsl, "StructuredBuffer<float> buffer0 : register(t0)"));
    check(contains(hlsl, "StructuredBuffer<uint> buffer1 : register(t1)"));
    check(contains(hlsl, "RWStructuredBuffer<float> buffer2 : register(u2)"));
    check(contains(hlsl, "RWStructuredBuffer<uint> buffer3 : register(u3)"));

    check(!contains(hlsl, "ByteAddressBuffer"));

    // The element type is inside the block, and readonly is the access - a
    // GLSL block says both in one declaration.
    check(contains(glsl, "readonly buffer Buffer0\n{\n    float buffer0[];"));
    check(contains(glsl, "readonly buffer Buffer1\n{\n    uint buffer1[];"));
    check(contains(glsl, ") buffer Buffer2\n{\n    float buffer2[];"));
    check(contains(glsl, ") buffer Buffer3\n{\n    uint buffer3[];"));

    expectGlslCompiles(graph);
};

// A render stage reads one as readily as a kernel does: the same storage bind,
// and only the element type its declaration carries differs.
auto tARenderStageReadsUIntElements =
    test("UIntBuffer/aRenderStageDeclaresUIntElements") = []
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto ids = builder.uintInputBuffer();
    auto palette = builder.inputBuffer();

    builder.position(float4(position, 0.0f, 1.0f));
    builder.fragment(float4(palette[ids[0u]], 0.0f, 0.0f, 1.0f));

    const auto& graph = builder.graph();

    check(contains(emitMetal(graph), "device const uint* buffer0"));
    check(contains(emitHlsl(graph), "StructuredBuffer<uint> buffer0 : register(t"));
    check(contains(emitGlsl(graph),
                   "readonly buffer Buffer0\n{\n    uint "
                   "buffer0[];"));

    expectGlslCompiles(graph);
};
