#include "Common.h"

#include <eacp/GPU/Codegen/ShaderEmitter.h>

// Threadgroup memory: what one dispatch group has in common, and the barrier
// that makes one thread's writes to it visible to the rest.
//
// The failure to rule out is that it is not shared at all. A kernel where each
// thread writes shared[lid] and then reads shared[lid] back works perfectly if
// "shared" memory is per-thread scratch - it is the same value, so the test
// passes on an implementation that shares nothing. So every test here has each
// thread read a slot **another thread wrote**: a reduction where one thread
// consumes the whole group's values, and an exchange where every thread reads
// the lane opposite its own. Neither can come out right unless the memory is
// genuinely common and the barrier genuinely waits.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
// The 1D dispatch's group. A shared array indexed by the group-local thread
// index wants exactly this many elements, and the dispatch wants a whole number
// of them - a kernel with a barrier may not be dispatched over anything else.
constexpr auto groupSize = ComputePass::threadGroupWidth;
constexpr auto groups = 5;
constexpr auto threadCount = groupSize * groups;

// Each thread contributes its own id; thread 0 of each group then walks all
// groupSize slots and writes the total. Every value but one came from another
// thread, and the total is checkable in closed form.
struct GroupSumKernel final : ComputeProgram
{
    GroupSumKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto lane = threadIndexInGroup();
        auto scratch = sharedArray<Float, groupSize>();

        write(scratch, lane, toFloat(id));
        barrier();

        // One thread per group does the summing, and the ifThen is safe here
        // because the barrier is above it: divergence after a barrier is
        // ordinary control flow, divergence *around* one is what is undefined.
        ifThen(lane == 0u,
               [&]
               {
                   auto total = var(0.f);
                   auto i = var(0);

                   loop(i < groupSize,
                        [&]
                        {
                            total += scratch[toUInt(i)];
                            i += 1;
                        });

                   write(sums, id / (unsigned) groupSize, total.get());
               });
    }

    Uniform<OutputBuffer> sums;

    EACP_SHADER(sums)
};

// Every thread reads the lane opposite its own, so no thread reads anything it
// wrote. Without a barrier the read races the write; without sharing it reads
// its own slot and the output is the identity rather than the reversal.
struct ExchangeKernel final : ComputeProgram
{
    ExchangeKernel() { compile(); }

    void define() override
    {
        auto id = threadId();
        auto lane = threadIndexInGroup();
        auto scratch = sharedArray<Float, groupSize>();

        write(scratch, lane, toFloat(id));
        barrier();

        write(output, id, scratch[(unsigned) (groupSize - 1) - lane]);
    }

    Uniform<OutputBuffer> output;

    EACP_SHADER(output)
};

Buffer makeOutput(int elements)
{
    return Buffer {Device::shared(),
                   nullptr,
                   sizeof(float) * (std::size_t) elements,
                   BufferUsage::Storage};
}

template <typename Kernel>
Vector<float> run(Kernel& kernel, const Buffer& output, int elements)
{
    kernel.prepare();

    auto commands = Device::shared().makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, threadCount);
    }

    commands.commit();

    auto values = Vector<float> {};
    values.resize(elements);
    output.read(values.data(), sizeof(float) * (std::size_t) elements);
    return values;
}
} // namespace

// Each group's total, summed by one thread out of values the other 63 put
// there. A group summing g*64..g*64+63 is (64*g*64) + (63*64/2).
auto tGroupSumSeesEveryLane = test("SharedMemory/oneThreadSumsItsWholeGroup") = []
{
    if (!Device::shared().isValid())
        return;

    auto sums = makeOutput(groups);
    auto kernel = GroupSumKernel {};
    kernel.sums = sums;

    auto values = run(kernel, sums, groups);
    auto correct = 0;

    for (auto group = 0; group < groups; ++group)
    {
        auto first = group * groupSize;
        auto expected =
            (float) (groupSize * first + groupSize * (groupSize - 1) / 2);

        if (values[group] == expected)
            ++correct;
    }

    check(correct == groups);
};

// The reversal. Thread `lane` of a group comes back holding what thread
// `63 - lane` wrote, which no amount of per-thread scratch could produce.
auto tExchangeCrossesLanes = test("SharedMemory/everyThreadReadsAnotherLane") = []
{
    if (!Device::shared().isValid())
        return;

    auto output = makeOutput(threadCount);
    auto kernel = ExchangeKernel {};
    kernel.output = output;

    auto values = run(kernel, output, threadCount);
    auto correct = 0;

    for (auto i = 0; i < threadCount; ++i)
    {
        auto base = (i / groupSize) * groupSize;
        auto expected = (float) (base + groupSize - 1 - (i % groupSize));

        if (values[i] == expected)
            ++correct;
    }

    check(correct == threadCount);

    // And it is genuinely a reversal rather than the identity, which is what a
    // kernel with unshared scratch would have produced.
    check(values[0] != 0.f);
};

// The declaration lands on opposite sides of the entry point on the two
// backends, which is the one place threadgroup memory is not the same shape
// twice - MSL's threadgroup is a local of the kernel, HLSL's groupshared is a
// global. Neither can be run against the other here, so both are read.
auto tSharedSourceIsRight =
    test("SharedMemory/bothBackendsDeclareAndSynchronise") = []
{
    auto builder = ShaderBuilder {};

    auto output = builder.outputBuffer();
    auto id = builder.threadId();
    auto lane = builder.threadIndexInGroup();
    auto scratch = builder.sharedArray<Float, 64>();

    builder.write(scratch, lane, toFloat(id));
    builder.barrier();
    builder.write(output, id, scratch[0u]);

    const auto& graph = builder.graph();
    auto metal = emitMetal(graph);
    auto hlsl = emitHlsl(graph);

    auto has = [](const std::string& source, const char* text)
    { return source.find(text) != std::string::npos; };

    check(has(metal, "threadgroup float s0[64];"));
    check(has(metal, "uint lid [[thread_index_in_threadgroup]]"));
    check(has(metal, "threadgroup_barrier(mem_flags::mem_threadgroup);"));

    check(has(hlsl, "groupshared float s0[64];"));
    check(has(hlsl, "uint lid : SV_GroupIndex"));
    check(has(hlsl, "GroupMemoryBarrierWithGroupSync();"));

    // MSL declares it inside the kernel and HLSL above it, so the two differ in
    // which side of the entry point the declaration falls on.
    check(metal.find("s0[64]") > metal.find("kernel void computeMain"));
    check(hlsl.find("s0[64]") < hlsl.find("void computeMain"));

    // A kernel that never asks where it sits in its group does not take the
    // parameter that says, so nothing already emitted changes shape.
    auto plain = ShaderBuilder {};
    auto plainOut = plain.outputBuffer();
    auto plainId = plain.threadId();
    plain.write(plainOut, plainId, toFloat(plainId));

    check(!has(emitMetal(plain.graph()), "lid"));
    check(!has(emitHlsl(plain.graph()), "lid"));
};
