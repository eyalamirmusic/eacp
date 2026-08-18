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

// One sample of the video track, in decode order. Times are exact integers
// in the track's timescale units; Mp4Demuxer::toSeconds converts.
struct Mp4Sample
{
    // The sample's bytes within the file.
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

// What the audio track announces about itself, straight out of its mdhd and
// sample entry. No sample tables: this is here so a caller can tell that a
// file carries sound of the expected shape and length, not to decode it.
struct Mp4AudioInfo
{
    bool present = false;

    // mdhd units per second, and the track length in them.
    std::uint32_t timescale = 0;
    std::uint64_t duration = 0;

    int numChannels = 0;
    int sampleRate = 0;

    double seconds() const
    {
        return timescale > 0 ? static_cast<double>(duration) / timescale : 0.0;
    }
};

// Parses the sample tables of one MP4/ISOBMFF video track: per-sample byte
// ranges, timestamps and keyframe flags, without touching the bitstream.
// This is the container half of a decode path — what a hardware decoder
// needs handed to it, per WindowsDecoder.md.
//
// Edit lists (elst) and fragmented files (moof) are out of scope: their
// boxes are skipped, and a file whose samples live only in fragments fails
// to parse. Malformed input of any kind makes open()/parse() return false;
// nothing here throws or reads out of bounds.
class Mp4Demuxer
{
public:
    // Maps the file and parses it. False on I/O failure or malformed data,
    // leaving the demuxer empty.
    bool open(const FilePath& path);

    // Parses caller-owned bytes, which must outlive every later call to
    // sampleBytes(). For tests and in-memory sources.
    bool parse(std::span<const std::uint8_t> fileBytes);

    bool isValid() const { return valid; }

    const Mp4TrackInfo& track() const { return trackInfo; }

    const Mp4AudioInfo& audioTrack() const { return audioInfo; }

    // Every sample of the track, in decode order.
    const Vector<Mp4Sample>& samples() const { return sampleList; }

    // The mapped bytes of one sample; empty for an out-of-range index.
    std::span<const std::uint8_t> sampleBytes(int index) const;

    double toSeconds(std::int64_t timeUnits) const;

private:
    std::optional<MemoryMappedFile> file;
    std::span<const std::uint8_t> fileData;
    Mp4TrackInfo trackInfo;
    Mp4AudioInfo audioInfo;
    Vector<Mp4Sample> sampleList;
    bool valid = false;
};
} // namespace eacp::Video
