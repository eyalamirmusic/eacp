#include "Common.h"

using namespace nano;

namespace FileWriterTests
{
namespace fs = std::filesystem;
using eacp::FilePath;
using eacp::FileWriter;

eacp::Span<const std::uint8_t> bytesOf(std::string_view text)
{
    return {reinterpret_cast<const std::uint8_t*>(text.data()), (int) text.size()};
}

auto tStreams = test("FileWriter/streamsAndCreatesParents") = []
{
    auto dir = scratchDirectory("writer-stream");
    auto path = dir / "a/b/out.bin";

    {
        auto writer = FileWriter {FilePath {path}};
        check(writer.isOpen());
        check(writer.position() == 0);
        check(writer.write("head"));
        check(writer.write(bytesOf({"\0\1\xFF", 3})));
        check(writer.position() == 7);
    }

    check(readTestFile(path) == std::string_view {"head\0\1\xFF", 7});
};

auto tTruncateMode = test("FileWriter/truncateModeStartsEmpty") = []
{
    auto dir = scratchDirectory("writer-truncate");
    auto path = dir / "out.txt";
    writeTestFile(path, "old contents");

    {
        auto writer = FileWriter {FilePath {path}};
        check(writer.write("new"));
    }

    check(readTestFile(path) == "new");

    {
        auto writer = FileWriter {FilePath {path}};
    }

    check(fs::file_size(path) == 0);
};

auto tAppendMode = test("FileWriter/appendModeKeepsContents") = []
{
    auto dir = scratchDirectory("writer-append");
    auto path = dir / "log.txt";
    writeTestFile(path, "one\n");

    {
        auto writer = FileWriter {FilePath {path}, FileWriter::Mode::append};
        check(writer.position() == 4);
        check(writer.write("two\n"));
    }

    {
        auto writer =
            FileWriter {FilePath {dir / "fresh.txt"}, FileWriter::Mode::append};
        check(writer.write("x"));
    }

    check(readTestFile(path) == "one\ntwo\n");
    check(readTestFile(dir / "fresh.txt") == "x");
};

auto tSeekPatches = test("FileWriter/seekPatchesEarlierBytes") = []
{
    auto dir = scratchDirectory("writer-seek");
    auto path = dir / "archive.bin";

    {
        auto writer = FileWriter {FilePath {path}};
        check(writer.write("HEADER----body"));
        check(writer.seek(6));
        check(writer.write("SZ"));
        check(writer.position() == 8);
        check(writer.seek(14));
        check(writer.write("!"));
        check(writer.flush());
        check(readTestFile(path) == "HEADERSZ--body!");
    }

    check(readTestFile(path) == "HEADERSZ--body!");
};

auto tSeekPastEnd = test("FileWriter/seekPastTheEndLeavesZeros") = []
{
    auto dir = scratchDirectory("writer-gap");
    auto path = dir / "gap.bin";

    {
        auto writer = FileWriter {FilePath {path}};
        check(writer.seek(3));
        check(writer.write("x"));
    }

    check(readTestFile(path) == std::string_view {"\0\0\0x", 4});
};

auto tTruncate = test("FileWriter/truncateCutsAtThePosition") = []
{
    auto dir = scratchDirectory("writer-cut");
    auto path = dir / "cut.txt";
    writeTestFile(path, "0123456789");

    {
        auto writer = FileWriter {FilePath {path}, FileWriter::Mode::append};
        check(writer.seek(4));
        check(writer.truncate());
        check(writer.write("ab"));
    }

    check(readTestFile(path) == "0123ab");
};

auto tTruncatePastEnd = test("FileWriter/truncatePastTheEndExtends") = []
{
    auto dir = scratchDirectory("writer-cut-past-end");
    auto path = dir / "grow.bin";

    {
        auto writer = FileWriter {FilePath {path}};
        check(writer.write("ab"));
        check(writer.seek(5));
        check(writer.truncate());
        check(writer.position() == 5);
    }

    check(readTestFile(path) == std::string_view {"ab\0\0\0", 5});
};

auto tTruncateInTruncateMode = test("FileWriter/truncateInTruncateMode") = []
{
    auto dir = scratchDirectory("writer-cut-fresh");
    auto path = dir / "fresh.txt";
    writeTestFile(path, "previous contents");

    {
        auto writer = FileWriter {FilePath {path}};
        check(writer.write("0123456789"));
        check(writer.seek(3));
        check(writer.truncate());
        check(writer.write("X"));
    }

    check(readTestFile(path) == "012X");
};

auto tUnopenable = test("FileWriter/failsWhereNothingCanBeWritten") = []
{
    auto dir = scratchDirectory("writer-fail");
    fs::create_directories(dir / "isADirectory");

    auto writer = FileWriter {FilePath {dir / "isADirectory"}};
    check(!writer.isOpen());
    check(!writer.write("x"));
    check(!writer.seek(1));
    check(!writer.flush());
    check(!writer.truncate());
    check(writer.position() == 0);
};
} // namespace FileWriterTests
