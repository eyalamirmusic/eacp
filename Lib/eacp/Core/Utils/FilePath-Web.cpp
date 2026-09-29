#include "Environment.h"
#include "FilePath.h"

// The page's in-memory file system (MEMFS), which is gone when the page is.
// Emscripten sets HOME to /home/web_user; the desktop's folders are folders
// under it.

namespace eacp
{
namespace
{
FilePath webUnderHome(std::string_view name)
{
    return FilePath::homeDirectory() / name;
}
} // namespace

FilePath FilePath::homeDirectory()
{
    auto path = getEnvValue("HOME");
    return FilePath {path.empty() ? std::string {"/home/web_user"} : path};
}

FilePath FilePath::documentsDirectory()
{
    return webUnderHome("Documents");
}

FilePath FilePath::downloadsDirectory()
{
    return webUnderHome("Downloads");
}

FilePath FilePath::musicDirectory()
{
    return webUnderHome("Music");
}

FilePath FilePath::moviesDirectory()
{
    return webUnderHome("Videos");
}

FilePath FilePath::picturesDirectory()
{
    return webUnderHome("Pictures");
}

FilePath FilePath::desktopDirectory()
{
    return webUnderHome("Desktop");
}

FilePath FilePath::tempDirectory()
{
    return FilePath {"/tmp"};
}

FilePath FilePath::appDataDirectory()
{
    return webUnderHome(".local/share");
}

FilePath FilePath::cacheDirectory()
{
    return webUnderHome(".cache");
}
} // namespace eacp
