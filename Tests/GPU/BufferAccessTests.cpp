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

// The wide stores, one per width, each at the index its matching read counts
// in. What they have to leave behind is byte for byte what the record write
// would have: the layout is the contract both sides name, so a row written wide
// is a row anything else can read narrow.
struct WideStoreKernel final : ComputeProgram
{
    WideStoreKernel() { compile(); }

    void define() override
    {
        auto i = threadId();

        write4(quads, i, input.read4(i) * 2.0f);
        write3(triples, i, input.read3(i));
        write2(pairs, i, input.read2(i));
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> quads;
    Uniform<OutputBuffer> triples;
    Uniform<OutputBuffer> pairs;

    EACP_SHADER(input, quads, triples, pairs)
};

// A record reversed in place, which is the case that catches a wide store whose
// value was not held first: componentwise, storing x = w leaves the components
// after it reading back what this very store has already overwritten.
struct WideReverseKernel final : ComputeProgram
{
    WideReverseKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto values = inPlace.read4(i);

        write4(inPlace, i, float4(values.w(), values.z(), values.y(), values.x()));
    }

    Uniform<OutputBuffer> inPlace;

    EACP_SHADER(inPlace)
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

// The wide stores lay down the same bytes at the same offsets the record write
// does, which is the whole of what a caller has to be able to rely on: the
// index counts records on both sides, and the run starts at index * N.
auto tWideStoresMatchTheLayout =
    test("BufferAccess/aWideStoreLaysTheRecordDown") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto threads = 16;
    constexpr auto count = threads * 4;
    constexpr auto bytes = count * (int) sizeof(float);

    auto source = Vector<float> {};

    for (auto i = 0; i < count; ++i)
        source.add((float) i * 0.5f - 3.0f);

    auto input = device.makeBuffer(source.data(), bytes, BufferUsage::Storage);
    auto quads = device.makeBuffer(bytes, BufferUsage::Storage);
    auto triples = device.makeBuffer(bytes, BufferUsage::Storage);
    auto pairs = device.makeBuffer(bytes, BufferUsage::Storage);

    auto kernel = WideStoreKernel {};
    kernel.input = input;
    kernel.quads = quads;
    kernel.triples = triples;
    kernel.pairs = pairs;
    kernel.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, threads);
    }

    commands.commit();

    auto wide = Vector<float>(count);
    auto three = Vector<float>(count);
    auto two = Vector<float>(count);

    quads.read(wide.data(), bytes);
    triples.read(three.data(), bytes);
    pairs.read(two.data(), bytes);

    for (auto thread = 0; thread < threads; ++thread)
    {
        for (auto lane = 0; lane < 4; ++lane)
            check(wide[thread * 4 + lane] == source[thread * 4 + lane] * 2.0f);

        for (auto lane = 0; lane < 3; ++lane)
            check(three[thread * 3 + lane] == source[thread * 3 + lane]);

        for (auto lane = 0; lane < 2; ++lane)
            check(two[thread * 2 + lane] == source[thread * 2 + lane]);
    }
};

// And the value reaches memory whole: a record reversed in place is the
// reversal, not the smear a componentwise expansion over an unnamed value would
// leave.
auto tWideStoreHoldsItsValue = test("BufferAccess/aWideStoreHoldsItsValue") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    constexpr auto threads = 16;
    constexpr auto count = threads * 4;
    constexpr auto bytes = count * (int) sizeof(float);

    auto source = Vector<float> {};

    for (auto i = 0; i < count; ++i)
        source.add((float) i);

    auto inPlace = device.makeBuffer(source.data(), bytes, BufferUsage::Storage);

    auto kernel = WideReverseKernel {};
    kernel.inPlace = inPlace;
    kernel.prepare();

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, threads);
    }

    commands.commit();

    auto values = Vector<float>(count);
    inPlace.read(values.data(), bytes);

    for (auto thread = 0; thread < threads; ++thread)
        for (auto lane = 0; lane < 4; ++lane)
            check(values[thread * 4 + lane] == source[thread * 4 + 3 - lane]);
};
