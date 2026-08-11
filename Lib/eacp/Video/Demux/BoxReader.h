#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

// Bounds-checked traversal of ISOBMFF box structures. Named rather than
// anonymous, because unity builds would merge sibling anonymous namespaces and
// collide on names like readU32.
namespace eacp::Video::Mp4
{
consteval std::uint32_t fourcc(const char (&tag)[5])
{
    auto byte = [](char c) { return std::uint32_t {static_cast<std::uint8_t>(c)}; };

    return byte(tag[0]) << 24 | byte(tag[1]) << 16 | byte(tag[2]) << 8
           | byte(tag[3]);
}

// Big-endian reader with sticky failure: a read past the end returns zero or an
// empty span and latches ok() false, so a pass can check once at the boundary.
// Never reads outside the span, never throws.
class BoxReader
{
public:
    explicit BoxReader(std::span<const std::uint8_t> bytesToRead)
        : bytes(bytesToRead)
    {
    }

    bool ok() const { return !failed; }
    std::size_t position() const { return offset; }
    std::size_t remaining() const { return bytes.size() - offset; }

    std::span<const std::uint8_t> readBytes(std::size_t count)
    {
        if (count > remaining())
        {
            failed = true;
            return {};
        }

        auto result = bytes.subspan(offset, count);
        offset += count;
        return result;
    }

    bool skip(std::size_t count)
    {
        if (count > remaining())
        {
            failed = true;
            return false;
        }

        offset += count;
        return true;
    }

    std::uint8_t readU8() { return static_cast<std::uint8_t>(readBigEndian(1)); }
    std::uint16_t readU16() { return static_cast<std::uint16_t>(readBigEndian(2)); }
    std::uint32_t readU32() { return static_cast<std::uint32_t>(readBigEndian(4)); }
    std::uint64_t readU64() { return readBigEndian(8); }
    std::int32_t readS32() { return static_cast<std::int32_t>(readU32()); }

private:
    std::uint64_t readBigEndian(std::size_t count)
    {
        auto data = readBytes(count);
        auto value = std::uint64_t {0};

        for (auto byte: data)
            value = value << 8 | byte;

        return value;
    }

    std::span<const std::uint8_t> bytes;
    std::size_t offset = 0;
    bool failed = false;
};

// `payload` excludes the header.
struct Box
{
    std::uint32_t type = 0;
    std::span<const std::uint8_t> payload;
};

// size == 0 extends to the end of the span, size == 1 means a 64-bit largesize
// follows the type. False at the end of the span and on malformed input alike.
inline bool nextBox(BoxReader& reader, Box& out)
{
    if (reader.remaining() == 0)
        return false;

    auto size = std::uint64_t {reader.readU32()};
    auto type = reader.readU32();
    auto headerSize = std::uint64_t {8};

    if (size == 1)
    {
        size = reader.readU64();
        headerSize = 16;
    }
    else if (size == 0)
    {
        size = headerSize + reader.remaining();
    }

    if (!reader.ok() || size < headerSize)
        return false;

    auto payloadSize = size - headerSize;

    if (payloadSize > reader.remaining())
        return false;

    out.type = type;
    out.payload = reader.readBytes(static_cast<std::size_t>(payloadSize));
    return reader.ok();
}

// The first direct child of `parent` with the given type.
inline bool
    findChild(std::span<const std::uint8_t> parent, std::uint32_t type, Box& out)
{
    auto reader = BoxReader {parent};
    auto box = Box {};

    while (nextBox(reader, box))
    {
        if (box.type == type)
        {
            out = box;
            return true;
        }
    }

    return false;
}
} // namespace eacp::Video::Mp4
