#include "ShaderCompiler.h"

#include "Backends.h"

#include <eacp/Core/App/Args.h>
#include <eacp/Core/Utils/Console.h>
#include <eacp/Core/Utils/Files.h>
#include <eacp/Core/Utils/Hash.h>
#include <eacp/Core/Utils/StdPath.h>
#include <eacp/Core/Utils/Strings.h>
#include <eacp/GPU/Codegen/ShaderBuilder.h>
#include <eacp/GPU/Codegen/ShaderSets.h>
#include <eacp/GPU/Shader/PrecompiledNames.h>

#include <exception>
#include <functional>

namespace eacp::GPU::ShaderCompiler
{
namespace
{
using Detail::Blob;
using Detail::MetalSdk;

constexpr auto noShaderSetsError =
    "no shader sets: write EACP_SHADER_SET() { visit<MyShader>(); } "
    "(eacp/GPU/Codegen/ShaderSets.h) in a translation unit listed in "
    "eacp_add_precompiled_shaders' SOURCES";

struct MetalPlan
{
    MetalSdk sdk;
    std::string minimumOs;
};

// "<sdk>=<version>" names one SDK; a bare version names every one.
std::optional<std::string> minimumOsFor(const Options& options, const MetalSdk& sdk)
{
    auto chosen = std::optional<std::string> {};

    for (const auto& entry: options.metalMinimumOs)
    {
        auto equals = entry.find('=');

        if (equals == std::string::npos)
            chosen = entry;
        else if (std::string_view {entry}.substr(0, equals) == sdk.name)
            return entry.substr(equals + 1);
    }

    return chosen;
}

std::optional<std::string> invalidMinimumOs(const Options& options)
{
    for (const auto& entry: options.metalMinimumOs)
    {
        auto equals = entry.find('=');
        auto version =
            equals == std::string::npos ? entry : entry.substr(equals + 1);

        if (equals != std::string::npos
            && !Detail::findMetalSdk(entry.substr(0, equals)))
            return Strings::concat("--metal-min-os names no SDK called '",
                                   entry.substr(0, equals),
                                   "'");

        if (!Detail::isValidOsVersion(version))
            return Strings::concat(
                "--metal-min-os needs a version such as 11.0, not '", entry, "'");
    }

    return std::nullopt;
}

std::string describeShader(int set, int graph, const ShaderGraph& shader)
{
    return Strings::concat("shader set ",
                           set + 1,
                           ", graph ",
                           graph + 1,
                           " (",
                           shader.isCompute() ? "compute kernel" : "render shader",
                           ")");
}

FilePath absolutePathOf(const FilePath& path)
{
    auto error = std::error_code {};
    auto absolute = std::filesystem::absolute(toStdPath(path), error);

    if (error)
        return path;

    return FilePath {absolute.lexically_normal()};
}

bool isBlobKey(std::string_view text)
{
    auto value = Strings::parseHex(text);
    return value && Hashing::toHex(*value) == text;
}

// <key>.<anything>.metallib, .dxbc or .spv: a name this compiler writes.
bool isBlobName(std::string_view name)
{
    constexpr auto keyLength = std::size_t {16};

    if (name.size() <= keyLength || name[keyLength] != '.'
        || !isBlobKey(name.substr(0, keyLength)))
        return false;

    return name.ends_with(".metallib") || name.ends_with(".dxbc")
           || name.ends_with(".spv");
}

std::string manifestTextOf(const Vector<FilePath>& blobs)
{
    auto text = std::string {};

    for (const auto& blob: blobs)
        text += blob.str() + "\n";

    return text;
}

class CompileSession
{
public:
    CompileSession(const Options& optionsToUse, Report& reportToUse)
        : options(optionsToUse)
        , report(reportToUse)
        , outputDirectory(absolutePathOf(optionsToUse.outputDirectory))
    {
    }

    bool prepare()
    {
        if (options.outputDirectory.empty())
            return fail("--out names no directory");

        if (!Files::createDirectories(outputDirectory))
            return fail("cannot create " + outputDirectory.str());

        if (options.metal && !prepareMetal())
            return false;

        if (options.dxbc && !Detail::isDxbcAvailable())
            return fail(
                "DXBC can only be compiled on a Windows host; pass --no-dxbc");

        if (options.spirv)
        {
            spirvTargets = Detail::spirvTargets();

            if (spirvTargets.empty())
                return fail("this build of the shader compiler has no SPIR-V "
                            "compiler (EACP_BUILD_SPIRV is off); pass --no-spirv");
        }

        return true;
    }

    void visit(int set, int graph, const ShaderGraph& shader)
    {
        if (failed())
            return;

        current = describeShader(set, graph, shader);

        if (options.metal)
            compileMetal(shader);

        if (options.dxbc && !failed())
            compileDxbc(shader);

        if (options.spirv && !failed())
            compileSpirv(shader);
    }

