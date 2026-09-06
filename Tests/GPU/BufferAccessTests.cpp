#include "Common.h"

#include <type_traits>

// The storage-buffer surface a kernel actually writes against: a literal
// element index, a readable output, and a buffer member that cannot be handed a
// temporary.
//
// The three are checked at the level each of them can go wrong at. The literal
// index and the output read are emission *and* numbers - the emitted text says
// the declaration is still writable and that no second binding appeared, and
// the read-back says the hardware agrees. The temporary is a compile-time
// question and has no run-time evidence at all, so it is pinned with
// static_assert: the whole point of the deleted overload is that the offending
// line never becomes a program.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
bool contains(const std::string& haystack, const std::string& needle)
{
    return haystack.find(needle) != std::string::npos;
}

int occurrences(const std::string& haystack, const std::string& needle)
{
    auto count = 0;

    for (auto at = haystack.find(needle); at != std::string::npos;
         at = haystack.find(needle, at + needle.size()))
        ++count;

    return count;
}

// One element read with a literal index, broadcast over the dispatch: the
// shape of a kernel scaling by a factor another kernel left in a one-element
// buffer.
struct LiteralIndexKernel final : ComputeProgram
{
    LiteralIndexKernel() { compile(); }

    void define() override
    {
        auto i = threadId();

        write(output, i * 4u, input[0]);
        write(output, i * 4u + 1u, input.read2(0u).y());
        write(output, i * 4u + 2u, input.read3(0u).z());
        write(output, i * 4u + 3u, input.read4(0u).w());
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;

    EACP_SHADER(input, output)
};

// Writes, then reads the element back and writes again - the read-after-write
// a softmax needs so that normalising does not mean exponentiating twice.
struct ReadBackKernel final : ComputeProgram
{
    ReadBackKernel() { compile(); }

    void define() override
    {
        auto i = threadId();

        write(output, i, input[i] * 2.0f);
        write(output, i, output[i] + 1.0f);
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;

    EACP_SHADER(input, output)
};

// The same read, held in a handle used on both sides of a store to the buffer
// it came from. A handle is an expression and not a snapshot - the emitter
// names one only while nothing it reads has moved, exactly as it does for a
// mutable local - so `seen` is 2x before the store and 20x after it.
struct RereadKernel final : ComputeProgram
{
    RereadKernel() { compile(); }

    void define() override
    {
        auto i = threadId();

        write(output, i, input[i]);

        auto seen = output[i];

        write(doubled, i, seen + seen);
        write(output, i, seen * 10.0f);
        write(after, i, seen + seen);
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;
    Uniform<OutputBuffer> doubled;
    Uniform<OutputBuffer> after;

    EACP_SHADER(input, output, doubled, after)
};

// Assigning a temporary would leave the program holding a pointer into a buffer
// destroyed on the same line, which is why the rvalue overload is deleted. The
// lvalue form is what every call site writes and must keep working.
static_assert(std::is_assignable_v<Uniform<InputBuffer>&, Buffer&>);
static_assert(std::is_assignable_v<Uniform<OutputBuffer>&, Buffer&>);
static_assert(std::is_assignable_v<Uniform<AtomicBuffer>&, Buffer&>);
static_assert(std::is_assignable_v<Uniform<Texture2D>&, Texture&>);
static_assert(std::is_assignable_v<Uniform<TextureCube>&, Texture&>);
static_assert(std::is_assignable_v<Uniform<TextureDepth2D>&, Texture&>);
static_assert(std::is_assignable_v<Uniform<WritableTexture2D>&, Texture&>);

static_assert(!std::is_assignable_v<Uniform<InputBuffer>&, Buffer>);
static_assert(!std::is_assignable_v<Uniform<OutputBuffer>&, Buffer>);
static_assert(!std::is_assignable_v<Uniform<AtomicBuffer>&, Buffer>);
static_assert(!std::is_assignable_v<Uniform<Texture2D>&, Texture>);
static_assert(!std::is_assignable_v<Uniform<TextureCube>&, Texture>);
static_assert(!std::is_assignable_v<Uniform<TextureDepth2D>&, Texture>);
static_assert(!std::is_assignable_v<Uniform<WritableTexture2D>&, Texture>);
} // namespace

// A literal index is a uint constant on the buffer's own graph, so it prints as
// one rather than needing a var() to carry it.
auto tLiteralIndexEmitsAConstant =
    test("BufferAccess/aLiteralIndexEmitsAConstant") = []
{
    auto builder = ShaderBuilder {};
    auto input = builder.inputBuffer();
    auto output = builder.outputBuffer();

    builder.write(output, builder.threadId(), input[0]);

    check(contains(emitMetal(builder.graph()), "buffer0[0u]"));
    check(contains(emitHlsl(builder.graph()), "buffer0[0u]"));
};

// The output declaration is unchanged by being read: writable on both backends,
// and still one binding rather than an input added beside it.
auto tOutputStaysWritable =
    test("BufferAccess/anOutputReadKeepsOneWritableBinding") = []
{
    auto builder = ShaderBuilder {};
    auto output = builder.outputBuffer();
    auto i = builder.threadId();

    builder.write(output, i, builder.constant(2.0f));
    builder.write(output, i, output[i] + 1.0f);

    auto metal = emitMetal(builder.graph());
    auto hlsl = emitHlsl(builder.graph());

    check(contains(metal, "device float* buffer0"));
    check(!contains(metal, "device const float* buffer0"));
    check(occurrences(metal, "buffer0 [[buffer(") == 1);

    check(contains(hlsl, "RWStructuredBuffer<float> buffer0 : register(u0)"));
    check(!contains(hlsl, "StructuredBuffer<float> buffer0 : register(t"));

    // The read is the subscript the store is, on both backends and in the order
    // written: nothing hoists the read above the write it has to observe.
    check(contains(metal, "buffer0[gid] = (buffer0[gid] + 1.0);"));
    check(contains(hlsl, "buffer0[gid] = (buffer0[gid] + 1.0);"));
};

// Element zero of a buffer the CPU filled, reached without manufacturing an
// index - and the vector reads on the same terms.
auto tLiteralIndexReadsElementZero =
    test("BufferAccess/aLiteralIndexReadsTheFirstElements") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    const float source[] = {3.0f, 5.0f, 7.0f, 11.0f};

