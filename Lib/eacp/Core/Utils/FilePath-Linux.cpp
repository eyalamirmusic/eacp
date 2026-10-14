#include "Environment.h"
#include "FilePath.h"
#include "Files.h"
#include "Strings.h"

#include <cerrno>
#include <fstream>
#include <optional>
#include <vector>

#include <pwd.h>
#include <unistd.h>

namespace eacp
{
namespace
{
// The home folder the password database records for this user, for a process
// started without $HOME (a service, or a sanitised environment).
std::string passwordDatabaseHome()
{
    auto bufferSize = ::sysconf(_SC_GETPW_R_SIZE_MAX);
    auto buffer = std::vector<char>(bufferSize > 0 ? (std::size_t) bufferSize
                                                   : std::size_t {16384});

    while (true)
    {
        auto entry = passwd {};
        auto* result = (passwd*) nullptr;
        auto error =
            ::getpwuid_r(::getuid(), &entry, buffer.data(), buffer.size(), &result);

        if (error == ERANGE && buffer.size() < 1024 * 1024)
        {
            buffer.resize(buffer.size() * 2);
            continue;
        }

        if (error != 0 || result == nullptr || result->pw_dir == nullptr)
            return {};

        return result->pw_dir;
    }
}

FilePath configHome()
{
    auto path = getEnvValue("XDG_CONFIG_HOME");

    if (!path.empty())
        return FilePath {path};

    return FilePath::homeDirectory() / ".config";
}

std::string withoutQuotes(std::string value)
{
    for (auto quote: {'"', '\''})
    {
        if (value.starts_with(quote))
        {
            value.erase(0, 1);

            if (value.ends_with(quote))
                value.pop_back();

            break;
        }
    }

    return value;
}

std::string replaceAll(std::string text, std::string_view from, std::string_view to)
{
    for (auto pos = text.find(from); pos != std::string::npos;
         pos = text.find(from, pos + to.size()))
        text.replace(pos, from.size(), to);

    return text;
}

// The value of a `KEY=value` line of user-dirs.dirs, or nullopt when the line
// assigns something else.
std::optional<std::string> userDirsValue(std::string_view line, std::string_view key)
{
    auto text = Strings::trim(line);

    if (!text.starts_with(key))
        return std::nullopt;

    auto rest = Strings::trim(std::string_view {text}.substr(key.size()));

    if (!rest.starts_with('='))
        return std::nullopt;

    return withoutQuotes(Strings::trim(std::string_view {rest}.substr(1)));
}

// Resolves one entry of user-dirs.dirs (the XDG user-dirs mechanism), e.g.
// XDG_MUSIC_DIR="$HOME/Music", the way JUCE does: an entry counts only when
// the folder it names exists, and $HOME/<fallback> is the answer otherwise.
FilePath xdgUserDirectory(std::string_view key, std::string_view fallback)
{
    auto home = FilePath::homeDirectory();
    auto stream = std::ifstream((configHome() / "user-dirs.dirs").str());

    for (auto line = std::string(); std::getline(stream, line);)
    {
        auto value = userDirsValue(line, key);

        if (!value.has_value() || value->empty())
            continue;

        auto path = FilePath {replaceAll(*value, "$HOME", home.str())};

        if (Files::isDirectory(path))
            return path;
    }

    return home / fallback;
}
} // namespace

FilePath FilePath::homeDirectory()
{
    auto path = getEnvValue("HOME");

    if (path.empty())
        path = passwordDatabaseHome();

    // JUCE's File drops the trailing separator, and paths built on this one
    // have to match it.
    if (path.size() > 1 && path.ends_with('/'))
        path.pop_back();

    return FilePath {path};
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

FilePath FilePath::commonAppDataDirectory()
{
    return FilePath {"/opt"};
}

FilePath FilePath::applicationsDirectory()
{
    return FilePath {"/usr"};
}

FilePath FilePath::appGroupContainerDirectory(std::string_view /*groupID*/)
{
    return {};
}
} // namespace eacp
