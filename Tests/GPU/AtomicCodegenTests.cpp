#include "CodegenCommon.h"

// The three languages disagree on whether an atomic add is an expression.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

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
