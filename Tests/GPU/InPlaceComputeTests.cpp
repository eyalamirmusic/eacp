#include "Common.h"

#include <cmath>
#include <string>

// Computing in place, which is what a 1:1 elementwise stage wants instead of a
// second allocation per stage.
//
// The buffer is bound once, to an output slot, and the kernel reads the element
// it is about to store to. Binding one buffer to an input slot and an output
// slot of the same kernel is the form that does not port: D3D12 needs the
// resource in a different state for each of the two bindings, and the second
// bind transitions it out from under the first. The tests below are the
// one-slot form and the reason the two-slot one would mislead even where the
// binding is legal.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
bool contains(const std::string& text, const char* needle)
{
    return text.find(needle) != std::string::npos;
}

int occurrences(const std::string& text, const char* needle)
{
    auto count = 0;
    auto length = std::string(needle).size();

    for (auto at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + length))
        ++count;

    return count;
}

constexpr auto elementCount = 256;

// The exact GELU, over the buffer it was handed and nothing else.
struct InPlaceGeluKernel final : ComputeProgram
{
    InPlaceGeluKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        auto x = output[i];

        write(output, i, 0.5f * x * (1.0f + erf(x * 0.70710678f)));
    }

    Uniform<OutputBuffer> output;

    EACP_SHADER(output)
};

double geluReference(double x)
{
    return 0.5 * x * (1.0 + std::erf(x * 0.70710678118654752));
}
} // namespace

auto tInPlaceGeluRewritesItsOwnBuffer =
    test("InPlace/aKernelRewritesTheBufferItWasHanded") = []
{
    auto& device = Device::shared();

    if (!device.isValid())
        return;

    auto values = Vector<float> {};

    for (auto i = 0; i < elementCount; ++i)
        values.add(((float) i - 128.f) / 32.f);

    auto bytes = elementCount * (int) sizeof(float);
    auto buffer = device.makeBuffer(values.data(), bytes, BufferUsage::Storage);

    auto kernel = InPlaceGeluKernel {};
    kernel.output = buffer;
    kernel.prepare(device);

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, elementCount);
    }

    commands.commit();

    auto result = Vector<float>(elementCount);
    buffer.read(result.data(), bytes);

    for (auto i = 0; i < elementCount; ++i)
        check(std::fabs((double) result[i] - geluReference((double) values[i]))
              <= 1.0e-5);
};

// The read is emitted as a statement ahead of the store, on both backends, so
// what the element held reaches the expression before the expression replaces
// it.
auto tTheReadPrecedesTheStore = test("InPlace/theReadIsEmittedBeforeTheStore") = []
{
    auto kernel = InPlaceGeluKernel {};

    for (const auto& source: {emitMetal(kernel.graph()),
                              emitHlsl(kernel.graph()),
                              emitGlsl(kernel.graph())})
    {
        check(contains(source, "float t0 = buffer0[gid];"));
        check(source.find("float t0 = buffer0[gid];")
              < source.find("buffer0[gid] = "));
    }

    expectGlslCompiles(kernel.graph());
};

// The other half of the contract, and why one buffer on two slots would not do
// what it looks like: a value read through one slot keeps its name across a
// store to another, so the second use of it is the value from before the store.
auto tAReadKeepsItsNameAcrossAnotherSlotsStore =
    test("InPlace/aReadIsNotRefreshedByAnotherSlotsStore") = []
{
    auto builder = ShaderBuilder {};

    auto input = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto i = builder.threadId();

    auto seen = input[i];

    builder.write(output, i, seen + 1.0f);
    builder.write(output, i, seen + 2.0f);

    for (const auto& source: {emitMetal(builder.graph()),
                              emitHlsl(builder.graph()),
                              emitGlsl(builder.graph())})
    {
        check(occurrences(source, "buffer0[gid]") == 1);
        check(contains(source, "buffer1[gid] = (t0 + 1.0);"));
        check(contains(source, "buffer1[gid] = (t0 + 2.0);"));
    }

    expectGlslCompiles(builder.graph());
};
