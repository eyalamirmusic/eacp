#include "CodegenCommon.h"

// The atomic half that is pure string generation: what the three languages are
// told to do, rather than what a device does when it does it. Split from
// AtomicTests.cpp so it runs where there is no GPU at all - and the Windows and
// Vulkan shapes are the ones most likely to be wrong, because the languages do
// not agree on whether an atomic add is an expression.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

// Every backend's source, generated on whichever host runs the suite. The two
// halves that cannot be executed here are the halves most likely to be wrong:
// the three languages do not agree on whether an atomic add is an expression,
// so the shapes they emit are genuinely different rather than the same text
// with different keywords.
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

    // GLSL: a storage block of uints, and atomicAdd - which, like Metal's
    // fetch-add and unlike InterlockedAdd, is an expression returning the old
    // value, so the name is declared by the call itself.
    check(has(glsl,
              "layout(std430, set = 0, binding = "
                  + std::to_string(vulkanComputeBufferBinding(0))
                  + ") buffer Buffer0\n{\n    uint buffer0[];\n};"));
    check(has(glsl, "uint v0 = atomicAdd(buffer0[0u], 1u);"));

    // And the buffer that is not atomic stays a run of floats on all three, so
    // declaring one atomic buffer does not retype the rest.
    check(has(metal, "device float* buffer1"));
    check(has(hlsl, "RWStructuredBuffer<float> buffer1 : register(u1)"));
    check(has(glsl, "buffer Buffer1\n{\n    float buffer1[];\n};"));

    expectGlslCompiles(graph);
};
