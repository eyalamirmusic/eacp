#include "AllocationCount.h"
#include "Common.h"
#include <atomic>
#include <cstdlib>
#include <new>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

#include <filesystem>
#include <fstream>
#include <numeric>

using namespace nano;
using eacp::FilePath;
using eacp::MemoryMappedFile;

namespace
{
std::filesystem::path scratchDirectory(const std::string& name)
{
    auto dir = std::filesystem::temp_directory_path() / ("eacp-mapped-" + name);

    std::filesystem::remove_all(dir);
    std::filesystem::create_directories(dir);

    return dir;
}

void write(const std::filesystem::path& path, std::string_view contents)
{
    auto out = std::ofstream {path, std::ios::binary | std::ios::trunc};
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
}

// Distinct in every byte, so a window a page out is a failed comparison.
std::string countingPattern(std::size_t size)
{
    auto contents = std::string(size, '\0');

    for (auto i = std::size_t {0}; i < size; ++i)
        contents[i] = static_cast<char>(i % 251);

    return contents;
}
} // namespace

auto tMapsWholeFile = test("MemoryMappedFile/mapsWholeFile") = []
{
    const auto dir = scratchDirectory("whole");
    const auto path = dir / "hello.txt";

    write(path, "hello world");

    const auto file = MemoryMappedFile {FilePath {path}};

    check(file.isValid());
    check(!file.empty());
    check(file.size() == 11);
    check(file.text() == "hello world");
    check(file.bytes().size() == 11);
    check(file.bytes()[0] == 'h');

    std::filesystem::remove_all(dir);
};

auto tMapsBinaryContent = test("MemoryMappedFile/mapsBinaryContent") = []
{
    const auto dir = scratchDirectory("binary");
    const auto path = dir / "nuls.bin";

    const auto contents = std::string {"a\0b\0c", 5};
    write(path, contents);

    const auto file = MemoryMappedFile {FilePath {path}};

    check(file.isValid());
    check(file.size() == 5);
    check(file.text() == contents);

    std::filesystem::remove_all(dir);
};

// Both platforms refuse a zero-length mapping, so valid and empty have to be
// separate answers.
auto tMapsAnEmptyFile = test("MemoryMappedFile/mapsAnEmptyFile") = []
{
    const auto dir = scratchDirectory("empty");
    const auto path = dir / "empty.bin";

    write(path, "");

    const auto file = MemoryMappedFile {FilePath {path}};

    check(file.isValid());
    check(file.empty());
    check(file.size() == 0);
    check(file.bytes().empty());
    check(file.text().empty());

    std::filesystem::remove_all(dir);
};

auto tMissingFileIsInvalid = test("MemoryMappedFile/missingFileIsInvalid") = []
{
    const auto dir = scratchDirectory("missing");

    const auto file = MemoryMappedFile {FilePath {dir / "nope.bin"}};

    check(!file.isValid());
    check(file.empty());
    check(file.bytes().empty());
    check(file.text().empty());

    std::filesystem::remove_all(dir);
};

auto tDirectoryIsInvalid = test("MemoryMappedFile/directoryIsInvalid") = []
{
    const auto dir = scratchDirectory("directory");

    const auto file = MemoryMappedFile {FilePath {dir}};

    check(!file.isValid());

    std::filesystem::remove_all(dir);
};

auto tKeepsThePath = test("MemoryMappedFile/keepsThePath") = []
{
    const auto dir = scratchDirectory("path");
    const auto path = dir / "named.txt";

    write(path, "x");

    const auto file = MemoryMappedFile {FilePath {path}};

    check(file.path() == FilePath {path});

    std::filesystem::remove_all(dir);
};

auto tMapsAWindow = test("MemoryMappedFile/mapsAWindow") = []
{
    const auto dir = scratchDirectory("window");
    const auto path = dir / "abc.txt";

    write(path, "abcdefghij");

    const auto file = MemoryMappedFile {FilePath {path}, 3, 4};

    check(file.isValid());
    check(file.size() == 4);
    check(file.text() == "defg");

    std::filesystem::remove_all(dir);
};

// mmap wants a page-size multiple and MapViewOfFile a 64KB one, so the mapping
// starts below the requested byte. 65536 + 1234 is a multiple of neither.
auto tMapsAnUnalignedWindow = test("MemoryMappedFile/mapsAnUnalignedWindow") = []
{
    const auto dir = scratchDirectory("unaligned");
    const auto path = dir / "pattern.bin";

    const auto contents = countingPattern(256 * 1024);
    write(path, contents);

    const auto offset = std::size_t {65536 + 1234};
    const auto length = std::size_t {5000};

    const auto file = MemoryMappedFile {FilePath {path}, offset, length};

    check(file.isValid());
    check(file.size() == length);
    check(file.text() == std::string_view {contents}.substr(offset, length));

    std::filesystem::remove_all(dir);
};

