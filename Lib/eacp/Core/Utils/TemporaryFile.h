#pragma once

#include "FilePath.h"

#include <string_view>

namespace eacp
{
// A unique path that is deleted, file or directory, when this goes out of
// scope. Only the name is reserved: nothing is created on disk until the
// caller writes there.
//
// Movable, not copyable: exactly one owner deletes the path.
class TemporaryFile
{
public:
    // A fresh name in FilePath::tempDirectory(), "eacp-temp-<random><suffix>".
    // The suffix is used as written, so pass ".zip", not "zip".
    explicit TemporaryFile(std::string_view suffix = {});

    // The same, in directory instead of the system temp directory.
    TemporaryFile(const FilePath& directory, std::string_view suffix);

    ~TemporaryFile();

    TemporaryFile(TemporaryFile&& other) noexcept;
    TemporaryFile& operator=(TemporaryFile&& other) noexcept;

    TemporaryFile(const TemporaryFile&) = delete;
    TemporaryFile& operator=(const TemporaryFile&) = delete;

    const FilePath& path() const;

    // A path beside the temporary one, for a fixed name in the same
    // directory. Not owned: it is not deleted with this object.
    FilePath sibling(std::string_view name) const;

private:
    void remove();

    FilePath filePath;
};
} // namespace eacp
