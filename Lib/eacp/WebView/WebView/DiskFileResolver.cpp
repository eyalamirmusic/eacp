#include "WebView.h"

#include <eacp/Core/Utils/Files.h>
#include <eacp/Core/Utils/Strings.h>

#include <algorithm>
#include <filesystem>
#include <string_view>
#include <system_error>

namespace eacp::Graphics
{
namespace
{
std::string pathFromFileURL(std::string_view url)
{
    auto schemeEnd = url.find("://");

    if (schemeEnd == std::string_view::npos)
        return {};

    auto rest = url.substr(schemeEnd + 3);
    auto cut = rest.find_first_of("?#");

    if (cut != std::string_view::npos)
        rest = rest.substr(0, cut);

    auto slash = rest.find('/');

    if (slash == std::string_view::npos)
        return {};

    return Strings::percentDecode(rest.substr(slash));
}
} // namespace

FilePathResolver diskFileResolver(std::vector<std::string> allowedRoots)
{
    return [roots = std::move(allowedRoots)](
               std::string_view url) -> std::optional<std::string>
    {
        auto pathStr = pathFromFileURL(url);

        if (pathStr.empty())
            return std::nullopt;

        auto ec = std::error_code {};
        auto path = std::filesystem::weakly_canonical(pathStr, ec);

        if (ec)
            path = std::filesystem::path {pathStr};

        auto allowed = roots.empty()
                    || std::any_of(roots.begin(), roots.end(),
                                   [&](const auto& root)
                                   { return Files::isUnderRoot(path, root); });

        if (!allowed || !std::filesystem::is_regular_file(path, ec))
            return std::nullopt;

        return path.string();
    };
}
} // namespace eacp::Graphics
