#pragma once

#include <eacp/Core/Core.h>

namespace eacp::Video
{

// One block of planar float audio, borrowed: a pointer per channel, numFrames
// samples in each. The layout every audio callback already has.
struct AudioBuffer
{
    Span<const float> channel(int index) const
    {
        return {channels[index], numFrames};
    }

    bool isValid() const
    {
        return channels != nullptr && numChannels > 0 && numFrames > 0;
    }

    const float* const* channels = nullptr;
    int numChannels = 0;
    int numFrames = 0;
};

// The audio track to write alongside the video, and the shape the recorder
// expects every pushed block to have.
//
// Media Foundation's AAC encoder takes 44100 or 48000 Hz and one or two
// channels only, so those are the portable bounds; begin() fails on Windows
// rather than quietly recording something else.
struct AudioSpec
{
    int sampleRate = 48000;
    int numChannels = 2;

    // Average AAC bitrate in bits per second. 0 picks 64 kbps per channel.
    int bitrate = 0;

    // How far ahead of the picture the pushed audio is, in frames: the output
    // device's latency plus however stale the captured pixels are by the time
    // they are stamped, which for a screen-captured window is the app's whole
    // present-to-composite path. The audio track is delayed by this much.
    //
    // A constant to be measured once per capture path rather than derived --
    // Apps/Video/RecordWithAudio records a sound on a visible event so the gap
    // can be read straight off the file.
    int latencyFrames = 0;

    bool operator==(const AudioSpec&) const = default;
};

inline int audioBitrateFor(const AudioSpec& spec)
{
    return spec.bitrate > 0 ? spec.bitrate : 64'000 * spec.numChannels;
}

// Where the audio frame at `frameIndex` lands on the recording's timeline.
// Anchored once, then advanced by sample count, so the track follows the audio
// device's own rate instead of the jitter of its callbacks.
inline double audioTimeFor(std::int64_t frameIndex,
                           double anchorSeconds,
                           const AudioSpec& spec)
{
    auto frames = static_cast<double>(frameIndex + spec.latencyFrames);
    return anchorSeconds + frames / spec.sampleRate;
}

} // namespace eacp::Video
