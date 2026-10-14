#include "Common.h"

using namespace nano;

namespace FilesCopyMoveTests
{
namespace fs = std::filesystem;
namespace Files = eacp::Files;
using eacp::FilePath;

FilePath at(const fs::path& path)
{
    return FilePath {path};
}

auto tExistence = test("FilesCopyMove/existsIsFileIsDirectory") = []
{
    auto dir = scratchDirectory("copymove-exists");
    writeTestFile(dir / "a.txt", "a");

    check(Files::exists(at(dir)));
    check(Files::isDirectory(at(dir)));
    check(!Files::isFile(at(dir)));

    check(Files::exists(at(dir / "a.txt")));
    check(Files::isFile(at(dir / "a.txt")));
    check(!Files::isDirectory(at(dir / "a.txt")));

    check(!Files::exists(at(dir / "missing")));
    check(!Files::isFile(at(dir / "missing")));
    check(!Files::isDirectory(at(dir / "missing")));
};

auto tCopyFile = test("FilesCopyMove/copyFileOverwritesAndCreatesParents") = []
{
    auto dir = scratchDirectory("copymove-copyfile");
    writeTestFile(dir / "from.bin", std::string_view {"\0\1\2\xFF", 4});

    check(Files::copyFile(at(dir / "from.bin"), at(dir / "nested/deeper/to.bin")));
    check(readTestFile(dir / "nested/deeper/to.bin")
          == std::string_view {"\0\1\2\xFF", 4});

    writeTestFile(dir / "existing.txt", "a much longer old content");
    check(Files::copyFile(at(dir / "from.bin"), at(dir / "existing.txt")));
    check(readTestFile(dir / "existing.txt").size() == 4);

    check(readTestFile(dir / "from.bin").size() == 4);
};

auto tCopyFileFailures =
    test("FilesCopyMove/copyFileRefusesMissingAndDirectories") = []
{
    auto dir = scratchDirectory("copymove-copyfile-fail");
    fs::create_directories(dir / "sub");

    check(!Files::copyFile(at(dir / "missing"), at(dir / "to")));
    check(!Files::copyFile(at(dir / "sub"), at(dir / "to")));
    check(!fs::exists(dir / "to"));
};

auto tCopyEmptyFile = test("FilesCopyMove/copyFileZeroLength") = []
{
    auto dir = scratchDirectory("copymove-empty");
    writeTestFile(dir / "empty", "");

    check(Files::copyFile(at(dir / "empty"), at(dir / "copy")));
    check(fs::exists(dir / "copy"));
    check(fs::file_size(dir / "copy") == 0);
};

auto tCopyDirectory = test("FilesCopyMove/copyDirectoryIsRecursive") = []
{
    auto dir = scratchDirectory("copymove-copydir");
    writeTestFile(dir / "src/a.txt", "a");
    writeTestFile(dir / "src/sub/b.txt", "b");
    writeTestFile(dir / "src/sub/deeper/c.txt", "c");
    fs::create_directories(dir / "src/emptyDir");

    check(Files::copyDirectory(at(dir / "src"), at(dir / "out/dst")));

    check(readTestFile(dir / "out/dst/a.txt") == "a");
    check(readTestFile(dir / "out/dst/sub/b.txt") == "b");
    check(readTestFile(dir / "out/dst/sub/deeper/c.txt") == "c");
    check(fs::is_directory(dir / "out/dst/emptyDir"));
    check(readTestFile(dir / "src/a.txt") == "a");
};

auto tCopyDirectoryReplaces = test("FilesCopyMove/copyDirectoryReplacesTarget") = []
{
    auto dir = scratchDirectory("copymove-copydir-replace");
    writeTestFile(dir / "src/a.txt", "new");
    writeTestFile(dir / "dst/a.txt", "old");
    writeTestFile(dir / "dst/stale.txt", "stale");

    check(Files::copyDirectory(at(dir / "src"), at(dir / "dst")));

    check(readTestFile(dir / "dst/a.txt") == "new");
    check(!fs::exists(dir / "dst/stale.txt"));
};

auto tCopyDirectoryFailures =
    test("FilesCopyMove/copyDirectoryRefusesBadSources") = []
{
    auto dir = scratchDirectory("copymove-copydir-fail");
    writeTestFile(dir / "file.txt", "x");
    writeTestFile(dir / "src/a.txt", "a");

    check(!Files::copyDirectory(at(dir / "missing"), at(dir / "dst")));
    check(!Files::copyDirectory(at(dir / "file.txt"), at(dir / "dst")));
    check(!fs::exists(dir / "dst"));

    // Into itself would never end, and onto itself would delete the source.
    check(!Files::copyDirectory(at(dir / "src"), at(dir / "src/inner")));
    check(!Files::copyDirectory(at(dir / "src"), at(dir / "src")));
    check(readTestFile(dir / "src/a.txt") == "a");
};

auto tMoveFile = test("FilesCopyMove/moveFileRenamesAndOverwrites") = []
{
    auto dir = scratchDirectory("copymove-move");
    writeTestFile(dir / "a.txt", "a");
    writeTestFile(dir / "b.txt", "b");

    check(Files::moveFile(at(dir / "a.txt"), at(dir / "b.txt")));
    check(!fs::exists(dir / "a.txt"));
    check(readTestFile(dir / "b.txt") == "a");

    check(Files::moveFile(at(dir / "b.txt"), at(dir / "new/parent/c.txt")));
    check(readTestFile(dir / "new/parent/c.txt") == "a");

    check(!Files::moveFile(at(dir / "missing"), at(dir / "d.txt")));
    check(
        Files::moveFile(at(dir / "new/parent/c.txt"), at(dir / "new/parent/c.txt")));
};

auto tMoveDirectory = test("FilesCopyMove/moveFileMovesDirectories") = []
{
    auto dir = scratchDirectory("copymove-movedir");
    writeTestFile(dir / "src/sub/a.txt", "a");
    writeTestFile(dir / "dst/old.txt", "old");

    check(Files::moveFile(at(dir / "src"), at(dir / "dst")));
    check(!fs::exists(dir / "src"));
    check(readTestFile(dir / "dst/sub/a.txt") == "a");
    check(!fs::exists(dir / "dst/old.txt"));

    check(!Files::moveFile(at(dir / "dst"), at(dir / "dst/sub/inside")));
    check(readTestFile(dir / "dst/sub/a.txt") == "a");

    // A case-only rename, which a case-insensitive volume sees as one entry.
    check(Files::moveFile(at(dir / "dst"), at(dir / "DST")));
    check(relativeNames(dir, Files::listDirectory(at(dir)))
          == std::vector<std::string> {"DST"});
    check(readTestFile(dir / "DST/sub/a.txt") == "a");
};

auto tCopyOntoAncestor =
    test("FilesCopyMove/copyDirectoryOntoItsAncestorKeepsTheSource") = []
{
    auto dir = scratchDirectory("copymove-copy-ancestor");
    writeTestFile(dir / "a/b/inner.txt", "inner");
    writeTestFile(dir / "a/outer.txt", "outer");

    check(!Files::copyDirectory(at(dir / "a/b"), at(dir / "a")));
    check(!Files::copyDirectory(at(dir / "a/b"), at(dir)));
    check(readTestFile(dir / "a/b/inner.txt") == "inner");
    check(readTestFile(dir / "a/outer.txt") == "outer");
};

auto tMoveOntoAncestor =
    test("FilesCopyMove/moveFileOntoItsAncestorKeepsTheSource") = []
{
    auto dir = scratchDirectory("copymove-move-ancestor");
    writeTestFile(dir / "a/b/inner.txt", "inner");
    writeTestFile(dir / "a/file.txt", "file");

    check(!Files::moveFile(at(dir / "a/b"), at(dir / "a")));
    check(!Files::moveFile(at(dir / "a/file.txt"), at(dir / "a")));
    check(!Files::moveFile(at(dir / "a/b/inner.txt"), at(dir)));

    check(readTestFile(dir / "a/b/inner.txt") == "inner");
    check(readTestFile(dir / "a/file.txt") == "file");
};

auto tRemoveFile = test("FilesCopyMove/removeFileOnlyRemovesFiles") = []
{
    auto dir = scratchDirectory("copymove-remove");
    writeTestFile(dir / "a.txt", "a");
    writeTestFile(dir / "sub/b.txt", "b");

    check(Files::removeFile(at(dir / "a.txt")));
    check(!fs::exists(dir / "a.txt"));

    check(!Files::removeFile(at(dir / "a.txt")));
    check(!Files::removeFile(at(dir / "sub")));
    check(fs::exists(dir / "sub/b.txt"));
};

auto tNonexistentSibling = test("FilesCopyMove/nonexistentSiblingCounts") = []
{
    auto dir = scratchDirectory("copymove-sibling");
    auto preset = at(dir / "Preset.xml");

    check(Files::nonexistentSibling(preset) == preset);

    writeTestFile(dir / "Preset.xml", "1");
    check(Files::nonexistentSibling(preset) == at(dir / "Preset(2).xml"));

    writeTestFile(dir / "Preset(2).xml", "2");
    check(Files::nonexistentSibling(preset) == at(dir / "Preset(3).xml"));

    // A name already carrying a number counts on from it.
    check(Files::nonexistentSibling(at(dir / "Preset(2).xml"))
          == at(dir / "Preset(3).xml"));

    writeTestFile(dir / "noext", "x");
    check(Files::nonexistentSibling(at(dir / "noext")) == at(dir / "noext(2)"));

    fs::create_directories(dir / "Folder");
    check(Files::nonexistentSibling(at(dir / "Folder")) == at(dir / "Folder(2)"));
};

auto tNonexistentChild = test("FilesCopyMove/nonexistentChild") = []
{
    auto dir = scratchDirectory("copymove-child");

    check(Files::nonexistentChild(at(dir), "Untitled", ".mid")
          == at(dir / "Untitled.mid"));

    writeTestFile(dir / "Untitled.mid", "");
    writeTestFile(dir / "Untitled(2).mid", "");
    check(Files::nonexistentChild(at(dir), "Untitled", ".mid")
          == at(dir / "Untitled(3).mid"));

    writeTestFile(dir / "Take (7)", "");
    check(Files::nonexistentChild(at(dir), "Take (7)", "") == at(dir / "Take (8)"));

    writeTestFile(dir / "x(a)", "");
    check(Files::nonexistentChild(at(dir), "x(a)", "") == at(dir / "x(a)(2)"));
};
} // namespace FilesCopyMoveTests
