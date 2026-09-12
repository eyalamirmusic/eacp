#include "Common.h"
#include <eacp/Core/Utils/StdPath.h>
#include <filesystem>

using namespace nano;
using eacp::FilePath;

auto tCommonDirectories = test("FilePath/commonDirectories") = []
{
    auto home = FilePath::homeDirectory();
    check(!home.empty());
    check(std::filesystem::exists(eacp::toStdPath(home)));

    check(!FilePath::documentsDirectory().empty());
    check(!FilePath::downloadsDirectory().empty());
    check(!FilePath::musicDirectory().empty());
    check(!FilePath::moviesDirectory().empty());
    check(!FilePath::picturesDirectory().empty());
    check(!FilePath::desktopDirectory().empty());
    check(!FilePath::appDataDirectory().empty());
    check(!FilePath::cacheDirectory().empty());
};

auto tAppDirectories = test("FilePath/appDirectories") = []
{
    auto root = FilePath::appDataDirectory();
    auto cacheRoot = FilePath::cacheDirectory();

    check(FilePath::appSupportDirectory("Acme", "My App")
          == root / "Acme" / "My App");
    check(FilePath::appCacheDirectory("Acme", "My App")
          == cacheRoot / "Acme" / "My App");

    // No company: the level is left out rather than left blank.
    check(FilePath::appSupportDirectory("", "My App") == root / "My App");
    check(FilePath::appSupportDirectory("", "My App").str().find("//")
          == std::string::npos);

    // This binary embeds no AppInfo, so the app level comes from somewhere
    // else - but it is never empty, and never the bare root.
    auto support = FilePath::appSupportDirectory();
    check(support.str().starts_with(root.str()));
    check(support.str().size() > root.str().size() + 1);
    check(FilePath::appCacheDirectory().str().starts_with(cacheRoot.str()));
};

auto tTempDirectory = test("FilePath/tempDirectory") = []
{
    auto temp = FilePath::tempDirectory();
    check(!temp.empty());
    check(temp.str().back() != '/');
    check(std::filesystem::exists(eacp::toStdPath(temp)));
};

auto tJoinFromDirectory = test("FilePath/joinFromDirectory") = []
{
    auto joined = FilePath::homeDirectory() / "eacp-test.txt";
    check(joined.extension() == ".txt");
    check(joined.str().find("//") == std::string::npos);
};

auto tParentDirectory = test("FilePath/parentDirectory") = []
{
    check(FilePath {"dir/sub/image.png"}.parentDirectory().str() == "dir/sub");
    check(FilePath {"/file.txt"}.parentDirectory().str() == "/");
    check(FilePath {"file.txt"}.parentDirectory().empty());
    check(FilePath {}.parentDirectory().empty());
};

// Escapes rather than literal characters, so the tests don't depend on the
// compiler's source-encoding assumptions. U+FFFD in a folder name is the
// real-world case: sync tools drop it into names they fail to transcode,
// and path::string()/generic_u8string() then throw on Windows.
auto tStdPathRoundTripsNonAnsiNames =
    test("FilePath/std path round-trips non-ANSI names") = []
{
    const auto original = std::filesystem::path {u8"Kicks \uFFFD 808"}
                          / std::filesystem::path {u8"snare\U0001F4A5.wav"};

    auto path = FilePath {original};

    check(!path.str().empty());
    check(path.extension() == ".wav");
    check(eacp::toStdPath(path) == original);
};

auto tWideRoundTripsText = test("FilePath/wide round-trips valid UTF-8") = []
{
    auto path = FilePath {std::filesystem::path {u8"caf\u00E9/\uFFFD.wav"}};
    check(FilePath {std::filesystem::path {path.wide()}} == path);
};

auto tInvalidUtf8NeverThrows =
    test("FilePath/invalid UTF-8 decodes to U+FFFD instead of throwing") = []
{
    auto path = FilePath {"bad\xFFname.wav"};

    const auto wide = path.wide();
    check(wide.find(wchar_t {0xFFFD}) != std::wstring::npos);
    check(!eacp::toStdPath(path).empty());
};
