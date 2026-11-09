#include "PrecompiledTestBlobs.h"

#include <eacp/Core/Platform/Platform.h>
#include <eacp/Core/Process/Process.h>
#include <eacp/Core/Utils/Files.h>
#include <eacp/Core/Utils/TemporaryFile.h>
#include <eacp/GPU/Shader/PrecompiledNames.h>

#include <NanoTest/NanoTest.h>

#include <TargetConditionals.h>

namespace eacp::GPU::PrecompiledTestBlobs
{
namespace
{
// The same choice ShaderLibrary-Apple.mm makes: the simulator and the device
// are one CMake configuration under the Xcode generator, so only the compiler
// knows which this is.
std::string_view nativeMetalSuffix()
{
#if TARGET_OS_SIMULATOR
    return Precompiled::iosSimulatorMetalSuffix;
#elif TARGET_OS_IPHONE
    return Precompiled::iosMetalSuffix;
#else
    return Precompiled::macMetalSuffix;
#endif
}

bool metalToolchainInstalled()
{
    static const auto installed =
        Processes::run("xcrun", {"-sdk", "macosx", "metal", "--version"}).exitCode
        == 0;

    return installed;
}
} // namespace

Vector<std::string> resourceNamesFor(const ShaderSource& source)
{
    return {
        Precompiled::resourceName(Precompiled::keyFor(source), nativeMetalSuffix())};
}

Resources compileAheadOfTime(const ShaderSource& source)
{
    if (!Platform::isMac() || !metalToolchainInstalled())
        return {};

    const auto input = TemporaryFile {".metal"};
    const auto output = TemporaryFile {".metallib"};

    Files::writeFile(input.path(), std::string_view {source.source});

    const auto result = Processes::run(
        "xcrun",
        {"-sdk", "macosx", "metal", input.path().str(), "-o", output.path().str()});

    nano::check(result.exitCode == 0, result.errorOutput);

    if (result.exitCode != 0)
        return {};

    const auto library = Files::readFile(output.path());

    return {{resourceNamesFor(source)[0], bytesOf(library.data(), library.size())}};
}
} // namespace eacp::GPU::PrecompiledTestBlobs
