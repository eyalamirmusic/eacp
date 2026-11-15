#pragma once

#include "Coff.h"

#include <cstring>
#include <optional>

namespace eacp::ExportList
{
// Little-endian reads that never pass the end of the bytes: a read that would
// throws Error, naming the file the bytes came from.
class ByteReader
{
public:
    explicit ByteReader(Bytes bytesToUse, std::string_view displayNameToUse = {})
        : bytes(bytesToUse)
        , displayName(displayNameToUse)
    {
    }

    std::uint64_t size() const { return bytes.getSize(); }
    const std::uint8_t* data() const { return bytes.data(); }

    std::uint8_t u8(std::uint64_t offset) const
    {
        return read<std::uint8_t>(offset);
    }

    std::uint16_t u16(std::uint64_t offset) const
    {
        return read<std::uint16_t>(offset);
    }

    std::uint32_t u32(std::uint64_t offset) const
    {
        return read<std::uint32_t>(offset);
    }

    // Up to length bytes, stopping at the first NUL.
    std::string_view paddedString(std::uint64_t offset, std::uint64_t length) const
    {
        checkRange(offset, length);
        auto start = reinterpret_cast<const char*>(data() + offset);
        auto text = std::string_view {start, static_cast<std::size_t>(length)};
        return text.substr(0, text.find('\0'));
    }

    // The NUL-terminated string at offset, or nullopt when no NUL ends it.
    std::optional<std::string_view> cString(std::uint64_t offset) const
    {
        checkRange(offset, 0);
        auto start = reinterpret_cast<const char*>(data() + offset);
        auto available = static_cast<std::size_t>(size() - offset);
        auto end = std::memchr(start, 0, available);

        if (end == nullptr)
            return std::nullopt;

        return std::string_view {
            start, static_cast<std::size_t>(static_cast<const char*>(end) - start)};
    }

    Bytes slice(std::uint64_t offset, std::uint64_t length) const
    {
        checkRange(offset, length);
        return Bytes {data() + offset, static_cast<std::size_t>(length)};
    }

private:
    template <typename T>
    T read(std::uint64_t offset) const
    {
        checkRange(offset, sizeof(T));
        auto value = T {};
        std::memcpy(&value, data() + offset, sizeof(T));
        return value;
    }

    void checkRange(std::uint64_t offset, std::uint64_t length) const
    {
        if (offset > size() || length > size() - offset)
            throw Error(std::string(displayName.empty() ? "input" : displayName)
                        + ": is truncated or corrupt (a read runs past its end)");
    }

    Bytes bytes;
    std::string_view displayName;
};
} // namespace eacp::ExportList
