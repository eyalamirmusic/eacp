#include "Common.h"

using namespace nano;
using eacp::FilePath;

auto tIsSameFileForOnePath = test("Files/isSameFile through two spellings") = []
{
    auto dir = scratchDirectory("files-same");
    writeTestFile(dir / "a.txt", "a");
    writeTestFile(dir / "b.txt", "b");

    auto spelled = FilePath {dir} / "sub" / ".." / "a.txt";
    std::filesystem::create_directories(dir / "sub");

    check(eacp::Files::isSameFile(FilePath {dir / "a.txt"}, spelled));
    check(!eacp::Files::isSameFile(FilePath {dir / "a.txt"},
                                   FilePath {dir / "b.txt"}));
    check(!eacp::Files::isSameFile(FilePath {dir / "a.txt"},
                                   FilePath {dir / "missing.txt"}));

    std::filesystem::remove_all(dir);
};

auto tAtomicWritesText = test("Files/writeFileAtomically takes text") = []
{
    auto dir = scratchDirectory("files-atomic-text");
    auto path = FilePath {dir / "sub" / "note.txt"};

    eacp::Files::writeFileAtomically(path, std::string_view {"first"});
    eacp::Files::writeFileAtomically(path, std::string_view {"second"});

    check(eacp::Files::readFile(path) == "second");

    std::filesystem::remove_all(dir);
};

auto tReadSymlinkOfPlainFile =
    test("Files/readSymlink of a plain file is empty") = []
{
    auto dir = scratchDirectory("files-readlink-plain");
    writeTestFile(dir / "plain.txt", "x");

    check(eacp::Files::readSymlink(FilePath {dir / "plain.txt"}).empty());
    check(eacp::Files::readSymlink(FilePath {dir / "missing.txt"}).empty());

    std::filesystem::remove_all(dir);
};
