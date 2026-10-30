#pragma once

#include <eacp/Core/Platform/Platform.h>
#include <eacp/Core/Utils/Containers.h>
#include <eacp/Core/Utils/FilePath.h>
#include <eacp/GPU/Codegen/Forward.h>

#include <functional>
#include <string>
#include <string_view>

// The build-time half of ahead-of-time shader compilation. A shader host
// executable, generated from CMake/ShaderHostMain.cpp.in, links an app's shader
// translation units, whose EACP_SHADER_SET()s (Codegen/ShaderSets.h) register
// themselves, and calls runMain, which emits each registered
// graph's source for every dialect it compiles, names it by the hash of that
// text and writes one blob per target into the output directory, for ResEmbed
// to embed under the EacpShaders category. A blob already there is current,
// since its name is the hash of the source it was compiled from.
//
// The names, spelled by Shader/PrecompiledNames.h for both halves, `<key>` being
// Precompiled::keyFor of the text ShaderBuilder::build() hands the runtime for
// that dialect:
//   Metal   <key>.metallib, <key>.ios.metallib, <key>.iossim.metallib
//   D3D12   <key>.vs.dxbc and <key>.ps.dxbc, or <key>.cs.dxbc
//   Vulkan  <key>.<target>.vert.spv and .frag.spv, or .comp.spv, for each of
//           vk11spv13, vk11spv14 and vk13spv16
namespace eacp::GPU::ShaderCompiler
{
// One set of shaders: shows the visitor every graph in it. The overloads that
// take none use every EACP_SHADER_SET() registered in the process.
using ShaderSet = std::function<void(const ShaderGraphVisitor&)>;

// ShaderSets::registered(), each as a ShaderSet.
Vector<ShaderSet> registeredSets();

// Whether this build of the tool carries the GLSL-to-SPIR-V compiler, which it
// does wherever eacp-spirv is built (EACP_BUILD_SPIRV).
bool isSpirvAvailable();

// Whether `xcrun -sdk <sdk> metal` can run: an installed Xcode answers
// `xcrun --find metal` even when its Metal Toolchain component is missing, so
// this asks the compiler itself for its version.
bool isMetalToolchainAvailable(std::string_view sdk);

// The command line, field for field. The defaults are the host's: Metal for
// macOS on a Mac, DXBC on Windows, SPIR-V wherever eacp-spirv was built.
struct Options
{
    FilePath outputDirectory;

    // Receives every blob path this run produced or found present, absolute,
    // one per line. Empty writes no manifest.
    FilePath manifest;

    // macosx, iphoneos or iphonesimulator, each its own metallib.
    Vector<std::string> metalSdks {"macosx"};

    // The oldest OS each metallib must load on, which also picks the Metal
    // language version: a plain version ("12.0") for every SDK, or
    // "<sdk>=<version>" for one. Unnamed SDKs take the deployment floor in
    // CMake/AppleSetup.cmake: macOS 11.0, iOS 15.0.
    Vector<std::string> metalMinimumOs;

    bool metal = Platform::isMac();
    bool dxbc = Platform::isWindows();
    bool spirv = isSpirvAvailable();
    bool verbose = false;
};

// What compile() did. A success deletes every blob-named file in the output
// directory that `blobs` does not list, so the directory mirrors the manifest.
// A failure leaves `error` naming the shader, the target and the compiler's
// log, removes the manifest and deletes nothing.
struct Report
{
    bool succeeded() const;

    Vector<FilePath> blobs;
    Vector<FilePath> compiled;
    Vector<FilePath> removed;
    std::string error;
};

// The blob name's stem for one dialect's source text.
std::string keyFor(std::string_view source);

// No sets at all is a failure: a forgotten EACP_SHADER_SET(), or its
// translation unit missing from eacp_add_precompiled_shaders' SOURCES.
Report compile(const Options& options, const Vector<ShaderSet>& sets);
Report compile(const Options& options);

// compile(), printing what it did with --verbose and the error otherwise.
// Returns the process exit code.
int run(const Options& options, const Vector<ShaderSet>& sets);
int run(const Options& options);

// Parses the command line into Options and runs:
//   --out <dir> --manifest <file> [--metal-sdk macosx|iphoneos|iphonesimulator]...
//   [--metal-min-os <version>|<sdk>=<version>]... [--no-metal] [--no-dxbc]
//   [--no-spirv] [--verbose]
int runMain(int argc, const char* const* argv, const Vector<ShaderSet>& sets);
int runMain(int argc, const char* const* argv);
} // namespace eacp::GPU::ShaderCompiler
