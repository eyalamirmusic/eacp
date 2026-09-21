#include "Projects.h"

#include <algorithm>
#include <filesystem>
#include <unordered_set>

namespace term
{
namespace fs = std::filesystem;

std::string sessionNameFor(const std::string& path)
{
    auto name = fs::path(path).filename().string();

    if (name.empty())
        name = path;

    std::replace(name.begin(), name.end(), '.', '_');
    return name;
}

std::vector<ProjectDir> scanProjects(const AppConfig& config)
{
    auto result = std::vector<ProjectDir> {};
    auto seen = std::unordered_set<std::string> {};

    auto add = [&](const fs::path& dir)
    {
        auto path = dir.generic_string();

        if (seen.insert(path).second)
            result.push_back({path, sessionNameFor(path)});
    };

    for (const auto& configured: config.searchDirs)
    {
        const auto root = fs::path(expandHome(configured));
        auto error = std::error_code {};

        if (!fs::is_directory(root, error))
            continue;

        add(root);

        for (auto& entry: fs::directory_iterator(root, error))
        {
            if (!entry.is_directory(error))
                continue;

            const auto name = entry.path().filename().string();

            if (!name.empty() && name[0] != '.')
                add(entry.path());
        }
    }

    return result;
}
} // namespace term
