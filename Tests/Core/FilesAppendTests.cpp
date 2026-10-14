#include "Common.h"

using namespace nano;

namespace FilesAppendTests
{
namespace fs = std::filesystem;
using eacp::FilePath;

eacp::Span<const std::uint8_t> bytesOf(std::string_view text)
{
    return {reinterpret_cast<const std::uint8_t*>(text.data()), (int) text.size()};
}

auto tAppendCreates = test("FilesAppend/appendCreatesTheFileAndParents") = []
{
    auto dir = scratchDirectory("append-create");
    auto path = dir / "logs/today.log";

    check(eacp::Files::appendFile(FilePath {path}, "first\n"));
    check(readTestFile(path) == "first\n");
};

auto tAppendAdds = test("FilesAppend/appendAddsToTheEnd") = []
{
    auto dir = scratchDirectory("append-adds");
    auto path = dir / "data.bin";
    writeTestFile(path, "abc");

    check(eacp::Files::appendFile(FilePath {path}, bytesOf({"\0\xFF", 2})));
    check(eacp::Files::appendFile(FilePath {path}, "def"));
    check(eacp::Files::appendFile(FilePath {path}, std::string_view {}));

    check(readTestFile(path)
          == std::string_view {"abc\0\xFF"
                               "def",
                               8});
};

auto tWriteAtOverwrites = test("FilesAppend/writeFileAtOverwritesInPlace") = []
{
    auto dir = scratchDirectory("append-writeat");
    auto path = dir / "archive.bin";
    writeTestFile(path, "0123456789");

    check(eacp::Files::writeFileAt(FilePath {path}, 2, bytesOf("ab")));
    check(readTestFile(path) == "01ab456789");

    check(eacp::Files::writeFileAt(FilePath {path}, 8, bytesOf("XYZ")));
    check(readTestFile(path) == "01ab4567XYZ");
};

auto tWriteAtExtends = test("FilesAppend/writeFileAtExtendsWithZeros") = []
{
    auto dir = scratchDirectory("append-writeat-extend");
    auto path = dir / "new/sparse.bin";

    check(eacp::Files::writeFileAt(FilePath {path}, 4, bytesOf("end")));
    check(readTestFile(path) == std::string_view {"\0\0\0\0end", 7});

    check(eacp::Files::writeFileAt(FilePath {path}, 0, bytesOf("")));
    check(fs::file_size(path) == 7);
};
} // namespace FilesAppendTests
