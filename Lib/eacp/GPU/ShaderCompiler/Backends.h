#pragma once

#include <eacp/Core/Utils/Containers.h>
#include <eacp/GPU/Spirv/SpirvCompiler.h>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// The three offline compilers ShaderCompiler.cpp drives, each turning one
// dialect's text into the bytes of one blob. Private to the target.
namespace eacp::GPU::ShaderCompiler::Detail
{
// One compile: the bytes on success, and whatever the compiler printed either
// way (warnings on success, the errors on failure).
struct Blob
{
    bool succeeded = false;
    Vector<std::uint8_t> bytes;
    std::string log;
};

// What one metallib is built for: the SDK xcrun selects, the blob's suffix,
// and the floor every Metal flag derives from.
struct MetalSdk
{
    bool isMac() const;

    std::string name;
    std::string suffix;
    std::string versionFlag;
    std::string defaultMinimumOs;
};

std::optional<MetalSdk> findMetalSdk(std::string_view name);

bool isValidOsVersion(std::string_view version);

// The flags that follow from an SDK and the oldest OS it must load on: the
// Metal language that OS has, raised to 3.1 (and the OS with it) for a source
// that declares bfloat SIMD-group matrices, which nothing older compiles. A
// device that old answers supportsBFloat16SimdMatrix false and never asks for
// the library, which is one per shader so no other shader is affected.
struct MetalTarget
{
    MetalSdk sdk;
    std::string minimumOs;
    std::string languageStandard;
};

MetalTarget metalTargetFor(const MetalSdk& sdk,
                           std::string_view minimumOs,
                           bool needsBFloat16);

bool probeMetalToolchain(std::string_view sdk);

// `xcrun -sdk <sdk> metal -std=<std> -m<platform>-version-min=<os> in -o out`,
// which compiles and links the one input into a metallib in a single step.
// Fast math stays on, as it is for the runtime's newLibraryWithSource.
Blob compileMetal(const MetalTarget& target, const std::string& source);

// FXC, in process, with the runtime's flags (ShaderLibrary-Windows.cpp). False
// and a failed Blob everywhere but Windows.
bool isDxbcAvailable();
Blob compileDxbc(const std::string& source,
                 const std::string& entry,
                 const char* profile);

// Every target a Vulkan device may ask for where eacp-spirv is built, and none
// where it is not.
Vector<Spirv::Target> spirvTargets();
Blob compileSpirv(Spirv::Stage stage,
                  Spirv::Target target,
                  const std::string& source);
} // namespace eacp::GPU::ShaderCompiler::Detail
