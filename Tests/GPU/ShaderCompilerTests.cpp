#include "CodegenCommon.h"

#include <eacp/Core/App/Args.h>
#include <eacp/Core/Utils/Files.h>
#include <eacp/Core/Utils/Hash.h>
#include <eacp/Core/Utils/TemporaryFile.h>
#include <eacp/GPU/Codegen/ComputeKernel.h>
#include <eacp/GPU/Codegen/ShaderSets.h>
#include <eacp/GPU/ShaderCompiler/Backends.h>
#include <eacp/GPU/ShaderCompiler/ShaderCompiler.h>

// The build-time compiler over a triangle and a kernel, into a scratch
// directory. Metal needs the Xcode Metal Toolchain, which CI's runners and a
// fresh Xcode lack, so its blobs are asserted only where the compiler runs.

namespace ShaderCompilerTests
{
using namespace nano;
using namespace eacp;
using namespace eacp::GPU;

ShaderBuilder recordTriangle()
{
    auto builder = ShaderBuilder {};

    auto position = builder.vertexInput<Float2>();
    auto color = builder.vertexInput<Float3>();
    auto varyingColor = builder.varying(color);

    builder.position(float4(position, 0.0f, 1.0f));
    builder.fragment(float4(varyingColor, 1.0f));

    return builder;
}

struct DoublingKernel final : ComputeKernel
{
    DoublingKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, input[i] * 2.f);
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;

    EACP_SHADER(input, output)
};

struct NegatingKernel final : ComputeKernel
{
    NegatingKernel() { compile(); }

    void define() override
    {
        auto i = threadId();
        write(output, i, -input[i]);
    }

    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;

    EACP_SHADER(input, output)
};

// A constructor argument the graph bakes in, so each factor is its own shader.
struct ScalingKernel final : ComputeKernel
{
    explicit ScalingKernel(float factorToUse)
        : factor(factorToUse)
    {
        compile();
    }

    void define() override
    {
        auto i = threadId();
        write(output, i, input[i] * factor);
    }

    float factor;
    Uniform<InputBuffer> input;
    Uniform<OutputBuffer> output;

    EACP_SHADER(input, output)
};

// visit() is protected, so the overload checks go through a set that opens it.
struct VisitProbe final : ShaderSetDefinition
{
    void define() const override {}

    using ShaderSetDefinition::visit;
};

template <typename... Shaders>
concept VisitsBuiltWithNoArguments =
    requires(const VisitProbe& set) { set.visit<Shaders...>(); };

template <typename Shader, typename... Args>
concept VisitsBuiltFrom = requires(const VisitProbe& set, Args&&... args) {
    set.visit<Shader>(std::forward<Args>(args)...);
};

static_assert(VisitsBuiltWithNoArguments<DoublingKernel>);
static_assert(VisitsBuiltWithNoArguments<DoublingKernel, NegatingKernel>);
static_assert(VisitsBuiltFrom<ScalingKernel, float>);
static_assert(!VisitsBuiltFrom<ScalingKernel, const char*>);
static_assert(!VisitsBuiltFrom<DoublingKernel, float>);

// With no arguments a kernel that needs one still picks the pack form, never
// the constructing one, so its static_assert is the error the caller sees.
static_assert(VisitsBuiltWithNoArguments<ScalingKernel>);

std::string keyOf(const ShaderGraph& graph, ShaderBackend backend)
{
    return Hashing::toHex(
        Hashing::fnv1a64(buildShader(graph, backend).source.source));
}

bool metalCompilerRuns()
{
    return Platform::isMac() && ShaderCompiler::isMetalToolchainAvailable("macosx");
}

ShaderCompiler::Options optionsFor(const TemporaryFile& directory)
{
    auto options = ShaderCompiler::Options {};
    options.outputDirectory = directory.path() / "blobs";
    options.manifest = directory.path() / "shaders.manifest";
    options.metal = metalCompilerRuns();
    return options;
}

Vector<std::string> linesOf(const std::string& text)
{
    auto lines = Vector<std::string> {};
    auto start = std::size_t {0};

    for (auto end = text.find('\n'); end != std::string::npos;
         start = end + 1, end = text.find('\n', start))
        lines.add(text.substr(start, end - start));

    if (start < text.size())
        lines.add(text.substr(start));

    return lines;
}

