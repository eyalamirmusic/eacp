#pragma once

#include <eacp/Core/Core.h>

#include <cstdint>
#include <optional>
#include <span>

namespace eacp::Video
{
enum class Mp4Codec
{
    Unknown,
    H264,
    Hevc
};

// Times are exact integers in the track's timescale units; toSeconds converts.
struct Mp4Sample
{
    Range<std::uint64_t> byteRange;

    std::int64_t decodeTime = 0;

    // decodeTime plus the ctts offset, which a v1 ctts can make negative.
    std::int64_t presentationTime = 0;

    std::uint32_t duration = 0;
    bool keyframe = false;
};

struct Mp4TrackInfo
{
    Mp4Codec codec = Mp4Codec::Unknown;

    // The avcC or hvcC payload bytes, verbatim.
    Vector<std::uint8_t> codecConfig;

    int width = 0;
    int height = 0;

    // mdhd units per second.
    std::uint32_t timescale = 0;

    // In timescale units; 0 when the container does not say.
    std::uint64_t duration = 0;
};

// Parses one MP4/ISOBMFF video track's sample tables, never the bitstream and
// never out of bounds. Edit lists (elst) and fragments (moof) are out of scope:
// a file whose samples live only in fragments fails to parse.
class Mp4Demuxer
{
public:
    // Maps and parses the file, leaving the demuxer empty on failure.
    bool open(const FilePath& path);

    // `fileBytes` is caller-owned and must outlive every later sampleBytes().
    bool parse(std::span<const std::uint8_t> fileBytes);

    bool isValid() const { return valid; }

    const Mp4TrackInfo& track() const { return trackInfo; }

    // In decode order.
    const Vector<Mp4Sample>& samples() const { return sampleList; }

    // Empty for an out-of-range index.
    std::span<const std::uint8_t> sampleBytes(int index) const;

    double toSeconds(std::int64_t timeUnits) const;

private:
    std::optional<MemoryMappedFile> file;
    std::span<const std::uint8_t> fileData;
    Mp4TrackInfo trackInfo;
    Vector<Mp4Sample> sampleList;
    bool valid = false;
};
} // namespace eacp::Video
