#pragma once

#include "Protobuf.h"

namespace eacp::ML::Blob
{
// MILBlob's storage format, version 2, as the phase 0 spike found Core ML
// accepts it: a 64-byte header, then per entry a 64-byte metadata record on a
// 64-byte boundary with the data right after it. The program's
// BlobFileValue.offset names the record, not the data.
enum class DataType : std::uint32_t
{
    float16 = 1,
    float32 = 2,
    uint8 = 3,
    int8 = 4,
    int32 = 14
};

inline constexpr std::uint64_t alignment = 64;
inline constexpr std::uint64_t headerSize = 64;
inline constexpr std::uint64_t metadataSize = 64;
inline constexpr std::uint32_t storageVersion = 2;
inline constexpr std::uint32_t metadataSentinel = 0xDEADBEEF;

class Writer
{
public:
    Writer();

    std::uint64_t append(DataType type, Span<const std::uint8_t> data);

    int count() const { return entries; }
    const Bytes& bytes() const { return out; }

private:
    void writeUInt32(std::uint32_t value);
    void writeUInt64(std::uint64_t value);
    void patchUInt32(int offset, std::uint32_t value);
    void padToAlignment();

    Bytes out;
    int entries = 0;
};
} // namespace eacp::ML::Blob
