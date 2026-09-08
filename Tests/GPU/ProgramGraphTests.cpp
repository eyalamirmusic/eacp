#include "Common.h"

#include <string>

// A shipped program hands out its graph, so the backend this machine cannot
// compile is still pinned against the kernel that ships rather than against a
// second copy of its body written on a bare ShaderBuilder.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace
{
bool contains(const std::string& text, const char* needle)
{
    return text.find(needle) != std::string::npos;
}

struct ScaleKernel final : ComputeProgram
{
    ScaleKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, input[i] * scale);
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;
    Uniform<Float> scale;

    EACP_SHADER(input, output, scale)
};

struct QuadVertex
{
    float position[2];
};
} // namespace

EACP_SHADER_VALUE(QuadVertex, Float2)

namespace
{
struct FlatColourProgram final : ShaderProgram
{
    FlatColourProgram() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);

        setPosition(float4(position, 0.f, 1.f));
        setFragment(colour);
    }

    Uniform<Float4> colour;

    EACP_SHADER(colour)
};
} // namespace

auto tComputeProgramExposesItsGraph =
    test("ProgramGraph/aKernelPinsBothBackends") = []
{
    auto kernel = ScaleKernel {};

    auto metal = emitMetal(kernel.graph());
    auto hlsl = emitHlsl(kernel.graph());
    auto glsl = emitGlsl(kernel.graph());

    check(metal == kernel.source().source || hlsl == kernel.source().source
          || glsl == kernel.source().source);

    check(contains(metal, "device const float* buffer0"));
    check(contains(metal, "device float* buffer1"));
    check(contains(metal, "buffer1[gid] = (buffer0[gid] * uniforms.u0);"));

    check(contains(hlsl, "StructuredBuffer<float> buffer0 : register(t0)"));
    check(contains(hlsl, "RWStructuredBuffer<float> buffer1 : register(u1)"));
    check(contains(hlsl, "buffer1[gid] = (buffer0[gid] * uniforms.u0);"));

    check(contains(glsl, "readonly buffer Buffer0\n{\n    float buffer0[];"));
    check(contains(glsl, ") buffer Buffer1\n{\n    float buffer1[];"));
    check(contains(glsl, "buffer1[gid] = (buffer0[gid] * uniforms.u0);"));

    expectGlslCompiles(kernel.graph());
};

auto tShaderProgramExposesItsGraph =
    test("ProgramGraph/aRenderProgramPinsBothBackends") = []
{
    auto program = FlatColourProgram {};

    auto metal = emitMetal(program.graph());
    auto hlsl = emitHlsl(program.graph());
    auto glsl = emitGlsl(program.graph());

    check(metal == program.source().source || hlsl == program.source().source
          || glsl == program.source().source);

    check(contains(metal, "vertex VertexOut vertexMain("));
    check(contains(metal, "fragment float4 fragmentMain("));

    check(contains(hlsl, "VertexOut vertexMain("));
    check(contains(hlsl, "float4 fragmentMain("));
    check(contains(hlsl, "cbuffer UniformsCB : register(b0)"));

    check(contains(glsl, "#ifdef EACP_VERTEX"));
    check(contains(glsl, "#ifdef EACP_FRAGMENT"));
    check(contains(glsl, "uniform Uniforms"));

    expectGlslCompiles(program.graph());
};