    // Deletes every blob in the output directory this run did not list, so the
    // directory holds exactly what the manifest names. Only after a run that
    // succeeded: a failed one has no list to mirror.
    void removeSupersededBlobs()
    {
        auto current = Vector<std::string> {};

        for (const auto& blob: report.blobs)
            current.add(blob.fileName());

        for (const auto& file: Files::listFiles(outputDirectory))
        {
            if (!isBlobName(file.fileName()) || current.contains(file.fileName()))
                continue;

            if (Files::removeFile(file))
            {
                report.removed.add(file);
                note("removed  " + file.str());
            }
        }
    }

    bool failed() const { return !report.error.empty(); }

    bool fail(std::string error)
    {
        report.error = std::move(error);
        return false;
    }

private:
    bool prepareMetal()
    {
        if (auto invalid = invalidMinimumOs(options))
            return fail(*invalid);

        for (const auto& name: options.metalSdks)
        {
            auto sdk = Detail::findMetalSdk(name);

            if (!sdk)
                return fail(Strings::concat(
                    "--metal-sdk must be macosx, iphoneos or iphonesimulator, not '",
                    name,
                    "'"));

            if (!Detail::probeMetalToolchain(sdk->name))
                return fail(Strings::concat(
                    "the Metal compiler for the ",
                    sdk->name,
                    " SDK cannot run (`xcrun -sdk ",
                    sdk->name,
                    " metal --version` failed). Install it with `xcodebuild "
                    "-downloadComponent MetalToolchain`, or pass --no-metal"));

            auto minimumOs =
                minimumOsFor(options, *sdk).value_or(sdk->defaultMinimumOs);
            metalPlans.add(MetalPlan {*sdk, std::move(minimumOs)});
        }

        return true;
    }

    void compileMetal(const ShaderGraph& shader)
    {
        auto text = buildShader(shader, ShaderBackend::Metal).source.source;

        if (text.empty())
            return;

        auto key = Precompiled::keyFor(text);
        auto bfloat = shader.usesPackedSimdMatrix(SimdMatrixElement::BFloat16);

        for (const auto& plan: metalPlans)
        {
            auto target = Detail::metalTargetFor(plan.sdk, plan.minimumOs, bfloat);

            produce(Precompiled::resourceName(key, plan.sdk.suffix),
                    Strings::concat(target.sdk.name, " ", target.languageStandard),
                    [&] { return Detail::compileMetal(target, text); });
        }
    }

    void compileDxbc(const ShaderGraph& shader)
    {
        auto generated = buildShader(shader, ShaderBackend::DirectX).source;

        if (generated.source.empty())
            return;

        auto key = Precompiled::keyFor(generated);

        auto stage = [&](std::string_view suffix,
                         const std::string& entry,
                         const char* profile)
        {
            produce(
                Precompiled::resourceName(key, suffix),
                profile,
                [&]
                { return Detail::compileDxbc(generated.source, entry, profile); });
        };

        if (generated.isCompute())
        {
            stage(Precompiled::computeDxbcSuffix, generated.computeEntry, "cs_5_0");
            return;
        }

        stage(Precompiled::vertexDxbcSuffix, generated.vertexEntry, "vs_5_0");

        if (generated.hasFragment())
            stage(Precompiled::pixelDxbcSuffix, generated.fragmentEntry, "ps_5_0");
    }

    void compileSpirv(const ShaderGraph& shader)
    {
        auto generated = buildShader(shader, ShaderBackend::Vulkan).source;

        if (generated.source.empty())
            return;

        auto key = Precompiled::keyFor(generated);

        for (auto target: spirvTargets)
        {
            auto stage = [&](Spirv::Stage spirvStage)
            {
                produce(Precompiled::spirvResourceName(key, target, spirvStage),
                        Strings::concat(Spirv::targetName(target),
                                        Precompiled::spirvStageSuffix(spirvStage)),
                        [&]
                        {
                            return Detail::compileSpirv(
                                spirvStage, target, generated.source);
                        });
            };

            if (generated.isCompute())
            {
                stage(Spirv::Stage::Compute);
                continue;
            }

            stage(Spirv::Stage::Vertex);

            if (generated.hasFragment())
                stage(Spirv::Stage::Fragment);
        }
    }

    void produce(const std::string& fileName,
                 const std::string& what,
                 const std::function<Blob()>& compileBlob)
    {
        if (failed())
            return;

        auto path = outputDirectory / fileName;

        if (report.blobs.contains(path))
            return;

        if (Files::isFile(path))
        {
            report.blobs.add(path);
            note("present  " + path.str());
            return;
        }

        auto blob = compileBlob();

        if (!blob.succeeded)
        {
            auto log = blob.log.empty()
                           ? std::string {"the compiler reported nothing"}
                           : blob.log;
            fail(
                Strings::concat(current, " did not compile for ", what, ":\n", log));
            return;
        }

        if (!blob.log.empty())
            note(blob.log);

        try
        {
            Files::writeFileAtomically(path, blob.bytes);
        }
        catch (const std::exception& error)
        {
            fail(Strings::concat("cannot write ", path.str(), ": ", error.what()));
            return;
        }

        report.blobs.add(path);
        report.compiled.add(path);
        note("compiled " + path.str());
    }