// Every name the contract gives the triangle and the kernels, for the dialects
// this host compiles.
Vector<std::string> expectedBlobNames(const ShaderCompiler::Options& options,
                                      const Vector<const ShaderGraph*>& kernels)
{
    auto triangle = recordTriangle();
    auto names = Vector<std::string> {};

    if (options.metal)
    {
        names.add(keyOf(triangle.graph(), ShaderBackend::Metal) + ".metallib");

        for (auto* kernel: kernels)
            names.add(keyOf(*kernel, ShaderBackend::Metal) + ".metallib");
    }

    if (options.dxbc)
    {
        auto render = keyOf(triangle.graph(), ShaderBackend::DirectX);
        names.add(render + ".vs.dxbc");
        names.add(render + ".ps.dxbc");

        for (auto* kernel: kernels)
            names.add(keyOf(*kernel, ShaderBackend::DirectX) + ".cs.dxbc");
    }

    if (options.spirv)
    {
        auto render = keyOf(triangle.graph(), ShaderBackend::Vulkan);

        for (auto target: {"vk11spv13", "vk11spv14", "vk13spv16"})
        {
            names.add(Strings::concat(render, ".", target, ".vert.spv"));
            names.add(Strings::concat(render, ".", target, ".frag.spv"));

            for (auto* kernel: kernels)
                names.add(Strings::concat(keyOf(*kernel, ShaderBackend::Vulkan),
                                          ".",
                                          target,
                                          ".comp.spv"));
        }
    }

    return names;
}

Vector<std::string> expectedBlobNames(const ShaderCompiler::Options& options)
{
    auto kernel = DoublingKernel {};
    return expectedBlobNames(options, {&kernel.graph()});
}

bool sameNames(const Vector<FilePath>& blobs, const Vector<std::string>& names)
{
    if (blobs.size() != names.size())
        return false;

    for (const auto& blob: blobs)
        if (!names.contains(blob.fileName()))
            return false;

    return true;
}

// The explicit form the overloads taking a Vector<ShaderSet> are tested with.
void visitTestShaders(const ShaderGraphVisitor& visit)
{
    visit(recordTriangle().graph());
    visit(DoublingKernel {}.graph());
}

// Every form of visit() through the macro, which registers the set in this
// test binary: the only EACP_SHADER_SET() GPUCodegenTests has.
EACP_SHADER_SET()
{
    visit(recordTriangle().graph());
    visit<DoublingKernel, NegatingKernel>();
    visit<ScalingKernel>(3.f);
    visit<ScalingKernel>(5.f);
}

auto tTheMacroRegistersItsSet =
    test("ShaderCompiler/aShaderSetMacroRegistersAndCompilesItsShaders") = []
{
    check(ShaderSets::registered().size() == 1);
    check(ShaderCompiler::registeredSets().size() == 1);

    auto visited = Vector<std::string> {};

    ShaderSets::registered()[0]->run(
        [&](const ShaderGraph& graph)
        { visited.add(keyOf(graph, ShaderBackend::Vulkan)); });

    auto doubling = DoublingKernel {};
    auto negating = NegatingKernel {};
    auto byThree = ScalingKernel {3.f};
    auto byFive = ScalingKernel {5.f};

    check(visited.size() == 5);
    check(visited[0] == keyOf(recordTriangle().graph(), ShaderBackend::Vulkan));
    check(visited[1] == keyOf(doubling.graph(), ShaderBackend::Vulkan));
    check(visited[2] == keyOf(negating.graph(), ShaderBackend::Vulkan));
    check(visited[3] == keyOf(byThree.graph(), ShaderBackend::Vulkan));
    check(visited[4] == keyOf(byFive.graph(), ShaderBackend::Vulkan));
    check(visited[3] != visited[4]);

    auto directory = TemporaryFile {};
    auto options = optionsFor(directory);
    auto report = ShaderCompiler::compile(options);

    check(report.succeeded(), report.error);
    check(sameNames(report.blobs,
                    expectedBlobNames(options,
                                      {&doubling.graph(),
                                       &negating.graph(),
                                       &byThree.graph(),
                                       &byFive.graph()})));
};

auto tNoSetsIsAnError = test("ShaderCompiler/failsWithNoShaderSets") = []
{
    auto directory = TemporaryFile {};
    auto options = optionsFor(directory);

    Files::createDirectories(directory.path());
    Files::writeFile(options.manifest, std::string_view {"stale\n"});

    auto report = ShaderCompiler::compile(options, {});

    check(!report.succeeded());
    check(contains(report.error, "EACP_SHADER_SET()"), report.error);
    check(contains(report.error, "SOURCES"), report.error);
    check(!Files::exists(options.manifest));
};

