#pragma once

#include <eacp/Core/Utils/FilePath.h>
#include <eacp/Core/Utils/MemoryMappedFile.h>

#include <mach-o/loader.h>

#include <cstdint>
#include <optional>
#include <string>

namespace eacp::CrashReporting::Detail
{
struct ImageSymbol
{
    std::string mangledName;
    std::uint64_t distance = 0;
};

// An image of the app's read from its file rather than from memory, for the
// libraries the app loaded that the handler never did: a thin Mach-O, or the
// slice of a universal one built for this process's architecture.
class MachImageFile
{
public:
    explicit MachImageFile(const FilePath& path);

    [[nodiscard]] bool isValid() const;
    [[nodiscard]] std::string getUuid() const;

    // The function a byte `offset` from the image's load address falls in,
    // from the image's own symbol table.
    [[nodiscard]] std::optional<ImageSymbol> findSymbol(std::uint64_t offset) const;

private:
    MemoryMappedFile file;
    std::optional<std::size_t> headerOffset;
};
} // namespace eacp::CrashReporting::Detail
