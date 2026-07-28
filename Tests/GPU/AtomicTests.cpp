#include "Common.h"

#include <eacp/GPU/Codegen/ShaderEmitter.h>

// Atomics: what a thread can do to memory another thread is touching at the
// same moment.
//
// Every test here has to be one a non-atomic version would fail, which is a
// higher bar than it sounds: `counter = counter + 1` across a thousand threads
// still lands on a plausible-looking number, just a smaller one, and a test
// checking the counter is "about right" would pass on a broken build. So the
// property checked is the one that cannot survive a lost update - that the
// values handed back are a *permutation* of 0..n-1, every one distinct and none
// skipped. That is also exactly what the operation is for: a thread that is
// given a number nobody else has is a thread that has been given a slot of a
// shared array.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
// Large enough that threads genuinely collide - a handful would run far enough
// apart to pass without any atomicity at all - and a multiple of nothing, so a
// group-size assumption shows up as a short answer.
constexpr auto threadCount = 4099;
constexpr auto bucketCount = 7;

// Every thread adds one to the same counter and keeps what it got back.
// Deliberately one counter rather than many: it is the maximum-contention case,
// and it is the one the binner's allocation stage is.
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

// The same thing spread over several counters, each thread adding to the bucket
// its id falls in. What this adds over the ticket kernel is the index being an
// expression rather than a literal, and the totals being checkable in closed
// form.
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

// Reads the counters back out through load(), which is the only way to see an
// atomic buffer's contents as numbers - the bits are integers, so binding the
// same buffer as an InputBuffer and reading floats gives nonsense.
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

// The property a lost update cannot fake. If two threads ever read the same
// counter value, two tickets are equal and one number in 0..n-1 is missing -
// so checking that every number appears exactly once checks atomicity itself
// rather than checking a total that a broken build would merely undershoot.
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

// The counter is left holding exactly the number of threads that touched it -
// the total the tickets were drawn from - and load() is what reads it.
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

    // A second pass rather than a second kernel in the same one: the read has to
    // see every add, and passes on one command buffer are ordered where threads
    // within a pass are not.
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
        // Thread i lands in bucket i % bucketCount, so the bucket's height is
        // how many of 0..threadCount-1 have that residue.
        auto expected =
            threadCount / bucketCount + (bucket < threadCount % bucketCount ? 1 : 0);

        if ((int) values[bucket] == expected)
            ++matched;

        total += (int) values[bucket];
    }

    check(matched == bucketCount);
    check(total == threadCount);
};

// Both backends' source, generated on whichever host runs the suite. The
// Windows half of this cannot be executed here, and it is the half most likely
// to be wrong: the two languages do not agree on whether an atomic add is an
// expression, so the shapes they emit are genuinely different rather than the
// same text with different keywords.
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

    // Metal: an atomic_uint buffer, the fetch-add returning the old value into
    // a name, and a load that unwraps the atomic to read it.
    check(has(metal, "device atomic_uint* buffer0"));
    check(has(metal,
              "atomic_fetch_add_explicit(&buffer0[0u], 1u, memory_order_relaxed)"));
    check(has(metal, "atomic_load_explicit(&buffer0[0u], memory_order_relaxed)"));

    // HLSL: a uint UAV, and InterlockedAdd - which returns nothing and writes
    // the old value through its third argument, so the name is declared first.
    check(has(hlsl, "RWStructuredBuffer<uint> buffer0 : register(u0)"));
    check(has(hlsl, "InterlockedAdd(buffer0[0u], 1u,"));

    // And the buffer that is not atomic stays a run of floats on both, so
    // declaring one atomic buffer does not retype the rest.
    check(has(metal, "device float* buffer1"));
    check(has(hlsl, "RWStructuredBuffer<float> buffer1 : register(u1)"));
};
