#include "Archive.h"
#include "ByteReader.h"

#include <eacp/Core/Utils/MemoryMappedFile.h>
#include <eacp/Core/Utils/Strings.h>

namespace eacp::ExportList
{
namespace
{
constexpr auto archiveMagic = std::string_view {"!<arch>\n"};
constexpr auto thinArchiveMagic = std::string_view {"!<thin>\n"};
constexpr auto memberHeaderSize = std::uint64_t {60};
constexpr auto headerTerminator = std::string_view {"`\n"};

std::string_view magicOf(Bytes bytes)
{
    if (bytes.getSize() < archiveMagic.size())
        return {};

    return {reinterpret_cast<const char*>(bytes.data()), archiveMagic.size()};
}

std::string_view trimTrailingSpaces(std::string_view text)
{
    auto end = text.find_last_not_of(' ');
    return end == std::string_view::npos ? std::string_view {}
                                         : text.substr(0, end + 1);
}

bool isAllDigits(std::string_view text)
{
    return !text.empty()
           && text.find_first_not_of("0123456789") == std::string_view::npos;
}

class ArchiveReader
{
public:
    ArchiveReader(Bytes bytesToUse, const FilePath& archivePathToUse)
        : archivePath(archivePathToUse)
        , displayName(archivePath.str())
        , reader(bytesToUse, displayName)
        , thin(magicOf(bytesToUse) == thinArchiveMagic)
    {
    }

    void visitMembers(const MemberVisitor& visit)
    {
        auto offset = std::uint64_t {archiveMagic.size()};

        while (offset < reader.size())
        {
            if (reader.size() - offset < memberHeaderSize)
                fail("ends inside a member header");

            auto header = Header {reader, offset};
            checkTerminator(header);

            auto size = memberSize(header);
            auto dataOffset = offset + memberHeaderSize;
            auto rawName = trimTrailingSpaces(reader.paddedString(offset, 16));
            auto stored = isStoredInline(rawName);

            if (stored && size > reader.size() - dataOffset)
                fail("has a member that runs past the end of the file");

            visitMember(rawName, dataOffset, size, stored, visit);

            offset = dataOffset + (stored ? size : 0);
            offset += offset & 1;
        }
    }

private:
    struct Header
    {
        const ByteReader& reader;
        std::uint64_t offset;

        std::string_view field(std::uint64_t start, std::uint64_t length) const
        {
            return reader.paddedString(offset + start, length);
        }
    };

    [[noreturn]] void fail(std::string_view problem) const
    {
        throw Error(Strings::concat(displayName, ": ", problem));
    }

    void checkTerminator(const Header& header) const
    {
        if (header.field(58, 2) != headerTerminator)
            fail("has a corrupt member header");
    }

    std::uint64_t memberSize(const Header& header) const
    {
        auto text = trimTrailingSpaces(header.field(48, 10));

        if (!isAllDigits(text))
            fail("has a member header with no size");

        auto value = Strings::tryParse<std::uint64_t>(text);

        if (!value)
            fail("has a member header with an unreadable size");

        return *value;
    }

    bool isSymbolTableOrNameTable(std::string_view rawName) const
    {
        return rawName.starts_with('/') && !isAllDigits(rawName.substr(1));
    }

    bool isStoredInline(std::string_view rawName) const
    {
        return !thin || isSymbolTableOrNameTable(rawName);
    }

    void visitMember(std::string_view rawName,
                     std::uint64_t dataOffset,
                     std::uint64_t size,
                     bool stored,
                     const MemberVisitor& visit)
    {
        if (rawName == "//")
        {
            auto table = reader.slice(dataOffset, size);
            longNames = {reinterpret_cast<const char*>(table.data()),
                         table.getSize()};
            return;
        }

        if (isSymbolTableOrNameTable(rawName))
            return;

        auto name = memberName(rawName);

        if (stored)
        {
            visit({name, reader.slice(dataOffset, size)});
            return;
        }

        visitThinMember(name, size, visit);
    }

    std::string_view memberName(std::string_view rawName) const
    {
        if (!rawName.starts_with('/'))
        {
            if (rawName.ends_with('/'))
                rawName.remove_suffix(1);

            return rawName;
        }

        auto offset = Strings::tryParse<std::uint64_t>(rawName.substr(1));

        if (!offset || *offset >= longNames.size())
            fail("names a member past the end of its long-name table");

        auto name = longNames.substr(static_cast<std::size_t>(*offset));
        name = name.substr(0, name.find_first_of(std::string_view {"\0\n", 2}));

        if (name.ends_with('/'))
            name.remove_suffix(1);

        return name;
    }

    void visitThinMember(std::string_view name,
                         std::uint64_t size,
                         const MemberVisitor& visit) const
    {
        auto memberPath = FilePath {name};

        if (!memberPath.isAbsolute())
            memberPath = archivePath.parentDirectory() / name;

        auto file = MemoryMappedFile {memberPath};

        if (!file.isValid())
            fail(Strings::concat("names a thin member that cannot be read: ",
                                 memberPath.str()));

        if (file.bytes().getSize() != size)
            fail(Strings::concat("names a thin member whose size has changed: ",
                                 memberPath.str()));

        visit({name, file.bytes()});
    }

    const FilePath& archivePath;
    std::string displayName;
    ByteReader reader;
    bool thin = false;
    std::string_view longNames;
};
} // namespace

bool isArchive(Bytes bytes)
{
    auto magic = magicOf(bytes);
    return magic == archiveMagic || magic == thinArchiveMagic;
}

void forEachArchiveMember(Bytes bytes,
                          const FilePath& archivePath,
                          const MemberVisitor& visit)
{
    ArchiveReader(bytes, archivePath).visitMembers(visit);
}
} // namespace eacp::ExportList
