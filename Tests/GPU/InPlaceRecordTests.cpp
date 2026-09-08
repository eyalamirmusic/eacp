#include "Common.h"

#include <eacp/GPU/Codegen/ShaderEmitter.h>

#include <cstdint>
#include <string>

// Reading a record out of an output buffer and storing a rearrangement of it
// back into the same record.
//
// A record write is N element stores, and a name standing for what the buffer
// held used to be given up at the first of them - so the second store re-read
// the element the first had just overwritten and the swap came out a
// broadcast. A record is one write, so all of its components take what it held
// before any of them ran; two element writes are still two statements.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
constexpr auto uintBytes = (int) sizeof(std::uint32_t);
constexpr auto floatBytes = (int) sizeof(float);

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

Vector<std::uint32_t> readUInts(const Buffer& buffer, int elements)
{
    auto values = Vector<std::uint32_t> {};
    values.resize(elements);
    buffer.read(values.data(), uintBytes * elements);
    return values;
}

Buffer makeFloats(const Vector<float>& values)
{
    return Buffer {Device::shared(),
                   values.data(),
                   floatBytes * values.size(),
                   BufferUsage::Storage};
}

Vector<float> readFloats(const Buffer& buffer, int elements)
{
    auto values = Vector<float> {};
    values.resize(elements);
    buffer.read(values.data(), floatBytes * elements);
    return values;
}

// Values a float cannot hold, so a uint record that came back through one
// would show it.
std::uint32_t sourceValue(int element)
{
    return 2654435761u * (std::uint32_t) element + 4026531840u;
}

struct UIntSwapKernel final : ComputeProgram
{
    UIntSwapKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto pair = output.read2(i);

        write(output, i, uint2(pair.y(), pair.x()));
    }

    Uniform<UIntOutputBuffer> output;

    EACP_SHADER(output)
};

struct FloatSwapKernel final : ComputeProgram
{
    FloatSwapKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto pair = output.read2(i);

        write(output, i, float2(pair.y(), pair.x()));
    }

    Uniform<OutputBuffer> output;

    EACP_SHADER(output)
};

struct FloatRotateKernel final : ComputeProgram
{
    FloatRotateKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto quad = output.read4(i);

        write(output, i, float4(quad.y(), quad.z(), quad.w(), quad.x()));
    }

    Uniform<OutputBuffer> output;

    EACP_SHADER(output)
};

// The rotate spelled as a swizzle, which is the value kind the emitter would
// otherwise leave inline and evaluate once per element store.
struct SwizzleRotateKernel final : ComputeProgram
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

// The same shape written as two element writes rather than one record write.
// Two writes are two statements, so the second observes the first.
struct ScalarCarryKernel final : ComputeProgram
{
    ScalarCarryKernel() { compile(); }

    void define() override
    {
        auto i = threadId() * 2u;
        auto x = output[i];

        write(output, i, x + 1.0f);
        write(output, i + 1u, x);
    }

    Uniform<OutputBuffer> output;

    EACP_SHADER(output)
};

void recordUIntSwap(ShaderBuilder& builder)
{
    auto output = builder.uintOutputBuffer();
    auto i = builder.threadId();
    auto pair = output.read2(i);

    builder.write(output, i, uint2(pair.y(), pair.x()));
}

void recordFloatSwap(ShaderBuilder& builder)
{
    auto output = builder.outputBuffer();
    auto i = builder.threadId();
    auto pair = output.read2(i);

    builder.write(output, i, float2(pair.y(), pair.x()));
}

void recordSwizzleRotate(ShaderBuilder& builder)
{
    auto output = builder.outputBuffer();
    auto i = builder.threadId();

    builder.write(output, i, output.read4(i).yzwx());
}

// The common shape: a record read, scaled, and stored back without the read
// being wanted again.
void recordScaleInPlace(ShaderBuilder& builder)
{
    auto output = builder.outputBuffer();
    auto i = builder.threadId();

    builder.write(output, i, output.read2(i) * 2.0f);
}

template <typename Kernel>
void runOver(Kernel& kernel, const Buffer& buffer, int threads)
{
    kernel.output = buffer;
    kernel.prepare();

    auto commands = Device::shared().makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, threads);
    }

    commands.commit();
}
} // namespace

// The record is read once, into one name, and both component stores are handed
// that name: nothing between them reads the buffer they are writing.
auto tUIntSwapReadsTheRecordOnce =
    test("InPlaceRecord/aUIntRecordIsReadOnceBeforeItsStores") = []
{
    auto builder = ShaderBuilder {};
    recordUIntSwap(builder);

    for (const auto& text: {emitMetal(builder.graph()), emitHlsl(builder.graph())})
    {
        check(contains(text, "uint2 t2 = uint2(buffer0[t0], buffer0[t1]);"));
        check(contains(text, "uint2 t3 = uint2((t2).y, (t2).x);"));
        check(contains(text, "buffer0[t0] = (t3).x;"));
        check(contains(text, "buffer0[t1] = (t3).y;"));
        check(occurrences(text, "uint2(buffer0[") == 1);

        auto first = text.find("buffer0[t0] = ");
        auto second = text.find("buffer0[t1] = ");

        check(first != std::string::npos && second != std::string::npos);
        check(text.find("buffer0[", first + 8) == second);
    }

    expectGlslCompiles(builder.graph());
};

// The same on a float output, which is the buffer a stage computing in place
// is handed.
auto tFloatSwapReadsTheRecordOnce =
    test("InPlaceRecord/aFloatRecordIsReadOnceBeforeItsStores") = []
{
    auto builder = ShaderBuilder {};
    recordFloatSwap(builder);

    for (const auto& text: {emitMetal(builder.graph()), emitHlsl(builder.graph())})
    {
        check(contains(text, "float2 t2 = float2(buffer0[t0], buffer0[t1]);"));
        check(contains(text, "float2 t3 = float2((t2).y, (t2).x);"));
        check(contains(text, "buffer0[t0] = (t3).x;"));
        check(contains(text, "buffer0[t1] = (t3).y;"));
        check(occurrences(text, "float2(buffer0[") == 1);
    }

    expectGlslCompiles(builder.graph());
};

