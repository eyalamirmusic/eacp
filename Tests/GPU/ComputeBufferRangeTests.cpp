#include "Common.h"

#include <eacp/GPU/Codegen/ShaderEmitter.h>

// Binding a compute buffer part-way into its resource: element zero of the
// kernel's buffer is the element at the range's offset.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
struct CopyKernel final : ComputeProgram
{
    CopyKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, input[i]);
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;

    EACP_SHADER(input, output)
};

// One add per thread into a counter of its own, so an offset shows up as which
// counters moved.
struct BumpKernel final : ComputeProgram
{
    BumpKernel() { compile(); }

    void define() override { atomicAdd(counters, threadId(), 1u); }

    Uniform<AtomicBuffer> counters;

    EACP_SHADER(counters)
};

constexpr auto floatBytes = (int) sizeof(float);
constexpr auto uintBytes = (int) sizeof(std::uint32_t);

Buffer makeRamp(int elements)
{
    auto values = Vector<float> {};
    values.resize(elements);

    for (auto i = 0; i < elements; ++i)
        values[i] = (float) i;

    return Buffer {Device::shared(),
                   values.data(),
                   floatBytes * elements,
                   BufferUsage::Storage};
}

Buffer makeFilled(int elements, float value)
{
    auto values = Vector<float> {};
    values.assign(elements, value);

    return Buffer {Device::shared(),
                   values.data(),
                   floatBytes * elements,
                   BufferUsage::Storage};
}

Buffer makeZeroedCounters(int elements)
{
    auto zeros = Vector<std::uint32_t> {};
    zeros.assign(elements, 0u);

    return Buffer {
        Device::shared(), zeros.data(), uintBytes * elements, BufferUsage::Storage};
}

Vector<float> readFloats(const Buffer& buffer, int elements)
{
    auto values = Vector<float> {};
    values.resize(elements);
    buffer.read(values.data(), floatBytes * elements);
    return values;
}

Vector<std::uint32_t> readCounters(const Buffer& buffer, int elements)
{
    auto values = Vector<std::uint32_t> {};
    values.resize(elements);
    buffer.read(values.data(), uintBytes * elements);
    return values;
}

// Everything ComputePass::dispatch(program, count) does apart from the buffer
// binds, which are what these tests are hand-rolling.
void dispatchBoundByHand(ComputePass& pass, ComputeProgram& program, int count)
{
    const auto* uniforms = program.packedUniforms(count);
    pass.setBytes(uniforms, program.uniformByteSize());
    pass.dispatch(count);
}

BufferRange elements(const Buffer& buffer, int first, int count)
{
    return {&buffer, first * floatBytes, count * floatBytes};
}
} // namespace

// An input bound at an offset: a copy from halfway into a ramp comes back as
// the second half of it.
auto tInputBoundAtOffset = test("GPU/computeInputBoundAtOffset") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto count = 8;

    auto input = makeRamp(2 * count);
    auto output = makeFilled(count, -1.0f);

    auto kernel = CopyKernel {};
    kernel.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.setPipeline(kernel.pipeline());
        pass.setInputBuffer(elements(input, count, count), kernel.input.slot);
        pass.setOutputBuffer(output, kernel.output.slot);
        dispatchBoundByHand(pass, kernel, count);
    }

    commands.commit();

    auto values = readFloats(output, count);

    for (auto i = 0; i < count; ++i)
        check(values[i] == (float) (count + i));
};

// An output bound at an offset: the pre-filled elements before it stay as they
// were.
auto tOutputBoundAtOffset = test("GPU/computeOutputBoundAtOffset") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto count = 4;
    constexpr auto capacity = 16;
    constexpr auto row = 4;

    auto input = makeRamp(count);
    auto output = makeFilled(capacity, -1.0f);

    auto kernel = CopyKernel {};
    kernel.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.setPipeline(kernel.pipeline());
        pass.setInputBuffer(input, kernel.input.slot);
        pass.setOutputBuffer(elements(output, row, count), kernel.output.slot);
        dispatchBoundByHand(pass, kernel, count);
    }

    commands.commit();

    auto values = readFloats(output, capacity);

    for (auto i = 0; i < capacity; ++i)
    {
        auto written = i >= row && i < row + count;
        check(values[i] == (written ? (float) (i - row) : -1.0f));
    }
};

