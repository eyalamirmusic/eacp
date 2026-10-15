// The two conversions that only exist on Windows: a native path arrives with
// backslashes and must come back generic, and a wide path can hold a lone
// surrogate that no UTF-8 encoder accepts. The portable half is in
// FilePathTests.cpp.

#include "Common.h"
#include <filesystem>
#include <string>

using namespace nano;
using eacp::FilePath;

auto tGenericSeparators =
    test("FilePath/std path converts to generic separators") = []
{
    auto path = FilePath {std::filesystem::path {L"C:\\dir\\file.wav"}};
    check(path.str() == "C:/dir/file.wav");
};

auto tNativeStringInvertsWideConversion =
    test("FilePath/nativeString restores the native separators") = []
{
    for (auto native: {L"C:\\dir\\file.wav",
                       L"\\\\server\\share\\dir",
                       L"\\\\?\\C:\\very\\long\\path.wav"})
    {
        auto path = FilePath {std::filesystem::path {native}};
        check(path.str().find('\\') == std::string::npos);
        check(path.wide().find(L'\\') == std::wstring::npos);
        check(FilePath {path.nativeString()}.wide() == native);
        check(FilePath::fromNative(path.nativeString()) == path);
    }
};

auto tLoneSurrogateNeverThrows =
    test("FilePath/lone surrogate becomes U+FFFD instead of throwing") = []
{
    auto name = std::wstring {L"bad"} + wchar_t {0xD800} + L"name.wav";
    auto path = FilePath {std::filesystem::path {name}};

    check(path.str() == "bad\xEF\xBF\xBDname.wav");
};
