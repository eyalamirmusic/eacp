#include "Common.h"

#include <eacp/Core/Utils/Environment.h>
#include <eacp/Core/Utils/Files.h>
#include <eacp/GPU/Dxc/DxcResource.h>

#include <cmath>

// The opt-in DXC path. The first half needs neither the network nor the
// compiler: whatever the locator answers, a library that cannot have DXC still
// compiles through FXC and the status says why. The second half takes the
// real release, and only under EACP_TEST_DXC=1, since it is a 32 MB download
// on a machine that has never fetched it. Each case puts back the options it
// found, so a suite run under EACP_D3D12_DXC=1 stays under DXC.

using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

namespace DxcTests
{
struct QuadVertex
{
    float position[2];
};
} // namespace DxcTests

EACP_SHADER_VALUE(DxcTests::QuadVertex, Float2)

namespace DxcTests
{
namespace
{
constexpr auto elementCount = 64;

constexpr QuadVertex fullScreenTriangle[] = {
    {{-1.f, -1.f}}, {{3.f, -1.f}}, {{-1.f, 3.f}}};

struct SquareKernel final : ComputeProgram
{
    SquareKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, input[i] * input[i] + 1.0f);
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;

    EACP_SHADER(input, output)
};

struct FlatShader final : ShaderProgram
{
    FlatShader() { compile(); }

    void define() override
    {
        auto position = vertexInput(&QuadVertex::position);

        setPosition(float4(position, 0.f, 1.f));
        setFragment(color);
    }

    Uniform<Float4> color;

    EACP_SHADER(color)
};

struct FlatView final : GPUView
{
    FlatView()
    {
        setSampleCount(1);

        shader.color = Array {0.25f, 0.5f, 0.75f, 1.f};
        shader.setVertices(fullScreenTriangle);
        shader.prepare(sampleCount());
    }

    void render(Frame& frame) override
    {
        auto pass = frame.beginPass({Graphics::Color::black()});
        pass.draw(shader);
    }

    FlatShader shader;
};

struct RestoreOptions
{
    RestoreOptions()
        : saved(Dxc::getOptions())
    {
    }

    ~RestoreOptions() { Dxc::setOptions(saved); }

    Dxc::Options saved;
};

bool wantsTheRelease()
{
    return getEnvValue("EACP_TEST_DXC") == "1";
}

bool kernelComputesSquares()
{
    auto values = Vector<float> {};

    for (auto i = 0; i < elementCount; ++i)
        values.add(static_cast<float>(i) * 0.5f);

    constexpr auto byteCount = static_cast<int>(sizeof(float)) * elementCount;
    auto& device = Device::shared();
    auto input = Buffer {device, values.data(), byteCount, BufferUsage::Storage};
    auto output = device.makeBuffer(byteCount);

    auto kernel = SquareKernel {};
    kernel.input = input;
    kernel.output = output;
    kernel.prepare();

    if (!kernel.isValid())
        return false;

    auto commands = device.makeCommandBuffer();

    {
        auto pass = commands.beginCompute();
        pass.dispatch(kernel, elementCount);
    }

    commands.commit();

    auto results = Vector<float> {};
    results.resize(elementCount);
    output.read(results.data(), byteCount);

    for (auto i = 0; i < elementCount; ++i)
        if (results[i] != values[i] * values[i] + 1.0f)
            return false;

    return true;
}

bool isNear(float value, float target)
{
    return std::abs(value - target) <= 0.01f;
}

bool renderDrawsTheColour()
{
    auto view = FlatView {};

    if (!view.shader.pipeline().isValid())
        return false;

    view.setBounds({0.f, 0.f, 16.f, 16.f});
    auto image = view.renderToImage(1.f);

    if (!image.isValid())
        return false;

    const auto pixel = image.at(8, 8);
    return isNear(pixel.r, 0.25f) && isNear(pixel.g, 0.5f) && isNear(pixel.b, 0.75f);
}

void enableWithLocator(std::function<FilePath()> locate)
{
    Dxc::setOptions({.enabled = true, .locateLibrary = std::move(locate)});
}

// Under EACP_TEST_DXC=1 a compiler that will not load is a failure, not a
// skip: the suite would otherwise report DXC green having run none of it.
bool enableTheRelease(Vector<std::string> arguments = Dxc::Options {}.arguments)
{
    auto info = Dxc::releaseInfo();
    info.timeout = Time::MS {10 * 60 * 1000};
    Dxc::setOptions({.enabled = true,
                     .locateLibrary = Dxc::onlineResourceLocator(info),
                     .arguments = std::move(arguments)});

    const auto status = Dxc::load();
    const auto reason = "EACP_TEST_DXC=1 but DXC did not load: " + status.error;

    check(status.state == Dxc::State::loaded, reason);
    return status.state == Dxc::State::loaded;
}
} // namespace

auto tDefaultIsOff = test("Dxc/defaultOptionsLeaveItDisabled") = []
{
    auto restore = RestoreOptions {};
    Dxc::setOptions({});

    check(!Dxc::isEnabled());
    check(Dxc::status().state == Dxc::State::disabled);
    check(Dxc::load().state == Dxc::State::disabled);
};

