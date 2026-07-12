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
