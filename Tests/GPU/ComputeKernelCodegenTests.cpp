#include "CodegenCommon.h"

#include <eacp/GPU/Codegen/ComputeKernel.h>

// A kernel derived from ComputeKernel links against eacp-gpu-codegen alone, as
// this binary does, and records the graph a bare ShaderBuilder does.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
struct GainKernel final : ComputeKernel
{
    GainKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, input[i] * gain);
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;
    Uniform<Float> gain;

    EACP_SHADER(input, output, gain)
};

void recordGain(ShaderBuilder& builder)
{
    auto input = builder.inputBuffer();
    auto output = builder.outputBuffer();
    auto gain = builder.uniform<Float>();

    auto i = builder.threadId();
    builder.write(output, i, input[i] * gain);
}
} // namespace

auto tComputeKernelRecordsWithoutADevice =
    test("ComputeKernel/recordsTheBodyABareBuilderDoes") = []
{
    auto kernel = GainKernel {};
    kernel.gain = 2.f;

    auto builder = ShaderBuilder {};
    recordGain(builder);

    check(emitMetal(kernel.graph()) == emitMetal(builder.graph()));
    check(emitHlsl(kernel.graph()) == emitHlsl(builder.graph()));
    check(emitGlsl(kernel.graph()) == emitGlsl(builder.graph()));
    check(kernel.source().source == builder.build().source.source);
    check(kernel.dispatchRank() == DispatchRank::OneD);

    expectGlslCompiles(kernel.graph());
};
