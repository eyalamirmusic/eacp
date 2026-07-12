#include "Environment.h"
#include "FilePath.h"

#include <fstream>

namespace eacp
{
namespace
{
FilePath configHome()
{
    auto path = getEnvValue("XDG_CONFIG_HOME");

    if (!path.empty())
        return FilePath {path};

    return FilePath::homeDirectory() / ".config";
}

// Resolves one entry of ~/.config/user-dirs.dirs (the XDG user-dirs
// mechanism), e.g. XDG_MUSIC_DIR="$HOME/Music". Falls back to
// $HOME/<fallback> when the file or the entry is missing.
FilePath xdgUserDirectory(std::string_view key, std::string_view fallback)
{
    auto fallbackPath = FilePath::homeDirectory() / fallback;
    auto stream = std::ifstream((configHome() / "user-dirs.dirs").str());
    auto prefix = std::string {key} + "=\"";

    for (auto line = std::string(); std::getline(stream, line);)
    {
        if (!line.starts_with(prefix))
            continue;

        auto closing = line.rfind('"');

        if (closing < prefix.size())
            return fallbackPath;

        auto value = line.substr(prefix.size(), closing - prefix.size());

        if (value.empty())
            return fallbackPath;

        constexpr auto homePrefix = std::string_view {"$HOME"};

        if (value.starts_with(homePrefix))
            return FilePath {FilePath::homeDirectory().str()
                             + value.substr(homePrefix.size())};

        return FilePath {value};
    }

    return fallbackPath;
}
} // namespace

FilePath FilePath::homeDirectory()
{
    return FilePath {getEnvValue("HOME")};
}

FilePath FilePath::documentsDirectory()
{
    return xdgUserDirectory("XDG_DOCUMENTS_DIR", "Documents");
}

FilePath FilePath::downloadsDirectory()
{
    return xdgUserDirectory("XDG_DOWNLOAD_DIR", "Downloads");
}

FilePath FilePath::musicDirectory()
{
    return xdgUserDirectory("XDG_MUSIC_DIR", "Music");
}

FilePath FilePath::moviesDirectory()
{
    return xdgUserDirectory("XDG_VIDEOS_DIR", "Videos");
}

FilePath FilePath::picturesDirectory()
{
    return xdgUserDirectory("XDG_PICTURES_DIR", "Pictures");
}

FilePath FilePath::desktopDirectory()
{
    return xdgUserDirectory("XDG_DESKTOP_DIR", "Desktop");
}

FilePath FilePath::tempDirectory()
{
    auto path = getEnvValue("TMPDIR");
    return path.empty() ? FilePath {"/tmp"} : FilePath {path};
}

FilePath FilePath::appDataDirectory()
{
    auto path = getEnvValue("XDG_DATA_HOME");
    return path.empty() ? homeDirectory() / ".local/share" : FilePath {path};
}

FilePath FilePath::cacheDirectory()
{
    auto path = getEnvValue("XDG_CACHE_HOME");
    return path.empty() ? homeDirectory() / ".cache" : FilePath {path};
}
} // namespace eacp
