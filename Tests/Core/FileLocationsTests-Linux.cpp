#include "Common.h"

#include <eacp/Core/Utils/Environment.h>

#include <pwd.h>
#include <unistd.h>

using namespace nano;

namespace FileLocationsLinuxTests
{
using eacp::FilePath;

// Sets an environment variable, or removes it, for the length of a scope and
// puts back what was there.
struct ScopedVariable
{
    ScopedVariable(std::string_view nameToUse, std::optional<std::string_view> value)
        : name(nameToUse)
        , previous(eacp::getEnv(nameToUse))
    {
        if (value.has_value())
            eacp::setEnv(name, *value);
        else
            eacp::unsetEnv(name);
    }

    ~ScopedVariable()
    {
        if (previous.has_value())
            eacp::setEnv(name, *previous);
        else
            eacp::unsetEnv(name);
    }

    ScopedVariable(const ScopedVariable&) = delete;
    ScopedVariable& operator=(const ScopedVariable&) = delete;

    std::string name;
    std::optional<std::string> previous;
};

std::string passwordDatabaseHomeForTest()
{
    auto* entry = ::getpwuid(::getuid());
    return entry != nullptr && entry->pw_dir != nullptr ? entry->pw_dir : "";
}

auto tHomeTrailingSlash = test("FileLocationsLinux/homeDropsATrailingSlash") = []
{
    {
        auto home = ScopedVariable {"HOME", "/home/someone/"};
        check(FilePath::homeDirectory().str() == "/home/someone");
    }

    {
        auto home = ScopedVariable {"HOME", "/home/someone"};
        check(FilePath::homeDirectory().str() == "/home/someone");
    }

    auto root = ScopedVariable {"HOME", "/"};
    check(FilePath::homeDirectory().str() == "/");
};

auto tHomeFallback = test("FileLocationsLinux/homeFallsBackToPasswordDatabase") = []
{
    auto expected = passwordDatabaseHomeForTest();

    if (expected.size() > 1 && expected.ends_with('/'))
        expected.pop_back();

    {
        auto home = ScopedVariable {"HOME", std::nullopt};
        check(FilePath::homeDirectory().str() == expected);
    }

    auto empty = ScopedVariable {"HOME", ""};
    check(FilePath::homeDirectory().str() == expected);
};

auto tUserDirs = test("FileLocationsLinux/userDirsFileIsReadLikeJuce") = []
{
    auto root = scratchDirectory("xdg-user-dirs");
    auto home = root / "home";
    auto config = root / "config";
    auto elsewhere = root / "elsewhere" / "Films";

    for (auto& folder: {home / "Music",
                        home / "Tunes",
                        home / "Docs",
                        home / "Pics",
                        home / "Desk",
                        elsewhere})
        std::filesystem::create_directories(folder);

    writeTestFile(config / "user-dirs.dirs",
                  "# written by xdg-user-dirs-update\n"
                  "XDG_MUSIC_DIR=\"$HOME/Gone\"\n"
                  "XDG_MUSIC_DIR=\"$HOME/Tunes\"\n"
                  "XDG_DOWNLOAD_DIR=\"$HOME/Missing\"\n"
                  "   \tXDG_DOCUMENTS_DIR=\"$HOME/Docs\"\n"
                  "XDG_PICTURES_DIR='$HOME/Pics'\n"
                  "XDG_DESKTOP_DIR=$HOME/Desk\n"
                  "XDG_VIDEOS_DIR=\""
                      + elsewhere.string() + "\"\n");

    auto homeVariable = ScopedVariable {"HOME", home.string()};
    auto configVariable = ScopedVariable {"XDG_CONFIG_HOME", config.string()};

    check(FilePath::musicDirectory().str() == (home / "Tunes").string());
    check(FilePath::downloadsDirectory().str() == (home / "Downloads").string());
    check(FilePath::documentsDirectory().str() == (home / "Docs").string());
    check(FilePath::picturesDirectory().str() == (home / "Pics").string());
    check(FilePath::desktopDirectory().str() == (home / "Desk").string());
    check(FilePath::moviesDirectory().str() == elsewhere.string());

    writeTestFile(config / "user-dirs.dirs", "XDG_MUSIC_DIR=\"$HOME/Music\"\n");
    check(FilePath::musicDirectory().str() == (home / "Music").string());

    std::filesystem::remove_all(root);
};

auto tUserDirsMissingFile = test("FileLocationsLinux/noUserDirsFileFallsBack") = []
{
    auto root = scratchDirectory("xdg-no-user-dirs");
    auto homeVariable = ScopedVariable {"HOME", (root / "home").string()};
    auto configVariable =
        ScopedVariable {"XDG_CONFIG_HOME", (root / "config").string()};

    check(FilePath::musicDirectory().str() == (root / "home" / "Music").string());
    check(FilePath::desktopDirectory().str()
          == (root / "home" / "Desktop").string());

    std::filesystem::remove_all(root);
};
} // namespace FileLocationsLinuxTests
