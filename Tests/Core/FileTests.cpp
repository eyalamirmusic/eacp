#include "Common.h"
#include <array>
#include <filesystem>
#include <fstream>
#include <span>

using namespace nano;
using eacp::File;

namespace
{
std::filesystem::path writeTempFile(const std::string& name,
                                    const std::string& contents)
{
    auto path = std::filesystem::temp_directory_path() / name;
    auto out = std::ofstream {path, std::ios::binary};
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return path;
}
} // namespace

auto tSizeAndExistence = test("File/sizeAndExistence") = []
{
    auto path = writeTempFile("eacp-file-test-size.bin", "hello world");
    auto file = File {path};

    check(file.exists());
    check(file.isRegularFile());
    check(file.size() == 11);

    std::filesystem::remove(path);
};

auto tMissingFile = test("File/missing") = []
{
    auto file = File {std::filesystem::temp_directory_path() / "eacp-missing.bin"};

    check(!file.exists());
    check(file.size() == 0);
    check(!file.openForRead());

    auto buffer = std::array<std::uint8_t, 4> {};
    check(file.read(0, buffer) == 0);
};

auto tSequentialRead = test("File/sequentialRead") = []
{
    auto path = writeTempFile("eacp-file-test-seq.bin", "abcdefgh");

    // Scope the File so its handle closes before remove: Windows refuses to
    // delete a file that still has an open handle, unlike POSIX.
    {
        auto file = File {path};
        auto buffer = std::array<std::uint8_t, 3> {};

        check(file.read(0, buffer) == 3);
        check(buffer[0] == 'a' && buffer[1] == 'b' && buffer[2] == 'c');

        check(file.read(3, buffer) == 3);
        check(buffer[0] == 'd' && buffer[1] == 'e' && buffer[2] == 'f');
    }

    std::filesystem::remove(path);
};

auto tSeekBackAndForth = test("File/seekBackAndForth") = []
{
    auto path = writeTempFile("eacp-file-test-seek.bin", "0123456789");

    {
        auto file = File {path};
        auto buffer = std::array<std::uint8_t, 4> {};

        check(file.read(6, buffer) == 4);
        check(buffer[0] == '6' && buffer[3] == '9');

        check(file.read(0, std::span<std::uint8_t> {buffer.data(), 2}) == 2);
        check(buffer[0] == '0' && buffer[1] == '1');
    }

    std::filesystem::remove(path);
};

auto tReadPastEnd = test("File/readPastEnd") = []
{
    auto path = writeTempFile("eacp-file-test-eof.bin", "xyz");

    {
        auto file = File {path};
        auto buffer = std::array<std::uint8_t, 8> {};

        check(file.read(0, buffer) == 3);
        check(file.read(3, buffer) == 0);
    }

    std::filesystem::remove(path);
};

auto tIsUnderRoot = test("File/isUnder") = []
{
    auto dir = std::filesystem::temp_directory_path() / "eacp-isunder";
    std::filesystem::create_directories(dir);
    auto inside = dir / "a.bin";
    {
        auto out = std::ofstream {inside, std::ios::binary};
        out << "x";
    }

    check(File {inside}.isUnder(dir));
    check(File {inside}.isUnder(std::filesystem::temp_directory_path()));

    // A sibling/child directory is not a containing root.
    check(!File {inside}.isUnder(dir / "sub"));

    // Escapes (a path outside the root) are rejected.
    check(
        !File {std::filesystem::temp_directory_path() / "eacp-outside.bin"}.isUnder(
            dir));

    std::filesystem::remove(inside);
};
