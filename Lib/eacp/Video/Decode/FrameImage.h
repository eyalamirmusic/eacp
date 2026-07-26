#pragma once

#include "VideoFrame.h"

#include <eacp/Graphics/Image/Image.h>

namespace eacp::Video
{
// Copies a decoded frame into a tightly packed, straight-RGBA Image — the
// CPU-side view of a frame, for thumbnails, an editor's filmstrip, pixel
// inspection and tests. Returns an empty image for an invalid frame.
//
// The render path never needs this: it wraps or uploads the frame's pixels
// directly. This deliberately costs a copy (and, on the zero-copy path, a
// buffer lock), which is why it is a free function rather than something
// VideoFrame does on its own.
Graphics::Image toImage(const VideoFrame& frame);

// Locks a platform pixel buffer and copies it out as straight RGBA. The one
// piece of toImage that has to know what a CVPixelBuffer is; backends whose
// frames always carry CPU pixels return an empty image.
Graphics::Image nativeBufferToImage(void* buffer);
} // namespace eacp::Video
