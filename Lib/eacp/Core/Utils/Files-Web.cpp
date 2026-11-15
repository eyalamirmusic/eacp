#include "Files.h"
#include "FilesPlatform.h"

#include <emscripten/em_js.h>

#include <filesystem>

namespace eacp
{
namespace
{
// The .js beside the .wasm, as Emscripten's own _scriptName has it: node's
// absolute path for it (a real one, under NODERAWFS), or in a page the path of
// its URL, which is the closest a page comes to an executable's path.
// clang-format off
EM_JS_DEPS(eacpWebFiles, "$stringToUTF8,$lengthBytesUTF8");

EM_JS(int, eacpWebScriptPath, (char* buffer, int capacity), {
    var name = typeof _scriptName === "string" ? _scriptName : "";
    if (name && !ENVIRONMENT_IS_NODE) {
        try { name = new URL(name).pathname; } catch (e) {}
    }
    if (capacity > 0)
        stringToUTF8(name, buffer, capacity);
    return lengthBytesUTF8(name);
});
// clang-format on

std::string readScriptPath()
{
    auto path =
        std::string(static_cast<std::size_t>(eacpWebScriptPath(nullptr, 0)), '\0');
    eacpWebScriptPath(path.data(), static_cast<int>(path.size()) + 1);
    return path;
}
} // namespace

namespace Files
{
FilePath executablePath()
{
    static const auto path = readScriptPath();
    return FilePath {path};
}

FilePath resourcesDirectory()
{
    auto executable = executablePath();
    return executable.empty() ? FilePath {} : executable.parentDirectory();
}
} // namespace Files

namespace Detail
{
std::string bundleResourcePath(const std::string& /*filename*/)
{
    return {};
}

bool hasHiddenAttribute(const std::filesystem::path& /*path*/)
{
    return false;
}

std::error_code replaceFile(const std::filesystem::path& from,
                            const std::filesystem::path& to)
{
    auto error = std::error_code {};
    std::filesystem::rename(from, to, error);
    return error;
}
} // namespace Detail
} // namespace eacp
