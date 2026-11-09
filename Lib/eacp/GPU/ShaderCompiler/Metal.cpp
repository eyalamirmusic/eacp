#include "Backends.h"

#include <eacp/Core/Process/Process.h>
#include <eacp/Core/Utils/Files.h>
#include <eacp/Core/Utils/Strings.h>
#include <eacp/Core/Utils/TemporaryFile.h>
#include <eacp/GPU/Shader/PrecompiledNames.h>

#include <algorithm>
#include <cstring>

namespace eacp::GPU::ShaderCompiler::Detail
{
namespace
{
struct MetalOsVersion
{
    int major = 0;
    int minor = 0;
};

std::optional<MetalOsVersion> parseMetalOsVersion(std::string_view text)
{
    auto dot = text.find('.');
    auto major = Strings::tryParse<int>(text.substr(0, dot));

    if (!major || *major <= 0)
        return std::nullopt;

    if (dot == std::string_view::npos)
        return MetalOsVersion {*major, 0};

    auto rest = text.substr(dot + 1);
    auto minorText = rest.substr(0, rest.find('.'));
    auto minor = Strings::tryParse<int>(minorText);

    if (!minor || *minor < 0)
        return std::nullopt;

    return MetalOsVersion {*major, *minor};
}

// The newest Metal language each OS release compiles, in tenths: 23 is 2.3.
// MTLLanguageVersion's availability in the SDK's MTLLibrary.h is the source.
// From 26 macOS and iOS share their numbers and their languages.
int metalLanguageFrom26(MetalOsVersion os)
{
    constexpr int fromOS26[] = {40, 41};
    return fromOS26[std::min(os.major, 27) - 26];
}

int macMetalLanguage(MetalOsVersion os)
{
    constexpr int fromMacOS11[] = {23, 24, 30, 31, 32};
    constexpr int fromMacOS10_13[] = {20, 21, 22};

    if (os.major >= 26)
        return metalLanguageFrom26(os);

    if (os.major >= 11)
        return fromMacOS11[std::min(os.major, 15) - 11];

    if (os.major == 10 && os.minor >= 13)
        return fromMacOS10_13[std::min(os.minor, 15) - 13];

    return 12;
}

int iosMetalLanguage(MetalOsVersion os)
{
    constexpr int fromIOS11[] = {20, 21, 22, 23, 24, 30, 31, 32};

    if (os.major >= 26)
        return metalLanguageFrom26(os);

    if (os.major >= 11)
        return fromIOS11[std::min(os.major, 18) - 11];

    return 12;
}

constexpr auto metalBFloat16Language = 31;

std::string metalLanguageStandard(bool isMac, int tenths)
{
    auto version = Strings::concat(tenths / 10, ".", tenths % 10);

    // Metal 3.0 unified the language names; before it each platform had its own.
    if (tenths >= 30)
        return "metal" + version;

    return (isMac ? "macos-metal" : "ios-metal") + version;
}

Vector<std::uint8_t> metalBytesOf(const std::string& text)
{
    auto bytes = Vector<std::uint8_t> {};
    bytes.resize(static_cast<int>(text.size()));

    if (!text.empty())
        std::memcpy(bytes.data(), text.data(), text.size());

    return bytes;
}

std::string metalProcessLog(const Processes::ProcessResult& result)
{
    return Strings::trim(result.output + result.errorOutput);
}
} // namespace

bool MetalSdk::isMac() const
{
    return name == "macosx";
}

std::optional<MetalSdk> findMetalSdk(std::string_view name)
{
    if (name == "macosx")
        return MetalSdk {"macosx",
                         std::string {Precompiled::macMetalSuffix},
                         "-mmacosx-version-min=",
                         "11.0"};

    if (name == "iphoneos")
        return MetalSdk {"iphoneos",
                         std::string {Precompiled::iosMetalSuffix},
                         "-mios-version-min=",
                         "15.0"};

    if (name == "iphonesimulator")
        return MetalSdk {"iphonesimulator",
                         std::string {Precompiled::iosSimulatorMetalSuffix},
                         "-mios-simulator-version-min=",
                         "15.0"};

    return std::nullopt;
}

bool isValidOsVersion(std::string_view version)
{
    return parseMetalOsVersion(version).has_value();
}

MetalTarget metalTargetFor(const MetalSdk& sdk,
                           std::string_view minimumOs,
                           bool needsBFloat16)
{
    auto target = MetalTarget {sdk, std::string {minimumOs}, {}};

    auto os = parseMetalOsVersion(minimumOs).value_or(MetalOsVersion {});
    auto language = sdk.isMac() ? macMetalLanguage(os) : iosMetalLanguage(os);

    if (needsBFloat16 && language < metalBFloat16Language)
    {
        language = metalBFloat16Language;
        target.minimumOs = sdk.isMac() ? "14.0" : "17.0";
    }

    target.languageStandard = metalLanguageStandard(sdk.isMac(), language);
    return target;
}

bool probeMetalToolchain(std::string_view sdk)
{
    auto result =
        Processes::run("xcrun", {"-sdk", std::string {sdk}, "metal", "--version"});

    return result.launched && result.exited && result.exitCode == 0;
}

Blob compileMetal(const MetalTarget& target, const std::string& source)
{
    auto input = TemporaryFile {".metal"};
    auto output = TemporaryFile {".metallib"};
    auto blob = Blob {};

    try
    {
        Files::writeFile(input.path(), std::string_view {source});
    }
    catch (const std::exception& error)
    {
        blob.log = error.what();
        return blob;
    }

    auto options = Processes::ProcessOptions {};
    options.executable = "xcrun";
    options.arguments = {"-sdk",
                         target.sdk.name,
                         "metal",
                         "-std=" + target.languageStandard,
                         target.sdk.versionFlag + target.minimumOs,
                         input.path().str(),
                         "-o",
                         output.path().str()};

    auto result = Processes::run(std::move(options));
    blob.log = metalProcessLog(result);

    if (!result.launched)
    {
        blob.log = "could not run xcrun";
        return blob;
    }

    if (!result.exited || result.exitCode != 0 || !Files::isFile(output.path()))
        return blob;

    blob.bytes = metalBytesOf(Files::readFile(output.path()));
    blob.succeeded = !blob.bytes.empty();
    return blob;
}
} // namespace eacp::GPU::ShaderCompiler::Detail
