#include "Common.h"

#include <eacp/GPU/Shader/PrecompiledShaders.h>

#include <string>
#include <string_view>

// ShaderLibrary::compilationInfo() on the current backend: a hand-written
// source broken on purpose reports the line and column it broke at, a source
// with a warning keeps it on a success where the compiler gives it, every
// invalid library holds an error, a clean compile says nothing, and a D3D
// variant compiled for one value set has its own. The Metal and GLSL
// positions are what the compilers report; the HLSL ones are FXC's documented
// columns, unverified until a Windows lane runs this. The parsers alone are
// CompilationInfo/'s, in GPUCodegenTests. Reflection/'s ShaderProgram case is
// here because GPUTests links the program.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace CompilationInfoDeviceTests
{
namespace
{
bool canTest()
{
    return Device::shared().isValid();
}

ShaderBackend currentBackend()
{
    return detail::nativeShaderBackend();
}

// Line 7 in every dialect: the undeclared identifier's.
ShaderSource brokenKernel()
{
    return nativeDialect("CompilationInfo/brokenKernel").withCompute("computeMain");
}

int brokenColumn()
{
    switch (currentBackend())
    {
        case ShaderBackend::Metal:
            return 16;
        case ShaderBackend::DirectX:
            return 21;
        case ShaderBackend::WebGPU:
            return 17;
        case ShaderBackend::Vulkan:
            break;
    }

    return 49;
}

// A warning and nothing else: Metal's #warning on line 3, glslang's unknown
// extension on line 2, FXC's implicit truncation on line 7, the WGSL
// compiler's unreachable statement on line 8.
ShaderSource warnedKernel()
{
    return nativeDialect("CompilationInfo/warnedKernel").withCompute("computeMain");
}

int warnedLine()
{
    switch (currentBackend())
    {
        case ShaderBackend::Metal:
            return 3;
        case ShaderBackend::DirectX:
            return 7;
        case ShaderBackend::WebGPU:
            return 8;
        case ShaderBackend::Vulkan:
            break;
    }

    return 2;
}

// Both render stages, the fragment's broken on line 13 of the GLSL - the
// stage a backend compiling them apart names.
ShaderSource brokenFragment()
{
    return nativeDialect("CompilationInfo/brokenFragment");
}

std::string describe(const ShaderLibrary& library)
{
    return library.compilationInfo().describe();
}

// The first message of that type, or an empty one where there is none, so a
// failed check does not take the suite down with it.
const CompilationMessage& firstOf(const CompilationInfo& info,
                                  CompilationMessageType type)
{
    static const auto none = CompilationMessage {.message = "(none)"};

    for (const auto& message: info.messages)
        if (message.type == type)
            return message;

    return none;
}

auto tBroken = test("CompilationInfoDevice/aBrokenKernelReportsWhereItBroke") = []
{
    if (!canTest())
        return;

    const auto library = Device::shared().makeShaderLibrary(brokenKernel());
    const auto& info = library.compilationInfo();

    check(!library.isValid());
    check(info.hasErrors(), describe(library));

    const auto& error = firstOf(info, CompilationMessageType::Error);
    check(error.line == 7, error.describe());
    check(error.column == brokenColumn(), error.describe());
    check(error.message.find("undeclaredThing") != std::string::npos,
          error.describe());

    if (currentBackend() == ShaderBackend::Metal
        || currentBackend() == ShaderBackend::WebGPU)
        check(!error.stage.has_value(), "a Metal or WGSL library is compiled whole");
    else
        check(error.stage == ShaderStage::Compute, error.describe());

    check(library.compilationInfo(PipelineConstants {}).messages.size()
          == info.messages.size());
};

auto tWarned = test("CompilationInfoDevice/aWarningIsKeptOnASuccess") = []
{
    if (!canTest())
        return;

    const auto library = Device::shared().makeShaderLibrary(warnedKernel());
    const auto& info = library.compilationInfo();

    check(library.isValid(), describe(library));
    check(!info.hasErrors(), describe(library));

    for (const auto& message: info.messages)
    {
        check(message.type == CompilationMessageType::Warning, message.describe());
        check(message.line == warnedLine(), message.describe());
    }

    // Metal drops the warnings of a library it builds; the others keep them.
    if (currentBackend() != ShaderBackend::Metal)
        check(info.countOf(CompilationMessageType::Warning) >= 1, describe(library));
};

auto tMetalWarning =
    test("CompilationInfoDevice/aMetalWarningComesBesideAnError") = []
{
    if (!canTest() || currentBackend() != ShaderBackend::Metal)
        return;

    const auto library = Device::shared().makeShaderLibrary(
        ShaderSource::msl(testShaderFile("CompilationInfo/warningBesideError.metal"))
            .withCompute("computeMain"));
    const auto& info = library.compilationInfo();

    check(!library.isValid());

    const auto& warning = firstOf(info, CompilationMessageType::Warning);
    check(warning.line == 3 && warning.column == 2, describe(library));
    check(warning.message.find("eacp sample warning") != std::string::npos);

    const auto& error = firstOf(info, CompilationMessageType::Error);
    check(error.line == 7 && error.column == 16, describe(library));
};

auto tFragment = test("CompilationInfoDevice/aBrokenStageIsNamed") = []
{
    if (!canTest())
        return;

    const auto library = Device::shared().makeShaderLibrary(brokenFragment());
    const auto& info = library.compilationInfo();

    check(!library.isValid());

    const auto& error = firstOf(info, CompilationMessageType::Error);
    check(error.message.find("missingColour") != std::string::npos,
          describe(library));

    switch (currentBackend())
    {
        case ShaderBackend::Metal:
            check(error.line == 11 && error.column == 12, error.describe());
            check(!error.stage.has_value());
            break;
        case ShaderBackend::Vulkan:
            check(error.line == 13, error.describe());
            check(error.stage == ShaderStage::Fragment, error.describe());

            for (const auto& message: info.messages)
                check(message.stage == ShaderStage::Fragment,
                      "the vertex stage compiled: " + message.describe());
            break;
        case ShaderBackend::DirectX:
            check(error.line == 8, error.describe());
            check(error.stage.has_value(), error.describe());
            break;
        case ShaderBackend::WebGPU:
            break;
    }
};

auto tInvalid = test("CompilationInfoDevice/everyInvalidLibraryHoldsAnError") = []
{
    if (!canTest())
        return;

    const auto empty = Device::shared().makeShaderLibrary(
        nativeDialect({}, {}, {}, {}).withCompute("computeMain"));
    check(!empty.isValid());
    check(empty.compilationInfo().hasErrors());
    check(!firstOf(empty.compilationInfo(), CompilationMessageType::Error)
               .hasPosition());

    Precompiled::clearForTesting();
    Precompiled::setRequiredForTesting(true);

    const auto required = Device::shared().makeShaderLibrary(warnedKernel());
    const auto& info = required.compilationInfo();

    Precompiled::clearForTesting();

    check(!required.isValid());
    check(info.messages.size() == 1, info.describe());
    check(info.hasErrors());
    check(firstOf(info, CompilationMessageType::Error)
                  .message.find("EACP_REQUIRE_PRECOMPILED_SHADERS")
              != std::string::npos,
          info.describe());
};

struct CleanKernel final : ComputeProgram
{
    CleanKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, input[i] * overrideConstant<Float>("gain", 1.f));
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;

    EACP_SHADER(input, output)
};

auto tClean = test("CompilationInfoDevice/aCleanCompileSaysNothing") = []
{
    if (!canTest())
        return;

    auto kernel = CleanKernel {};
    const auto library = Device::shared().makeShaderLibrary(kernel.source());

    check(library.isValid(), describe(library));
    check(library.compilationInfo().messages.empty(), describe(library));

    const auto constants =
        PipelineConstants {}.set(PipelineConstant::ofFloat(0, 3.f));
    check(library.compilationInfo(constants).messages.empty(),
          library.compilationInfo(constants).describe());
};

// On D3D a value set is a compile of its own, so a set the source cannot take
// fails there alone; Metal and Vulkan specialise one compiled library, whose
// messages every set shares.
ShaderSource valueSetKernel()
{
    return nativeDialect("CompilationInfo/valueSetKernel")
        .withCompute("computeMain")
        .withPipelineConstants();
}

auto tValueSets = test("CompilationInfoDevice/eachValueSetHasItsOwnCompile") = []
{
    if (!canTest())
        return;

    const auto library = Device::shared().makeShaderLibrary(valueSetKernel());
    check(library.isValid(), describe(library));
    check(!library.compilationInfo().hasErrors(), describe(library));

    const auto seven = PipelineConstants {}.set(PipelineConstant::ofInt(0, 7));
    const auto two = PipelineConstants {}.set(PipelineConstant::ofInt(0, 2));
    const auto sevenInfo = library.compilationInfo(seven);

    check(!library.compilationInfo(two).hasErrors(),
          library.compilationInfo(two).describe());

    if (currentBackend() == ShaderBackend::DirectX)
    {
        check(sevenInfo.hasErrors(), sevenInfo.describe());

        const auto& error = firstOf(sevenInfo, CompilationMessageType::Error);
        check(error.line == 10 && error.stage == ShaderStage::Compute,
              error.describe());
        check(!library.compilationInfo().hasErrors(),
              "the library's own compile is untouched");
    }
    else
    {
        check(sevenInfo.messages.size() == library.compilationInfo().messages.size(),
              sevenInfo.describe());
    }
};

struct Tinted
{
    float position[2];
};

struct NamedProgram final : ShaderProgram
{
    NamedProgram() { compile(); }

    void define() override
    {
        auto position = vertexInput(&Tinted::position);
        setPosition(float4(position * scale, 0.f, 1.f));
        setFragment(sample(albedo, smooth, varying(position)) * tint);
    }

    Uniform<Float> scale;
    Uniform<Float4> tint;
    Uniform<Texture2D> albedo;
    Uniform<SamplerBinding> smooth;

    EACP_SHADER(scale, tint, albedo, smooth)
};

auto tProgramNames = test("Reflection/aShaderProgramNamesItsMembers") = []
{
    auto program = NamedProgram {};
    const auto& reflection = program.reflection();

    check(reflection.uniformMembers.size() == 2);

    if (reflection.uniformMembers.size() == 2)
    {
        check(reflection.uniformMembers[0].name == "scale");
        check(reflection.uniformMembers[1].name == "tint");
        check(reflection.uniformMembers[1].offset == 16);
    }

    check(reflection.uniformBlockSize == program.uniformByteSize());

    const auto* albedo = reflection.findBinding("albedo");
    check(albedo != nullptr && albedo->kind == BindingKind::Texture);
    check(albedo != nullptr && albedo->slot == 0);
    check(albedo != nullptr && albedo->stages == StageVisibility {.fragment = true});

    const auto* smooth = reflection.findBinding("smooth");
    check(smooth != nullptr && smooth->kind == BindingKind::Sampler);
    check(smooth != nullptr && smooth->slot == 1);

    const auto* block = reflection.findBinding(BindingKind::UniformBlock, 0);
    check(block != nullptr
          && block->stages == StageVisibility {.vertex = true, .fragment = true});
    check(program.vertexReadsUniforms() && program.fragmentReadsUniforms());

    check(reflection.vertexInputs.size() == 1);
    check(reflection.varyings.size() == 1);
};
} // namespace
} // namespace CompilationInfoDeviceTests
