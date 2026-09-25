#include "CodegenCommon.h"

#include <algorithm>

// The three languages disagree on whether an atomic add is an expression.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
struct TicketKernel
{
    TicketKernel()
    {
        counter = builder.atomicBuffer();
        output = builder.outputBuffer();
        auto id = builder.threadId();
        auto ticket = builder.atomicAdd(counter, 0u, 1u);

        builder.write(output, id, toFloat(ticket) + toFloat(counter.load(0u)));
    }

    const ShaderGraph& graph() const { return builder.graph(); }

    ShaderBuilder builder;
    AtomicBuffer counter;
    OutputBuffer output;
};
} // namespace

auto tAtomicSourceIsRight = test("Atomic/bothBackendsDeclareAndAddAtomically") = []
{
    auto kernel = TicketKernel {};

    const auto& graph = kernel.graph();
    auto metal = emitMetal(graph);
    auto hlsl = emitHlsl(graph);
    auto glsl = emitGlsl(graph);

    auto has = [](const std::string& source, std::string_view text)
    { return source.find(text) != std::string::npos; };

    check(has(metal, "device atomic_uint* buffer0"));
    check(has(metal,
              "atomic_fetch_add_explicit(&buffer0[0u], 1u, memory_order_relaxed)"));
    check(has(metal, "atomic_load_explicit(&buffer0[0u], memory_order_relaxed)"));

    // InterlockedAdd returns nothing and writes the old value through its third
    // argument, so the name has to be declared before the call.
    check(has(hlsl, "RWStructuredBuffer<uint> buffer0 : register(u0)"));
    check(has(hlsl, "InterlockedAdd(buffer0[0u], 1u,"));

    check(has(glsl,
              "layout(std430, set = 0, binding = "
                  + std::to_string(vulkanComputeBufferBinding(0))
                  + ") buffer Buffer0\n{\n    uint buffer0[];\n};"));
    check(has(glsl, "uint v0 = atomicAdd(buffer0[0u], 1u);"));

    check(has(metal, "device float* buffer1"));
    check(has(hlsl, "RWStructuredBuffer<float> buffer1 : register(u1)"));
    check(has(glsl, "buffer Buffer1\n{\n    float buffer1[];\n};"));

    expectGlslCompiles(graph);
};

// The CPU runs groups in order and a group's lanes in ascending order, each
// add finishing across the group before the load, so the tickets are the
// thread ids past the starting count and every lane of a group loads the
// count its group left behind. Lanes past the extent add nothing.
auto tAtomicRuns = test("Atomic/bothBackendsDeclareAndAddAtomically/runs") = []
{
    constexpr auto count = 150;
    constexpr auto start = 10u;
    constexpr auto group = (int) ComputePass::threadGroupWidth;

    auto kernel = TicketKernel {};
    auto executor = CpuCompute::Executor {kernel.graph()};
    expectPlans(executor);

    auto counter = filledWith(1, start);
    auto output = filledWith(count, -1.f);

    auto bindings = CpuCompute::Bindings {};
    bindings.set(kernel.counter, counter);
    bindings.set(kernel.output, output);
    check(executor.dispatch(bindings, count));

    check(counter[0] == start + (std::uint32_t) count);

    for (auto i = 0; i < count; ++i)
    {
        auto ticket = start + (std::uint32_t) i;
        auto loaded =
            start + (std::uint32_t) std::min((i / group + 1) * group, count);
        check(output[i] == (float) ticket + (float) loaded);
    }
};
