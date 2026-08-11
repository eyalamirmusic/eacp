#pragma once

#include <eacp/Core/Core.h>
#include <eacp/Graphics/Primitives/Primitives.h>

namespace eacp::Video
{
struct SyntheticClipOptions
{
    int width = 1920;
    int height = 1080;
    int fps = 30;
    double duration = 10.0;

    // Average H.264 bitrate in bits per second. 0 lets the encoder choose.
    int bitrate = 0;

    bool operator==(const SyntheticClipOptions&) const = default;
};

// A short cycle of widely separated colours, so a decoded frame can be matched
// back to its index even after H.264 has been over it.
Graphics::Color syntheticFrameColor(int index);

int syntheticFrameCount(const SyntheticClipOptions& options);

// Overwrites `path` with a clip whose frames are flat syntheticFrameColor
// fields under a sweeping pale bar. Blocks, pumping the event loop, so call it
// on the main thread.
bool writeSyntheticClip(const FilePath& path,
                        const SyntheticClipOptions& options = {});

// The same clip, cached under a name derived from the options and generated
// only when missing. Empty path if it could not be produced.
FilePath cachedSyntheticClip(const SyntheticClipOptions& options = {});
} // namespace eacp::Video
