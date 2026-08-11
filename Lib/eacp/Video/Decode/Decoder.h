#pragma once

#include "VideoFrame.h"

namespace eacp::Video
{
struct VideoInfo
{
    // Decoded pixel buffer size, before any display rotation.
    int width = 0;
    int height = 0;

    // Seconds; 0 when the container does not say.
    double duration = 0.0;

    // Nominal track rate, 0 when unknown or variable. Fills in the on-screen
    // duration of frames whose container gives none.
    double frameRate = 0.0;

    // Clockwise display rotation (0/90/180/270); the decoded pixels are
    // unrotated.
    int rotationDegrees = 0;
};

enum class SeekMode
{
    // Land exactly on the requested time, decoding from the preceding keyframe.
    Accurate,

    // Land on the nearest keyframe at or before the time. Backends may treat
    // this as Accurate when they cannot do better.
    Keyframe
};

// Decodes one video track into frames, in presentation order. Synchronous and
// policy-free: no clock, queue, dropping or play state. Not thread-safe —
// FrameStream owns one and drives it from its decode thread.
struct Decoder
{
    virtual ~Decoder() = default;

    // Selects the file's first video track; false when there is none this
    // backend can decode.
    virtual bool open(const FilePath& path) = 0;

    virtual VideoInfo info() const = 0;

    // False at end of stream and on a decode error, which are not
    // distinguished.
    virtual bool nextFrame(VideoFrame& out) = 0;

    // The next nextFrame() returns the frame covering `seconds`, clamped to the
    // file. Clears end-of-stream, so seeking back from the end resumes.
    virtual void seek(double seconds, SeekMode mode) = 0;
};

// AVFoundation on Apple, Media Foundation on Windows. Never null — a backend
// that cannot work reports it from open().
OwningPointer<Decoder> makeDecoder();
} // namespace eacp::Video
