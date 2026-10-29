#pragma once

#include <eacp/Core/Utils/FilePath.h>
#include <eacp/Core/Utils/MemoryMappedFile.h>

#include <mach-o/loader.h>

#include <cstdint>
#include <optional>
#include <string>

namespace eacp::CrashReporting::Detail
{
struct FunctionContainingOffset
{
    std::string mangledName;
    std::uint64_t distance = 0;
};

// Reads function names for a library the app loaded from that library's file on
// disk, since the crash handler process never loaded it. Only the handler uses
// it, after a crash.
class LoadedLibrarySymbolReader
{
public:
    explicit LoadedLibrarySymbolReader(const FilePath& path);

    [[nodiscard]] bool isValid() const;
    [[nodiscard]] std::string getBuildUuid() const;

    // The function a byte `offset` from the library's load address falls in,
    // from the library file's own symbol table.
    [[nodiscard]] std::optional<FunctionContainingOffset>
        findFunctionContainingOffset(std::uint64_t offset) const;

private:
    MemoryMappedFile file;
    std::optional<std::size_t> thisArchitectureHeaderOffset;
};
} // namespace eacp::CrashReporting::Detail
