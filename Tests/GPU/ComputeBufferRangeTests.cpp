#include "CpuCrossCheck.h"

#include <eacp/GPU/Codegen/ShaderEmitter.h>

#include <span>

// Binding a compute buffer part-way into its resource: element zero of the
// kernel's buffer is the element at the range's offset. The cases bound by hand
// on the GPU are bound by hand on the CPU too, as a subspan of a host array.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;
using namespace eacp::GPU::CrossChecks;

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

// Four floats at a time, so the read is the one vector load rather than four
// subscripts - the whole point of the range offset below.
struct RecordCopyKernel final : ComputeProgram
{
    RecordCopyKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, input.read4(i));
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

Vector<float> rampValues(int elements)
{
    auto values = Vector<float> {};
    values.resize(elements);

    for (auto i = 0; i < elements; ++i)
        values[i] = (float) i;

    return values;
}

Buffer makeRamp(int elements)
{
    auto values = rampValues(elements);

    return Buffer {Device::shared(),
                   values.data(),
                   floatBytes * elements,
                   BufferUsage::Storage};
}

// The CPU's BufferRange: count elements of a host array from first on.
std::span<float> hostElements(Vector<float>& values, int first, int count)
{
    return std::span<float> {values.data(), (std::size_t) values.size()}.subspan(
        (std::size_t) first, (std::size_t) count);
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

// The grid a ranged storage bind's offset has to sit on, counted in elements of
// the given size: one on Metal and D3D12, four on Mesa's lavapipe. Every offset
// below is a multiple of it, so each one is non-zero and bindable everywhere.
int rowElements(int elementBytes)
{
    return Device::shared().storageBufferOffsetAlignment() / elementBytes;
}
} // namespace

// An input bound at an offset: a copy from halfway into a ramp comes back as
// the second half of it.
auto tInputBoundAtOffset = test("GPU/computeInputBoundAtOffset") = []
{
    const auto count = 8;
    const auto first = rowElements(floatBytes);

    {
        auto input = rampValues(first + count);
        auto output = filled(count, -1.0f);

        auto kernel = CopyKernel {};
        auto bindings = CpuCompute::Bindings {};
        check(bindings.set(kernel.input, hostElements(input, first, count)));
        check(bindings.set(kernel.output, output));
        dispatchOnCpu(kernel, bindings, count);

        for (auto i = 0; i < count; ++i)
            check(output[i] == (float) (first + i), "cpu");
    }

    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto input = makeRamp(first + count);
    auto output = makeFilled(count, -1.0f);

    auto kernel = CopyKernel {};
    kernel.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.setPipeline(kernel.pipeline());
        pass.setInputBuffer(elements(input, first, count), kernel.input.slot);
        pass.setOutputBuffer(output, kernel.output.slot);
        dispatchBoundByHand(pass, kernel, count);
    }

    commands.commit();

    auto values = readFloats(output, count);

    for (auto i = 0; i < count; ++i)
        check(values[i] == (float) (first + i));
};

// A vector read through a range that does not start on a sixteen-byte boundary,
// which is the case the Metal load is shaped around: a ranged bind's offset has
// to sit on Device::storageBufferOffsetAlignment and that is four bytes here, so
// read4 cannot ask for a float4's alignment and reads a packed vector instead.
auto tVectorReadFromAnOffsetRange =
    test("GPU/computeVectorReadFromAnOffsetRange") = []
{
    const auto records = 6;
    const auto count = records * 4;
    const auto first = rowElements(floatBytes);

    {
        auto input = rampValues(first + count);
        auto output = filled(count, -1.0f);

        auto kernel = RecordCopyKernel {};
        auto bindings = CpuCompute::Bindings {};
        check(bindings.set(kernel.input, hostElements(input, first, count)));
        check(bindings.set(kernel.output, output));
        dispatchOnCpu(kernel, bindings, records);

        for (auto i = 0; i < count; ++i)
            check(output[i] == (float) (first + i), "cpu");
    }

    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto input = makeRamp(first + count);
    auto output = makeFilled(count, -1.0f);

    auto kernel = RecordCopyKernel {};
    kernel.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.setPipeline(kernel.pipeline());
        pass.setInputBuffer(elements(input, first, count), kernel.input.slot);
        pass.setOutputBuffer(output, kernel.output.slot);
        dispatchBoundByHand(pass, kernel, records);
    }

    commands.commit();

    auto values = readFloats(output, count);

    for (auto i = 0; i < count; ++i)
        check(values[i] == (float) (first + i));
};

