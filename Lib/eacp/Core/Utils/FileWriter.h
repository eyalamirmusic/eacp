#pragma once

#include "Common.h"
#include "FilePath.h"

namespace eacp
{
// RAII handle for writing a file in pieces: streaming, appending, or patching
// bytes at recorded offsets. The file is closed when this goes out of scope.
//
// Neither copyable nor movable: it owns an open stream, and a moved-from
// writer would have none to answer for. Hold it behind a unique_ptr to hand
// it around.
class FileWriter
{
public:
    enum class Mode
    {
        // Start from an empty file.
        truncate,

        // Keep what is there and start writing at its end. seek() can still
        // move back, so earlier bytes can be overwritten.
        append
    };

    // Opens path for writing, creating the file and its parent directories.
    // Check isOpen() before writing.
    explicit FileWriter(const FilePath& path, Mode mode = Mode::truncate);

    FileWriter(const FileWriter&) = delete;
    FileWriter& operator=(const FileWriter&) = delete;
    FileWriter(FileWriter&&) = delete;
    FileWriter& operator=(FileWriter&&) = delete;

    const FilePath& path() const;
    bool isOpen() const;

    // Writes at the current position, overwriting or extending the file.
    bool write(Span<const std::uint8_t> bytes);
    bool write(std::string_view text);

    // Moves the write position. Past the end is allowed: the next write
    // extends the file, and the gap reads as zeros.
    bool seek(std::uint64_t offset);
    std::uint64_t position() const;

    bool flush();

    // Cuts the file off at the current position.
    bool truncate();

private:
    struct Impl;

    FilePath filePath;
    Pimpl<Impl> impl;
};
} // namespace eacp
