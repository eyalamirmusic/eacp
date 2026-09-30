#pragma once

// Device-free: GPUCodegenTests links eacp-gpu-codegen, and eacp-cpu-compute to
// run the graphs it emits.
#include <eacp/Core/Platform/Platform.h>
#include <eacp/GPU/Codegen/ShaderBindings.h>
#include <eacp/GPU/Codegen/ShaderBuilder.h>
#include <eacp/GPU/Codegen/ShaderEmitter.h>
#include <eacp/GPU/Codegen/UniformLayout.h>
#include <eacp/GPU/CpuCompute/CpuCompute.h>
#include <eacp/GPU/Frame/ComputePass.h>
#include <eacp/GPU/Frame/RenderPass.h>

#include <NanoTest/NanoTest.h>

#ifdef EACP_HAS_SPIRV
#include <eacp/GPU/Spirv/SpirvCompiler.h>
#endif

#include <eacp/Core/Process/Process.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <random>
#include <source_location>
#include <string>

// Where eacp-spirv is built (Linux by default) the emitted GLSL is compiled.

#ifdef EACP_HAS_SPIRV
inline void expectStageCompiles(eacp::GPU::Spirv::Stage stage,
                                const std::string& glsl,
                                const std::source_location& location)
{
    using eacp::GPU::Spirv::Target;

    for (auto target: {Target::vulkan13Spirv16, Target::vulkan11Spirv13})
    {
        const auto result = eacp::GPU::Spirv::compileGlsl(stage, glsl, target);
        nano::check(result.succeeded(), result.log, location);
    }
}
#endif

inline void expectGlslCompiles(
    const std::string& glsl,
    bool isCompute = false,
    const std::source_location& location = std::source_location::current())
{
#ifdef EACP_HAS_SPIRV
    using eacp::GPU::Spirv::Stage;

    if (isCompute)
    {
        expectStageCompiles(Stage::Compute, glsl, location);
        return;
    }

    expectStageCompiles(Stage::Vertex, glsl, location);
    expectStageCompiles(Stage::Fragment, glsl, location);
#else
    (void) glsl;
    (void) isCompute;
    (void) location;
#endif
}

// Emitted WGSL is validated by naga (`cargo install naga-cli`) wherever one is
// found: EACP_NAGA, then the PATH, then the two places cargo installs to. With
// none the check is skipped, and says so once.
inline std::string nagaPath()
{
    namespace fs = std::filesystem;

    if (const auto* given = std::getenv("EACP_NAGA"))
        return given;

#ifdef _WIN32
    constexpr auto executable = "naga.exe";
    constexpr auto separator = ';';
#else
    constexpr auto executable = "naga";
    constexpr auto separator = ':';
#endif

    auto candidates = eacp::Vector<fs::path> {};

    if (const auto* path = std::getenv("PATH"))
    {
        auto directories = std::string(path);
        auto start = std::size_t {0};

        while (start <= directories.size())
        {
            auto end = directories.find(separator, start);

            if (end == std::string::npos)
                end = directories.size();

            if (end > start)
                candidates.add(fs::path(directories.substr(start, end - start))
                               / executable);

            start = end + 1;
        }
    }

    if (const auto* home = std::getenv("HOME"))
    {
        candidates.add(fs::path(home) / ".local" / "bin" / executable);
        candidates.add(fs::path(home) / ".cargo" / "bin" / executable);
    }

    auto error = std::error_code {};

    for (const auto& candidate: candidates)
        if (fs::is_regular_file(candidate, error))
            return candidate.string();

    return {};
}

inline const std::string& naga()
{
    static const auto path = nagaPath();
    return path;
}

inline void expectWgslValid(
    const std::string& wgsl,
    const std::source_location& location = std::source_location::current())
{
    namespace fs = std::filesystem;

    if (naga().empty())
    {
        static auto noted = false;

        if (!noted)
            std::fprintf(stderr,
                         "note: no naga found, emitted WGSL is not validated "
                         "(cargo install naga-cli, or set EACP_NAGA)\n");

        noted = true;
        return;
    }

    // ctest runs each case as its own process, so the name must differ across
    // processes as well as within one.
    static const auto run = std::to_string(std::random_device {}());
    static auto counter = std::atomic<int> {0};

    auto file = fs::temp_directory_path()
                / ("eacp-wgsl-" + run + "-" + std::to_string(counter++) + ".wgsl");

    {
        auto out = std::ofstream(file, std::ios::binary);
        out << wgsl;
    }

    auto result = eacp::Processes::run(naga(), {file.string()});

    auto error = std::error_code {};
    fs::remove(file, error);

    nano::check(result.exited && result.exitCode == 0,
                result.output + result.errorOutput + "\n" + wgsl,
                location);
}

inline void expectWgslValid(
    const eacp::GPU::ShaderGraph& graph,
    const std::source_location& location = std::source_location::current())
{
    expectWgslValid(eacp::GPU::emitWgsl(graph), location);
}

// Every graph a suite checks as GLSL is checked as WGSL too.
inline void expectGlslCompiles(
    const eacp::GPU::ShaderGraph& graph,
    const std::source_location& location = std::source_location::current())
{
    expectGlslCompiles(eacp::GPU::emitGlsl(graph), graph.isCompute(), location);
    expectWgslValid(graph, location);
}

// The `.../runs` cases dispatch the same graph on the CPU executor.

template <typename T>
eacp::Vector<T> filledWith(int count, T value)
{
    auto values = eacp::Vector<T> {};
    values.resize(count, value);
    return values;
}

inline void expectPlans(
    const eacp::GPU::CpuCompute::Executor& executor,
    const std::source_location& location = std::source_location::current())
{
    nano::check(executor.isValid(), executor.reason(), location);
}
