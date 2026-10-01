#include "Blob.h"

namespace eacp::ML::Blob
{
Writer::Writer()
{
    writeUInt32(0);
    writeUInt32(storageVersion);

    for (auto reserved = 0; reserved < 7; ++reserved)
        writeUInt64(0);
}

std::uint64_t Writer::append(DataType type, Span<const std::uint8_t> data)
{
    padToAlignment();

    auto recordOffset = static_cast<std::uint64_t>(out.getSize());

    writeUInt32(metadataSentinel);
    writeUInt32(static_cast<std::uint32_t>(type));
    writeUInt64(data.getSize());
    writeUInt64(recordOffset + metadataSize);
    writeUInt64(0);

    for (auto reserved = 0; reserved < 4; ++reserved)
        writeUInt64(0);

    out.getVector().insert(out.end(), data.begin(), data.end());

    ++entries;
    patchUInt32(0, static_cast<std::uint32_t>(entries));

    return recordOffset;
}

void Writer::writeUInt32(std::uint32_t value)
{
    for (auto shift = 0; shift < 32; shift += 8)
        out.add(static_cast<std::uint8_t>(value >> shift));
}

void Writer::writeUInt64(std::uint64_t value)
{
    for (auto shift = 0; shift < 64; shift += 8)
        out.add(static_cast<std::uint8_t>(value >> shift));
}

void Writer::patchUInt32(int offset, std::uint32_t value)
{
    for (auto byte = 0; byte < 4; ++byte)
        out[offset + byte] = static_cast<std::uint8_t>(value >> (byte * 8));
}

void Writer::padToAlignment()
{
    auto size = static_cast<std::uint64_t>(out.getSize());
    auto padded = (size + alignment - 1) / alignment * alignment;
    out.resize(padded, 0);
}
} // namespace eacp::ML::Blob