// A swizzle is a value the emitter names nowhere else. As a record's value it
// takes a name anyway, so the four element stores are handed the one read.
auto tASwizzledRecordIsHeld = test("InPlaceRecord/aSwizzledRecordIsHeldToo") = []
{
    auto builder = ShaderBuilder {};
    recordSwizzleRotate(builder);

    for (const auto& text: {emitMetal(builder.graph()), emitHlsl(builder.graph())})
    {
        check(contains(text,
                       "float4 t4 = (float4(buffer0[t0], buffer0[t1], "
                       "buffer0[t2], buffer0[t3])).yzwx;"));
        check(contains(text, "buffer0[t0] = (t4).x;"));
        check(contains(text, "buffer0[t3] = (t4).w;"));
        check(occurrences(text, "float4(buffer0[") == 1);
    }

    expectGlslCompiles(builder.graph());
};

// A read the rest of the block does not want again takes no name of its own:
// the scale is what is named, and the record is spelled inside it.
auto tAScaledRecordStaysInline =
    test("InPlaceRecord/aRecordWantedOnceStaysInline") = []
{
    auto builder = ShaderBuilder {};
    recordScaleInPlace(builder);

    for (const auto& text: {emitMetal(builder.graph()), emitHlsl(builder.graph())})
    {
        check(
            contains(text, "float2 t2 = (float2(buffer0[t0], buffer0[t1]) * 2.0);"));
        check(!contains(text, "float2 t2 = float2(buffer0[t0], buffer0[t1]);"));
        check(occurrences(text, "float2(buffer0[") == 1);
    }

    expectGlslCompiles(builder.graph());
};

// Records of two unsigned integers, swapped where they lie.
auto tUIntPairSwapsInPlace = test("InPlaceRecord/aUIntPairSwapsWhereItLies") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto records = 128;

    auto source = Vector<std::uint32_t> {};

    for (auto element = 0; element < records * 2; ++element)
        source.add(sourceValue(element));

    auto buffer = makeUInts(source);
    auto kernel = UIntSwapKernel {};

    runOver(kernel, buffer, records);

    auto values = readUInts(buffer, records * 2);

    for (auto record = 0; record < records; ++record)
    {
        check(values[record * 2] == source[record * 2 + 1]);
        check(values[record * 2 + 1] == source[record * 2]);
    }
};

// The same pair on a float output.
auto tFloatPairSwapsInPlace = test("InPlaceRecord/aFloatPairSwapsWhereItLies") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto records = 128;

    auto source = Vector<float> {};

    for (auto element = 0; element < records * 2; ++element)
        source.add((float) element + 0.5f);

    auto buffer = makeFloats(source);
    auto kernel = FloatSwapKernel {};

    runOver(kernel, buffer, records);

    auto values = readFloats(buffer, records * 2);

    for (auto record = 0; record < records; ++record)
    {
        check(values[record * 2] == source[record * 2 + 1]);
        check(values[record * 2 + 1] == source[record * 2]);
    }
};

// Four components rotated one place, which needs all four to survive the first
// store.
auto tFloatQuadRotatesInPlace =
    test("InPlaceRecord/aFloatQuadRotatesWhereItLies") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto records = 64;

    auto source = Vector<float> {};

    for (auto element = 0; element < records * 4; ++element)
        source.add((float) element + 0.25f);

    auto buffer = makeFloats(source);
    auto kernel = FloatRotateKernel {};

    runOver(kernel, buffer, records);

    auto values = readFloats(buffer, records * 4);

    for (auto record = 0; record < records; ++record)
        for (auto lane = 0; lane < 4; ++lane)
            check(values[record * 4 + lane] == source[record * 4 + (lane + 1) % 4]);
};

// The swizzled rotate through Metal, where the four components have to survive
// the first store together.
auto tSwizzleRotateRunsInPlace =
    test("InPlaceRecord/aSwizzledQuadRotatesWhereItLies") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto records = 64;

    auto source = Vector<float> {};

    for (auto element = 0; element < records * 4; ++element)
        source.add((float) element + 0.75f);

    auto buffer = makeFloats(source);
    auto kernel = SwizzleRotateKernel {};

    runOver(kernel, buffer, records);

    auto values = readFloats(buffer, records * 4);

    for (auto record = 0; record < records; ++record)
        for (auto lane = 0; lane < 4; ++lane)
            check(values[record * 4 + lane] == source[record * 4 + (lane + 1) % 4]);
};

// Where the swap above is one record write, this is two element writes, and
// the line between them is what the record fix rests on: a record's components
// take what the buffer held before the first of them, while a statement of its
// own reads the element as it stands - the contract BufferAccess pins and the
// README's In place section states.
auto tTwoWritesAreTwoStatements =
    test("InPlaceRecord/twoElementWritesAreTwoStatements") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto pairs = 128;

    auto source = Vector<float> {};

    for (auto element = 0; element < pairs * 2; ++element)
        source.add((float) element * 0.5f);

    auto buffer = makeFloats(source);
    auto kernel = ScalarCarryKernel {};

    runOver(kernel, buffer, pairs);

    auto values = readFloats(buffer, pairs * 2);

    for (auto pair = 0; pair < pairs; ++pair)
    {
        check(values[pair * 2] == source[pair * 2] + 1.0f);
        check(values[pair * 2 + 1] == source[pair * 2] + 1.0f);
    }
};
