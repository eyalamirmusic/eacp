#pragma once

#include "Coff.h"

#include <eacp/Core/Utils/FilePath.h>

namespace eacp::ExportList
{
// "!<arch>\n", as MSVC lib and llvm-lib write, or llvm-lib's thin "!<thin>\n".
bool isArchive(Bytes bytes);

// One object inside an archive: its name as the archive spells it, and its
// bytes, which live only for the call they are handed to.
struct ArchiveMember
{
    std::string_view name;
    Bytes bytes;
};

using MemberVisitor = std::function<void(const ArchiveMember&)>;

// Calls visit for every object member, in archive order, skipping the linker
// members and the long-name table. A thin archive's members are read from the
// files it names, relative to the archive's directory. Throws Error, naming
// the archive, for a corrupt header or a thin member that cannot be read.
void forEachArchiveMember(Bytes bytes,
                          const FilePath& archivePath,
                          const MemberVisitor& visit);
} // namespace eacp::ExportList