// The same two binds through program members assigned a BufferRange.
auto tProgramMembersTakeRanges = test("GPU/computeProgramMembersTakeRanges") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto count = 4;
    constexpr auto capacity = 16;
    constexpr auto sourceRow = 8;
    constexpr auto destRow = 4;

    auto input = makeRamp(capacity);
    auto output = makeFilled(capacity, -1.0f);

    auto kernel = CopyKernel {};
    kernel.input = elements(input, sourceRow, count);
    kernel.output = elements(output, destRow, count);
    kernel.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, count);
    }

    commands.commit();

    auto values = readFloats(output, capacity);

    for (auto i = 0; i < capacity; ++i)
    {
        auto written = i >= destRow && i < destRow + count;
        check(values[i] == (written ? (float) (sourceRow + i - destRow) : -1.0f));
    }
};

// The regression the range overloads have to leave alone: a member assigned a
// whole GPU::Buffer still binds from byte zero.
auto tWholeBufferStillBindsFromZero =
    test("GPU/computeWholeBufferBindsFromZero") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto count = 8;

    auto input = makeRamp(count);
    auto output = makeFilled(count, -1.0f);

    auto kernel = CopyKernel {};
    kernel.input = input;
    kernel.output = output;
    kernel.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, count);
    }

    commands.commit();

    auto values = readFloats(output, count);

    for (auto i = 0; i < count; ++i)
        check(values[i] == (float) i);
};

// An atomic buffer bound at an offset: thread i's add lands on counter
// offset + i, and the counters below the offset stay zero.
auto tAtomicBoundAtOffset = test("GPU/computeAtomicBoundAtOffset") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto capacity = 8;
    constexpr auto first = 4;
    constexpr auto count = 3;

    auto counters = makeZeroedCounters(capacity);

    auto kernel = BumpKernel {};
    kernel.counters = BufferRange {&counters, first * uintBytes, count * uintBytes};
    kernel.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, count);
    }

    commands.commit();

    auto values = readCounters(counters, capacity);

    for (auto i = 0; i < capacity; ++i)
    {
        auto bumped = i >= first && i < first + count;
        check(values[i] == (bumped ? 1u : 0u));
    }
};

// A range that names nothing, one starting before its buffer and one starting
// at or past its end all bind nothing. A legal buffer is bound at each slot
// first, since reading an unbound slot is undefined on both backends.
auto tOutOfRangeBindsNothing = test("GPU/computeOutOfRangeRangeBindsNothing") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto count = 4;

    auto input = makeRamp(count);
    auto scratch = makeFilled(count, 0.0f);
    auto output = makeFilled(count, -1.0f);

    auto kernel = CopyKernel {};
    kernel.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.setPipeline(kernel.pipeline());
        pass.setInputBuffer(input, kernel.input.slot);
        pass.setOutputBuffer(scratch, kernel.output.slot);

        pass.setInputBuffer(BufferRange {}, kernel.input.slot);
        pass.setInputBuffer(BufferRange {&input, input.size(), 0},
                            kernel.input.slot);

        pass.setOutputBuffer(BufferRange {}, kernel.output.slot);
        pass.setOutputBuffer(BufferRange {&output, -floatBytes, floatBytes},
                             kernel.output.slot);
        pass.setOutputBuffer(BufferRange {&output, output.size(), 0},
                             kernel.output.slot);

        dispatchBoundByHand(pass, kernel, count);
    }

    commands.commit();

    auto written = readFloats(scratch, count);
    auto untouched = readFloats(output, count);

    for (auto i = 0; i < count; ++i)
    {
        check(written[i] == (float) i);
        check(untouched[i] == -1.0f);
    }
};

// The four-byte offset alignment ComputePass.h states rests on both backends
// declaring four-byte elements, and never a raw ByteAddressBuffer.
auto tStorageBufferStrideIsFourBytes =
    test("GPU/computeStorageBufferStrideIsFour") = []
{
    auto builder = ShaderBuilder {};

    auto input = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto counters = builder.atomicBuffer();
    auto id = builder.threadId();

    builder.atomicAdd(counters, id, 1u);
    builder.write(output, id, input[id]);

    const auto& graph = builder.graph();
    auto metal = emitMetal(graph);
    auto hlsl = emitHlsl(graph);

    auto has = [](const std::string& source, const char* text)
    { return source.find(text) != std::string::npos; };

    check(has(metal, "device const float* buffer0"));
    check(has(metal, "device float* buffer1"));
    check(has(metal, "device atomic_uint* buffer2"));

    check(has(hlsl, "StructuredBuffer<float> buffer0 : register(t0)"));
    check(has(hlsl, "RWStructuredBuffer<float> buffer1 : register(u1)"));
    check(has(hlsl, "RWStructuredBuffer<uint> buffer2 : register(u2)"));

    check(!has(hlsl, "ByteAddressBuffer"));
};
