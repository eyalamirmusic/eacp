#pragma once

#include <eacp/Core/Utils/Common.h>
#include <eacp/Core/Utils/FilePath.h>

// Run-time shader compiles through the DirectX Shader Compiler instead of FXC:
// Shader Model 6.0 DXIL from dxcompiler.dll, loaded at run time from wherever
// locateLibrary says. D3D12 only; everywhere else the options are kept and
// nothing changes. Precompiled blobs and the backend's own internal shaders are
// untouched either way. See "Compiling with DXC at run time" in the README.
namespace eacp::GPU::Dxc
{
struct Options
{
    bool enabled = false;

    // The directory holding dxcompiler.dll, and dxil.dll beside it when the
    // release ships one. Called once, on the thread that compiles the first
    // shader after the options were set, and may block. Empty means there is
    // no compiler to load, and shaders go on compiling through FXC; so does a
    // throw, whose message becomes Status::error.
    std::function<FilePath()> locateLibrary = [] { return FilePath {}; };

    // Passed to every compile after -E <entry> -T <profile> -Ges. -O1 is the
    // level FXC compiles at by default; WARP access-violates running DXC's
    // own -O3 output for the blocked SIMD-matrix kernels (see the README).
    Vector<std::string> arguments {"-O1"};
};

enum class State
{
    disabled,
    notLoaded,
    loading,
    loaded,
    failed,
    unsupported
};

struct Status
{
    State state = State::disabled;

    // IDxcVersionInfo's "major.minor.commits (hash)"; empty until loaded.
    std::string version;

    // What locateLibrary returned.
    FilePath directory;

    // Why the compiler is not in use when the state is failed.
    std::string error;

    // Stages compiled through DXC in this process, under any options.
    int compiledStages = 0;
};

// Takes effect for the next ShaderLibrary built. Setting options drops the
// compiler the previous ones loaded, so the next compile locates it again.
void setOptions(Options options);
Options getOptions();
bool isEnabled();

// Locates and loads the compiler now when enabled and not yet tried, instead
// of at the first compile, and says how that went.
Status load();

Status status();

constexpr std::string_view stateName(State state)
{
    switch (state)
    {
        case State::disabled:
            return "disabled";
        case State::notLoaded:
            return "not loaded";
        case State::loading:
            return "loading";
        case State::loaded:
            return "loaded";
        case State::failed:
            return "failed";
        case State::unsupported:
            return "unsupported";
    }

    return "unknown";
}
} // namespace eacp::GPU::Dxc