auto tWritesEveryBlobAndTheManifest =
    test("ShaderCompiler/writesEveryBlobItsHostCompilesAndListsThem") = []
{
    auto directory = TemporaryFile {};
    auto options = optionsFor(directory);

    auto report = ShaderCompiler::compile(options, {&visitTestShaders});

    check(report.succeeded(), report.error);
    check(options.spirv == ShaderCompiler::isSpirvAvailable());
    check(options.dxbc == Platform::isWindows());
    check(sameNames(report.blobs, expectedBlobNames(options)));
    check(report.compiled.size() == report.blobs.size());

    if (ShaderCompiler::isSpirvAvailable())
        check(report.blobs.size() >= 9);

    for (const auto& blob: report.blobs)
    {
        check(blob.isAbsolute(), blob.str());
        check(Files::isFile(blob), blob.str());
        check(!Files::readFile(blob).empty(), blob.str());
    }

    auto listed = linesOf(Files::readFile(options.manifest));
    check(listed.size() == report.blobs.size());

    for (const auto& blob: report.blobs)
        check(listed.contains(blob.str()), blob.str());
};

auto tTheKeyIsTheHashOfWhatBuildEmits =
    test("ShaderCompiler/namesABlobByTheHashOfTheTextBuildEmits") = []
{
    auto triangle = recordTriangle();
    auto kernel = DoublingKernel {};
    auto native = detail::nativeShaderBackend();

    check(buildShader(triangle.graph(), native).source.source
          == triangle.build().source.source);
    check(buildShader(kernel.graph(), native).source.source
          == kernel.source().source);

    check(ShaderCompiler::keyFor(triangle.build().source.source)
          == keyOf(triangle.graph(), native));
    check(ShaderCompiler::keyFor(emitGlsl(triangle.graph()))
          == keyOf(triangle.graph(), ShaderBackend::Vulkan));
    check(ShaderCompiler::keyFor(emitHlsl(kernel.graph()))
          == keyOf(kernel.graph(), ShaderBackend::DirectX));
    check(ShaderCompiler::keyFor(emitMetal(kernel.graph()))
          == keyOf(kernel.graph(), ShaderBackend::Metal));
    check(ShaderCompiler::keyFor("").size() == 16);
};

auto tASecondRunCompilesNothing =
    test("ShaderCompiler/aSecondRunFindsEveryBlobAndCompilesNone") = []
{
    auto directory = TemporaryFile {};
    auto options = optionsFor(directory);
    auto sets = Vector<ShaderCompiler::ShaderSet> {&visitTestShaders};

    auto first = ShaderCompiler::compile(options, sets);
    auto firstManifest = Files::readFile(options.manifest);
    auto second = ShaderCompiler::compile(options, sets);

    check(first.succeeded(), first.error);
    check(second.succeeded(), second.error);
    check(second.compiled.empty());
    check(second.blobs == first.blobs);
    check(Files::readFile(options.manifest) == firstManifest);
};

auto tAShaderVisitedTwiceIsListedOnce =
    test("ShaderCompiler/listsAShaderTwoSetsShareOnce") = []
{
    auto directory = TemporaryFile {};
    auto options = optionsFor(directory);

    auto report =
        ShaderCompiler::compile(options, {&visitTestShaders, &visitTestShaders});

    check(report.succeeded(), report.error);
    check(sameNames(report.blobs, expectedBlobNames(options)));
};

// A blob no shader names any more - an edited shader's old key - is deleted, so
// the directory mirrors the manifest; anything not named like a blob stays. A
// failed run has no list to mirror and deletes nothing.
auto tSupersededBlobsArePruned =
    test("ShaderCompiler/removesSupersededBlobsOnlyAfterASuccessfulRun") = []
{
    auto directory = TemporaryFile {};
    auto options = optionsFor(directory);
    auto sets = Vector<ShaderCompiler::ShaderSet> {&visitTestShaders};

    auto superseded = Vector<FilePath> {
        options.outputDirectory / "0123456789abcdef.metallib",
        options.outputDirectory / "0123456789abcdef.vs.dxbc",
        options.outputDirectory / "0123456789abcdef.vk13spv16.frag.spv"};
    auto unrelated =
        Vector<FilePath> {options.outputDirectory / "notes.txt",
                          options.outputDirectory / "FEDCBA9876543210.metallib",
                          options.outputDirectory / "0123456789abcdef.spv.txt"};

    Files::createDirectories(options.outputDirectory);

    for (const auto& file: superseded)
        Files::writeFile(file, std::string_view {"old"});

    for (const auto& file: unrelated)
        Files::writeFile(file, std::string_view {"keep"});

    auto failing = options;
    failing.metal = true;
    failing.metalSdks = {"watchos"};

    check(!ShaderCompiler::compile(failing, sets).succeeded());

    for (const auto& file: superseded)
        check(Files::isFile(file), file.str());

    auto report = ShaderCompiler::compile(options, sets);

    check(report.succeeded(), report.error);
    check(report.removed.size() == superseded.size());

    auto removedNames = Vector<std::string> {};

    for (const auto& file: report.removed)
        removedNames.add(file.fileName());

    for (const auto& file: superseded)
    {
        check(!Files::exists(file), file.str());
        check(removedNames.contains(file.fileName()), file.str());
    }

    for (const auto& file: unrelated)
        check(Files::isFile(file), file.str());

    for (const auto& blob: report.blobs)
        check(Files::isFile(blob), blob.str());

    auto listed = linesOf(Files::readFile(options.manifest));
    check(listed.size() == report.blobs.size());

    auto again = ShaderCompiler::compile(options, sets);
    check(again.succeeded(), again.error);
    check(again.removed.empty());
};

