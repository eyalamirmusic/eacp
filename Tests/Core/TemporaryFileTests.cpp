#include "Common.h"

using namespace nano;

namespace TemporaryFileTests
{
namespace fs = std::filesystem;
using eacp::FilePath;
using eacp::TemporaryFile;

auto tUniqueName = test("TemporaryFile/uniqueNameInTheTempDirectory") = []
{
    auto first = TemporaryFile {};
    auto second = TemporaryFile {};

    check(first.path() != second.path());
    check(first.path().parentDirectory() == FilePath::tempDirectory());
    check(first.path().fileName().starts_with("eacp-temp-"));
    check(!eacp::Files::exists(first.path()));
};

auto tSuffix = test("TemporaryFile/suffixIsUsedAsWritten") = []
{
    auto zip = TemporaryFile {".zip"};

    check(zip.path().extension() == ".zip");
    check(zip.path().fileName().ends_with(".zip"));
};

auto tDeletesFile = test("TemporaryFile/deletesTheFileWhenDone") = []
{
    auto path = FilePath {};

    {
        auto temp = TemporaryFile {".txt"};
        path = temp.path();
        writeTestFile(eacp::toStdPath(path), "data");
        check(eacp::Files::isFile(path));
    }

    check(!eacp::Files::exists(path));
};

auto tDeletesDirectory = test("TemporaryFile/deletesADirectoryTree") = []
{
    auto path = FilePath {};

    {
        auto temp = TemporaryFile {};
        path = temp.path();
        writeTestFile(eacp::toStdPath(path / "nested/file.txt"), "x");
    }

    check(!eacp::Files::exists(path));
};

auto tInDirectory = test("TemporaryFile/inAGivenDirectory") = []
{
    auto dir = scratchDirectory("temporary-in-dir");

    {
        auto temp = TemporaryFile {FilePath {dir}, ".mid"};
        check(temp.path().parentDirectory() == FilePath {dir});
        check(temp.sibling("fixed.mid") == FilePath {dir / "fixed.mid"});
        writeTestFile(eacp::toStdPath(temp.path()), "x");
    }

    check(fs::is_empty(dir));
    fs::remove_all(dir);
};

auto tMoveAssignReleasesOld =
    test("TemporaryFile/moveAssignDeletesThePathItHeld") = []
{
    auto newPath = FilePath {};
    auto oldPath = FilePath {};

    {
        auto target = TemporaryFile {".old"};
        oldPath = target.path();
        writeTestFile(eacp::toStdPath(oldPath), "old");

        auto source = TemporaryFile {".new"};
        newPath = source.path();
        writeTestFile(eacp::toStdPath(newPath), "new");

        target = std::move(source);

        check(!eacp::Files::exists(oldPath));
        check(target.path() == newPath);
        check(readTestFile(eacp::toStdPath(newPath)) == "new");
    }

    check(!eacp::Files::exists(newPath));
};

auto tMove = test("TemporaryFile/moveTransfersOwnership") = []
{
    auto path = FilePath {};

    {
        auto outer = TemporaryFile {};

        {
            auto inner = TemporaryFile {".bin"};
            path = inner.path();
            writeTestFile(eacp::toStdPath(path), "x");
            outer = std::move(inner);
        }

        check(eacp::Files::exists(path));
        check(outer.path() == path);

        auto moved = TemporaryFile {std::move(outer)};
        check(moved.path() == path);
        check(eacp::Files::exists(path));
    }

    check(!eacp::Files::exists(path));
};
} // namespace TemporaryFileTests
