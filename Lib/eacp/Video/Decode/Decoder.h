#pragma once

#include "VideoFrame.h"

namespace eacp::Video
{
// What a decoder reports about the track it opened.
struct VideoInfo
{
    // The size of the decoded pixel buffers, before any display rotation.
    int width = 0;
    int height = 0;

    // Seconds. 0 when the container does not say.
    double duration = 0.0;

    // The track's nominal rate, used to fill in the on-screen duration of
    // frames whose container gives none. 0 when unknown or variable.
    double frameRate = 0.0;

    // The clockwise rotation the track asks to be displayed with (0/90/180/270).
    // The decoded pixels are unrotated; this is the display instruction.
    int rotationDegrees = 0;
};

enum class SeekMode
{
    // Land exactly on the requested time: decode from the preceding keyframe
    // and discard up to it. What an editor's playhead needs.
    Accurate,

    // Land on the nearest keyframe at or before the time — much cheaper, and
    // enough while a scrub bar is still moving. Backends may treat this as
    // Accurate when they cannot do better.
    Keyframe
};

// Decodes one video track into frames, in presentation order.
//
// Deliberately synchronous, single-threaded and free of policy: no clock, no
// queue, no frame dropping, no notion of playing or paused. All of that is
// portable C++ living above this in FrameStream and Player, so supporting a new
// container, codec or platform means implementing these four calls and nothing
// else.
//
// Not thread-safe: FrameStream owns one and drives it from its decode thread.
struct Decoder
{
    virtual ~Decoder() = default;

    // Opens the file and selects its first video track. False if the file is
    // missing, unreadable, or has no video track this backend can decode.
    virtual bool open(const FilePath& path) = 0;

    virtual VideoInfo info() const = 0;

    // Writes the next frame in presentation order and returns true. False at
    // the end of the stream, and also on a decode error — playback stops the
    // same way either way, so the two are not distinguished here.
    virtual bool nextFrame(VideoFrame& out) = 0;

    // Repositions the stream so that the next nextFrame() returns the frame
    // covering `seconds`, clamped to the file. Also clears the end-of-stream
    // condition, so seeking back from the end resumes decoding.
    virtual void seek(double seconds, SeekMode mode) = 0;
};

// Builds the platform decoder: AVFoundation on Apple, Media Foundation on
// Windows. Never null — a backend that cannot work reports it from open().
OwningPointer<Decoder> makeDecoder();
} // namespace eacp::Video