auto tAnUnknownSdkIsRefused = test("ShaderCompiler/refusesAnUnknownMetalSdk") = []
{
    auto directory = TemporaryFile {};
    auto options = optionsFor(directory);
    options.metal = true;
    options.metalSdks = {"watchos"};

    Files::createDirectories(directory.path());
    Files::writeFile(options.manifest, std::string_view {"stale\n"});

    auto report = ShaderCompiler::compile(options, {&visitTestShaders});

    check(!report.succeeded());
    check(contains(report.error, "watchos"), report.error);
    check(report.blobs.empty());
    check(!Files::exists(options.manifest));
};

auto tRunMainReadsTheCommandLine =
    test("ShaderCompiler/runMainTakesTheContractCommandLine") = []
{
    auto directory = TemporaryFile {};
    auto out = (directory.path() / "out").str();
    auto manifest = (directory.path() / "out.manifest").str();

    const char* arguments[] = {"ShaderHost",
                               "--out",
                               out.c_str(),
                               "--manifest",
                               manifest.c_str(),
                               "--no-metal"};

    auto code = ShaderCompiler::runMain(6, arguments, {&visitTestShaders});

    check(code == 0);
    check(Files::isFile(manifest));

    auto lines = linesOf(Files::readFile(manifest));

    for (const auto& line: lines)
        check(!contains(line, ".metallib"), line);

    if (ShaderCompiler::isSpirvAvailable() || Platform::isWindows())
        check(!lines.empty());

    const char* missingOut[] = {"ShaderHost", "--no-metal"};
    check(ShaderCompiler::runMain(2, missingOut, {}) == Args::badArguments);
};

auto tMetalFlagsFollowTheMinimumOs =
    test("ShaderCompiler/picksTheMetalLanguageTheMinimumOsHas") = []
{
    using ShaderCompiler::Detail::findMetalSdk;
    using ShaderCompiler::Detail::metalTargetFor;

    auto mac = *findMetalSdk("macosx");
    auto phone = *findMetalSdk("iphoneos");
    auto simulator = *findMetalSdk("iphonesimulator");

    check(mac.suffix == ".metallib");
    check(phone.suffix == ".ios.metallib");
    check(simulator.suffix == ".iossim.metallib");
    check(!findMetalSdk("watchos"));

    check(metalTargetFor(mac, "11.0", false).languageStandard == "macos-metal2.3");
    check(metalTargetFor(mac, "13.0", false).languageStandard == "metal3.0");
    check(metalTargetFor(phone, "15.0", false).languageStandard == "ios-metal2.4");
    check(metalTargetFor(simulator, "17.2", false).languageStandard == "metal3.1");
    check(metalTargetFor(mac, "15.4", false).languageStandard == "metal3.2");
    check(metalTargetFor(mac, "26.0", false).languageStandard == "metal4.0");
    check(metalTargetFor(phone, "27.0", false).languageStandard == "metal4.1");
    check(metalTargetFor(mac, "28.0", false).languageStandard == "metal4.1");

    check(mac.versionFlag == "-mmacosx-version-min=");
    check(phone.versionFlag == "-mios-version-min=");
    check(simulator.versionFlag == "-mios-simulator-version-min=");

    auto bfloat = metalTargetFor(mac, "11.0", true);
    check(bfloat.languageStandard == "metal3.1");
    check(bfloat.minimumOs == "14.0");
    check(metalTargetFor(mac, "15.0", true).minimumOs == "15.0");

    check(ShaderCompiler::Detail::isValidOsVersion("10.15"));
    check(!ShaderCompiler::Detail::isValidOsVersion("eleven"));
};
} // namespace ShaderCompilerTests