    auto input = device.makeBuffer(source, BufferUsage::Storage);
    auto output = device.makeBuffer((int) sizeof(source), BufferUsage::Storage);

    auto kernel = LiteralIndexKernel {};
    kernel.input = input;
    kernel.output = output;
    kernel.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, 1);
    }

    commands.commit();

    float values[4] = {};
    output.read(values, (int) sizeof(values));

    for (auto i = 0; i < 4; ++i)
        check(values[i] == source[i]);
};

// 2x written, then read back and raised by one. Computing 2x twice would give
// the same answer, so the kernel writes it in two statements: the second reads
// what the first stored.
auto tReadsBackWhatItWrote = test("BufferAccess/aKernelReadsBackWhatItWrote") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto count = 128;

    auto source = Vector<float> {};

    for (auto i = 0; i < count; ++i)
        source.add((float) i * 0.25f - 4.0f);

    auto input = device.makeBuffer(
        source.data(), count * (int) sizeof(float), BufferUsage::Storage);

    auto output =
        device.makeBuffer(count * (int) sizeof(float), BufferUsage::Storage);

    auto kernel = ReadBackKernel {};
    kernel.input = input;
    kernel.output = output;
    kernel.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, count);
    }

    commands.commit();

    auto values = Vector<float>(count);
    output.read(values.data(), count * (int) sizeof(float));

    for (auto i = 0; i < count; ++i)
        check(values[i] == source[i] * 2.0f + 1.0f);
};

// The name the emitter gives a repeated read is given up by a store to the
// buffer it read, so the same handle used after one sees the stored value.
auto tStoreGivesUpTheReadsName =
    test("BufferAccess/aStoreGivesUpTheNameOfAReadOfIt") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto count = 64;

    auto source = Vector<float> {};

    for (auto i = 0; i < count; ++i)
        source.add((float) i + 1.0f);

    auto bytes = count * (int) sizeof(float);

    auto input = device.makeBuffer(source.data(), bytes, BufferUsage::Storage);
    auto output = device.makeBuffer(bytes, BufferUsage::Storage);
    auto doubled = device.makeBuffer(bytes, BufferUsage::Storage);
    auto after = device.makeBuffer(bytes, BufferUsage::Storage);

    auto kernel = RereadKernel {};
    kernel.input = input;
    kernel.output = output;
    kernel.doubled = doubled;
    kernel.after = after;
    kernel.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, count);
    }

    commands.commit();

    auto beforeStore = Vector<float>(count);
    auto afterStore = Vector<float>(count);
    doubled.read(beforeStore.data(), bytes);
    after.read(afterStore.data(), bytes);

    for (auto i = 0; i < count; ++i)
    {
        check(beforeStore[i] == source[i] * 2.0f);
        check(afterStore[i] == source[i] * 20.0f);
    }
};

// The run-time face of the static_asserts above, so the suite reports on the
// rule rather than only failing to build when it is broken.
auto tTemporaryBufferIsRefused =
    test("BufferAccess/aTemporaryResourceCannotBeAssigned") = []
{
    check(std::is_assignable_v<Uniform<InputBuffer>&, Buffer&>);
    check(!std::is_assignable_v<Uniform<InputBuffer>&, Buffer>);
    check(!std::is_assignable_v<Uniform<OutputBuffer>&, Buffer>);
    check(!std::is_assignable_v<Uniform<AtomicBuffer>&, Buffer>);
    check(!std::is_assignable_v<Uniform<Texture2D>&, Texture>);
    check(!std::is_assignable_v<Uniform<WritableTexture2D>&, Texture>);
};
