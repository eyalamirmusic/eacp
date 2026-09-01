// The one thing a mapping can be handed on POSIX that has no pages behind it.
// The portable half is in MemoryMappedFileTests.cpp.

#include "Common.h"
#include <sys/stat.h>

#include <filesystem>
#include <string>

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
} // namespace

// A FIFO has no size and no pages to map. It is here for the open rather than
// the rejection: opening one for reading waits for a writer, so a plain
// O_RDONLY would not fail this test, it would hang it forever.
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