    void note(std::string_view line) const
    {
        if (options.verbose)
            Console::print(line);
    }

    const Options& options;
    Report& report;
    FilePath outputDirectory;
    Vector<MetalPlan> metalPlans;
    Vector<Spirv::Target> spirvTargets;
    std::string current;
};

void writeManifest(const Options& options, Report& report)
{
    if (options.manifest.empty())
        return;

    auto manifest = absolutePathOf(options.manifest);

    try
    {
        Files::createDirectories(manifest.parentDirectory());
        Files::writeFile(manifest, std::string_view {manifestTextOf(report.blobs)});
    }
    catch (const std::exception& error)
    {
        report.error =
            Strings::concat("cannot write ", manifest.str(), ": ", error.what());
    }
}

// A failed run leaves no manifest, so nothing downstream mistakes the last
// good one for this run's.
void removeManifest(const Options& options)
{
    if (!options.manifest.empty())
        Files::removeFile(options.manifest);
}
} // namespace

bool Report::succeeded() const
{
    return error.empty();
}

bool isSpirvAvailable()
{
    return !Detail::spirvTargets().empty();
}

bool isMetalToolchainAvailable(std::string_view sdk)
{
    return Detail::probeMetalToolchain(sdk);
}

std::string keyFor(std::string_view source)
{
    return Precompiled::keyFor(source);
}

Vector<ShaderSet> registeredSets()
{
    auto sets = Vector<ShaderSet> {};

    for (const auto* set: ShaderSets::registered())
        sets.add([set](const ShaderGraphVisitor& visitor) { set->run(visitor); });

    return sets;
}

Report compile(const Options& options, const Vector<ShaderSet>& sets)
{
    auto report = Report {};
    auto session = CompileSession {options, report};

    if (sets.empty())
    {
        session.fail(noShaderSetsError);
        removeManifest(options);
        return report;
    }

    try
    {
        if (session.prepare())
        {
            for (auto set = 0; set < sets.size(); ++set)
            {
                auto graph = 0;
                sets[set]([&](const ShaderGraph& shader)
                          { session.visit(set, graph++, shader); });
            }
        }
    }
    catch (const std::exception& error)
    {
        session.fail(Strings::concat("the shader compiler stopped: ", error.what()));
    }

    if (report.succeeded())
    {
        session.removeSupersededBlobs();
        writeManifest(options, report);
    }
    else
        removeManifest(options);

    return report;
}

Report compile(const Options& options)
{
    return compile(options, registeredSets());
}

int run(const Options& options, const Vector<ShaderSet>& sets)
{
    auto report = compile(options, sets);

    if (!report.succeeded())
    {
        Console::printError("eacp shader compiler: " + report.error);
        return 1;
    }

    if (options.verbose)
        Console::print(Strings::concat(report.blobs.size(),
                                       " shader blobs, ",
                                       report.compiled.size(),
                                       " compiled this run, ",
                                       report.removed.size(),
                                       " superseded removed"));

    return 0;
}

int run(const Options& options)
{
    return run(options, registeredSets());
}

int runMain(int argc, const char* const* argv, const Vector<ShaderSet>& sets)
{
    auto options = Options {};
    auto noMetal = false;
    auto noDxbc = false;
    auto noSpirv = false;

    auto cli = Args::Parser {
        "ShaderCompiler",
        "Compiles an app's shaders ahead of time into blobs for ResEmbed."};

    cli.option("--out", options.outputDirectory, "the directory the blobs go in")
        .hint("<dir>")
        .required();
    cli.option("--manifest", options.manifest, "the list of blobs, one per line")
        .hint("<file>");
    cli.option("--metal-sdk", options.metalSdks, "an SDK to build a metallib for")
        .choices({"macosx", "iphoneos", "iphonesimulator"})
        .unique();
    cli.option("--metal-min-os",
               options.metalMinimumOs,
               "the oldest OS a metallib loads on, as <version> or <sdk>=<version>")
        .hint("<version>")
        .defaultText("macosx=11.0, iphoneos=15.0, iphonesimulator=15.0");
    cli.flag("--no-metal", noMetal, "build no metallib");
    cli.flag("--no-dxbc", noDxbc, "build no DXBC");
    cli.flag("--no-spirv", noSpirv, "build no SPIR-V");
    cli.flag("--verbose", options.verbose, "print every blob");

    if (auto code = cli.exitCodeFor(cli.parse(argc, argv)))
        return *code;

    options.metal = options.metal && !noMetal;
    options.dxbc = options.dxbc && !noDxbc;
    options.spirv = options.spirv && !noSpirv;

    return run(options, sets);
}

int runMain(int argc, const char* const* argv)
{
    return runMain(argc, argv, registeredSets());
}
} // namespace eacp::GPU::ShaderCompiler