auto tMapsToTheEndFromAnOffset =
    test("MemoryMappedFile/mapsToTheEndFromAnOffset") = []
{
    const auto dir = scratchDirectory("to-end");
    const auto path = dir / "abc.txt";

    write(path, "abcdefghij");

    const auto file = MemoryMappedFile {FilePath {path}, 7};

    check(file.isValid());
    check(file.text() == "hij");

    std::filesystem::remove_all(dir);
};

auto tClampsALengthPastTheEnd = test("MemoryMappedFile/clampsALengthPastTheEnd") = []
{
    const auto dir = scratchDirectory("clamp");
    const auto path = dir / "abc.txt";

    write(path, "abcdefghij");

    const auto file = MemoryMappedFile {FilePath {path}, 6, 4096};

    check(file.isValid());
    check(file.size() == 4);
    check(file.text() == "ghij");

    std::filesystem::remove_all(dir);
};

auto tMapsTheEmptyWindowAtTheEnd =
    test("MemoryMappedFile/mapsTheEmptyWindowAtTheEnd") = []
{
    const auto dir = scratchDirectory("at-end");
    const auto path = dir / "abc.txt";

    write(path, "abcdefghij");

    const auto file = MemoryMappedFile {FilePath {path}, 10};

    check(file.isValid());
    check(file.empty());

    std::filesystem::remove_all(dir);
};

auto tOffsetPastTheEndIsInvalid =
    test("MemoryMappedFile/offsetPastTheEndIsInvalid") = []
{
    const auto dir = scratchDirectory("past-end");
    const auto path = dir / "abc.txt";

    write(path, "abcdefghij");

    const auto file = MemoryMappedFile {FilePath {path}, 11};

    check(!file.isValid());
    check(file.empty());

    std::filesystem::remove_all(dir);
};

auto tZeroLengthWindowIsValid = test("MemoryMappedFile/zeroLengthWindowIsValid") = []
{
    const auto dir = scratchDirectory("zero-length");
    const auto path = dir / "abc.txt";

    write(path, "abcdefghij");

    const auto file = MemoryMappedFile {FilePath {path}, 3, 0};

    check(file.isValid());
    check(file.empty());

    std::filesystem::remove_all(dir);
};

// The mapping outlives the descriptor it was made from.
auto tOutlivesTheOpenFile = test("MemoryMappedFile/outlivesTheOpenFile") = []
{
    const auto dir = scratchDirectory("lifetime");
    const auto path = dir / "pattern.bin";

    const auto contents = countingPattern(128 * 1024);
    write(path, contents);

    auto file = MemoryMappedFile {FilePath {path}};
    check(file.isValid());

    auto moved = std::move(file);

    check(moved.isValid());
    check(moved.text() == contents);

    std::filesystem::remove_all(dir);
};

#if !defined(_WIN32)
// Opening a FIFO for reading waits for a writer, so a plain O_RDONLY would hang
// this test rather than fail it.
auto tFifoIsInvalid = test("MemoryMappedFile/fifoIsInvalid") = []
{
    const auto dir = scratchDirectory("fifo");
    const auto path = dir / "pipe";

    if (::mkfifo(path.c_str(), 0600) != 0)
        return;

    const auto file = MemoryMappedFile {FilePath {path}};

    check(!file.isValid());

    std::filesystem::remove_all(dir);
};
#endif

// A mapping costs the path and the Pimpl whatever the file's size: 4KB is far
// above that and far below the 4MB being mapped.
auto tMappingDoesNotAllocateTheFile =
    test("MemoryMappedFile/mappingDoesNotAllocateTheFile") = []
{
    const auto dir = scratchDirectory("cost");
    const auto path = dir / "big.bin";

    const auto size = std::size_t {4 * 1024 * 1024};
    write(path, std::string(size, 'x'));

    const auto filePath = FilePath {path};

    auto counter = AllocationCount {};
    const auto file = MemoryMappedFile {filePath};
    const auto bytes = counter.bytes();

    check(file.isValid());
    check(file.size() == size);
    check(bytes < 4096);

    // The pages come from the page cache, not the heap.
    check(std::accumulate(file.bytes().begin(), file.bytes().end(), std::size_t {0})
          == size * std::size_t {'x'});

    std::filesystem::remove_all(dir);
};
