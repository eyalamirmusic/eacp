#pragma once

#include "Audio.h"

#include <eacp/Graphics/Primitives/Primitives.h>

#include <optional>

namespace eacp::Video
{
// How to build a synthetic clip. The defaults are a ten-second 1080p30 clip,
// which is enough decode work to be worth looking at on a frame graph.
struct SyntheticClipOptions
{
    int width = 1920;
    int height = 1080;
    int fps = 30;
    double duration = 10.0;

    // Average H.264 bitrate in bits per second. 0 lets the encoder choose.
    int bitrate = 0;

    // Adds an audio track carrying a steady tone, for anything that needs a
    // file with both media in it.
    std::optional<AudioSpec> audio;

    bool operator==(const SyntheticClipOptions&) const = default;
};

// The colour the frame at `index` is filled with. A short cycle of widely
// separated colours, so a decoded frame can be matched back to the frame it
// came from even after H.264 has been over it: neighbouring frames are never
// near-misses of each other.
Graphics::Color syntheticFrameColor(int index);

// The number of frames writeSyntheticClip produces for these options.
int syntheticFrameCount(const SyntheticClipOptions& options);

// Encodes a synthetic H.264 clip to `path`, overwriting whatever is there.
//
// Each frame is a flat field of syntheticFrameColor(index) with a pale bar
// sweeping across the middle third, so the clip has real motion to decode while
// the corners stay a known colour a test can assert on. With options.audio set
// it also carries a tone, written frame by frame alongside the picture.
//
// Runs the encoder to completion, which means pumping the event loop: call it
// on the main thread. Returns false if the encoder could not be set up or the
// file could not be finalised.
bool writeSyntheticClip(const FilePath& path,
                        const SyntheticClipOptions& options = {});

// The same clip, cached in the user's cache directory under a name derived from
// the options, and generated only when it is not already there. For samples,
// benchmarks and tests that want heavy content without a media file in the
// repository. Returns an empty path if the clip could not be produced.
FilePath cachedSyntheticClip(const SyntheticClipOptions& options = {});
} // namespace eacp::Video
