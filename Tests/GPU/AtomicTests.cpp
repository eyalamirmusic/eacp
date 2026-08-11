#include "Common.h"

#include <eacp/GPU/Codegen/ShaderEmitter.h>

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
// Large enough that threads genuinely collide, and a multiple of no group size.
constexpr auto threadCount = 4099;
constexpr auto bucketCount = 7;

// One counter deliberately: the maximum-contention case.
struct TicketKernel final : ComputeProgram
{
    TicketKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto ticket = atomicAdd(counter, 0u, 1u);

        write(tickets, id, toFloat(ticket));
    }

    Uniform<AtomicBuffer> counter;
    Uniform<OutputBuffer> tickets;

    EACP_SHADER(counter, tickets)
};

// Adds an index that is an expression rather than a literal.
struct HistogramKernel final : ComputeProgram
{
    HistogramKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto bucket = id % (unsigned) bucketCount;

        atomicAdd(counts, bucket, 1u);
    }

    Uniform<AtomicBuffer> counts;

    EACP_SHADER(counts)
};

// load() is the only way to read an atomic buffer: the bits are integers, so
// binding it as an InputBuffer and reading floats gives nonsense.
struct CountReadKernel final : ComputeProgram
{
    CountReadKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        write(output, id, toFloat(counts.load(id)));
    }

    Uniform<AtomicBuffer> counts;
    Uniform<OutputBuffer> output;

    EACP_SHADER(counts, output)
};

Buffer makeZeroed(int elements)
{
    auto zeros = Vector<std::uint32_t> {};
    zeros.assign(elements, 0u);

    return Buffer {Device::shared(),
                   zeros.data(),
                   sizeof(std::uint32_t) * (std::size_t) elements,
                   BufferUsage::Storage};
}

Vector<float> readFloats(const Buffer& buffer, int elements)
{
    auto values = Vector<float> {};
    values.resize(elements);
    buffer.read(values.data(), sizeof(float) * (std::size_t) elements);
    return values;
}
} // namespace

// A lost update merely undershoots a total, so the permutation of 0..n-1 is
// what pins atomicity.
auto tTicketsArePermutation = test("Atomic/everyThreadGetsADistinctTicket") = []
{
    if (!Device::shared().isValid())
        return;

    auto counter = makeZeroed(1);
    auto tickets = Buffer {Device::shared(),
                           nullptr,
                           sizeof(float) * threadCount,
                           BufferUsage::Storage};

    auto kernel = TicketKernel {};
    kernel.counter = counter;
    kernel.tickets = tickets;
    kernel.prepare();

    auto commands = Device::shared().makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, threadCount);
    }

    commands.commit();

    auto values = readFloats(tickets, threadCount);
    auto seen = Vector<char> {};
    seen.assign(threadCount, 0);

    auto inRange = true;

    for (auto value: values)
    {
        auto ticket = (int) value;

        if (ticket < 0 || ticket >= threadCount)
        {
            inRange = false;
            continue;
        }

        seen[ticket] += 1;
    }

    check(inRange);

    auto distinct = 0;

    for (auto count: seen)
        if (count == 1)
            ++distinct;

    check(distinct == threadCount);
};

auto tHistogramTotalsAreExact = test("Atomic/bucketCountsAreExact") = []
{
    if (!Device::shared().isValid())
        return;

    auto counts = makeZeroed(bucketCount);
    auto output = Buffer {Device::shared(),
                          nullptr,
                          sizeof(float) * bucketCount,
                          BufferUsage::Storage};

    auto histogram = HistogramKernel {};
    histogram.counts = counts;
    histogram.prepare();

    auto reader = CountReadKernel {};
    reader.counts = counts;
    reader.output = output;
    reader.prepare();

    auto commands = Device::shared().makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(histogram, threadCount);
    }

    // A second pass, not a second dispatch: passes are ordered, threads are not.
    {
        auto pass = commands.beginCompute();
        pass.dispatch(reader, bucketCount);
    }

    commands.commit();

    auto values = readFloats(output, bucketCount);
    auto total = 0;
    auto matched = 0;

    for (auto bucket = 0; bucket < bucketCount; ++bucket)
    {
        // Thread i lands in bucket i % bucketCount.
        auto expected =
            threadCount / bucketCount + (bucket < threadCount % bucketCount ? 1 : 0);

        if ((int) values[bucket] == expected)
            ++matched;

        total += (int) values[bucket];
    }

    check(matched == bucketCount);
    check(total == threadCount);
};

// Both backends' source, generated on whichever host runs the suite: the HLSL
// half cannot be executed here, and the two languages emit different shapes.
auto tAtomicSourceIsRight = test("Atomic/bothBackendsDeclareAndAddAtomically") = []
{
    auto builder = ShaderBuilder {};

    auto counter = builder.atomicBuffer();
    auto output = builder.outputBuffer();
    auto id = builder.threadId();
    auto ticket = builder.atomicAdd(counter, 0u, 1u);

    builder.write(output, id, toFloat(ticket) + toFloat(counter.load(0u)));

    const auto& graph = builder.graph();
    auto metal = emitMetal(graph);
    auto hlsl = emitHlsl(graph);

    auto has = [](const std::string& source, const char* text)
    { return source.find(text) != std::string::npos; };

    check(has(metal, "device atomic_uint* buffer0"));
    check(has(metal,
              "atomic_fetch_add_explicit(&buffer0[0u], 1u, memory_order_relaxed)"));
    check(has(metal, "atomic_load_explicit(&buffer0[0u], memory_order_relaxed)"));

    // InterlockedAdd returns nothing: the old value comes back through arg 3.
    check(has(hlsl, "RWStructuredBuffer<uint> buffer0 : register(u0)"));
    check(has(hlsl, "InterlockedAdd(buffer0[0u], 1u,"));

    check(has(metal, "device float* buffer1"));
    check(has(hlsl, "RWStructuredBuffer<float> buffer1 : register(u1)"));
};