// An output bound at an offset: the pre-filled elements before it stay as they
// were.
auto tOutputBoundAtOffset = test("GPU/computeOutputBoundAtOffset") = []
{
    const auto count = 4;
    const auto row = rowElements(floatBytes);
    const auto capacity = row + 2 * count;

    {
        auto input = rampValues(count);
        auto output = filled(capacity, -1.0f);

        auto kernel = CopyKernel {};
        auto bindings = CpuCompute::Bindings {};
        check(bindings.set(kernel.input, input));
        check(bindings.set(kernel.output, hostElements(output, row, count)));
        dispatchOnCpu(kernel, bindings, count);

        for (auto i = 0; i < capacity; ++i)
        {
            auto written = i >= row && i < row + count;
            check(output[i] == (written ? (float) (i - row) : -1.0f), "cpu");
        }
    }

    auto& device = Device::shared();

    if (!device.isValid())
        return;

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
    const auto count = 4;
    const auto destRow = rowElements(floatBytes);
    const auto sourceRow = 2 * destRow;
    const auto capacity = sourceRow + 2 * count;

    auto kernel = CopyKernel {};

    CrossCheck {kernel}
        .input(kernel.input, rampValues(capacity), sourceRow, count)
        .output(kernel.output, filled(capacity, -1.0f), destRow, count)
        .run(count,
             [&](const Readback& readback)
             {
                 const auto& values = readback.floats(kernel.output);

                 for (auto i = 0; i < capacity; ++i)
                 {
                     auto written = i >= destRow && i < destRow + count;
                     check(values[i]
                               == (written ? (float) (sourceRow + i - destRow)
                                           : -1.0f),
                           readback.name());
                 }
             });
};

// The regression the range overloads have to leave alone: a member assigned a
// whole GPU::Buffer still binds from byte zero.
auto tWholeBufferStillBindsFromZero =
    test("GPU/computeWholeBufferBindsFromZero") = []
{
    constexpr auto count = 8;

    auto kernel = CopyKernel {};

    CrossCheck {kernel}
        .input(kernel.input, rampValues(count))
        .output(kernel.output, count, -1.0f)
        .run(count,
             [&](const Readback& readback)
             {
                 const auto& values = readback.floats(kernel.output);

                 for (auto i = 0; i < count; ++i)
                     check(values[i] == (float) i, readback.name());
             });
};

// An atomic buffer bound at an offset: thread i's add lands on counter
// offset + i, and the counters below the offset stay zero.
auto tAtomicBoundAtOffset = test("GPU/computeAtomicBoundAtOffset") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    const auto count = 3;
    const auto first = 2 * rowElements(uintBytes);
    const auto capacity = first + count + 1;

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
//
// The CPU's range past the end is an empty span, which is bound rather than
// ignored: a read through it is zero and a store through it goes nowhere.
auto tOutOfRangeBindsNothing = test("GPU/computeOutOfRangeRangeBindsNothing") = []
{
    constexpr auto count = 4;

    {
        auto input = rampValues(count);
        auto scratch = filled(count, -2.0f);
        auto output = filled(count, -1.0f);

        auto kernel = CopyKernel {};
        auto bindings = CpuCompute::Bindings {};
        check(bindings.set(kernel.input, hostElements(input, count, 0)));
        check(bindings.set(kernel.output, scratch));
        dispatchOnCpu(kernel, bindings, count);

        check(bindings.set(kernel.input, input));
        check(bindings.set(kernel.output, hostElements(output, count, 0)));
        dispatchOnCpu(kernel, bindings, count);

        for (auto i = 0; i < count; ++i)
        {
            check(scratch[i] == 0.0f, "cpu");
            check(output[i] == -1.0f, "cpu");
        }
    }

    auto& device = Device::shared();

    if (!device.isValid())
        return;

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
