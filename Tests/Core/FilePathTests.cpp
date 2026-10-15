#include "Common.h"
#include <eacp/Core/Utils/FilesPlatform.h>
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

// The swap nativeString() and fromNative() make on Windows, run on every
// platform with the separators passed in.
auto tReplaceSeparatorsWindowsForms =
    test("FilePath/replaceSeparators converts the Windows forms both ways") = []
{
    using eacp::Detail::replaceSeparators;

    auto toNative = [](std::string_view text)
    { return replaceSeparators(text, '/', '\\'); };

    auto fromNative = [](std::string_view text)
    { return replaceSeparators(text, '\\', '/'); };

    check(toNative("C:/dir/file.wav") == "C:\\dir\\file.wav");
    check(toNative("//server/share/dir") == "\\\\server\\share\\dir");
    check(toNative("//?/C:/very/long") == "\\\\?\\C:\\very\\long");
    check(toNative("relative/name") == "relative\\name");
    check(toNative("C:/") == "C:\\");
    check(toNative("").empty());

    check(fromNative("C:\\dir\\file.wav") == "C:/dir/file.wav");
    check(fromNative("\\\\server\\share\\dir") == "//server/share/dir");
    check(fromNative("\\\\?\\C:\\very\\long") == "//?/C:/very/long");
    check(fromNative("C:\\mixed/forms") == "C:/mixed/forms");

    // Multi-byte UTF-8 passes through untouched.
    check(toNative("caf\xC3\xA9/\xEF\xBF\xBD.wav")
          == "caf\xC3\xA9\\\xEF\xBF\xBD.wav");
};

auto tNativeStringRoundTrips =
    test("FilePath/nativeString round-trips through fromNative") = []
{
    for (auto text: {"C:/dir/file.wav",
                     "//server/share/dir",
                     "/usr/local/lib",
                     "relative/name.txt",
                     ""})
    {
        auto path = FilePath {text};
        check(FilePath::fromNative(path.nativeString()) == path);
    }

    auto home = FilePath::homeDirectory();
    check(FilePath::fromNative(home.nativeString()) == home);
};

auto tNativeStringPerPlatform =
    test("FilePath/nativeString uses the platform's separator") = []
{
    auto path = FilePath {"dir/sub/file.wav"};

    if constexpr (eacp::Platform::isWindows())
    {
        check(path.nativeString() == "dir\\sub\\file.wav");
        check(FilePath::fromNative("dir\\sub\\file.wav") == path);
    }
    else
    {
        // '\' is an ordinary file name character outside Windows, so neither
        // direction touches it.
        check(path.nativeString() == path.str());
        check(FilePath::fromNative("dir\\name").str() == "dir\\name");
    }
};