auto tNothingToLoad = test("Dxc/anEmptyLocationFallsBackToFxc") = []
{
    if (!Device::shared().isValid())
        return;

    auto restore = RestoreOptions {};
    auto calls = 0;
    enableWithLocator(
        [&calls]
        {
            ++calls;
            return FilePath {};
        });

    check(Dxc::status().state == Dxc::State::notLoaded);

    const auto before = Dxc::status().compiledStages;

    check(kernelComputesSquares());
    check(renderDrawsTheColour());

    const auto after = Dxc::status();
    check(calls == 1);
    check(after.state == Dxc::State::failed);
    check(!after.error.empty());
    check(after.compiledStages == before);

    Dxc::setOptions({});
    check(Dxc::status().state == Dxc::State::disabled);
    check(Dxc::status().error.empty());
    check(kernelComputesSquares());
};

auto tNoCompilerThere = test("Dxc/aFolderWithoutTheCompilerSaysWhichDll") = []
{
    auto restore = RestoreOptions {};
    enableWithLocator([] { return FilePath::tempDirectory(); });

    const auto status = Dxc::load();
    check(status.state == Dxc::State::failed);
    check(status.directory == FilePath::tempDirectory());
    check(status.error.find("dxcompiler.dll") != std::string::npos);
    check(status.version.empty());
};

auto tThrowingLocator = test("Dxc/aLocatorThatThrowsKeepsItsMessage") = []
{
    auto restore = RestoreOptions {};
    enableWithLocator([]() -> FilePath { throw std::runtime_error {"no network"}; });

    const auto status = Dxc::load();
    check(status.state == Dxc::State::failed);
    check(status.error.find("no network") != std::string::npos);

    if (Device::shared().isValid())
        check(kernelComputesSquares());
};

auto tNewOptionsLocateAgain = test("Dxc/settingOptionsAgainLocatesAgain") = []
{
    auto restore = RestoreOptions {};
    auto calls = 0;
    auto locate = [&calls]
    {
        ++calls;
        return FilePath {};
    };

    enableWithLocator(locate);
    Dxc::load();
    Dxc::load();
    check(calls == 1);

    enableWithLocator(locate);
    check(Dxc::status().state == Dxc::State::notLoaded);
    Dxc::load();
    check(calls == 2);
};

auto tRelease = test("Dxc/theReleaseCompilesKernelsAndPipelines") = []
{
    if (!wantsTheRelease() || !Device::shared().isValid())
        return;

    auto restore = RestoreOptions {};

    if (!enableTheRelease())
        return;

    const auto loaded = Dxc::status();
    check(loaded.version.starts_with("1."));
    check(loaded.directory.fileName() == Dxc::architectureFolder());
    check(Files::exists(loaded.directory / "dxcompiler.dll"));

    check(kernelComputesSquares());
    check(renderDrawsTheColour());

    const auto after = Dxc::status();
    const auto reason = "DXC was dropped while compiling: " + after.error;
    check(after.state == Dxc::State::loaded, reason);
    check(after.compiledStages == loaded.compiledStages + 3);
};

auto tBrokenShader = test("Dxc/aShaderDxcRejectsLeavesTheLibraryInvalid") = []
{
    if (!wantsTheRelease() || !Device::shared().isValid())
        return;

    auto restore = RestoreOptions {};

    if (!enableTheRelease())
        return;

    const auto before = Dxc::status().compiledStages;
    auto source = ShaderSource::hlsl(
        "[numthreads(64, 1, 1)] void computeMain(uint3 id : SV_DispatchThreadID)"
        " { undeclared[id.x] = 1; }");
    source.withCompute("computeMain");

    auto library = ShaderLibrary {Device::shared(), source};

    check(!library.isValid());
    check(Dxc::status().compiledStages == before);
    check(Dxc::status().state == Dxc::State::loaded);
};

// Options::arguments reach the compile: the same source builds until a define
// passed through them takes the branch that does not.
auto tArguments = test("Dxc/argumentsReachTheCompiler") = []
{
    if (!wantsTheRelease() || !Device::shared().isValid())
        return;

    auto restore = RestoreOptions {};
    auto source =
        ShaderSource::hlsl("RWStructuredBuffer<uint> output : register(u0);\n"
                           "[numthreads(64, 1, 1)]\n"
                           "void computeMain(uint3 id : SV_DispatchThreadID)\n"
                           "{\n"
                           "#ifdef EACP_DXC_BREAK\n"
                           "    undeclared[id.x] = 1;\n"
                           "#endif\n"
                           "    output[id.x] = id.x;\n"
                           "}\n");
    source.withCompute("computeMain");

    if (!enableTheRelease({"-O1"}))
        return;

    check(ShaderLibrary {Device::shared(), source}.isValid());

    if (!enableTheRelease({"-O1", "-D", "EACP_DXC_BREAK"}))
        return;

    check(!ShaderLibrary {Device::shared(), source}.isValid());
};
} // namespace DxcTests
